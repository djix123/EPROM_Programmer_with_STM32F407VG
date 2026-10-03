#include "sst39sf040.h"
#include "stm32f4xx_hal.h"

/* ---------------------------------------------------------------------
 * Bit-banged GPIO driver (no FSMC).
 *
 * STM32F401CE target note: this MCU's 48-pin package (LQFP48/UFQFPN48)
 * has no Port D or Port E at all, and Port C is only partially present
 * (PC13-PC15 only -- PC0-PC3 are NOT bonded out on this package, unlike
 * the 64/100-pin parts; confirmed against ST datasheet DS9716 Table 8,
 * whose UQFN48 column shows "-" for PC0-PC3) -- unlike the LQFP100
 * STM32F407VE/VG this driver originally targeted, where the full Ports D
 * and E gave two whole 16-pin ports to dedicate to the bus. The layout
 * below re-derives the same "one port write per bus phase" trick using
 * Ports A/B/C instead.
 * It happens that on this package *every* GPIO is 5V-tolerant (FT) --
 * see the FT rationale in README.md -- so, unlike the F407 port, there
 * was no need to dodge non-FT pins when choosing this layout.
 *
 * Pin map, chosen so the address bus is two single-instruction port
 * writes and the data byte is another:
 *
 *   GPIOB = A0-A10 on bits 0-10, A12-A15 on bits 12-15, always output.
 *       PB11 does NOT exist on this package's 48-pin package -- Port B
 *       is only 15 pins here (unlike the F407's full 16), so A11 can't
 *       live at its "natural" bit 11 position. It's relocated to PA15
 *       (see below) instead of repacking A12-A15 down by one bit, so
 *       every other address line keeps its 1:1 An-to-PBn wiring. One
 *       write to GPIOB->ODR still sets the whole word (bit 11 is simply
 *       masked out -- there's no pad behind it to drive).
 *
 *   GPIOA:
 *     bits 0-7   = D0-D7  (bidirectional: input while reading, output
 *                  while writing -- switched via MODER)
 *     bits 8-10  = A16,A17,A18 (always output)
 *     bits 11-12 = USB_OTG_FS (D-/D+) -- CubeMX-owned, untouched here
 *     bit  13    = WE# (always output, active low) -- repurposed SWDIO
 *     bit  14    = OE# (always output, active low) -- repurposed SWCLK
 *                  SWD stops working once SST_Init() claims these two
 *                  pins; reflash by holding BOOT0 through a reset (ST's
 *                  system bootloader runs instead and leaves SWD alone --
 *                  see README "Flashing / debugging"). The mapping is
 *                  chosen for reset safety: until SST_Init() runs, PA13
 *                  keeps SWDIO's internal pull-up, holding WE# deasserted
 *                  so no write cycle can occur; PA14 keeps SWCLK's
 *                  pull-down, asserting OE#, which is harmless (D0-D7 are
 *                  MCU inputs then). Swapping them would hold WE# low
 *                  through reset.
 *     bit  15    = A11 (always output) -- the only free pin left on a
 *                  port that already has an address BSRR write, so
 *                  putting A11 here keeps the address bus at 2 writes
 *                  total instead of needing a 3rd port. PA15 defaults
 *                  to JTDI (full-JTAG) at reset, same as PB3/PB4 below
 *                  it in GPIOA's init -- harmless since full JTAG isn't
 *                  used.
 *   A11 and A16-A18 are set together via one BSRR write (atomic
 *   set/reset of just those bits) so it never disturbs the data pins or
 *   OE#/WE# living on the same port.
 *
 *   GPIOC: unused. CE# is NOT driven by the MCU. It should be pulled
 *   HIGH (external pull-up) so the flash stays deselected by default --
 *   e.g. while the MCU is in reset or running ST's bootloader during a
 *   reflash -- and must be set to GND by hand (jumper/switch) before the
 *   flash is accessed. The firmware assumes CE# is already low once it
 *   is running; with CE# high, reads return garbage and writes/erases
 *   are ignored. PC13-PC15 are the only Port C pins
 *   on this package and sit behind the backup-domain power switch (2MHz /
 *   30pF cap, RTC/LSE can claim them); PC14/PC15 were tried for OE#/WE#
 *   but didn't work on Black-Pill-style boards (32.768kHz crystal + load
 *   caps), which is why OE#/WE# are on PA13/PA14 above.
 *
 *   Pin-existence facts here are from ST's DS9716 (STM32F401xB/C) Table
 *   8; the F401CE's own datasheet is DS10086 (F401xD/E), whose 48-pin
 *   pinout is identical.
 *
 * With CE# grounded for a session, OE#/WE# do all the per-cycle work and
 * WE# must never glitch low -- see the reset-safety note on PA13 above.
 * ------------------------------------------------------------------- */

#define OE_WE_PORT  GPIOA
#define WE_PIN      GPIO_PIN_13
#define OE_PIN      GPIO_PIN_14

#define OE_LOW()   (OE_WE_PORT->BSRR = ((uint32_t)OE_PIN) << 16)
#define OE_HIGH()  (OE_WE_PORT->BSRR = (uint32_t)OE_PIN)
#define WE_LOW()   (OE_WE_PORT->BSRR = ((uint32_t)WE_PIN) << 16)
#define WE_HIGH()  (OE_WE_PORT->BSRR = (uint32_t)WE_PIN)

/* Bus timing margin. 30 cycles is ~360ns at the F401's max 84MHz HCLK
 * (roughly 2x more conservative in real time than the ~180ns this same
 * cycle count gave on the F407's 168MHz HCLK) -- deliberately
 * conservative against typical 70-150ns flash access/pulse-width specs,
 * but NOT bench-verified for your exact chip's speed grade. Tighten
 * only after checking your datasheet and ideally a scope capture.
 *
 * OE#/WE# are on normal high-speed PA13/PA14, so no pin speed cap
 * constrains how far this can be tightened. */
