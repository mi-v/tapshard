"""Compare freestanding firmware SHA-256 with an independent implementation."""
import ctypes
import hashlib
import random
import subprocess
import tempfile
import unittest
from pathlib import Path


class Sha256Test(unittest.TestCase):
    def test_vectors_and_padding_boundaries(self):
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory) / "sha256.so"
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                            "-std=c99", "-O2", str(root / "bootloader/sha256.c"),
                            "-o", str(library)], check=True, timeout=20)
            sha256 = ctypes.CDLL(str(library)).boot_sha256
            sha256.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
            sha256.restype = None
            rng = random.Random(0x52840)
            samples = [b"", b"abc", b"a" * 1000000, b"\xff" * 40960]
            samples += [rng.randbytes(n) for n in
                        list(range(256)) + [4095, 4096, 4097, 40959, 40960, 40961]]
            for data in samples:
                out = ctypes.create_string_buffer(32)
                sha256(data, len(data), out)
                self.assertEqual(out.raw, hashlib.sha256(data).digest(), len(data))


if __name__ == "__main__":
    unittest.main()
