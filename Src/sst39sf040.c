#include "sst39sf040.h"
#include "stm32f4xx_hal.h"

/* ---------------------------------------------------------------------
 * Bit-banged GPIO driver (no FSMC).
 *
 * Pin map, chosen so the address bus is one single-instruction port
 * write and the data byte is another:
 *
 *   GPIOD (all 16 pins, dedicated) = A0-A15, always output.
 *       One write to GPIOD->ODR sets the entire low address word.
 *
 *   GPIOE:
 *     bits 0-7   = D0-D7  (bidirectional: input while reading, output
 *                  while writing -- switched via MODER)
 *     bits 8-10  = A16,A17,A18 (always output)
 *     bits 11-13 = OE#, WE#, CE# (always output, active low)
 *     bits 14-15 = unused
 *   A16-A18 and the control lines are set via BSRR (atomic set/reset of
 *   just those bits) so they never disturb the data pins living on the
 *   same port.
 *
 * CE# is asserted once in SST_Init() and left low for the whole
 * session -- this is the only device on the bus, so there's no need to
 * toggle chip select per access. OE#/WE# do the real per-cycle work.
 * ------------------------------------------------------------------- */

#define OE_PIN   GPIO_PIN_11
#define WE_PIN   GPIO_PIN_12
#define CE_PIN   GPIO_PIN_13

#define OE_LOW()   (GPIOE->BSRR = ((uint32_t)OE_PIN) << 16)
#define OE_HIGH()  (GPIOE->BSRR = (uint32_t)OE_PIN)
#define WE_LOW()   (GPIOE->BSRR = ((uint32_t)WE_PIN) << 16)
#define WE_HIGH()  (GPIOE->BSRR = (uint32_t)WE_PIN)
#define CE_LOW()   (GPIOE->BSRR = ((uint32_t)CE_PIN) << 16)
#define CE_HIGH()  (GPIOE->BSRR = (uint32_t)CE_PIN)

/* Bus timing margin. 30 cycles is ~180ns at 168MHz HCLK -- deliberately
 * conservative against typical 70-150ns flash access/pulse-width specs,
 * but NOT bench-verified for your exact chip's speed grade. Tighten
 * only after checking your datasheet and ideally a scope capture. */
#define BUS_DELAY_CYCLES 30u

static inline void bus_delay(void)
{
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < BUS_DELAY_CYCLES) { }
}

/* A0-A15 on GPIOD (whole port, one write) + A16-A18 on GPIOE bits 8-10
 * (BSRR, so D0-D7/control on the same port are untouched). */
static inline void set_address(uint32_t addr)
{
    GPIOD->ODR = (uint16_t)(addr & 0xFFFFu);

    uint32_t hi   = (addr >> 16) & 0x7u;      /* A16,A17,A18 */
    uint32_t bits = hi << 8;                  /* target: PE8,PE9,PE10 */
    uint32_t mask = 0x7u << 8;
    GPIOE->BSRR = (bits & mask) | ((~bits & mask) << 16);
}

/* GPIOE pins 0-7 mode bits live in the low 16 bits of MODER (2 bits/pin).
 * Clearing them = input (00); setting the 01-per-pin pattern = output. */
static inline void data_pins_input(void)
{
    GPIOE->MODER &= ~0x0000FFFFu;
}

static inline void data_pins_output(void)
{
    uint32_t moder = GPIOE->MODER & ~0x0000FFFFu;
    GPIOE->MODER = moder | 0x00005555u;
}

static inline void write_data(uint8_t data)
{
    /* bits 0-7 only -- upper 16 bits of both masks are 0, so this never
     * touches A16-A18/OE#/WE#/CE# on bits 8-13 */
    GPIOE->BSRR = (uint32_t)data | ((uint32_t)(uint8_t)(~data) << 16);
}

static inline uint8_t read_data(void)
{
    return (uint8_t)(GPIOE->IDR & 0xFFu);
}

static uint8_t bb_read_byte(uint32_t addr)
{
    set_address(addr);
    data_pins_input();
    bus_delay();            /* address setup before enabling output */
    OE_LOW();
    bus_delay();             /* OE-to-data-valid access time */
    uint8_t data = read_data();
    OE_HIGH();
    return data;
}

static void bb_write_byte(uint32_t addr, uint8_t data)
{
    set_address(addr);
    data_pins_output();
    write_data(data);
    bus_delay();             /* address/data setup before WE pulse */
    WE_LOW();
    bus_delay();              /* WE pulse width */
    WE_HIGH();
    bus_delay();              /* data hold after WE rises */
}

/* JEDEC/SDP unlock addresses -- standard for SST/AMD-compatible parallel
 * flash, byte-addressed. */
#define SST_ADDR_5555   0x5555u
#define SST_ADDR_2AAA   0x2AAAu

