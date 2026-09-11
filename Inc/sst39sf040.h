#ifndef SST39SF040_H
#define SST39SF040_H

#include <stdint.h>
#include <stdbool.h>

/* No FSMC memory window in this build -- access is bit-banged over GPIO
 * (see SST_Init() in sst39sf040.c for the pin map). SST_BASE_ADDR isn't
 * needed anymore; addresses passed to the functions below are plain
 * byte offsets into the chip, same as before. */

#define SST_SIZE_BYTES       (512UL * 1024UL)   /* 4 Mbit = 512 KB -- both
                                                    supported chips are this size */

/* NOTE: sector size is NOT fixed at compile time -- the SST39SF040 (4KB
 * sectors) and AM29F040B (64KB sectors) are both supported, auto-detected
 * at runtime from the chip's manufacturer/device ID. Use SST_GetSectorSize()
 * below rather than assuming a value. These two macros describe only the
 * SST39SF040 default profile used before a chip has been identified. */
#define SST_DEFAULT_SECTOR_SIZE  (4UL * 1024UL)
#define SST_MANUFACTURER_ID      0xBFu   /* SST39SF040 -- kept for reference */
#define SST_DEVICE_ID             0xB7u   /* SST39SF040 -- kept for reference */

typedef enum {
    SST_OK = 0,
    SST_ERR_TIMEOUT,
    SST_ERR_VERIFY,
    SST_ERR_ID,
    SST_ERR_RANGE
} sst_status_t;

void         SST_Init(void);

/* Reads the chip's manufacturer/device ID and, if it matches a known
 * profile (SST39SF040 or AM29F040B), switches SST_GetSectorSize() /
 * SST_GetChipName() to reflect that chip. Returns SST_ERR_ID (but still
 * fills in the raw IDs) if the chip isn't recognized. */
sst_status_t SST_ReadID(uint8_t *manufacturer_id, uint8_t *device_id);

uint8_t      SST_ReadByte(uint32_t addr);
void         SST_ReadBuffer(uint32_t addr, uint8_t *buf, uint32_t len);
sst_status_t SST_ChipErase(void);
sst_status_t SST_SectorErase(uint32_t sector_addr);
sst_status_t SST_WriteByte(uint32_t addr, uint8_t data);
sst_status_t SST_WriteBuffer(uint32_t addr, const uint8_t *buf, uint32_t len);
sst_status_t SST_VerifyBuffer(uint32_t addr, const uint8_t *buf, uint32_t len);

/* Reflects whichever chip SST_ReadID() last matched (SST39SF040 by
 * default, before any successful ID read). */
uint32_t     SST_GetSectorSize(void);
const char  *SST_GetChipName(void);

#endif /* SST39SF040_H */
