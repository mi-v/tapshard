"""Check portable symbol-tool discovery without a probe or installed SDK."""

import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from toolchain import resolve_nm


class ToolchainTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="tapshard tools ")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.build = self.root / "build with spaces"
        (self.build / "zephyr").mkdir(parents=True)
        self.elf = self.build / "zephyr/zephyr.elf"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.environment = patch.dict(os.environ, {"PATH": str(self.bin)})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def executable(self, name):
        path = self.bin / name
        path.write_text("#!/bin/sh\nexit 0\n")
        path.chmod(0o755)
        return str(path)

    def cache(self, path):
        (self.build / "CMakeCache.txt").write_text(
            f"// Symbol tool\nCMAKE_NM:FILEPATH={path}\nCMAKE_NM-ADVANCED:INTERNAL=1\n"
        )

    def test_explicit_override_wins(self):
        explicit = self.executable("explicit nm")
        self.cache(self.executable("cached nm"))
        self.assertEqual(resolve_nm(self.elf, explicit), explicit)

    def test_cache_path_with_spaces_wins_over_path(self):
        cached = self.executable("cached nm")
        self.executable("arm-zephyr-eabi-nm")
        self.cache(cached)
        self.assertEqual(resolve_nm(self.elf), cached)

    def test_bootloader_elf_uses_same_build_metadata(self):
        cached = self.executable("cached nm")
        self.cache(cached)
        boot_elf = self.build / "security-bootloader/bootloader.elf"
        self.assertEqual(resolve_nm(boot_elf), cached)

    def test_no_cache_uses_path(self):
        fallback = self.executable("arm-zephyr-eabi-nm")
        self.assertEqual(resolve_nm(self.elf), fallback)

    def test_stale_cache_uses_path(self):
        self.cache(self.root / "removed SDK/nm")
        fallback = self.executable("arm-zephyr-eabi-nm")
        self.assertEqual(resolve_nm(self.elf), fallback)

    def test_invalid_override_does_not_silently_fall_back(self):
        self.cache(self.executable("cached nm"))
        with self.assertRaisesRegex(FileNotFoundError, "--nm"):
            resolve_nm(self.elf, str(self.root / "missing"))

    def test_unavailable_tool_has_actionable_error(self):
        with self.assertRaisesRegex(FileNotFoundError, "--nm"):
            resolve_nm(self.elf)


if __name__ == "__main__":
    unittest.main()
