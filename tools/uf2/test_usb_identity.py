"""Reject unrelated shared-ID devices and ambiguous native-USB targets."""
from pathlib import Path
import tempfile
import unittest

from usb_identity import SERIAL_PREFIX, list_usb_devices, read_usb_ids, select_device


class UsbIdentityTest(unittest.TestCase):
    def device(self, suffix="0123456789ABCDEF", **changes):
        device = {"vid": 0x16c0, "pid": 0x27df, "serial": SERIAL_PREFIX + suffix}
        device.update(changes)
        return device

    def test_selects_token_among_shared_id_devices(self):
        token = self.device()
        unrelated = self.device(serial="someone@example.com:other-device")
        wrong_id = self.device(pid=0x27db)
        self.assertIs(select_device([unrelated, wrong_id, token], 0x16c0, 0x27df), token)

    def test_missing_or_misleading_serial_does_not_match(self):
        for serial in ("", "totp-01", "x" + SERIAL_PREFIX + "0123456789ABCDEF",
                       SERIAL_PREFIX + "0123456789ABCDEFextra", SERIAL_PREFIX + "0123",
                       SERIAL_PREFIX + "0123456789abcdef", SERIAL_PREFIX + "G" * 16):
            with self.subTest(serial=serial), self.assertRaisesRegex(ValueError, "found 0"):
                select_device([self.device(serial=serial)], 0x16c0, 0x27df)
        with self.assertRaisesRegex(ValueError, "found 0"):
            select_device([{"vid": 0x16c0, "pid": 0x27df}], 0x16c0, 0x27df)

    def test_multiple_tokens_are_ambiguous(self):
        with self.assertRaisesRegex(ValueError, "found 2"):
            select_device([self.device(), self.device("FEDCBA9876543210")], 0x16c0, 0x27df)

    def test_reads_build_overrides_and_rejects_old_build(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "zephyr").mkdir()
            config = build / "zephyr/.config"
            config.write_text("CONFIG_TOTP_USB_PID=0x0001\nCONFIG_TOTP_USB_VID=0x1209\n")
            self.assertEqual(read_usb_ids(build), (0x1209, 0x0001))
            config.write_text("CONFIG_TOTP_USB_PID=0x0001\n")
            with self.assertRaisesRegex(ValueError, "rebuild"):
                read_usb_ids(build)

    def test_sysfs_enumeration_handles_interfaces_and_missing_serials(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "1-1:1.0").mkdir()
            for name, serial in (("1-1", SERIAL_PREFIX + "0123456789ABCDEF"), ("1-2", None)):
                path = root / name
                path.mkdir()
                (path / "idVendor").write_text("16c0\n")
                (path / "idProduct").write_text("27df\n")
                if serial:
                    (path / "serial").write_text(serial + "\n")
            devices = list_usb_devices(root)
            self.assertEqual(len(devices), 2)
            self.assertEqual(select_device(devices, 0x16c0, 0x27df)["serial"],
                             SERIAL_PREFIX + "0123456789ABCDEF")


if __name__ == "__main__":
    unittest.main()
