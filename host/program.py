#!/usr/bin/env python3
"""
program.py -- host-side programmer for the STM32F407VE USB-CDC flash tool.
Supports the SST39SF040 and AM29F040B parallel flash chips (auto-detected
by the firmware from the chip's manufacturer/device ID).

Requires: pip install pyserial

Usage:
    python program.py --port /dev/ttyACM0 --info
    python program.py firmware.bin --port /dev/ttyACM0
    python program.py firmware.bin --port COM5 --chip-erase
    python program.py firmware.bin --port COM5 --no-verify
    python program.py app.bin --port COM5 --address 0x10000
    python program.py --port COM5 --dump backup.bin
    python program.py --port COM5 --dump partial.bin --address 0x10000 --length 0x8000
    python program.py --port COM5 --chip-erase
    python program.py patch.bin --port COM5 --address 0x1200 --merge
"""
import argparse
import struct
import sys
import time

import serial  # pip install pyserial

FRAME_SOF_REQ = 0xAA
FRAME_SOF_RESP = 0x55

CMD_PING = 0x01
CMD_GET_INFO = 0x02
CMD_CHIP_ERASE = 0x03
CMD_SECTOR_ERASE = 0x04
CMD_WRITE = 0x05
CMD_READ = 0x06

STATUS_NAMES = {
    0x00: "OK",
    0x01: "ERR_CRC",
    0x02: "ERR_TIMEOUT",
    0x03: "ERR_VERIFY",
    0x04: "ERR_ID",
    0x05: "ERR_RANGE",
    0x06: "ERR_UNKNOWN_CMD",
}

# Mirrors chip_table[] in Src/sst39sf040.c -- if you add a chip profile
# there, add the matching (mfr, dev): name entry here too.
KNOWN_CHIPS = {
    (0xBF, 0xB7): "SST39SF040",
    (0x01, 0xA4): "AM29F040B",
}

WRITE_CHUNK_SIZE = 256   # bytes of flash data per WRITE command (max 508)
READ_CHUNK_SIZE = 256    # bytes per READ command (max 512, firmware-side cap)

# Margin above the firmware's own worst-case erase timeouts (chip_table[]
# in Src/sst39sf040.c) -- must stay >= those or a slow-but-still-successful
# erase will look like a dropped connection to the host.
CHIP_ERASE_TIMEOUT_S = 25.0
SECTOR_ERASE_TIMEOUT_S = 5.0


