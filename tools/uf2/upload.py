#!/usr/bin/env python3
"""Upload a bench UF2 through native USB and verify the complete install by SWD.

Environment endpoints are arguments, not committed machine configuration.
The factory bootloader must already be restored. Test secrets only.
"""
import argparse
import hashlib
import json
import shlex
import subprocess
import time
from pathlib import Path

from pyocd.core.helpers import ConnectHelper
from validate import validate


def run(command, **kwargs):
    return subprocess.run(command, check=True, timeout=20, **kwargs)


def probe(frequency):
    return ConnectHelper.session_with_chosen_probe(blocking=False, target_override="nrf52840",
        frequency=frequency,
        options={"connect_mode": "attach", "auto_unlock": False})


def read_bytes(target, address, size):
    result = bytearray()
    for offset in range(0, size, 256):
        result.extend(target.read_memory_block8(address + offset, min(256, size - offset)))
    return bytes(result)


def upload(args):
    validate((args.build / "zephyr/zephyr.uf2").read_bytes())
    image = (args.build / "zephyr/zephyr.bin").read_bytes()
    bootloader = (args.build / "security-bootloader/bootloader.bin").read_bytes()
    if len(bootloader) != 40960 or bootloader[0x108:0x10c] != b"\x01\0\0\0":
        raise ValueError("only the unlocked bench image is accepted")
    ssh = ["ssh", "-i", str(args.ssh_key), "-o", "BatchMode=yes", "-o",
           "ConnectTimeout=8", "-o", "StrictHostKeyChecking=yes", args.ssh_host]
    with probe(args.frequency) as session:
        target = session.board.target
        if target.read32(0xf4100) == 0x534f5450:
            raise ValueError("restore the factory bootloader using recovery first")
        factory = read_bytes(target, 0xf4000, 0xa000)
        mbr = read_bytes(target, 0, 0x26000)
        backup = read_bytes(target, 0xe0000, 0xc000)
        target.write32(0x4000051c, 0x57)
        target.reset()
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        found = subprocess.run(ssh + ["lsusb -d 239a:00b3"], capture_output=True, timeout=12)
        if found.returncode == 0:
            break
        time.sleep(.5)
    else:
        raise RuntimeError("factory UF2 USB did not enumerate on host")
    for command in (["lxc", "config", "device", "remove", args.vm, args.device],
                    ["lxc", "config", "device", "add", args.vm, args.device, "usb",
                     "vendorid=239a", "productid=00b3", "required=false"]):
        run(ssh + [shlex.join(command)])
    disk = Path("/dev/disk/by-label/NICENANO")
    deadline = time.monotonic() + 15
    while not disk.exists():
        if time.monotonic() > deadline:
            raise RuntimeError("UF2 block device did not appear in VM")
        time.sleep(.25)
    args.mount.mkdir(exist_ok=True)
    run(["mount", "-t", "vfat", "-o", "rw,nosuid,nodev,noexec", str(disk), str(args.mount)])
    try:
        run(["cp", str(args.build / "zephyr/zephyr.uf2"), str(args.mount / "TOTP.UF2")])
        # The bootloader resets on the final block, so sync may see disconnect.
        sync = subprocess.run(["sync", "-f", str(args.mount)], timeout=15, capture_output=True)
    finally:
        subprocess.run(["umount", str(args.mount)], timeout=10, check=False)
    time.sleep(3)
    with probe(args.frequency) as session:
        target = session.board.target
        assert read_bytes(target, 0x26000, len(image)) == image
        assert read_bytes(target, 0xf4000, 0xa000) == bootloader
        assert read_bytes(target, 0xe0000, 0xa000) == factory
        assert read_bytes(target, 0, 0x26000) == mbr
        reused = backup[0xa030:0xa034] == b"ILAV"
        if reused:
            assert read_bytes(target, 0xe0000, 0xc000) == backup
        assert target.read32(0x10001208) & 0xff != 0, "bench unexpectedly protected"
    run(ssh + ["lsusb -d 1209:0001"])
    return {"test": "uf2-install", "image_sha256": hashlib.sha256(image).hexdigest(),
            "bootloader_sha256": hashlib.sha256(bootloader).hexdigest(),
            "factory_sha256": hashlib.sha256(factory).hexdigest(),
            "backup_reused": reused, "mbr_preserved": True,
            "sync_status": sync.returncode}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ssh-host", required=True)
    parser.add_argument("--ssh-key", required=True, type=Path)
    parser.add_argument("--vm", required=True)
    parser.add_argument("--device", default="totp-uf2")
    parser.add_argument("--build", default=Path("build-uf2-bench"), type=Path)
    parser.add_argument("--frequency", default=500000, type=int)
    parser.add_argument("--mount", default=Path("/tmp/totp-uf2-mount"), type=Path)
    args = parser.parse_args()
    print(json.dumps(upload(args), indent=2))


if __name__ == "__main__":
    main()
