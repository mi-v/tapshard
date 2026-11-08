#!/usr/bin/env python3
"""Destructive bench checks for the UNLOCKED bootloader; use test secrets only.

Run with tools/run-limited.sh and the pyOCD Python environment. No auto-unlock:
this tool must never erase a protected board to regain debugger access.
"""
import argparse
import hashlib
import json
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from toolchain import resolve_nm

from pyocd.core.helpers import ConnectHelper
from pyocd.core.exceptions import TransferError
from pyocd.core.target import Target
from pyocd.flash.loader import FlashLoader

MARKER_ADDR = 0x2003FFF0
MARKER_MAGIC = 0x31545352
BENCH_WIPE_MAGIC = 0x45504957


def marker_words(stage, magic=MARKER_MAGIC):
    return [magic, stage, stage ^ 0xffffffff, magic ^ 0xffffffff]


def marker_stage(target):
    words = list(struct.unpack("<4I", read_bytes(target, MARKER_ADDR, 16)))
    stage = words[1]
    return stage if stage in (0xa1, 0xa2) and words == marker_words(stage) else 0


def write_marker(target, stage=0, magic=MARKER_MAGIC):
    words = marker_words(stage, magic) if stage or magic == BENCH_WIPE_MAGIC else [0] * 4
    target.write_memory_block32(MARKER_ADDR, [0] + words[1:])
    target.write32(MARKER_ADDR, words[0])
    target.flush()


def reconnect_after_reset(target):
    target.dp.connect()
    target.session.notify(Target.Event.POST_RESET, target.selected_core)


def pulse_pin(target):
    target.session.notify(Target.Event.PRE_RESET, target.selected_core)
    probe = target.session.probe
    probe.assert_reset(True)
    try:
        time.sleep(.02)
    finally:
        probe.assert_reset(False)
    reconnect_after_reset(target)


def symbols(elf: Path, nm: str | None = None):
    result = subprocess.run([resolve_nm(elf, nm), "-n", str(elf)], check=True, text=True,
                            capture_output=True, timeout=10)
    return {parts[2]: int(parts[0], 16) for line in result.stdout.splitlines()
            if len(parts := line.split()) == 3}


def wait_halted(target, seconds=3):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if target.get_state() == Target.State.HALTED:
            return
        time.sleep(.01)
    raise RuntimeError("bootloader breakpoint was not reached")


def read_bytes(target, address, size):
    # Short transfers are more reliable than long pipelined DAPLink bursts
    # through VM USB passthrough. Each chunk remains independently addressed.
    result = bytearray()
    for offset in range(0, size, 256):
        result.extend(target.read_memory_block8(address + offset, min(256, size - offset)))
    return bytes(result)


def wipe(target, table, kind, manual=False):
    target.halt()
    sentinel = bytes(range(256)) * 1024
    verified = False
    try:
        target.write_memory_block8(0x20000000, sentinel)
        write_marker(target, magic=BENCH_WIPE_MAGIC)
        if manual:
            target.write_core_register("primask", 1)
            target.write_core_register("pc", table["Default_Handler"])
            target.resume()
            print("ARMED: press the board's reset button ONCE within 60 seconds", flush=True)
            deadline = time.monotonic() + 60
            while True:
                reconnect_after_reset(target)
                target.halt()
                if target.read_core_register("pc") == table["boot_wipe_wait"]:
                    break
                target.resume()
                if time.monotonic() > deadline:
                    raise RuntimeError("manual pin reset not observed")
                time.sleep(.1)
        elif kind == "watchdog":
            target.write_core_register("primask", 1)
            target.write_core_register("pc", table["Default_Handler"])
            target.write32(0x40010504, 32768)
            target.write32(0x40010508, 1)
            target.write32(0x4001050c, 1)  # Run while WFE sleeps; pause in debug.
            target.write32(0x40010000, 1)
            target.resume()
            time.sleep(1.2)
            reconnect_after_reset(target)
            target.halt()
        elif kind == "pin":
            pulse_pin(target)
            target.halt()
        else:
            target.reset(Target.ResetType.SYSRESETREQ)
            time.sleep(.05)
            target.halt()
        assert target.read_core_register("pc") == table["boot_wipe_wait"], "not stopped before first stack use"
        memory = read_bytes(target, 0x20000000, 0x40000)
        assert memory == b"\0" * 0x40000, "SRAM not completely erased"
        assert all(target.read_core_register(f"r{i}") == 0 for i in (5, 6, 7, 8, 9))
        reason = target.read_core_register("r4")
        expected = {"software": 4, "pin": 1, "watchdog": 2}[kind]
        assert reason == expected, (kind, reason)
        verified = True
        return {"test": "wipe", "reset": kind, "reason": reason,
                "erased": len(memory), "before_stack_and_marker_regeneration": True}
    finally:
        if not verified:
            write_marker(target)
            target.reset(Target.ResetType.SYSRESETREQ)
        elif target.get_state() == Target.State.HALTED:
            target.write_core_register("pc", table["boot_after_wipe"])
            target.resume()