def crc8(data: bytes) -> int:
    """Must match crc8() in flash_usb_protocol.c: poly 0x07, init 0x00."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


class FlashLink:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 3.0):
        # Baud rate is meaningless for USB CDC (it's not a real UART) but
        # pyserial's constructor requires a value; the device ignores it.
        self.ser = serial.Serial(port, baud, timeout=timeout)
        time.sleep(0.3)  # let the OS finish enumerating the virtual COM port
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def _send_frame(self, cmd: int, payload: bytes = b""):
        header = bytes([cmd]) + struct.pack("<H", len(payload))
        frame_crc = crc8(header + payload)
        frame = bytes([FRAME_SOF_REQ]) + header + payload + bytes([frame_crc])
        self.ser.write(frame)

    def _read_exact(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = self.ser.read(n - len(buf))
            if not chunk:
                raise TimeoutError(f"timed out waiting for {n} bytes from device")
            buf += chunk
        return buf

    def _transact(self, cmd: int, payload: bytes = b"", timeout: float = None):
        """Send a command and return (status, payload) -- never raises on
        a non-OK status, so callers can decide how to handle it. If
        `timeout` is given, the serial read timeout is temporarily raised
        to it for this call (needed for erase commands, which can legitimately
        take much longer than a normal command's response time)."""
        old_timeout = self.ser.timeout
        if timeout is not None:
            self.ser.timeout = timeout
        try:
            self._send_frame(cmd, payload)
            while True:
                sof = self._read_exact(1)
                if sof[0] == FRAME_SOF_RESP:
                    break
            header = self._read_exact(3)
            status = header[0]
            plen = struct.unpack("<H", header[1:3])[0]
            resp_payload = self._read_exact(plen) if plen else b""
            crc_recv = self._read_exact(1)[0]
            if crc8(header + resp_payload) != crc_recv:
                raise IOError("CRC mismatch in response from device")
            return status, resp_payload
        finally:
            if timeout is not None:
                self.ser.timeout = old_timeout

    def command(self, cmd: int, payload: bytes = b"", timeout: float = None):
        """Like _transact(), but raises IOError on a non-OK status. Used
        for commands where a failure should always abort the run."""
        status, resp_payload = self._transact(cmd, payload, timeout=timeout)
        if status != 0x00:
            name = STATUS_NAMES.get(status, f"0x{status:02X}")
            raise IOError(f"device returned {name} for command 0x{cmd:02X}")
        return resp_payload

    def ping(self):
        self.command(CMD_PING)

    def get_info(self, require_known: bool = True):
        """Returns (status, mfr, dev, chip_size, sector_size). If
        require_known is True (the default, used before erasing/
        programming), raises IOError when the chip ID isn't recognized --
        getting the sector size wrong would be dangerous. Pass False (used
        by --info) to inspect an unrecognized chip's raw ID instead."""
        status, payload = self._transact(CMD_GET_INFO)
        mfr, dev = payload[0], payload[1]
        chip_size = struct.unpack("<I", payload[2:6])[0]
        sector_size = struct.unpack("<I", payload[6:10])[0]
        if require_known and status != 0x00:
            name = STATUS_NAMES.get(status, f"0x{status:02X}")
            raise IOError(f"chip ID not recognized ({name}): "
                          f"manufacturer=0x{mfr:02X} device=0x{dev:02X}")
        return status, mfr, dev, chip_size, sector_size

    def chip_erase(self):
        # Firmware-side timeout is up to 20s for the AM29F040B (see
        # chip_erase_timeout_us in chip_table[], Src/sst39sf040.c) -- give
        # it margin above that rather than the default 3s command timeout.
        self.command(CMD_CHIP_ERASE, timeout=CHIP_ERASE_TIMEOUT_S)

    def sector_erase(self, addr: int):
        # Firmware-side timeout is up to 2s for the AM29F040B (see
        # sector_erase_timeout_us in chip_table[], Src/sst39sf040.c).
        self.command(CMD_SECTOR_ERASE, struct.pack("<I", addr), timeout=SECTOR_ERASE_TIMEOUT_S)

    def write(self, addr: int, data: bytes):
        self.command(CMD_WRITE, struct.pack("<I", addr) + data)

    def read(self, addr: int, length: int) -> bytes:
        return self.command(CMD_READ, struct.pack("<I", addr) + struct.pack("<I", length))


def print_chip_info(status: int, mfr: int, dev: int, chip_size: int, sector_size: int):
    name = KNOWN_CHIPS.get((mfr, dev))
    print(f"Manufacturer ID : 0x{mfr:02X}")
    print(f"Device ID       : 0x{dev:02X}")
    if name:
        print(f"Chip            : {name}")
    else:
        print("Chip            : unrecognized -- check wiring/power, or add this ID to "
              "KNOWN_CHIPS here and chip_table[] in Src/sst39sf040.c")
    print(f"Total size      : {chip_size} bytes ({chip_size // 1024} KB)")
    if name:
        print(f"Sector size     : {sector_size} bytes ({sector_size // 1024} KB) "
              f"x {chip_size // sector_size} sectors")
    else:
        print(f"Sector size     : {sector_size} bytes (fallback value -- unreliable "
              f"until the chip is recognized)")
    if status != 0x00:
        print(f"(device status: {STATUS_NAMES.get(status, hex(status))})")


def dump_chip(link: FlashLink, out_path: str, address: int, length: int = None):
    """Reads `length` bytes starting at `address` (default: from `address`
    to the end of the chip) and saves them to out_path. Read-only --
    nothing on the chip is touched."""
    status, mfr, dev, chip_size, sector_size = link.get_info(require_known=True)
    print_chip_info(status, mfr, dev, chip_size, sector_size)

    if address >= chip_size:
        sys.exit(f"Error: address 0x{address:06X} is past the end of the "
                  f"{chip_size}-byte chip")

    if length is None:
        length = chip_size - address
    if address + length > chip_size:
        sys.exit(f"Error: dumping {length} bytes from 0x{address:06X} would run past "
                  f"the end of the {chip_size}-byte chip (max {chip_size - address} bytes "
                  f"from this address)")

    print(f"Reading {length} bytes from 0x{address:06X}...")
    t0 = time.time()
    data = bytearray()
    for offset in range(0, length, READ_CHUNK_SIZE):
        chunk_len = min(READ_CHUNK_SIZE, length - offset)
        data += link.read(address + offset, chunk_len)
        done = offset + chunk_len
        print(f"  {done * 100 // length}% ({done}/{length} bytes)", end="\r")
    print(f"\nRead done in {time.time() - t0:.2f}s")

    with open(out_path, "wb") as f:
        f.write(data)
    print(f"Saved {len(data)} bytes to {out_path}")


def read_range(link: FlashLink, address: int, length: int) -> bytes:
    """Chunked read of `length` bytes starting at `address` (no progress
    output -- used internally for small merge reads, not user-facing dumps)."""
    data = bytearray()
    for offset in range(0, length, READ_CHUNK_SIZE):
        chunk_len = min(READ_CHUNK_SIZE, length - offset)
        data += link.read(address + offset, chunk_len)
    return bytes(data)


def merge_partial_sectors(link: FlashLink, image: bytes, address: int, sector_size: int):
    """If `image` doesn't fill the sectors it lands in all the way to their
    edges, reads back the existing data in those leading/trailing gaps and
    folds it into a widened, sector-aligned buffer so the caller can erase
    and rewrite whole sectors without losing what was already there. Must be
    called before erasing. Returns (new_address, new_image); if the image
    already exactly fills whole sectors, returns (address, image) unchanged."""
    sector_start = (address // sector_size) * sector_size
    sector_end = ((address + len(image) - 1) // sector_size + 1) * sector_size
    lead_len = address - sector_start
    trail_len = sector_end - (address + len(image))

    if lead_len == 0 and trail_len == 0:
        return address, image

    print(f"Merge: preserving existing data outside the image within its sector(s) "
          f"(reading {lead_len + trail_len} bytes from 0x{sector_start:06X}..0x{sector_end:06X} "
          f"before erasing)...")
    lead_data = read_range(link, sector_start, lead_len) if lead_len else b""
    trail_data = read_range(link, address + len(image), trail_len) if trail_len else b""
    return sector_start, lead_data + bytes(image) + trail_data


def erase_chip(link: FlashLink):
    """Erases the entire chip and nothing else -- used when --chip-erase is
    given without an image to program."""
    status, mfr, dev, chip_size, sector_size = link.get_info(require_known=True)
    print_chip_info(status, mfr, dev, chip_size, sector_size)

    print("Erasing entire chip...")
    t0 = time.time()
    link.chip_erase()
    print(f"Chip erase done in {time.time() - t0:.2f}s")


def program(link: FlashLink, image: bytes, address: int, chip_erase: bool,
            verify: bool, chunk_size: int, merge: bool = False, force: bool = False):
    status, mfr, dev, chip_size, sector_size = link.get_info(require_known=True)
    print_chip_info(status, mfr, dev, chip_size, sector_size)

    if address >= chip_size:
        sys.exit(f"Error: address 0x{address:06X} is past the end of the "
                  f"{chip_size}-byte chip")
    if address + len(image) > chip_size:
        sys.exit(f"Error: image is {len(image)} bytes starting at 0x{address:06X} "
                  f"(ends at 0x{address + len(image):06X}), chip only holds "
                  f"{chip_size} bytes")
    if not chip_erase and address % sector_size != 0:
        if merge:
            print(f"Note: address 0x{address:06X} is not sector-aligned "
                  f"(sector size is {sector_size} bytes) -- other data already sharing "
                  f"that sector will be preserved via --merge.")
        else:
            print(f"Note: address 0x{address:06X} is not sector-aligned "
                  f"(sector size is {sector_size} bytes) -- the whole sector containing "
                  f"it will still be erased, so any other data already in that sector "
                  f"will be lost too.")
            if not force:
                answer = input("Continue and erase that sector anyway? [y/N] ").strip().lower()
                if answer not in ("y", "yes"):
                    sys.exit("Aborted.")

    if merge:
        address, image = merge_partial_sectors(link, image, address, sector_size)

    if chip_erase:
        print("Erasing entire chip (this takes longer but leaves nothing stale behind)...")
        t0 = time.time()
        link.chip_erase()
        print(f"Chip erase done in {time.time() - t0:.2f}s")
    else:
        first_sector = address // sector_size
        last_sector = (address + len(image) - 1) // sector_size
        num_sectors = last_sector - first_sector + 1
        print(f"Erasing sectors {first_sector}..{last_sector} "
              f"(0x{first_sector * sector_size:06X}..0x{(last_sector + 1) * sector_size - 1:06X})...")
        t0 = time.time()
        for i, s in enumerate(range(first_sector, last_sector + 1)):
            link.sector_erase(s * sector_size)
            print(f"  sector {i + 1}/{num_sectors}", end="\r")
        print(f"\nSector erase done in {time.time() - t0:.2f}s")

    print(f"Programming at 0x{address:06X}...")
    t0 = time.time()
    total = len(image)
    for offset in range(0, total, chunk_size):
        chunk = image[offset:offset + chunk_size]
        link.write(address + offset, chunk)
        done = offset + len(chunk)
        print(f"  {done * 100 // total}% ({done}/{total} bytes)", end="\r")
    print(f"\nProgramming done in {time.time() - t0:.2f}s")

    if verify:
        print("Verifying (reading back and comparing)...")
        t0 = time.time()
        for offset in range(0, total, READ_CHUNK_SIZE):
            expected = image[offset:offset + READ_CHUNK_SIZE]
            actual = link.read(address + offset, len(expected))
            if actual != expected:
                mismatch = next(i for i in range(len(expected)) if expected[i] != actual[i])
                sys.exit(f"\nVerify FAILED at offset 0x{address + offset + mismatch:06X}: "
                         f"expected 0x{expected[mismatch]:02X}, got 0x{actual[mismatch]:02X}")
            done = offset + len(expected)
            print(f"  {done * 100 // total}%", end="\r")
        print(f"\nVerify OK in {time.time() - t0:.2f}s")


def main():
    ap = argparse.ArgumentParser(
        description="Program an SST39SF040 or AM29F040B via the STM32F407VE USB-CDC flasher")
    ap.add_argument("image", nargs="?",
                     help="path to the .bin file to program (omit with --info, --dump, or "
                          "a standalone --chip-erase)")
    ap.add_argument("--port", required=True, help="serial port, e.g. /dev/ttyACM0 or COM5")
    ap.add_argument("--address", type=lambda s: int(s, 0), default=0,
                     help="flash byte offset, decimal or 0x-prefixed hex (default 0x0). When "
                          "programming, this is where the image is written -- erase still "
                          "happens a whole sector at a time, so if this isn't sector-aligned "
                          "the rest of that sector's existing contents will be erased too. "
                          "When used with --dump, this is where the read starts.")
    ap.add_argument("--info", action="store_true",
                     help="print chip ID / manufacturer / device / size / sector info and "
                          "exit -- no image needed, nothing is erased or written")
    ap.add_argument("--dump", metavar="OUT.bin",
                     help="read the chip (or --length bytes starting at --address) and save "
                          "to OUT.bin, then exit -- read-only, nothing is erased or written")
    ap.add_argument("--length", type=lambda s: int(s, 0), default=None,
                     help="bytes to read with --dump (default: everything from --address to "
                          "the end of the chip). Ignored when programming.")
    ap.add_argument("--chip-erase", action="store_true",
                     help="erase the ENTIRE chip before programming, instead of only the "
                          "sectors the image covers (slower, but guarantees a clean chip "
                          "with no leftover data past the image). If no image is given, "
                          "erases the chip and exits -- no programming or verification")
    ap.add_argument("--merge", action="store_true",
                     help="if --address isn't sector-aligned and/or the image doesn't fill "
                          "out the rest of its last sector, read back the existing data in "
                          "those leading/trailing gaps first and merge it back in, so other "
                          "data already sharing the erased sectors isn't lost. Only valid "
                          "when programming an image; incompatible with --chip-erase (which "
                          "erases the whole chip regardless).")
    ap.add_argument("--force", action="store_true",
                     help="skip the confirmation prompt shown when --address isn't "
                          "sector-aligned and --merge isn't given (proceeds straight to "
                          "erasing, discarding whatever else shares that sector)")
    ap.add_argument("--no-verify", action="store_true", help="skip read-back verification")
    ap.add_argument("--chunk-size", type=int, default=WRITE_CHUNK_SIZE,
                     help="bytes of flash data per USB write command (default 256, max 508)")
    args = ap.parse_args()

    erase_only = args.chip_erase and not args.image

    modes_selected = sum([bool(args.info), bool(args.dump), bool(args.image), erase_only])
    if modes_selected == 0:
        ap.error("provide an image to program, or use --info, --dump, or --chip-erase")
    if modes_selected > 1:
        ap.error("--info, --dump, --chip-erase (without an image), and programming an "
                  "image are mutually exclusive -- pick one")

    if args.chunk_size > 508:
        sys.exit("--chunk-size must be <= 508 (firmware payload cap minus 4-byte address)")

    if args.merge and not args.image:
        ap.error("--merge only applies when programming an image")
    if args.merge and args.chip_erase:
        ap.error("--merge and --chip-erase are mutually exclusive -- --chip-erase erases "
                  "the whole chip anyway, so there's nothing to preserve")

    link = FlashLink(args.port)
    try:
        link.ping()

        if args.info:
            status, mfr, dev, chip_size, sector_size = link.get_info(require_known=False)
            print_chip_info(status, mfr, dev, chip_size, sector_size)
            return

        if args.dump:
            dump_chip(link, args.dump, address=args.address, length=args.length)
            return

        if erase_only:
            erase_chip(link)
            print("Done.")
            return

        with open(args.image, "rb") as f:
            image = f.read()

        program(link, image, address=args.address, chip_erase=args.chip_erase,
                verify=not args.no_verify, chunk_size=args.chunk_size, merge=args.merge,
                force=args.force)
        print("Done.")
    finally:
        link.close()


if __name__ == "__main__":
    main()
