"""Artifact-boundary tests: UF2 must never overwrite boot/backup/settings."""

import struct
import unittest

from validate import APP_START, APP_RAM_END, FAMILY, MAGIC, validate


def image(address=APP_START, *, flags=0x2000, family=FAMILY):
    payload = struct.pack("<2I", APP_RAM_END, APP_START + 0x81) + bytes(248)
    header = struct.pack("<8I", MAGIC[0], MAGIC[1], flags, address, 256, 0, 1, family)
    return header + payload + bytes(220) + struct.pack("<I", MAGIC[2])


class ValidateTests(unittest.TestCase):
    def test_application(self):
        self.assertEqual(validate(image())["start"], APP_START)

    def test_protected_regions(self):
        for address in (0, 0x1000, 0x25000, 0xE0000, 0xEC000, 0xF4000,
                        0xFE000, 0xFF000, 0x10001000):
            with self.subTest(address=hex(address)), self.assertRaises(ValueError):
                validate(image(address))

    def test_unexpected_block_types(self):
        for flags in (0, 1, 0x2001, 0x6000):
            with self.subTest(flags=flags), self.assertRaises(ValueError):
                validate(image(flags=flags))
        with self.assertRaises(ValueError):
            validate(image(family=0))

    def test_incomplete_and_duplicate(self):
        for data in (b"", image()[:-1], image() + image()):
            with self.assertRaises(ValueError):
                validate(data)

    def test_vectors_and_magic(self):
        for offset, value in ((0, 0), (32, 0x10001000), (36, 0x1001), (508, 0)):
            data = bytearray(image())
            struct.pack_into("<I", data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                validate(data)

    def test_stack_cannot_overlap_reserved_marker_region(self):
        for stack in (APP_RAM_END + 8, 0x2003FFF0, 0x20040000):
            data = bytearray(image())
            struct.pack_into("<I", data, 32, stack)
            with self.subTest(stack=hex(stack)), self.assertRaises(ValueError):
                validate(data)


if __name__ == "__main__":
    unittest.main()