def physical_reset(target, verify_stage=True):
    previous = marker_stage(target)
    pulse_pin(target)
    stage = marker_stage(target)
    expected = {0xa1: 0xa2, 0xa2: 0}.get(previous, 0xa1)
    if verify_stage and stage != expected:
        raise RuntimeError(f"physical reset stage {stage:#x}, expected {expected:#x}; check wiring")
    return stage


def pin_check(target):
    target.selected_core.clear_reset_catch(Target.ResetType.NSRST)
    if target.get_state() == Target.State.HALTED:
        target.resume()
    write_marker(target)
    single = physical_reset(target)
    time.sleep(1)
    assert marker_stage(target) == 0
    assert target.read32(0xf4100) == 0x534f5450
    first = physical_reset(target)
    time.sleep(.12)
    second = physical_reset(target, verify_stage=False)
    time.sleep(1)
    assert marker_stage(target) == 0
    assert target.read32(0xf4100) == 0x534f5450
    return {"test": "pin-check", "reset_line_working": True,
            "single_stage": hex(single), "double_stages": [hex(first), hex(second)],
            "timeout_clears_stage": True, "factory_not_restored": True,
            "counter_advances": second == 0xa2, "success": second == 0xa2}


def marker_check(target):
    cases = []
    for word in range(4):
        words = marker_words(0xa2)
        words[word] ^= 1
        cases.append((f"corrupt-word-{word}", words))
    for stage in (0, 0xa3, 0xdeadbeef, 0xffffffff):
        cases.append((f"invalid-stage-{stage:#x}", marker_words(stage)))
    for name, words in cases:
        target.write_memory_block32(MARKER_ADDR, words)
        target.flush()
        pulse_pin(target)
        assert marker_stage(target) == 0xa1, name
        assert target.read32(0xf4100) == 0x534f5450, name
    time.sleep(1)
    assert read_bytes(target, MARKER_ADDR, 16) == bytes(16)
    return {"test": "marker-check", "rejected": [name for name, _ in cases],
            "timeout_cleared_all_marker_words": True}


def gesture_reset(target, table, simulated):
    if not simulated:
        physical_reset(target)
        return
    # Exercise recovery logic independently of a missing nRESET wire. This
    # explicitly does NOT qualify the physical gesture: after a real software
    # reset and SRAM wipe, substitute RESETPIN in the C entry argument.
    previous = marker_stage(target)
    target.reset_and_halt(Target.ResetType.SYSRESETREQ)
    entry = table["boot_main"]
    target.set_breakpoint(entry, Target.BreakpointType.HW)
    target.write32(0xe0002000, 3)
    target.resume()
    wait_halted(target)
    target.remove_breakpoint(entry)
    target.write_core_register("r0", 1)
    target.write_core_register("r1", previous)