#define SST_CMD_UNLOCK1     0xAAu
#define SST_CMD_UNLOCK2     0x55u
#define SST_CMD_AUTOSEL     0x90u
#define SST_CMD_PROGRAM     0xA0u
#define SST_CMD_ERASE       0x80u
#define SST_CMD_CHIP_ERASE  0x10u
#define SST_CMD_SECT_ERASE  0x30u
#define SST_CMD_RESET       0xF0u

/* Byte-program timing is essentially identical across both supported
 * chips (a few us to tens of us), so it isn't part of the per-chip
 * profile below. */
#define SST_BYTE_PROGRAM_TIMEOUT_US    20u      /* typ ~10us */

/* ---------------------------------------------------------------------
 * Chip profiles.
 *
 * The SST39SF040 and AM29F040B are both 512KB, 5V-only, 32-pin parallel
 * flash chips that share the exact same JEDEC pinout and the same
 * AMD/Fujitsu command set (unlock 0xAA@5555/0x55@2AAA, byte program
 * 0xA0, chip erase 0x10, sector erase 0x30, software ID 0x90/0xF0).
 * That's why one driver and one set of wiring covers both -- they only
 * differ in manufacturer/device ID, sector size, and erase timing, all
 * captured here. We read the ID at runtime and pick the matching
 * profile automatically; no wiring or build-time changes needed to
 * swap chips.
 *
 * Sector/chip-erase timeouts are conservative estimates with margin,
 * not exact datasheet figures -- check your chip's exact datasheet
 * (manufacturer and speed grade can vary these) if you tighten them.
 * ------------------------------------------------------------------- */
typedef struct {
    const char *name;
    uint8_t     mfr_id;
    uint8_t     dev_id;
    uint32_t    sector_size;
    uint32_t    sector_erase_timeout_us;
    uint32_t    chip_erase_timeout_us;
} chip_profile_t;

static const chip_profile_t chip_table[] = {
    /* name          mfr   dev   sector size   sector erase to   chip erase to */
    { "SST39SF040", 0xBFu, 0xB7u,  4096u,          30000u,          100000u  }, /* 4KB sectors, typ ~25/50ms */
    { "AM29F040B",  0x01u, 0xA4u, 65536u,        2000000u,        20000000u  }, /* 64KB sectors, generous margin */
};
#define CHIP_TABLE_COUNT (sizeof(chip_table) / sizeof(chip_table[0]))

/* Default to the first profile until SST_ReadID() confirms which chip
 * is actually present -- gives a sane fallback sector size if a caller
 * erases before reading the ID. */
static const chip_profile_t *active_profile = &chip_table[0];

static const chip_profile_t *find_profile(uint8_t mfr, uint8_t dev)
{
    for (uint32_t i = 0; i < CHIP_TABLE_COUNT; i++) {
        if (chip_table[i].mfr_id == mfr && chip_table[i].dev_id == dev) {
            return &chip_table[i];
        }
    }
    return NULL;
}

uint32_t SST_GetSectorSize(void)
{
    return active_profile->sector_size;
}

const char *SST_GetChipName(void)
{
    return active_profile->name;
}

/* ---- microsecond delay via DWT cycle counter (also used for bus_delay) ---- */
static void enable_dwt_cycle_counter(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

static void delay_us(uint32_t us)
{
    uint32_t cycles = (SystemCoreClock / 1000000U) * us;
    uint32_t start  = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles) { }
}

static void unlock_sequence(void)
{
    bb_write_byte(SST_ADDR_5555, SST_CMD_UNLOCK1);
    bb_write_byte(SST_ADDR_2AAA, SST_CMD_UNLOCK2);
}

void SST_Init(void)
{
    enable_dwt_cycle_counter();

    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* GPIOD: A0-A15, all 16 pins, plain push-pull output -- this port is
     * 100% dedicated to the address bus so a single GPIOD->ODR write can
     * blast the whole word in one GPIOD->ODR write (see set_address()).
     *
     * Speed is HIGH, not VERY_HIGH: this bus is meant for hand-wired
     * point-to-point connections (breadboard/dupont wire), not a
     * controlled-impedance PCB trace. VERY_HIGH's faster edge rate
     * mainly buys overshoot/ringing on that kind of wiring, not real
     * throughput -- our bus_delay() margins already dominate the actual
     * access time, so there's nothing to gain from the fastest setting
     * here. Worth revisiting if this ever moves to a real PCB. */
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Pin   = GPIO_PIN_All;
    HAL_GPIO_Init(GPIOD, &gpio);
    GPIOD->ODR = 0x0000u;

    /* GPIOE bits 8-13: A16,A17,A18 + OE#,WE#,CE#, all always-output.
     *
     * IMPORTANT: pre-load OE#/WE#/CE# HIGH via BSRR *before* switching
     * these pins to output mode. GPIOx_ODR resets to 0 on every MCU
     * reset, and HAL_GPIO_Init() only touches MODER/OSPEEDR/etc, never
     * ODR -- so without this, the instant these pins become outputs
     * they'd all start driven LOW (falsely asserted), and the
     * OE_HIGH()/WE_HIGH() calls that follow would then produce a real
     * WE# rising edge while CE# is still low: exactly the trigger for a
     * write cycle, at address 0x00000, with whatever garbage happens to
     * be on the not-yet-configured data pins. That's a spurious write
     * to the flash on every single boot. Preloading ODR first means the
     * pins come up already high with no transient low state at all. */
    GPIOE->BSRR = (uint32_t)(OE_PIN | WE_PIN | CE_PIN);

    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 |
               GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOE, &gpio);
    /* Already high from the BSRR preload above -- these are just
     * belt-and-suspenders confirmation, not load-bearing. */
    OE_HIGH();
    WE_HIGH();
    CE_HIGH();   /* stays high until asserted once at the end of this fn */

    /* GPIOE bits 0-7: D0-D7, default to input (safe: avoids driving
     * against the flash chip at power-up). Switched to output on demand
     * by data_pins_output() during writes. */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pin  = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 |
                GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOE, &gpio);

    /* Only device on this bus -- enable chip select once and leave it. */
    CE_LOW();
}

