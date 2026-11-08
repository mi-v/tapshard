#!/usr/bin/env python3
"""Small, dependency-light helpers for the TOTP token hardware test bench."""

from __future__ import annotations

import argparse
import base64
import fcntl
import hashlib
import hmac
import json
import os
import pty
import re
import select
import struct
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_SYMBOLS = (
    "selftest_failed",
    "dbg_state",
    "dbg_idle_reason",
    "dbg_connected",
    "dbg_sec_level",
    "dbg_sec_changed_err",
    "dbg_enc_key_size",
    "dbg_pairing_count",
    "dbg_pairing_bonded",
    "dbg_adv_preferred",
    "dbg_adv_fallback",
    "dbg_notify_sent",
    "dbg_notify_err",
    "dbg_notify_completed",
    "dbg_notify_retries",
    "dbg_notify_aborts",
    "dbg_usb_vbus_ready",
    "dbg_usb_vbus_removed",
    "dbg_disk_wipe_count",
    "dbg_disk_wipe_err",
)

# _IOW('E', 0x90, int): exclusively route this event node to the HIL reader.
EVIOCGRAB = 0x40044590


def load_symbols(elf: Path, nm: str | None = None) -> dict[str, tuple[int, int]]:
    # Host-only capture/TOTP/PTY commands also run from a standalone copy of
    # this file on a test host. Only ELF/SWD commands need the repository helper.
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from toolchain import resolve_nm

    result = subprocess.run(
        [resolve_nm(elf, nm), "-S", "--defined-only", str(elf)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    symbols: dict[str, tuple[int, int]] = {}
    pattern = re.compile(
        r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+\S\s+(\S+)$"
    )
    for line in result.stdout.splitlines():
        match = pattern.match(line)
        if match:
            symbols[match.group(3)] = (
                int(match.group(1), 16),
                int(match.group(2), 16),
            )
    return symbols


def signed(value: int, width: int) -> int:
    bits = width * 8
    sign = 1 << (bits - 1)
    return value - (1 << bits) if value & sign else value


def pyocd_session(target_name: str, frequency: int):
    try:
        from pyocd.core.helpers import ConnectHelper
    except ImportError as exc:
        raise SystemExit(
            "SWD commands require pyOCD. Activate your development environment "
            "and install it with: python -m pip install pyocd"
        ) from exc
    return ConnectHelper.session_with_chosen_probe(
        blocking=False,
        target_override=target_name,
        frequency=frequency,
        options={"connect_mode": "attach", "auto_unlock": False},
    )


def read_symbol(target, address: int, size: int) -> int:
    if size == 1:
        return target.read8(address)
    if size == 2:
        return target.read16(address)
    if size == 4:
        return target.read32(address)
    raise ValueError(f"unsupported scalar size {size}")


def write_symbol(target, address: int, size: int, value: int) -> None:
    if size == 1:
        target.write8(address, value)
    elif size == 2:
        target.write16(address, value)
    elif size == 4:
        target.write32(address, value)
    else:
        raise ValueError(f"unsupported scalar size {size}")
    target.flush()


def command_symbols(args: argparse.Namespace) -> int:
    symbols = load_symbols(args.elf, args.nm)
    wanted = args.symbol or sorted(symbols)
    output = {
        name: {"address": f"0x{symbols[name][0]:08x}", "size": symbols[name][1]}
        for name in wanted
        if name in symbols
    }
    missing = sorted(set(wanted) - set(output))
    if missing:
        raise SystemExit(f"symbols not found: {', '.join(missing)}")
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0


def command_snapshot(args: argparse.Namespace) -> int:
    symbols = load_symbols(args.elf, args.nm)
    wanted = args.symbol or DEFAULT_SYMBOLS
    missing = [name for name in wanted if name not in symbols]
    if missing:
        raise SystemExit(f"symbols not found: {', '.join(missing)}")
    values: dict[str, int] = {}
    with pyocd_session(args.target, args.frequency) as session:
        target = session.board.target
        for name in wanted:
            address, size = symbols[name]
            values[name] = signed(read_symbol(target, address, size), size)
    record = {"captured_unix_ms": time.time_ns() // 1_000_000, "values": values}
    encoded = json.dumps(record, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    return 0


def command_write(args: argparse.Namespace) -> int:
    symbols = load_symbols(args.elf, args.nm)
    if args.symbol not in symbols:
        raise SystemExit(f"symbol not found: {args.symbol}")
    address, size = symbols[args.symbol]
    with pyocd_session(args.target, args.frequency) as session:
        write_symbol(session.board.target, address, size, args.value)
    return 0


def command_press(args: argparse.Namespace) -> int:
    symbols = load_symbols(args.elf, args.nm)
    if "dbg_force_press" not in symbols:
        raise SystemExit("symbol not found: dbg_force_press")
    address, size = symbols["dbg_force_press"]
    with pyocd_session(args.target, args.frequency) as session:
        target = session.board.target
        write_symbol(target, address, size, 0)
        for index in range(args.count):
            write_symbol(target, address, size, 1)
            time.sleep(args.hold_ms / 1000)
            write_symbol(target, address, size, 0)
            if index + 1 < args.count:
                time.sleep(args.gap_ms / 1000)
    return 0


def command_totp(args: argparse.Namespace) -> int:
    normalized = "".join(args.secret.split()).upper()
    normalized += "=" * ((8 - len(normalized) % 8) % 8)
    try:
        secret = base64.b32decode(normalized, casefold=True)
    except ValueError as exc:
        raise SystemExit(f"invalid base32 secret: {exc}") from exc
    timestamp = args.at if args.at is not None else int(time.time())
    counter = timestamp // args.period
    digest = hmac.new(secret, counter.to_bytes(8, "big"), hashlib.sha1).digest()
    offset = digest[-1] & 0x0F
    value = int.from_bytes(digest[offset : offset + 4], "big") & 0x7FFFFFFF
    print(f"{value % (10 ** args.digits):0{args.digits}d}")
    return 0


def command_soak_plan(args: argparse.Namespace) -> int:
    modes = (
        ("windows-preferred", 25),
        ("linux-preferred", 25),
        ("windows-to-linux-fallback", 25),
        ("linux-to-windows-fallback", 25),
    )
    cycles = []
    number = 1
    for mode, count in modes:
        for iteration in range(1, count + 1):
            cycles.append({"cycle": number, "iteration": iteration, "mode": mode})
            number += 1
    encoded = json.dumps({"schema": 1, "cycles": cycles}, indent=2)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
    return 0


def load_snapshot(path: Path) -> dict[str, int]:
    document = json.loads(path.read_text(encoding="utf-8"))
    return document["values"]


def delta(before: dict[str, int], after: dict[str, int], name: str) -> int:
    return after[name] - before[name]


def command_assert_cycle(args: argparse.Namespace) -> int:
    before = load_snapshot(args.before)
    after = load_snapshot(args.after)
    fallback = args.mode.endswith("fallback")
    checks = {
        "preferred_delta": delta(before, after, "dbg_adv_preferred") == 1,
        "fallback_delta": delta(before, after, "dbg_adv_fallback") == int(fallback),
        "connected_delta": delta(before, after, "dbg_connected") == 1,
        "secured": after["dbg_sec_level"] >= 2 and after["dbg_sec_changed_err"] == 0,
        "notify_completed_delta": delta(before, after, "dbg_notify_completed")
        == args.reports,
        "notify_abort_delta": delta(before, after, "dbg_notify_aborts") == 0,
        "notify_error": after["dbg_notify_err"] == 0,
        "idle": after["dbg_state"] == 0,
    }
    if args.expected:
        if not args.capture:
            raise SystemExit("--expected requires --capture")
        captured = args.capture.read_bytes()
        appended = captured[args.capture_before_size :].decode("utf-8")
        lines = [line for line in appended.splitlines() if line.strip()]
        actual = ""
        if lines:
            try:
                actual = str(json.loads(lines[-1])["value"])
            except (json.JSONDecodeError, KeyError, TypeError):
                actual = lines[-1].strip()
        checks["capture_grew"] = len(captured) > args.capture_before_size
        checks["expected_output"] = actual == args.expected
    if args.other_capture:
        checks["other_host_silent"] = (
            args.other_capture.stat().st_size == args.other_before_size
        )
    record = {
        "schema": 1,
        "recorded_unix_ms": time.time_ns() // 1_000_000,
        "cycle": args.cycle,
        "mode": args.mode,
        "checks": checks,
        "passed": all(checks.values()),
    }
    encoded = json.dumps(record, sort_keys=True)
    if args.results:
        args.results.parent.mkdir(parents=True, exist_ok=True)
        with args.results.open("a", encoding="utf-8") as stream:
            stream.write(encoded + "\n")
    print(encoded)
    return 0 if record["passed"] else 1


def command_pty(args: argparse.Namespace) -> int:
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        raise SystemExit("PTY command is empty")
    return os.waitstatus_to_exitcode(pty.spawn(command))


def find_input_event(name_part: str) -> Path | None:
    for name_path in sorted(Path("/sys/class/input").glob("event*/device/name")):
        try:
            device_name = name_path.read_text(encoding="utf-8").strip()
        except OSError:
            continue
        if name_part.casefold() in device_name.casefold():
            return Path("/dev/input") / name_path.parents[1].name
    return None


def command_capture_linux(args: argparse.Namespace) -> int:
    deadline = time.monotonic() + args.timeout
    device = None
    while time.monotonic() < deadline:
        event_path = find_input_event(args.name)
        if event_path is not None:
            try:
                device = event_path.open("rb", buffering=0)
                break
            except (FileNotFoundError, PermissionError):
                # sysfs exposes hotplugged UHID input nodes before udev has
                # necessarily applied their final ownership and mode.
                pass
        time.sleep(0.05)
    if device is None:
        raise SystemExit(
            f"readable input device containing {args.name!r} did not appear"
        )

    key_digits = {11: "0", **{code: str(code - 1) for code in range(2, 11)}}
    event_struct = struct.Struct("@llHHI")
    value = ""
    with device:
        try:
            fcntl.ioctl(device.fileno(), EVIOCGRAB, 1)
        except OSError as exc:
            raise SystemExit(f"could not exclusively grab {event_path}: {exc}") from exc
        while time.monotonic() < deadline:
            ready, _, _ = select.select([device], [], [], 0.25)
            if not ready:
                continue
            data = device.read(event_struct.size)
            if len(data) != event_struct.size:
                continue
            _, _, event_type, code, key_value = event_struct.unpack(data)
            if event_type != 1 or key_value != 1:
                continue
            if code in key_digits:
                value += key_digits[code]
            elif code == 28:
                record = {
                    "capturedUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    "value": value,
                }
                encoded = json.dumps(record, separators=(",", ":"))
                if args.output:
                    args.output.parent.mkdir(parents=True, exist_ok=True)
                    with args.output.open("a", encoding="utf-8") as stream:
                        stream.write(encoded + "\n")
                print(encoded)
                return 0
    raise SystemExit("input device appeared but no Enter-terminated value arrived")


def add_swd_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--elf", type=Path, default=Path("build/zephyr/zephyr.elf"))
    parser.add_argument(
        "--nm",
        help="ARM nm executable (default: build CMake cache, then PATH)",
    )
    parser.add_argument("--target", default="nrf52840")
    parser.add_argument("--frequency", type=int, default=1_000_000)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="subcommand", required=True)

    child = subparsers.add_parser("symbols", help="resolve ELF symbols")
    add_swd_arguments(child)
    child.add_argument("--symbol", action="append")
    child.set_defaults(func=command_symbols)

    child = subparsers.add_parser("snapshot", help="capture SWD counters as JSON")
    add_swd_arguments(child)
    child.add_argument("--symbol", action="append")
    child.add_argument("--output", type=Path)
    child.set_defaults(func=command_snapshot)

    child = subparsers.add_parser("write", help="write one scalar ELF symbol")
    add_swd_arguments(child)
    child.add_argument("symbol")
    child.add_argument("value", type=lambda value: int(value, 0))
    child.set_defaults(func=command_write)

    child = subparsers.add_parser("press", help="inject timed button clicks")
    add_swd_arguments(child)
    child.add_argument("--count", type=int, default=1)
    child.add_argument("--hold-ms", type=int, default=100)
    child.add_argument("--gap-ms", type=int, default=150)
    child.set_defaults(func=command_press)

    child = subparsers.add_parser("totp", help="calculate an independent RFC 6238 code")
    child.add_argument("secret")
    child.add_argument("--digits", type=int, choices=(6, 8), default=6)
    child.add_argument("--period", type=int, default=30)
    child.add_argument("--at", type=int)
    child.set_defaults(func=command_totp)

    child = subparsers.add_parser("soak-plan", help="write the staged 100-cycle plan")
    child.add_argument("--output", type=Path)
    child.set_defaults(func=command_soak_plan)

    child = subparsers.add_parser("assert-cycle", help="validate one HIL cycle")
    child.add_argument("--before", type=Path, required=True)
    child.add_argument("--after", type=Path, required=True)
    child.add_argument(
        "--mode",
        required=True,
        choices=(
            "windows-preferred",
            "linux-preferred",
            "windows-to-linux-fallback",
            "linux-to-windows-fallback",
        ),
    )
    child.add_argument("--cycle", type=int, required=True)
    child.add_argument("--reports", type=int, required=True)
    child.add_argument("--expected")
    child.add_argument("--capture", type=Path)
    child.add_argument("--capture-before-size", type=int, default=0)
    child.add_argument("--other-capture", type=Path)
    child.add_argument("--other-before-size", type=int, default=0)
    child.add_argument("--results", type=Path)
    child.set_defaults(func=command_assert_cycle)

    child = subparsers.add_parser("pty", help="run a command with a real controlling TTY")
    child.add_argument("command", nargs=argparse.REMAINDER)
    child.set_defaults(func=command_pty)

    child = subparsers.add_parser(
        "capture-linux", help="capture one Enter-terminated Linux HID value"
    )
    child.add_argument("--name", default="TapShard TOTP token")
    child.add_argument("--timeout", type=float, default=20.0)
    child.add_argument("--output", type=Path)
    child.set_defaults(func=command_capture_linux)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if getattr(args, "count", 1) < 1:
        raise SystemExit("count must be positive")
    if getattr(args, "hold_ms", 0) < 0 or getattr(args, "gap_ms", 0) < 0:
        raise SystemExit("press timings cannot be negative")
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