def recovery(target, table, simulated=False, manual=False):
    factory = read_bytes(target, 0xe0000, 0xa000)
    metadata = read_bytes(target, 0xea000, 52)
    assert struct.unpack_from("<4I", metadata) == (0x504f5442, 1, 0xf4000, 0xa000)
    assert metadata[16:48] == hashlib.sha256(factory).digest()
    assert struct.unpack_from("<I", metadata, 48)[0] == 0x56414c49
    mbr = read_bytes(target, 0, 0x26000)
    # Catch the C entry on each reset, with SRAM already erased. Real SWD
    # transfers can take >750 ms, so suspend at boot_main while preparing the
    # next physical pin reset. The retained stage is then set by real code.
    entry = table["boot_main"]
    try:
        if manual:
            print("ARMED: press reset THREE times, less than 750 ms apart, within 60 seconds", flush=True)
            deadline = time.monotonic() + 60
            while True:
                try:
                    # GPREGRET is set before COPY_BL. Avoid reading flash
                    # while NVMC erase/program stalls the flash bus.
                    if target.read32(0x4000051c) == 0x57:
                        break
                except TransferError:
                    reconnect_after_reset(target)
                if time.monotonic() > deadline:
                    raise RuntimeError("manual triple reset did not restore factory bootloader")
                time.sleep(.05)
        elif not simulated:
            write_marker(target)
            assert physical_reset(target) == 0xa1
            time.sleep(.12)
            assert physical_reset(target) == 0xa2
            time.sleep(.12)
            pulse_pin(target)  # No debug pause or injected reset reason.
        else:
            for stage in (0xa1, 0xa2):
                gesture_reset(target, table, simulated)
                target.resume()
                deadline = time.monotonic() + .5
                while marker_stage(target) != stage:
                    if time.monotonic() > deadline:
                        raise RuntimeError("reset gesture stage not reached")
                target.halt()
            gesture_reset(target, table, simulated)
            target.resume()
        time.sleep(3)
        if manual:
            reconnect_after_reset(target)
        active = read_bytes(target, 0xf4000, 0xa000)
        assert active == factory, "factory bootloader did not restore exactly"
        assert read_bytes(target, 0xec000, 0x8000) == b"\xff" * 0x8000
        assert read_bytes(target, 0, 0x26000) == mbr
        assert read_bytes(target, 0xe0000, 0xa034) == factory + metadata
        return {"test": "recovery", "simulated_pin_reason": simulated, "manual": manual,
                "factory_sha256": hashlib.sha256(active).hexdigest(),
                "settings_erased": 32768, "mbr_preserved": True, "backup_preserved": True}
    finally:
        target.remove_breakpoint(entry)
        if target.get_state() == Target.State.HALTED:
            target.resume()


def invalid_backup(session, app_table, address, table, try_recovery=False):
    target = session.board.target
    original = read_bytes(target, address, 4096)
    active = read_bytes(target, 0xf4000, 0xa000)
    storage = read_bytes(target, 0xec000, 0x8000)
    sentinel = bytes(range(256)) * 128

    def program(data, settings):
        target.reset_and_halt(Target.ResetType.SYSRESETREQ)
        loader = FlashLoader(session, chip_erase="sector", smart_flash=False,
                             progress=lambda _: None)
        loader.add_data(address, data)
        loader.add_data(0xec000, settings)
        loader.commit()
        target.reset(Target.ResetType.SYSRESETREQ)
        time.sleep(.5)

    try:
        program(bytes(4) + original[4:], sentinel)
        stage = target.read32(app_table["dbg_boot_stage"])
        assert stage == (2**32 - 5), f"provisioning gate did not fail closed: {stage:#x}"
        if try_recovery:
            write_marker(target)
            assert physical_reset(target) == 0xa1
            time.sleep(.12)
            assert physical_reset(target) == 0xa2
            time.sleep(.12)
            pulse_pin(target)
            time.sleep(1)
            stage = target.read32(app_table["dbg_boot_stage"])
            assert stage == (2**32 - 2), "failed recovery status not propagated to diagnostic app"
        assert read_bytes(target, 0xf4000, 0xa000) == active
        assert read_bytes(target, 0xec000, 0x8000) == sentinel
        return {"test": "invalid-recovery" if try_recovery else "invalid-backup",
                "simulated_pin_reason": False, "corrupted_page": hex(address),
                "boot_stage": -2 if try_recovery else -5, "active_bootloader_preserved": True,
                "settings_preserved": True, "nonempty_settings_sentinel_verified": True}
    finally:
        program(original, storage)
        assert target.read32(app_table["dbg_boot_stage"]) == 6


def nonpin(target, table, app_table):
    for stage in (0xa1, 0xa2):
        write_marker(target, stage)
        target.reset(Target.ResetType.SYSRESETREQ)
        time.sleep(.5)
        assert marker_stage(target) == 0
        assert target.read32(app_table["dbg_boot_stage"]) == 6
        assert target.read32(0xf4100) == 0x534f5450
    target.halt()
    write_marker(target, 0xa2)
    target.write_core_register("primask", 1)
    target.write_core_register("pc", table["Default_Handler"])
    target.write32(0x40010504, 32768)
    target.write32(0x40010508, 1)
    target.write32(0x4001050c, 1)
    target.write32(0x40010000, 1)
    target.resume()
    time.sleep(2)
    reconnect_after_reset(target)
    assert marker_stage(target) == 0
    assert target.read32(app_table["dbg_boot_stage"]) == 6
    return {"test": "nonpin", "software_clears_stages": [0xa1, 0xa2],
            "watchdog_clears_stage": 0xa2}