uint8_t SST_ReadByte(uint32_t addr)
{
    return bb_read_byte(addr);
}

void SST_ReadBuffer(uint32_t addr, uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = bb_read_byte(addr + i);
    }
}

sst_status_t SST_ReadID(uint8_t *manufacturer_id, uint8_t *device_id)
{
    unlock_sequence();
    bb_write_byte(SST_ADDR_5555, SST_CMD_AUTOSEL);

    *manufacturer_id = bb_read_byte(0x0000);
    *device_id       = bb_read_byte(0x0001);

    /* exit software-ID mode back to normal read array mode */
    bb_write_byte(0x0000, SST_CMD_RESET);

    const chip_profile_t *match = find_profile(*manufacturer_id, *device_id);
    if (match == NULL) {
        return SST_ERR_ID;
    }
    active_profile = match;   /* sector size / erase timeouts now match the real chip */
    return SST_OK;
}

/* DQ7 data-polling: while a program/erase is in progress, reading the
 * address being written returns the *complement* of the target bit 7.
 * Once it matches (and a repeat read confirms it, to dodge a race right
 * at completion), the operation is done. */
static sst_status_t poll_completion(uint32_t addr, uint8_t expected,
                                     uint32_t timeout_us, uint32_t poll_interval_us)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_us) {
        uint8_t readback = bb_read_byte(addr);
        if ((readback & 0x80u) == (expected & 0x80u)) {
            readback = bb_read_byte(addr);
            if ((readback & 0x80u) == (expected & 0x80u)) {
                return SST_OK;
            }
        }
        delay_us(poll_interval_us);
        elapsed += poll_interval_us;
    }
    return SST_ERR_TIMEOUT;
}

sst_status_t SST_WriteByte(uint32_t addr, uint8_t data)
{
    if (addr >= SST_SIZE_BYTES) return SST_ERR_RANGE;

    unlock_sequence();
    bb_write_byte(SST_ADDR_5555, SST_CMD_PROGRAM);
    bb_write_byte(addr, data);

    return poll_completion(addr, data, SST_BYTE_PROGRAM_TIMEOUT_US, 1);
}

sst_status_t SST_WriteBuffer(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (addr + len > SST_SIZE_BYTES) return SST_ERR_RANGE;

    for (uint32_t i = 0; i < len; i++) {
        sst_status_t st = SST_WriteByte(addr + i, buf[i]);
        if (st != SST_OK) return st;
    }
    return SST_OK;
}

sst_status_t SST_SectorErase(uint32_t sector_addr)
{
    uint32_t sector_size = active_profile->sector_size;

    if (sector_addr >= SST_SIZE_BYTES)      return SST_ERR_RANGE;
    if (sector_addr % sector_size != 0)     return SST_ERR_RANGE;

    unlock_sequence();
    bb_write_byte(SST_ADDR_5555, SST_CMD_ERASE);
    unlock_sequence();
    bb_write_byte(sector_addr, SST_CMD_SECT_ERASE);

    /* erased bytes read back as 0xFF */
    return poll_completion(sector_addr, 0xFF, active_profile->sector_erase_timeout_us, 100);
}

sst_status_t SST_ChipErase(void)
{
    unlock_sequence();
    bb_write_byte(SST_ADDR_5555, SST_CMD_ERASE);
    unlock_sequence();
    bb_write_byte(SST_ADDR_5555, SST_CMD_CHIP_ERASE);

    return poll_completion(0x0000, 0xFF, active_profile->chip_erase_timeout_us, 500);
}

sst_status_t SST_VerifyBuffer(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (addr + len > SST_SIZE_BYTES) return SST_ERR_RANGE;
    for (uint32_t i = 0; i < len; i++) {
        if (bb_read_byte(addr + i) != buf[i]) return SST_ERR_VERIFY;
    }
    return SST_OK;
}