#define BUS_DELAY_CYCLES 30u

static inline void bus_delay(void)
{
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < BUS_DELAY_CYCLES) { }
}

/* A0-A10,A12-A15 on GPIOB (whole port minus the nonexistent PB11, one
 * write) + A11,A16-A18 on GPIOA bits 15,8-10 (BSRR, so D0-D7 on the
 * same port are untouched). */
static inline void set_address(uint32_t addr)
{
    GPIOB->ODR = (uint16_t)(addr & 0xF7FFu);  /* bit 11 excluded: no pin there */

    uint32_t a11    = (addr >> 11) & 0x1u;    /* A11 -> PA15 */
    uint32_t a16_18 = (addr >> 16) & 0x7u;    /* A16,A17,A18 -> PA8,PA9,PA10 */
    uint32_t bits = (a11 << 15) | (a16_18 << 8);
    uint32_t mask = (0x1u << 15) | (0x7u << 8);
    GPIOA->BSRR = (bits & mask) | ((~bits & mask) << 16);
}

/* GPIOA pins 0-7 mode bits live in the low 16 bits of MODER (2 bits/pin).
 * Clearing them = input (00); setting the 01-per-pin pattern = output.
 * A16-A18 (bits 8-10), USB (11-12), WE#/OE# (13-14) and A11 (15) live in
 * the upper bits of this same register and are untouched by this mask. */
static inline void data_pins_input(void)
{
    GPIOA->MODER &= ~0x0000FFFFu;
}

static inline void data_pins_output(void)
{
    uint32_t moder = GPIOA->MODER & ~0x0000FFFFu;
    GPIOA->MODER = moder | 0x00005555u;
}

static inline void write_data(uint8_t data)
{
    /* bits 0-7 only -- upper 16 bits of both masks are 0, so this never
     * touches A16-A18, USB, WE#/OE# or A11 on bits 8-15 */
    GPIOA->BSRR = (uint32_t)data | ((uint32_t)(uint8_t)(~data) << 16);
}

static inline uint8_t read_data(void)
{
    return (uint8_t)(GPIOA->IDR & 0xFFu);
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
    /* Bus turnaround: if the previous cycle was a read (DQ7 polling, ID
     * readback), OE# only just went high, and the flash keeps driving
     * D0-D7 for its output-disable time after that (tDF/tOHZ, ~20-30ns,
     * plus the OE# edge itself on hand-wired buses). Wait before the MCU
     * starts driving the same pins, to avoid contention. */
    bus_delay();
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

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* GPIOB: A0-A10,A12-A15, plain push-pull output -- this port is
     * dedicated to the address bus so a single GPIOB->ODR write can
     * blast the whole word in one go (see set_address()). PB11 doesn't
     * exist on this package, so it's excluded from the pin mask here
     * (harmless either way -- HAL_GPIO_Init() on a nonexistent pin bit
     * is a no-op -- but explicit beats relying on that).
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
    gpio.Pin   = GPIO_PIN_All & ~GPIO_PIN_11;
    HAL_GPIO_Init(GPIOB, &gpio);
    GPIOB->ODR = 0x0000u;

    /* GPIOA bits 8-10,15: A16,A17,A18,A11, always-output. No power-on
     * glitch hazard here (unlike OE#/WE# below): these aren't
     * active-low control lines, so whatever address value they happen
     * to come up driving is harmless as long as OE#/WE# are deasserted.
     * Bit 15 (A11) overrides PA15's reset-default JTDI function, which
     * is fine since full JTAG isn't used. Bits 11-12 (USB) are left
     * alone; bits 13-14 (WE#/OE#) are set up below. */
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* Control lines, always-output, active low: WE# = PA13, OE# = PA14
     * (repurposed SWDIO/SWCLK -- SWD stops working from here on; see
     * the GPIOA comment at the top of this file). CE# isn't driven
     * here: it's pulled high externally and must be grounded by hand
     * before the flash is accessed.
     *
     * IMPORTANT: pre-load OE#/WE# HIGH via BSRR *before* switching these
     * pins to output mode. GPIOx_ODR resets to 0 on every MCU reset, and
     * HAL_GPIO_Init() only touches MODER/OSPEEDR/etc, never ODR -- so
     * without this, the instant these pins become outputs they'd both
     * start driven LOW (falsely asserted), and the WE_HIGH() call that
     * follows would then produce a real WE# rising edge with CE# always
     * low: exactly the trigger for a write cycle, at address 0x00000,
     * with whatever garbage is on the not-yet-configured data pins. That
     * would be a spurious write to the flash on every single boot.
     * Preloading ODR first means the pins come up already high with no
     * transient low state at all. */
    OE_WE_PORT->BSRR = (uint32_t)(OE_PIN | WE_PIN);

    /* PA13/PA14: same HIGH-speed push-pull output as the rest of the bus
     * (`gpio` still holds that config). This overrides their AF0 SWD
     * function and drops SWDIO's pull-up / SWCLK's pull-down. */
    gpio.Pin = OE_PIN | WE_PIN;
    HAL_GPIO_Init(OE_WE_PORT, &gpio);
    /* Already high from the BSRR preload above -- belt-and-suspenders. */
    OE_HIGH();
    WE_HIGH();

    /* GPIOA bits 0-7: D0-D7, default to input (safe: avoids driving
     * against the flash chip at power-up). Switched to output on demand
     * by data_pins_output() during writes. */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pin  = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 |
                GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &gpio);
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