def copy_resume(target, table, build, direction):
    factory = read_bytes(target, 0xe0000, 0xa000)
    backup = read_bytes(target, 0xe0000, 0xc000)
    mbr = read_bytes(target, 0, 0x26000)
    custom = (build / "security-bootloader/bootloader.bin").read_bytes()
    expected = factory if direction == "restore" else custom
    if direction == "restore":
        for stage in (0xa1, 0xa2):
            gesture_reset(target, table, True)
            target.resume()
            deadline = time.monotonic() + .5
            while marker_stage(target) != stage:
                if time.monotonic() > deadline:
                    raise RuntimeError("simulated recovery stage not reached")
            target.halt()
        gesture_reset(target, table, True)
    else:
        assert read_bytes(target, 0xf4000, 0xa000) == factory
        target.reset_and_halt(Target.ResetType.SYSRESETREQ)
        target.write32(0x4000051c, 0x6d)  # Factory skip-DFU -> application installer.
    target.set_watchpoint(0xf4000, 4, Target.WatchpointType.WRITE)
    try:
        target.resume()
        wait_halted(target, 8)
        assert target.get_halt_reason() == Target.HaltReason.WATCHPOINT
        pc = target.read_core_register("pc")
        assert pc < 0x1000, f"copy interruption was not in MBR: {pc:#x}"
        partial = read_bytes(target, 0xf4000, 0xa000)
        assert partial[:4] == expected[:4], "first destination word not copied"
        assert partial != expected, "copy already complete"
        assert read_bytes(target, 0xfe000, 0x1000) != b"\xff" * 4096
        target.remove_watchpoint(0xf4000, 4, Target.WatchpointType.WRITE)
        target.reset(Target.ResetType.SYSRESETREQ)
        time.sleep(3)
        assert read_bytes(target, 0xf4000, 0xa000) == expected
        assert read_bytes(target, 0xe0000, 0xc000) == backup
        assert read_bytes(target, 0, 0x26000) == mbr
        return {"test": "copy-resume", "direction": direction, "interrupted_pc": hex(pc),
                "partial_copy_observed": True, "complete_after_reset": True,
                "backup_preserved": True, "mbr_preserved": True}
    finally:
        target.remove_watchpoint(0xf4000, 4, Target.WatchpointType.WRITE)
        if target.get_state() == Target.State.HALTED:
            target.resume()


