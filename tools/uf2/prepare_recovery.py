#!/usr/bin/env python3
"""Validate the pinned factory recovery HEX and preserve board voltage/reset UICR.

Run with the pyOCD Python environment (intelhex dependency). This only prepares
an ignored bench artifact; it never connects to or writes a target.
"""

import argparse
import hashlib
import struct
from pathlib import Path

from intelhex import IntelHex

FACTORY_SHA256 = "882b345c22bb1b38236ca055c81d411a6b00014272880e289015d1eb6de9ebf1"


def prepare(factory: Path, uicr_path: Path, output: Path) -> None:
    if hashlib.sha256(factory.read_bytes()).hexdigest() != FACTORY_SHA256:
        raise ValueError("unexpected recovery image; expected official nice!nano 0.9.2/S140 6.1.1")
    image = IntelHex(str(factory))
    allowed = ((0, 0x26000), (0xF4000, 0xFE000), (0x10001014, 0x1000101C))
    if any(not any(lo <= address < hi for lo, hi in allowed)
           for address in image.addresses()):
        raise ValueError("factory HEX writes outside the expected factory regions")
    uicr = uicr_path.read_bytes()
    if len(uicr) != 0x1000:
        raise ValueError("expected the complete 4096-byte UICR backup")
    for offset, expected in ((0x14, 0xF4000), (0x18, 0xFE000)):
        value = struct.unpack("<I", image.tobinarray(
            start=0x10001000 + offset, size=4).tobytes())[0]
        if value != expected:
            raise ValueError("unexpected factory boot address or MBR parameter page")
    # Keep this board's reset-pin routing and VDDH regulator setting through
    # a full-chip recovery. Never copy bond material or access-protection words.
    for offset in (0x200, 0x204, 0x20C):
        value = uicr[offset:offset + 4]
        if value != b"\xff" * 4:
            image.puts(0x10001000 + offset, value)
    if output.exists():
        raise ValueError("refusing to overwrite an existing recovery artifact")
    image.write_hex_file(str(output))
    print(f"Prepared pinned recovery image: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("factory", type=Path)
    parser.add_argument("uicr", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        prepare(args.factory, args.uicr, args.output)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Recovery preparation failed: {exc}\n")


if __name__ == "__main__":
    main()
