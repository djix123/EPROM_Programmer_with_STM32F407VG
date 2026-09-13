# SST39SF040 / AM29F040B programmer for STM32F401CE (USB-CDC controlled)

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

This `stm32f401ce` branch is a port from the original STM32F407VE/VG
target (100-pin LQFP, two full 16-pin ports free for the bus) down to
the much smaller STM32F401CE (48-pin LQFP48/UFQFPN48 -- the chip on
"Black Pill"-style hobby boards). **This port has not yet been
bench-verified against physical F401CE hardware** -- the pin map and
GPIO setup were re-derived carefully from the STM32F401 datasheet (see
the FT/package notes below), but only the original F407 build has the
"confirmed against real hardware" track record described next. Treat
this branch as needing the same chip-ID-readback / erase / program /
verify bring-up pass the F407 build already went through before relying
on it.

The F407 build was tested working on real hardware: an STM32F407VGT6
board bit-banging a socketed AM29F040B, exercised end-to-end from
`host/program.py` -- chip ID readback, chip/sector erase, program, and
read-verify all confirmed against the physical chip.

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

STM32F401CE ships as a 48-pin part -- LQFP48 (e.g. STM32F401CET6) or
UFQFPN48 (e.g. STM32F401CEU6, the marking on WeAct "Black Pill V3.0"
style boards). Both packages share the same pinout, so everything below
applies to either.

The 48-pin package is a much bigger constraint than the F407's LQFP100:
**Port D and Port E don't exist on it at all**, and **Port C is only
partially present** (PC0-PC3 and PC13-PC15 -- 7 of its 16 pins). Only
Port A and Port B are fully broken out (16 pins each). That rules out
the F407 driver's original "one full port for the low address word,
another full port for data + high address + control" layout outright --
there's no second full 16-pin port left once Port A gives up 4 pins to
USB (PA11/PA12) and SWD (PA13/PA14). This branch's layout instead uses
all of Port B for the low address word (mirroring the F407's dedicated
address port) and splits data + high address across the free bits of
Port A, with the three control lines moved to their own footprint on
Port C -- see the comment block at the top of `sst39sf040.c` for the
exact bit layout and why.

**Don't assume this generalizes to every 48-pin STM32F4.** The pin
counts above (Port C = PC0-3 + PC13-15, no Port D/E) are specific to the
STM32F401's LQFP48/UFQFPN48; other F4-family parts in a 48-pin package
can differ.

## ⚠️ Power / voltage

The SST39SF040 is a **5V-only part**. Power it from a separate 5V rail,
not the STM32's 3.3V. Good news for this target: **every GPIO pin on
the STM32F401's 48-pin package is 5V-tolerant ("FT")** -- confirmed
against ST's DS10086 pin definition table -- except PC14/PC15/PH0/PH1
when actually configured as an oscillator input, which doesn't apply to
any pin used below. That's a meaningfully simpler story than the F407,
where FT status varies pin-by-pin and had to be checked individually;
here, the chip driving 5V back onto any of these pins during reads is
safe by construction, not by careful pin selection. Decouple VCC/GND at
the chip with a 0.1µF cap, and tie any unused control pins per the
datasheet rather than leaving them floating.

## Wiring (bit-banged GPIO, no FSMC, no external ICs needed)

Pin layout is chosen so the whole 16-bit low address word is one single
GPIOB register write, and the data byte is one register on GPIOA --
see the comment block at the top of `sst39sf040.c` for exactly how.

| Flash pin | STM32 pin | Flash pin | STM32 pin |
|---|---|---|---|
| A0  | PB0  | A11 | PB11 |
| A1  | PB1  | A12 | PB12 |
| A2  | PB2  | A13 | PB13 |
| A3  | PB3  | A14 | PB14 |
| A4  | PB4  | A15 | PB15 |
| A5  | PB5  | A16 | PA8 |
| A6  | PB6  | A17 | PA9 |
| A7  | PB7  | A18 | PA10 |
| A8  | PB8  | D0  | PA0 |
| A9  | PB9  | D1  | PA1 |
| A10 | PB10 | D2  | PA2 |
| | | D3  | PA3 |
| | | D4  | PA4 |
| | | D5  | PA5 |
| | | D6  | PA6 |
| | | D7  | PA7 |

| Signal | STM32 pin |
|---|---|
| OE# | PC0 |
| WE# | PC1 |
| CE# | PC2 |

