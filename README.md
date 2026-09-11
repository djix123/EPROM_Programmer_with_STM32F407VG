# SST39SF040 / AM29F040B programmer for STM32F407VE/VG (USB-CDC controlled)

Reads/erases/programs an SST39SF040 **or AM29F040B** parallel flash chip
by bit-banging its parallel bus directly over GPIO, controlled from a PC
over a USB virtual COM port. Push a `.bin` file from a Python script and
it gets erased, written, and verified -- or just query the chip's
ID/size/sector info.

Both chips share the same 32-pin JEDEC pinout and the same AMD/Fujitsu
command set, so the wiring below is identical for either one. The
firmware reads the manufacturer/device ID at runtime and automatically
picks the right sector size (SST39SF040: 4KB sectors; AM29F040B: 64KB
sectors) and erase timeouts -- no rebuilding or rewiring to switch chips,
just plug in whichever one you have.

## Architecture

- `sst39sf040.h/.c` -- low-level driver. Bit-bangs the parallel bus
  directly over GPIO (no FSMC); this file issues the SST/AMD JEDEC
  command sequences (unlock, program, erase) and does DQ7 data-polling
  to detect completion.
- `flash_usb_protocol.h/.c` -- a small framed binary protocol running over
  USB CDC (ping / get-info / chip-erase / sector-erase / write / read),
  parsed independently of how USB happens to chunk the bytes.
- `main.c` -- brings both up and services the protocol in a loop.
- `host/program.py` -- PC-side tool: erase, program, verify a `.bin` file.

## ⚠️ Package note

STM32F407VE**T** and STM32F407VG**T** both use the 100-pin LQFP100
package -- "V" is the pin-count code, "E"/"G" only changes Flash size,
so they're pin-identical and this project works unmodified on either.

