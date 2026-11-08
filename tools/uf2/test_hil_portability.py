"""Host-only HIL workflows must not require a particular machine or a probe."""

import importlib.util
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


class HilPortabilityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location(
            "soak_portability", ROOT / "tools/hil/multihost_soak.py"
        )
        cls.soak = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.soak)

    def test_standalone_host_helper(self):
        with tempfile.TemporaryDirectory(prefix="standalone HIL ") as directory:
            helper = Path(directory) / "totp_hil.py"
            shutil.copyfile(ROOT / "tools/hil/totp_hil.py", helper)
            # Isolated mode excludes the repository and PYTHONPATH. This is
            # the same single-file deployment used by a remote capture host.
            result = subprocess.run([
                sys.executable, "-I", str(helper), "totp",
                "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", "--at", "59", "--digits", "8",
            ], cwd=directory, check=True, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.stdout.strip(), "94287082")

    def test_python_default_and_explicit_remote_paths(self):
        args = self.soak.build_parser().parse_args([
            "--results", "unused", "--secret", "JBSWY3DPEHPK3PXP",
            "--token-address", "00:11:22:33:44:55", "--linux-host", "user@linux",
            "--linux-key", "key", "--linux-helper", "/home/test user/totp_hil.py",
            "--windows-host", "user@windows", "--windows-key", "key",
            "--windows-script", r"C:\Users\Test User\Windows-Hil.ps1",
        ])
        self.assertEqual(args.pyocd_python, Path(sys.executable))
        self.assertEqual(args.linux_helper, "/home/test user/totp_hil.py")
        self.assertEqual(args.windows_script, r"C:\Users\Test User\Windows-Hil.ps1")

    def test_remote_paths_are_single_arguments(self):
        runner = self.soak.Soak.__new__(self.soak.Soak)
        runner.args = SimpleNamespace(
            linux_key=Path("key"), linux_host="user@linux",
            linux_helper="/home/test user/totp_hil.py",
            token_address="00:11:22:33:44:55",
            windows_key=Path("key"), windows_host="user@windows",
            windows_script=r"C:\Users\Test User\Windows-Hil.ps1",
        )
        with patch.object(self.soak, "run", return_value="") as run:
            runner.windows("CaptureInfo", "-Path", r"C:\capture data\capture.jsonl")
            command = run.call_args.args[0][-1]
            self.assertIn('"C:\\Users\\Test User\\Windows-Hil.ps1"', command)
            self.assertIn('"C:\\capture data\\capture.jsonl"', command)
            runner.connect_linux()
            command = run.call_args.args[0][-1]
            self.assertEqual(shlex.split(command)[1], runner.args.linux_helper)


if __name__ == "__main__":
    unittest.main()