CE# is driven low once at startup and left there for the whole session
(this is the only device on the bus, so there's no need to toggle chip
select per access) -- OE#/WE#/CE# do the actual per-cycle work.

**Check your specific board before wiring.** PB2 doubles as BOOT1
(sampled at reset only when BOOT0 is pulled high to select system/RAM
bootloader mode; irrelevant with BOOT0 held low for normal flash boot,
which is this project's default). PC13 often carries an onboard LED or
button on Black Pill-style boards -- using it here (it isn't, in the
table above) would fight that. PA11-PA14 (USB D-/D+, SWDIO/SWCLK) and
PH0/PH1 (HSE crystal, if your board has one) are reserved and must not
be reused for the bus -- this driver never touches them.

USB: PA11 (USB_DM) / PA12 (USB_DP), device-only OTG FS -- CubeMX wires
these automatically when you enable the peripheral below.

**A note on confidence:** the pin-existence and FT facts above (which
ports/pins are present, and 5V-tolerant, on the LQFP48/UFQFPN48 package)
are drawn from ST's official STM32F401 datasheet (DS10086) pin
definition table. Unlike the LQFP100 facts on the F407 side of this
project, they have not additionally been cross-checked against a
physical board's silkscreen/schematic for this specific target -- if
your exact board (e.g. a Black Pill clone) documents a different pin
assignment for something you're relying on here (LED, button, crystal),
trust your board's own schematic over this table. The `BUS_DELAY_CYCLES`
margin in `sst39sf040.c` (~360ns at the F401's max 84MHz between each
bus phase) is a deliberately conservative estimate against typical
70-150ns flash timings, not something bench-verified against your exact
chip's speed grade -- if you see intermittent read/write errors, that
constant is the first thing to increase; if things work reliably and you
want more speed, it's also the first thing to try tightening, ideally
while watching the OE#/WE#/data waveforms on a scope.

## ⚠️ Porting the CubeMX-managed build

The pin map/GPIO setup lives in hand-written `sst39sf040.c` (already
done on this branch). The CubeMX-owned build -- device startup file,
linker script, HAL config, clock tree -- has also now been retargeted,
but the CubeMX version in use here had **no "Change target MCU/Board"
option**, so instead a brand-new `EPROM_Programmer_with_STM32F401CE.ioc`
was generated alongside the original `EPROM_Programmer_with_STM32F407VG.ioc`
into this same project directory. That's a reasonable path, but it has
two sharp edges worth knowing if you repeat it (or re-generate again):

1. **Generating from a new `.ioc` overwrites `Src/main.c` with a fresh
   CubeMX template**, silently discarding the `SST_Init()` /
   `FlashProto_Poll()` calls this project needs -- exactly what happened
   here and had to be restored by hand afterward. Those calls now live
   inside CubeMX's own `/* USER CODE BEGIN ... END */` markers in
   `main.c` (`SST_Init()` in the `SysInit` block, `FlashProto_Poll()` in
   the `WHILE` block), so a *future* "Generate Code" from the same
   `.ioc` should preserve them automatically -- but always diff `main.c`
   after regenerating to be sure, and reapply the `usbd_cdc_if.c` patch
   from "USB CDC setup" below (whether that one survives a given
   regeneration is inconsistent -- it happened to carry over this time,
   but check it every time regardless).
2. **CubeMX does *not* own `cmake/gcc-arm-none-eabi.cmake` or
   `cmake/starm-clang.cmake`** -- both hardcode the linker script
   filename (`-T ".../STM32F407xx_FLASH.ld"`) as a literal string, and
   CubeMX's code generation never touches them. Generating the new
   `.ioc` correctly produced `STM32F401xx_FLASH.ld` (512KB flash/96KB
   RAM, no CCM region -- matches the real F401CE) alongside the old
   `STM32F407xx_FLASH.ld` (1MB/128KB+64KB CCM), but **the build kept
   silently linking against the old F407 script** until both `.cmake`
   files were hand-edited to point at the new one. This is the kind of
   bug that doesn't show up as a build error -- it links fine either
   way since this firmware image easily fits in either memory map -- it
   just quietly gives the linker permission to place code/data outside
   the real F401CE's actual flash/RAM, which would only bite on
   hardware. **After any MCU retarget, always check
   `cmake --build build/Debug` output's final "Memory region" table
   matches your actual chip's flash/RAM size**, not just that it links.
3. `cmake/stm32cubemx/CMakeLists.txt` (the one file CubeMX *does* fully
   own) already got the `STM32F401xE` define and
   `startup_stm32f401xe.s` correctly on regeneration -- no manual fix
   needed there, unlike the two files above.
4. The old `EPROM_Programmer_with_STM32F407VG.ioc`,
   `STM32F407xx_FLASH.ld`, and `startup_stm32f407xx.s` are now unused on
   this branch (nothing references them) but were left in place rather
   than deleted -- remove them if you're confident this branch will
   never need to build for F407 again.

This branch's build has been verified to actually compile and link
against the correct F401CE memory map (`cmake --build`, checked into
CI-equivalent conditions locally). It has **not** been bench-verified
against physical hardware yet -- see the top of this README -- run the
same chip-ID / erase / program / verify pass the F407 build already
passed before trusting it with real data.

## USB CDC setup (do this in CubeMX)

Rolling a USB device stack by hand is a bad idea -- ST's CubeMX-generated
middleware is the standard, well-tested path, so use it and plug our
protocol code into it:

1. Open the project in STM32CubeMX (already retargeted per the section
   above if you're building for STM32F401CE).
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

## Build

Requires the `arm-none-eabi-gcc` toolchain on `PATH`, plus CMake and
Ninja.

```bash
cmake --preset Debug        # or Release
cmake --build build/Debug   # or build/Release
```

This produces `build/Debug/EPROM_Programmer_with_STM32F407VG.elf`,
which the flashing commands below expect.

### Releases

Prebuilt firmware (`.elf`/`.bin`/`.hex`) plus `host/program.py` are
attached to each tagged release --
[latest release](https://github.com/djix123/EPROM_Programmer_with_STM32F407VG/releases/latest).

`.github/workflows/release.yml` builds the firmware (Release preset)
in CI and, when the trigger is a tag matching `v*.*.*`, publishes a
GitHub Release with the built `.elf`, `.bin`, `.hex`, and
`host/program.py` attached. It can also be run manually
(workflow_dispatch) as a build-only sanity check without cutting a
release. To cut one:

```bash
git tag v1.0.0
git push origin v1.0.0
```

### Changelog

- **v0.9.2** -- `host/program.py`: add `--merge`, which reads back and
  preserves existing data in the leading/trailing gaps of a
  non-sector-aligned or partial-sector write instead of erasing it; add
  a confirmation prompt (skippable with `--force`) before a
  non-sector-aligned write erases a sector without `--merge`.
- **v0.9.1** -- `host/program.py`: `--chip-erase` no longer requires an
  image -- given alone, it erases the whole chip and exits. Also fixes
  a latent bug where a real chip/sector erase on the AM29F040B could
  exceed the host's fixed 3s serial read timeout and be misreported as
  a dropped connection; erase commands now use a timeout matching the
  firmware's own erase timeout budget (`chip_table[]` in
  `Src/sst39sf040.c`).
- **v0.9.0** -- Initial working release: bit-banged SST39SF040/AM29F040B
  flash driver, USB-CDC framed protocol, `host/program.py` PC-side tool,
  OpenOCD/ST-Link debugging setup, and this release-automation workflow.
  Tested against real AM29F040B hardware.

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
  are erased. If no `.bin` file is given, this erases the chip and
  exits -- e.g. `python host/program.py --port COM5 --chip-erase` wipes
  the chip without programming anything.
- `--merge` -- if `--address` isn't sector-aligned and/or the image
  doesn't fill out the rest of its last sector, read back the existing
  data in those leading/trailing gaps first and merge it back in before
  erasing/programming, so other data already sharing the erased sectors
  isn't lost. Only valid when programming an image; mutually exclusive
  with `--chip-erase` (which erases the whole chip regardless of what's
  there).
- `--force` -- skip the confirmation prompt shown when `--address` isn't
  sector-aligned and `--merge` isn't given (proceeds straight to
  erasing, discarding whatever else shares that sector). Has no effect
  when `--merge` is used, since that path never prompts.
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

## License

The original code in this repository (`sst39sf040.h/.c`,
`flash_usb_protocol.h/.c`, `main.c`, `host/`, and build/config files) is
MIT-licensed -- see [LICENSE](LICENSE).

The rest of `Src/`, `Inc/`, `Drivers/`, and `Middlewares/` is
STMicroelectronics/ARM code generated or vendored via STM32CubeMX (HAL
drivers, CMSIS, USB device stack) and stays under its own upstream
license (Apache-2.0, BSD-3-Clause, or ST's SLA0044) as reproduced in the
`LICENSE.txt` under each `Drivers/`/`Middlewares/` subdirectory.