LQFP100 itself has a real limitation worth knowing about even though it
doesn't affect this design: **Port F and Port G don't exist on this
package** (confirmed against ST's pin tables in DS8626), which rules out
driving the flash chip's address bus through FSMC's normal non-muxed
mode. This project sidesteps that entirely by not using FSMC at all --
every address/data/control line is a plain bit-banged GPIO pin on Ports
D and E, both of which are fully present on LQFP100.

## ⚠️ Power / voltage

The SST39SF040 is a **5V-only part**. Power it from a separate 5V rail,
not the STM32's 3.3V. The GPIO pins used below are 5V-tolerant ("FT") on
the F407, so the chip driving 5V back onto them during reads is safe.
Decouple VCC/GND at the chip with a 0.1µF cap, and tie any unused control
pins per the datasheet rather than leaving them floating.

## Wiring (bit-banged GPIO, no FSMC, no external ICs needed)

Pin layout is chosen so the whole 16-bit low address word is one single
GPIOD register write, and the data byte is one register on GPIOE --
see the comment block at the top of `sst39sf040.c` for exactly how.

| Flash pin | STM32 pin | Flash pin | STM32 pin |
|---|---|---|---|
| A0  | PD0  | A11 | PD11 |
| A1  | PD1  | A12 | PD12 |
| A2  | PD2  | A13 | PD13 |
| A3  | PD3  | A14 | PD14 |
| A4  | PD4  | A15 | PD15 |
| A5  | PD5  | A16 | PE8 |
| A6  | PD6  | A17 | PE9 |
| A7  | PD7  | A18 | PE10 |
| A8  | PD8  | D0  | PE0 |
| A9  | PD9  | D1  | PE1 |
| A10 | PD10 | D2  | PE2 |
| | | D3  | PE3 |
| | | D4  | PE4 |
| | | D5  | PE5 |
| | | D6  | PE6 |
| | | D7  | PE7 |

| Signal | STM32 pin |
|---|---|
| OE# | PE11 |
| WE# | PE12 |
| CE# | PE13 |

CE# is driven low once at startup and left there for the whole session
(this is the only device on the bus, so there's no need to toggle chip
select per access) -- OE#/WE#/CE# do the actual per-cycle work.

USB: PA11 (USB_DM) / PA12 (USB_DP), device-only OTG FS -- CubeMX wires
these automatically when you enable the peripheral below.

**Check your specific board before wiring.** Some "Black F407" style
dev boards route an onboard microSD slot's SDIO_CMD line to PD2, which
this design uses for A8. If your board has one of those and you don't
want to desolder/reroute it, move A8 to a spare pin (PE14 or PE15 are
free) and update `sst39sf040.c`'s `set_address()`/GPIO init accordingly
-- just make sure whatever you pick isn't also claimed by something
else on your specific board.

**A note on confidence:** the pin-existence facts above (which ports are
present on LQFP100) are verified against ST's official datasheet. The
`BUS_DELAY_CYCLES` margin in `sst39sf040.c` (~180ns at 168MHz between
each bus phase) is a deliberately conservative estimate against typical
70-150ns flash timings, not something bench-verified against your exact
chip's speed grade -- if you see intermittent read/write errors, that
constant is the first thing to increase; if things work reliably and you
want more speed, it's also the first thing to try tightening, ideally
while watching the OE#/WE#/data waveforms on a scope.

## USB CDC setup (do this in CubeMX)

Rolling a USB device stack by hand is a bad idea -- ST's CubeMX-generated
middleware is the standard, well-tested path, so use it and plug our
protocol code into it:

1. Open the project in STM32CubeMX (or create one for your exact part,
   STM32F407VETx or STM32F407VGTx, if you don't have one yet -- they're
   pin-identical for everything in this project).
2. **Connectivity → USB_OTG_FS**: set Mode to `Device_Only`.
3. **Middleware → USB_DEVICE**: set Class to `Communication Device Class
   (Virtual Port Com)`.
4. Generate code. This creates `USB_DEVICE/App/` and
   `USB_DEVICE/Target/` with `usbd_cdc_if.c`, `usb_device.c`, etc., and
   pulls in `Middlewares/ST/STM32_USB_Device_Library`.
5. Open the generated `USB_DEVICE/App/usbd_cdc_if.c` and find
   `CDC_Receive_FS()`. Add one line at the top, before the buffer is
   re-armed:

   ```c
   static int8_t CDC_Receive_FS(uint8_t* Buf, uint32_t *Len)
   {
     FlashProto_OnUsbRx(Buf, *Len);           /* <-- add this line */

     USBD_CDC_SetRxBuffer(&hUsbDeviceFS, &Buf[0]);
     USBD_CDC_ReceivePacket(&hUsbDeviceFS);
     return (USBD_OK);
   }
   ```

   Add `#include "flash_usb_protocol.h"` near the top of that file.
6. Drop `flash_usb_protocol.h` into `Core/Inc/` (alongside
   `sst39sf040.h`) and `flash_usb_protocol.c` + `sst39sf040.c` + this
   `main.c` into `Core/Src/` (replacing the generated `main.c`, or
   merging the `SST_Init()` / `MX_USB_DEVICE_Init()` / `FlashProto_Poll()`
   calls into it).
7. Build and flash as usual.

The default CDC RX buffer (`APP_RX_DATA_SIZE`, usually 2048 bytes in the
generated `usbd_cdc_if.c`) is plenty for our largest frame (~520 bytes);
no need to change it.

## Flashing / debugging (OpenOCD + ST-Link)

The repo includes `openocd.cfg` and `STM32F407.svd` for flashing and
debugging over an ST-Link probe with OpenOCD -- this is what's set up
and tested in CLion (Settings → Build, Execution, Deployment → Embedded
Development → OpenOCD, and an OpenOCD run/debug configuration pointing
at the built `.elf`).

- `openocd.cfg` sources `interface/stlink.cfg` and `target/stm32f4x.cfg`
  from OpenOCD's stock config set -- no vendor-specific ST-Link variant
  or transport override should be needed for a standard ST-Link/V2 or
  onboard ST-Link. Uncomment the alternate `interface/*` line if your
  probe needs it.
- `STM32F407.svd` is the peripheral register map, used by CLion's (or
  GDB's) peripheral/SFR register view during a debug session.

From the command line, OpenOCD can also be driven directly, e.g.:

```bash
openocd -f openocd.cfg -c "program build/Debug/EPROM_Programmer_with_STM32F407VG.elf verify reset exit"
```

## Running the host tool

```bash
pip install pyserial
python host/program.py firmware.bin --port /dev/ttyACM0
```

On Windows, `--port` will be something like `COM5` (check Device Manager
-- it enumerates as an STMicroelectronics Virtual COM Port). On Linux
it's typically `/dev/ttyACM0`.

To just check what's plugged in without touching the chip's contents:

```bash
python host/program.py --port /dev/ttyACM0 --info
```

```
Manufacturer ID : 0xBF
Device ID       : 0xB7
Chip            : SST39SF040
Total size      : 524288 bytes (512 KB)
Sector size     : 4096 bytes (4 KB) x 128 sectors
```

Options:
- `--dump OUT.bin` -- read the chip and save it to a file instead of
  programming, then exit. Nothing is erased or written. Combine with
  `--address`/`--length` to read only part of the chip (e.g. back up
  just the app region before reprogramming it). Defaults to the whole
  chip from address 0.
- `--length N` -- bytes to read with `--dump` (decimal or `0x`-prefixed
  hex). Only used with `--dump`; defaults to everything from
  `--address` to the end of the chip.
- `--address ADDR` -- flash byte offset (decimal or `0x`-prefixed hex,
  default `0x0`). For programming, this is where the image is written --
  useful for laying out multiple images on one chip, e.g. a bootloader
  at `0x0` and an application at `0x10000`. Erasing still happens a
  whole sector at a time, so if `ADDR` isn't sector-aligned the rest of
  that sector's existing data gets erased along with it; the tool prints
  a note when that's the case. For `--dump`, this is where the read
  starts.
- `--info` -- print manufacturer/device ID, chip name, total size, and
  sector size, then exit. No `.bin` file needed, nothing is erased or
  written. Also useful as a quick "is this the SST39SF040 or the
  AM29F040B?" check, or a wiring sanity check before programming.
- `--chip-erase` -- erase the **entire chip** first instead of just the
  sectors the image covers. Slower, but leaves nothing stale beyond the
  image. Without this flag, only the sectors needed for the image size
  are erased.
- `--no-verify` -- skip the read-back verification pass.
- `--chunk-size N` -- bytes of flash data per USB write command
  (default 256, max 508). Lower it if you see CRC errors on a flaky
  cable; raise it for slightly less USB round-trip overhead.

Typical output:

```
Chip ID: manufacturer=0xBF device=0xB7 size=524288 bytes, sector=4096 bytes
Erasing sectors 0..31 (131072 bytes)...
Sector erase done in 0.94s
Programming...
Programming done in 3.21s
Verifying (reading back and comparing)...
Verify OK in 0.87s
Done.
```

## Protocol reference (for extending the tool)

Every command is a request frame answered by exactly one response frame:

```
Request:  0xAA  CMD  LEN_LO  LEN_HI  <payload>  CRC8
Response: 0x55  STATUS  LEN_LO  LEN_HI  <payload>  CRC8
```

CRC8 (poly `0x07`, init `0x00`) covers everything after the SOF byte.

| CMD | Name | Request payload | Response payload |
|---|---|---|---|
| 0x01 | PING | -- | -- |
| 0x02 | GET_INFO | -- | mfr(1) dev(1) chip_size(4 LE) sector_size(4 LE) |
| 0x03 | CHIP_ERASE | -- | -- |
| 0x04 | SECTOR_ERASE | addr(4 LE) | -- |
| 0x05 | WRITE | addr(4 LE) + data(≤512B) | -- |
| 0x06 | READ | addr(4 LE) + len(4 LE) | data(≤512B) |

STATUS: `0x00` OK, `0x01` CRC error, `0x02` timeout, `0x03` verify
failed, `0x04` chip ID mismatch, `0x05` out of range, `0x06` unknown
command.

## Adding another chip

Both the firmware and `program.py` keep chip profiles in one small table
each -- to support a third AMD/Fujitsu-command-set chip, add a row to
`chip_table[]` in `Src/sst39sf040.c` (name, manufacturer ID, device ID,
sector size, erase timeouts) and the matching `(mfr, dev): "name"` entry
to `KNOWN_CHIPS` in `host/program.py`. No protocol or wiring changes
needed as long as the new chip uses the same command set and 512KB size.

## What to extend next

1. **Progress bar / rate reporting** using something like `tqdm` instead
   of the current carriage-return counter.
2. **Speed up the bus timing.** `BUS_DELAY_CYCLES` in `sst39sf040.c` is
   deliberately conservative; once it's working reliably, try lowering
   it toward your chip's actual 70/90/120ns access-time spec for faster
   erase/program/read cycles -- watch for read/write errors as you go.
3. **A GUI wrapper** (e.g. a small Tk or web front-end) around
   `program.py`'s `FlashLink` class for less command-line-averse users.