def backup_resume(target, table, app_table, build, point):
    factory = read_bytes(target, 0xe0000, 0xa000)
    recovery(target, table, simulated=True)
    target.reset_and_halt(Target.ResetType.SYSRESETREQ)
    # Invalidate only the metadata page while the known factory is active.
    target.write32(0x4001e504, 2)
    target.write32(0x4001e508, 0xea000)
    deadline = time.monotonic() + 1
    while not target.read32(0x4001e400):
        if time.monotonic() > deadline:
            raise RuntimeError("metadata erase timeout")
    target.write32(0x4001e504, 0)
    target.reset_and_halt(Target.ResetType.SYSRESETREQ)
    target.write32(0x4000051c, 0x6d)
    entry = app_table["boot_security_init"]
    counter = app_table["dbg_boot_backup_page"]
    target.set_breakpoint(entry, Target.BreakpointType.HW)
    target.write32(0xe0002000, 3)
    try:
        target.resume()
        wait_halted(target)
        target.remove_breakpoint(entry)
        target.set_watchpoint(counter, 4, Target.WatchpointType.WRITE)
        target.resume()
        for _ in range(15):
            wait_halted(target)
            assert target.get_halt_reason() == Target.HaltReason.WATCHPOINT
            actual = target.read32(counter)
            if actual == point:
                break
            target.resume()
        else:
            raise RuntimeError("backup checkpoint not observed")
        assert target.read32(0xf4100) != 0x534f5450, "active factory replaced before backup commit"
        valid = target.read32(0xea030) == 0x56414c49
        assert valid == (point == 12), "backup validity marker committed at wrong stage"
        target.remove_watchpoint(counter, 4, Target.WatchpointType.WRITE)
        target.reset_and_halt(Target.ResetType.SYSRESETREQ)
        target.write32(0x4000051c, 0x6d)
        target.resume()
        # NVMC stalls flash-bus reads during page erase/program. Let capture
        # and MBR copy finish before polling, so SWD does not trip WAIT/FAULT.
        time.sleep(3)
        deadline = time.monotonic() + 6
        while target.read32(app_table["dbg_boot_stage"]) != 6:
            if time.monotonic() > deadline:
                raise RuntimeError("installer did not recover interrupted backup")
            time.sleep(.05)
        assert read_bytes(target, 0xe0000, 0xa000) == factory
        # Successful install resets through the wiping loader; checkpoint RAM
        # is reinitialized on that final boot for both recreation and reuse.
        assert target.read32(counter) == 0xffffffff
        expected_boot = (build / "security-bootloader/bootloader.bin").read_bytes()
        assert read_bytes(target, 0xf4000, 0xa000) == expected_boot
        return {"checkpoint": point, "valid_before_reset": valid,
                "backup_recreated": point != 12, "installed_after_reset": True}
    finally:
        target.remove_breakpoint(entry)
        target.remove_watchpoint(counter, 4, Target.WatchpointType.WRITE)
        if target.get_state() == Target.State.HALTED:
            target.resume()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test", choices=("wipe", "recovery", "invalid-backup", "invalid-recovery", "nonpin", "copy-resume", "backup-resume", "pin-check", "marker-check"))
    parser.add_argument("--checkpoint", type=int, choices=range(13))
    parser.add_argument("--direction", choices=("install", "restore"), default="restore")
    parser.add_argument("--corrupt", choices=("metadata", "payload"), default="metadata")
    parser.add_argument("--simulate-pin", action="store_true",
                        help="test recovery logic with debugger-injected reset reason; not physical acceptance")
    parser.add_argument("--manual", action="store_true", help="wait for the user to press physical reset")
    parser.add_argument("--reset", choices=("software", "pin", "watchdog"), default="software")
    parser.add_argument("--build", type=Path, default=Path("build-uf2-bench"))
    parser.add_argument("--frequency", type=int, default=500000)
    parser.add_argument("--nm", help="ARM nm executable (default: build CMake cache, then PATH)")
    args = parser.parse_args()
    if args.manual and (args.simulate_pin or (args.test == "wipe" and args.reset != "pin")):
        parser.error("--manual requires physical pin reset and cannot simulate the reason")
    table = symbols(args.build / "security-bootloader/bootloader.elf", args.nm)
    with ConnectHelper.session_with_chosen_probe(blocking=False, target_override="nrf52840",
            frequency=args.frequency,
            options={"connect_mode": "attach", "auto_unlock": False}) as session:
        target = session.board.target
        marker = target.read32(0xf4100)
        if not (args.test == "copy-resume" and args.direction == "install"):
            assert marker == 0x534f5450 and target.read32(0xf4108) == 1, "requires unlocked bench bootloader"
            assert target.read32(0xf4104) == 2, "requires SRAM-marker bootloader v2"
        if args.test == "pin-check":
            result = pin_check(target)
        elif args.test == "marker-check":
            result = marker_check(target)
        elif args.test == "wipe":
            result = wipe(target, table, args.reset, args.manual)
        elif args.test == "recovery":
            result = recovery(target, table, args.simulate_pin, args.manual)
        elif args.test in ("invalid-backup", "invalid-recovery"):
            app_table = symbols(args.build / "zephyr/zephyr.elf", args.nm)
            result = invalid_backup(session, app_table,
                                    0xea000 if args.corrupt == "metadata" else 0xe0000,
                                    table, args.test == "invalid-recovery")
        elif args.test == "nonpin":
            app_table = symbols(args.build / "zephyr/zephyr.elf", args.nm)
            result = nonpin(target, table, app_table)
        elif args.test == "copy-resume":
            result = copy_resume(target, table, args.build, args.direction)
        else:
            app_table = symbols(args.build / "zephyr/zephyr.elf", args.nm)
            results = []
            points = [args.checkpoint] if args.checkpoint is not None else range(13)
            for point in points:
                result = backup_resume(target, table, app_table, args.build, point)
                results.append(result)
                print(json.dumps(result), flush=True)
            result = {"test": "backup-resume", "reset_interruption_checkpoints": results}
        print(json.dumps(result, indent=2))
        if result.get("success") is False:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
