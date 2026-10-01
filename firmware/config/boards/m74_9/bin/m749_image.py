#!/usr/bin/env python3
"""Build the M74.9 three-stage image (OpenBLT second-stage + main application).

Merges the OpenBLT bootloader ELF and the main application ELF into one
addressed image spanning the two application CRC segments, computes the
CRC-32/MPEG-2 over both segments and stores it little-endian at 0x080FFFFC,
then emits Intel HEX or Motorola S-record output. The two software segments and
the CRC match the OEM resident-loader contract (see
docs/memory-layout-and-bootloader-details.md in the fw-m74.9 repo and
firmware/config/boards/m74_9/firmware/m749*.ld).

The merged image is what the M749 CLI flashes through the OEM UDS loader; the
bootloader and the main app share the one application CRC word.
"""

import argparse
import struct
from pathlib import Path

APP_START = 0x08001000   # OpenBLT vectors
CAL_START = 0x08060000   # calibration (never part of a software image)
CAL_CRC   = 0x0807FFFC
APP_SECOND = 0x08080000  # OpenBLT code (fixed OEM entry)
APP_CRC    = 0x080FFFFC  # application CRC word
APP_END    = 0x08100000  # exclusive

# The two application CRC segments (full domain: OpenBLT + main app).
SOFTWARE = ((APP_START, CAL_START), (APP_SECOND, APP_CRC))


def crc32_mpeg2(data, crc=0xFFFFFFFF):
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04C11DB7 if crc & 0x80000000 else 0)) & 0xFFFFFFFF
    return crc


def load_segments(elf):
    """Return [(load_address, bytes)] for PT_LOAD segments, using the LMA (p_paddr)."""
    if len(elf) < 52 or elf[:4] != b"\x7fELF":
        raise ValueError("expected a little-endian ELF image")
    if elf[4] != 1 or elf[5] != 1:
        raise ValueError("expected ELF32 little-endian")
    if struct.unpack_from("<H", elf, 16) != (2,):
        raise ValueError("expected an ARM executable")
    phoff = struct.unpack_from("<I", elf, 28)[0]
    phentsize = struct.unpack_from("<H", elf, 42)[0]
    phnum = struct.unpack_from("<H", elf, 44)[0]
    if phentsize != 32 or phoff + phentsize * phnum > len(elf):
        raise ValueError("invalid ELF program headers")
    segments = []
    for i in range(phnum):
        kind, offset, _, paddr, size, _, _, _ = struct.unpack_from("<8I", elf, phoff + i * phentsize)
        if kind != 1 or size == 0:
            continue
        if offset + size > len(elf):
            raise ValueError("truncated ELF load segment")
        segments.append((paddr, elf[offset:offset + size]))
    return segments


def in_software(addr, size):
    return any(start <= addr and addr + size <= end for start, end in SOFTWARE)


def build_image(openblt_elf, app_elf):
    image = bytearray(b"\xFF" * (APP_END - APP_START))
    populated = bytearray(len(image))
    for elf_bytes in (openblt_elf, app_elf):
        for addr, data in load_segments(elf_bytes):
            if addr < APP_START or addr + len(data) > APP_END:
                raise ValueError(f"segment 0x{addr:x} outside the application window")
            if not in_software(addr, len(data)):
                raise ValueError(f"segment 0x{addr:x} overlaps calibration or a CRC trailer")
            off = addr - APP_START
            if any(populated[off:off + len(data)]):
                raise ValueError(f"overlapping load segments at 0x{addr:x}")
            image[off:off + len(data)] = data
            populated[off:off + len(data)] = b"\x01" * len(data)
    # OpenBLT application vectors: SP 0x20020000, reset 0x08080001.
    if struct.unpack_from("<II", image) != (0x20020000, APP_SECOND + 1):
        raise ValueError("incorrect OpenBLT stack/reset vectors")
    crc = 0xFFFFFFFF
    for start, end in SOFTWARE:
        crc = crc32_mpeg2(image[start - APP_START:end - APP_START], crc)
    struct.pack_into("<I", image, APP_CRC - APP_START, crc)
    return [(APP_START, bytes(image[:CAL_START - APP_START])),
            (APP_SECOND, bytes(image[APP_SECOND - APP_START:]))]


def ihex_record(kind, address, data):
    raw = bytes([len(data)]) + struct.pack(">HB", address, kind) + data
    return ":" + (raw + bytes([-sum(raw) & 255])).hex().upper()


def encode(ranges, output_format):
    lines = []
    upper = None
    for address, data in ranges:
        for offset in range(0, len(data), 32):
            dest = address + offset
            block = data[offset:offset + 32]
            if output_format == "hex":
                if dest >> 16 != upper:
                    upper = dest >> 16
                    lines.append(ihex_record(4, 0, struct.pack(">H", upper)))
                lines.append(ihex_record(0, dest & 0xFFFF, block))
            elif output_format == "srec":
                raw = bytes([len(block) + 5]) + struct.pack(">I", dest) + block
                lines.append("S3" + (raw + bytes([~sum(raw) & 255])).hex().upper())
            else:
                raise ValueError("raw binaries are disabled; use addressed HEX or SREC")
    if output_format == "hex":
        lines.append(ihex_record(1, 0, b""))
    else:
        raw = bytes([5]) + struct.pack(">I", APP_SECOND + 1)
        lines.append("S7" + (raw + bytes([~sum(raw) & 255])).hex().upper())
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--openblt", type=Path, required=True, help="OpenBLT second-stage ELF")
    parser.add_argument("--app", type=Path, required=True, help="main application ELF")
    parser.add_argument("--format", choices=("hex", "srec"), required=True)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        ranges = build_image(args.openblt.read_bytes(), args.app.read_bytes())
        args.output.write_text(encode(ranges, args.format), encoding="ascii")
    except (ValueError, OSError, struct.error) as error:
        parser.exit(1, f"M74.9 image rejected: {error}\n")


if __name__ == "__main__":
    main()
