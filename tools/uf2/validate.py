#!/usr/bin/env python3
"""Reject anything except a complete nRF52840 application-only UF2 image."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

APP_START = 0x26000
APP_END = 0xE0000
APP_RAM_END = 0x2003FC00
FAMILY = 0xADA52840
MAGIC = (0x0A324655, 0x9E5D5157, 0x0AB16F30)


def validate(data: bytes) -> dict[str, int]:
    if not data or len(data) % 512:
        raise ValueError("UF2 must contain complete 512-byte blocks")
    count = len(data) // 512
    blocks: dict[int, tuple[int, bytes]] = {}
    for offset in range(0, len(data), 512):
        block = data[offset:offset + 512]
        first, second, flags, address, length, number, total, family = (
            struct.unpack_from("<8I", block)
        )
        if (first, second, struct.unpack_from("<I", block, 508)[0]) != MAGIC:
            raise ValueError("bad UF2 magic")
        if flags != 0x2000 or family != FAMILY:
            raise ValueError("expected plain nRF52840 flash blocks with family ID")
        if length != 256 or address % 256:
            raise ValueError("expected 256-byte aligned UF2 payloads")
        if address < APP_START or address + length > APP_END:
            raise ValueError(f"block targets protected/non-application address {address:#x}")
        if total != count or number >= count or number in blocks:
            raise ValueError("incomplete or duplicate UF2 block numbering")
        blocks[number] = (address, block[32:32 + length])
    ordered = [blocks[number] for number in range(count)]
    for number, (address, _) in enumerate(ordered):
        if address != APP_START + number * 256:
            raise ValueError("UF2 application has gaps, overlaps, or a wrong start")
    stack, reset = struct.unpack_from("<2I", ordered[0][1])
    if not 0x20000000 < stack <= APP_RAM_END or stack % 8:
        raise ValueError("invalid application initial stack pointer")
    if not reset & 1 or not APP_START <= (reset & ~1) < APP_START + count * 256:
        raise ValueError("invalid application Thumb reset vector")
    return {"blocks": count, "start": APP_START, "end": APP_START + count * 256}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    try:
        result = validate(args.image.read_bytes())
    except (OSError, ValueError) as exc:
        parser.exit(1, f"UF2 validation failed: {exc}\n")
    print(f"Application UF2 verified: {result['blocks']} blocks, "
          f"{result['start']:#x}..{result['end'] - 1:#x}")


if __name__ == "__main__":
    main()
