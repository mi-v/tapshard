#!/usr/bin/env python3
"""Identify TapShard by its configured USB IDs and complete serial prefix.

This module also runs standalone on a Linux USB host, without pyOCD or a build.
"""
import argparse
import json
from pathlib import Path
import re

SERIAL_PREFIX = "mivlad@gmail.com:tapshard-"
SERIAL_PATTERN = re.compile(re.escape(SERIAL_PREFIX) + r"[0-9A-F]{16}")


def read_usb_ids(build):
    """Read the effective IDs, including overrides, from a Zephyr build."""
    config = Path(build) / "zephyr/.config"
    values = {}
    for line in config.read_text().splitlines():
        key, separator, value = line.partition("=")
        if separator and key in ("CONFIG_TOTP_USB_VID", "CONFIG_TOTP_USB_PID"):
            values[key] = int(value, 0)
    try:
        ids = values["CONFIG_TOTP_USB_VID"], values["CONFIG_TOTP_USB_PID"]
    except KeyError as exc:
        raise ValueError(f"Missing USB VID/PID in {config}; rebuild this firmware") from exc
    if not all(1 <= value <= 0xffff for value in ids):
        raise ValueError(f"Invalid USB VID/PID in {config}")
    return ids


def list_usb_devices(root=Path("/sys/bus/usb/devices")):
    devices = []
    for path in sorted(root.iterdir()):
        try:
            vid = int((path / "idVendor").read_text().strip(), 16)
            pid = int((path / "idProduct").read_text().strip(), 16)
        except (OSError, ValueError):
            continue
        try:
            serial = (path / "serial").read_text().strip()
        except OSError:
            serial = ""
        devices.append({"path": str(path), "vid": vid, "pid": pid, "serial": serial})
    return devices


def select_device(devices, vid, pid):
    matches = [device for device in devices
               if device["vid"] == vid and device["pid"] == pid
               and SERIAL_PATTERN.fullmatch(device.get("serial", ""))]
    if len(matches) != 1:
        raise ValueError(f"Expected one TapShard with {vid:04x}:{pid:04x} and "
                         f"serial {SERIAL_PREFIX}<16 uppercase hex digits>; found {len(matches)}")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vid", type=lambda value: int(value, 16), required=True)
    parser.add_argument("--pid", type=lambda value: int(value, 16), required=True)
    args = parser.parse_args()
    try:
        device = select_device(list_usb_devices(), args.vid, args.pid)
    except ValueError as exc:
        parser.exit(1, str(exc) + "\n")
    print(json.dumps(device))


if __name__ == "__main__":
    main()
