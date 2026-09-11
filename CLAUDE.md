# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for an STM32F407VE/VG that reads/erases/programs an SST39SF040
or AM29F040B parallel NOR flash chip by bit-banging its parallel bus
directly over GPIO (no FSMC), controlled from a PC over USB-CDC (virtual
COM port). A PC-side tool pushes a `.bin` file over the serial link to
erase, write, and verify it, or just query chip ID/size/sector info.

Note: this is not (yet) a git repository.

## Build

Requires the `arm-none-eabi-gcc` toolchain on `PATH` and CMake + Ninja.

```bash
cmake --preset Debug        # or Release
cmake --build build/Debug   # or build/Release
```

This produces `build/Debug/EPROM_Programmer_with_STM32F407VG.elf`. There
are no unit tests in this repo — correctness is validated by flashing
the board and exercising it against real hardware (chip ID readback,
program/verify round-trip) via the host tool.

Flashing/debugging is expected via an external tool (e.g. ST-Link,
OpenOCD, CLion's embedded debugger) — nothing in-repo drives that.

Host tool (after flashing):

```bash
pip install pyserial
python host/program.py --port /dev/ttyACM0 --info      # sanity check
python host/program.py firmware.bin --port /dev/ttyACM0
```

## Architecture

The firmware has exactly two hand-written modules; everything else
under `Src/`/`Inc/` is STM32CubeMX-generated boilerplate (clock config,
USB device stack, HAL glue) that should be treated as generated code —
prefer regenerating from the `.ioc` file over hand-editing it, except
for the one deliberate patch documented below.

- **`sst39sf040.h/.c`** — low-level flash driver. Bit-bangs the 19-bit
  address bus and 8-bit data bus directly over GPIO ports D and E (see
  the pin table in README.md and the comment block at the top of the
  `.c` file for the exact mapping — it's chosen so the whole low
  address word is one GPIOD register write and the data byte is one
  GPIOE register write). Issues the SST/AMD JEDEC command sequences
  (unlock, program, erase) and does DQ7 data-polling to detect
  completion. Chip identity (SST39SF040 vs AM29F040B — different sector
  sizes and erase timeouts) is auto-detected at runtime via
  `SST_ReadID()`, driven by a small `chip_table[]` — adding a third
  AMD/Fujitsu-command-set chip means adding one row there (see README
  "Adding another chip").
- **`flash_usb_protocol.h/.c`** — a small framed binary protocol
  (ping / get-info / chip-erase / sector-erase / write / read) running
  over USB CDC, parsed independently of how USB happens to chunk bytes
  (`FlashProto_OnUsbRx()` feeds a staging buffer from the USB RX
  interrupt, `FlashProto_Poll()` parses/executes complete frames from
  the main loop). Frame format, command table, and status codes are
  documented in README.md "Protocol reference" — keep that doc in sync
  with any protocol change since it's also the spec for the PC-side
  tool.
- **`main.c`** — brings both up (`SST_Init()`, `MX_USB_DEVICE_Init()`)
  and services the protocol in a `while(1)` loop calling
  `FlashProto_Poll()`. Everything here is otherwise CubeMX-generated
  (`SystemClock_Config()`, `Error_Handler()`).
- **CubeMX/USB integration**: `USB_DEVICE/App/usbd_cdc_if.c`'s
  generated `CDC_Receive_FS()` has one manually-added line
  (`FlashProto_OnUsbRx(Buf, *Len);`) before the RX buffer is re-armed —
  this is the one hand-edit inside otherwise-generated code, and it
  needs to be re-applied if the USB middleware is ever regenerated from
  the `.ioc` file. See README.md "USB CDC setup" for the exact patch.
- **`host/program.py`** — PC-side tool (`pip install pyserial`). Its
  `FlashLink` class implements the same framed protocol as
  `flash_usb_protocol.c` (`crc8()` must stay byte-for-byte identical to
  the firmware's; frame parsing mirrors `_transact()`/`command()`).
  Three mutually-exclusive modes off one `argparse` CLI: program an
  image (default), `--info` (ID/size/sector query, no chip access), or
  `--dump OUT.bin` (read-only, honors `--address`/`--length`). Program
  mode erases only the sectors an image touches unless `--chip-erase`
  is passed, writes in `--chunk-size` chunks (default 256, firmware cap
  508), and read-verifies afterward unless `--no-verify` is set. Keeps
  a `KNOWN_CHIPS = {(mfr, dev): name}` table that must be kept in sync
  with `chip_table[]` in `Src/sst39sf040.c` when adding a chip —
  `get_info(require_known=True)` (used before erase/program) refuses to
  proceed on an unrecognized chip since guessing sector size would be
  dangerous; `--info` passes `require_known=False` to still show raw
  IDs for an unknown chip.

## Hardware constraints that shape the code

- **LQFP100 has no Port F or Port G** — this is *why* the design
  bit-bangs GPIO on Ports D/E instead of using FSMC's normal non-muxed
  address-bus mode. Don't "simplify" by reintroducing FSMC without
  accounting for this.
- **The flash chip is 5V-only**, powered from a separate rail from the
  STM32's 3.3V; the GPIOs used are 5V-tolerant ("FT") pins by design —
  if pins are ever reassigned, they must stay within the FT set.
- **`BUS_DELAY_CYCLES`** in `sst39sf040.c` is a deliberately
  conservative bus-timing margin (not bench-verified against a specific
  chip's speed grade). It's the first thing to touch when tuning for
  reliability (increase) vs. speed (decrease) — see README "What to
  extend next".
