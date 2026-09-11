#ifndef FLASH_USB_PROTOCOL_H
#define FLASH_USB_PROTOCOL_H

#include <stdint.h>

/* Call this from the CubeMX-generated CDC_Receive_FS() in usbd_cdc_if.c,
 * passing it the same (Buf, *Len) that callback received. See README
 * "USB CDC setup" for the exact one-line edit. Safe to call from USB
 * interrupt context -- it only copies bytes into a staging buffer. */
void FlashProto_OnUsbRx(uint8_t *data, uint32_t len);

/* Call this once per main loop iteration. Parses any complete command
 * frames that have arrived and executes them. Erase/program commands
 * block here for a few ms to tens of ms while they poll the flash chip
 * for completion -- that's fine, the host just waits for the response. */
void FlashProto_Poll(void);

#endif /* FLASH_USB_PROTOCOL_H */
