"""Ensure runaway memory and child processes stay bounded."""
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

RUNNER = Path(__file__).resolve().parents[1] / "run-limited.sh"


class LimitsTest(unittest.TestCase):
    def test_allocation_fails_without_oom(self):
        result = subprocess.run(["sh", str(RUNNER), "--memory-mib", "128",
                                 sys.executable, "-c", "bytearray(256 * 1024 * 1024)"],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn("MemoryError", result.stderr)

    def test_timeout_terminates_descendants(self):
        with tempfile.TemporaryDirectory() as directory:
            pidfile = Path(directory) / "pid"
            code = ("import subprocess,sys; from pathlib import Path; "
                    "child=subprocess.Popen(['sleep','60']); "
                    "Path(sys.argv[1]).write_text(str(child.pid)); child.wait()")
            result = subprocess.run(["sh", str(RUNNER), "--seconds", "1",
                                     sys.executable, "-c", code, str(pidfile)],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 124)
            pid = int(pidfile.read_text())
            stat = Path(f"/proc/{pid}/stat")
            time.sleep(.05)
            self.assertTrue(not stat.exists() or stat.read_text().split()[2] == "Z")
