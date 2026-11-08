#!/usr/bin/env python3
"""Run deterministic Windows/Linux BLE HID host-switching soak cycles."""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import json
import shlex
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


def run(command: list[str], *, capture: bool = False) -> str:
	result = subprocess.run(
		command,
		check=True,
		text=True,
		stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
	)
	return result.stdout.strip() if capture else ""


def ssh_command(key: Path, host: str, command: str) -> list[str]:
	return [
		"ssh", "-i", str(key), "-o", "BatchMode=yes", "-o", "LogLevel=ERROR",
		host, command,
	]


def parse_last_json(output: str) -> dict:
	for line in reversed(output.splitlines()):
		try:
			return json.loads(line.lstrip("\ufeff"))
		except json.JSONDecodeError:
			continue
	raise RuntimeError(f"command returned no JSON: {output!r}")


def code_at(secret: bytes, timestamp: int) -> str:
	counter = timestamp // 30
	digest = hmac.new(secret, counter.to_bytes(8, "big"), hashlib.sha1).digest()
	offset = digest[-1] & 0x0f
	value = int.from_bytes(digest[offset:offset + 4], "big") & 0x7fffffff
	return f"{value % 1_000_000:06d}"


def capture_timestamp(record: dict) -> int:
	stamp = record["capturedUtc"].replace("Z", "+00:00")
	return int(datetime.fromisoformat(stamp).timestamp())


def wait_away_from_boundary() -> None:
	phase = time.time() % 30
	if phase < 2:
		time.sleep(2.2 - phase)
	elif phase > 24:
		time.sleep(32.2 - phase)


class Soak:
	def __init__(self, args: argparse.Namespace):
		self.args = args
		self.results = args.results
		self.results.mkdir(parents=True, exist_ok=args.resume)
		self.records = self.results / "cycles.jsonl"
		self.secret = base64.b32decode(args.secret)
		self.cycle = self.completed_cycles() if args.resume else 0
		self.linux_scan_pid: int | None = None

	def completed_cycles(self) -> int:
		if not self.records.exists():
			return 0
		records = [
			json.loads(line)
			for line in self.records.read_text(encoding="utf-8").splitlines()
			if line.strip()
		]
		if any(not record.get("passed") for record in records):
			raise RuntimeError("cannot resume a result set containing a failed record")
		if [record.get("cycle") for record in records] != list(
			range(1, len(records) + 1)
		):
			raise RuntimeError("cannot resume a non-contiguous cycle record")
		return len(records)

	def helper(self, *arguments: str, capture: bool = False) -> str:
		return run(
		[
			str(self.args.pyocd_python), str(self.args.helper), *arguments,
			"--elf", str(self.args.elf),
		],
		capture=capture,
		)

	def snapshot(self, label: str) -> dict[str, int]:
		path = self.results / f"{self.cycle:03d}-{label}.json"
		output = self.helper("snapshot", "--output", str(path), capture=True)
		return json.loads(output)["values"]

	def windows(self, action: str, *arguments: str, capture: bool = False) -> str:
		command = subprocess.list2cmdline([
			"powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
			"-File", self.args.windows_script, "-Action", action, *arguments,
		])
		return run(
			ssh_command(self.args.windows_key, self.args.windows_host, command),
			capture=capture,
		)

	def windows_info(self) -> dict:
		return parse_last_json(self.windows(
			"CaptureInfo", "-Path", self.args.windows_capture, capture=True
		))

	def linux_power(self, on: bool) -> None:
		command = (
			f"python3 {shlex.quote(self.args.linux_helper)} pty -- bluetoothctl power "
			f"{'on' if on else 'off'}"
		)
		run(ssh_command(self.args.linux_key, self.args.linux_host, command))
		time.sleep(1.5)

	def start_linux_scan(self) -> None:
		command = (
			f"nohup python3 {shlex.quote(self.args.linux_helper)} pty -- "
			"bluetoothctl --timeout 600 scan on "
			">/tmp/totp-hil-bluez-scan.log 2>&1 </dev/null & echo $!"
		)
		output = run(
			ssh_command(self.args.linux_key, self.args.linux_host, command),
			capture=True,
		)
		self.linux_scan_pid = int(output.splitlines()[-1])
		time.sleep(1.5)
		if not self.linux_scan_alive():
			raise RuntimeError("Linux Bluetooth discovery exited early")
		status = run(
			ssh_command(
				self.args.linux_key,
				self.args.linux_host,
				"bluetoothctl show",
			),
			capture=True,
		)
		if "Discovering: yes" not in status:
			raise RuntimeError("BlueZ did not enter discovery mode")

	def linux_scan_alive(self) -> bool:
		if self.linux_scan_pid is None:
			return False
		command = f"kill -0 {self.linux_scan_pid} 2>/dev/null"
		result = subprocess.run(
			ssh_command(self.args.linux_key, self.args.linux_host, command),
			stdout=subprocess.DEVNULL,
			stderr=subprocess.DEVNULL,
		)
		return result.returncode == 0

	def ensure_linux_scan(self) -> None:
		if self.linux_scan_alive():
			return
		self.linux_scan_pid = None
		self.start_linux_scan()

	def stop_linux_scan(self) -> None:
		if self.linux_scan_pid is None:
			return
		pid = self.linux_scan_pid
		command = (
			f"pkill -TERM -P {pid} 2>/dev/null || true; "
			f"kill -TERM {pid} 2>/dev/null || true"
		)
		run(ssh_command(self.args.linux_key, self.args.linux_host, command))
		for _ in range(30):
			if not self.linux_scan_alive():
				break
			time.sleep(0.1)
		else:
			command = f"kill -KILL {pid} 2>/dev/null || true"
			run(ssh_command(self.args.linux_key, self.args.linux_host, command))
		self.linux_scan_pid = None

	def capture_linux(self) -> subprocess.Popen[str]:
		command = (
			f"python3 {shlex.quote(self.args.linux_helper)} capture-linux --timeout 20"
		)
		return subprocess.Popen(
			ssh_command(self.args.linux_key, self.args.linux_host, command),
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			text=True,
		)

	def connect_linux(self) -> None:
		command = (
			f"python3 {shlex.quote(self.args.linux_helper)} pty -- bluetoothctl "
			f"--timeout 10 connect {shlex.quote(self.args.token_address)}"
		)
		run(ssh_command(self.args.linux_key, self.args.linux_host, command))

	def press(self) -> None:
		self.helper("press", "--hold-ms", "120")

	def record_cycle(self, mode: str, intended: str) -> None:
		self.cycle += 1
		print(f"cycle {self.cycle:03d}: {mode}", flush=True)
		windows_before = self.windows_info()
		before = self.snapshot("before")
		wait_away_from_boundary()
		linux_capture = None
		connection_trigger = "automatic"
		if intended == "linux":
			self.ensure_linux_scan()
			linux_capture = self.capture_linux()
			time.sleep(0.4)
		self.press()

		if intended == "linux":
			if mode.endswith("fallback"):
				# BlueZ can stop auto-reconnecting HID devices after many rapid
				# adapter power cycles. Still allow the ordinary reconnect first;
				# if it has not produced input after the filtered window, connect
				# explicitly while the unfiltered advertisement is active.
				time.sleep(6.5)
				if linux_capture.poll() is None:
					self.connect_linux()
					connection_trigger = "explicit"
			stdout, stderr = linux_capture.communicate(timeout=22)
			if linux_capture.returncode != 0:
				raise RuntimeError(f"Linux capture failed: {stderr.strip()}")
			capture = parse_last_json(stdout)
			windows_after = self.windows_info()
			other_silent = windows_after["length"] == windows_before["length"]
		else:
			output = self.windows(
				"WaitCapture", "-Path", self.args.windows_capture,
				"-MinimumLength", str(windows_before["length"]),
				"-TimeoutSeconds", "20", capture=True,
			)
			info = parse_last_json(output)
			capture = json.loads(info["line"].lstrip("\ufeff"))
			other_silent = True

		time.sleep(3)
		after = self.snapshot("after")
		timestamp = capture_timestamp(capture)
		expected = code_at(self.secret, timestamp)
		fallback = mode.endswith("fallback")
		checks = {
			"preferred_delta": after["dbg_adv_preferred"] - before["dbg_adv_preferred"] == 1,
			"fallback_delta": after["dbg_adv_fallback"] - before["dbg_adv_fallback"] == int(fallback),
			"connected_delta": after["dbg_connected"] - before["dbg_connected"] == 1,
			"report_delta": after["dbg_notify_completed"] - before["dbg_notify_completed"] == 15,
			"no_abort": after["dbg_notify_aborts"] == before["dbg_notify_aborts"],
			"no_notify_error": after["dbg_notify_err"] == 0,
			"secured": after["dbg_sec_level"] >= 2 and after["dbg_sec_changed_err"] == 0,
			"idle": after["dbg_state"] == 0,
			"selftests": after["selftest_failed"] == 0,
			"pairing_count": after["dbg_pairing_count"] == 2,
			"expected_code": capture["value"] == expected,
			"other_host_silent": other_silent,
		}
		record = {
			"schema": 1,
			"cycle": self.cycle,
			"mode": mode,
			"intended_host": intended,
			"connection_trigger": connection_trigger,
			"capture": capture,
			"expected": expected,
			"checks": checks,
			"passed": all(checks.values()),
		}
		with self.records.open("a", encoding="utf-8") as stream:
			stream.write(json.dumps(record, sort_keys=True) + "\n")
		if not record["passed"]:
			raise RuntimeError(f"cycle {self.cycle} failed: {checks}")

	def set_windows(self) -> None:
		self.stop_linux_scan()
		self.linux_power(False)
		self.windows("AdapterOn")
		time.sleep(2)

	def set_linux(self) -> None:
		self.windows("AdapterOff")
		self.linux_power(True)
		self.start_linux_scan()

	def execute(self) -> None:
		schedule = (
			[("windows-preferred", "windows")] * self.args.cycles_per_mode
			+ [("windows-to-linux-fallback", "linux")]
			+ [("linux-preferred", "linux")] * self.args.cycles_per_mode
			+ [("linux-to-windows-fallback", "windows")]
		)
		for _ in range(self.args.cycles_per_mode - 1):
			schedule.extend(
				(
					("windows-to-linux-fallback", "linux"),
					("linux-to-windows-fallback", "windows"),
				)
			)
		if self.cycle > len(schedule):
			raise RuntimeError("result set has more cycles than this schedule")

		try:
			active_host = None
			for mode, intended in schedule[self.cycle:]:
				if intended != active_host:
					if intended == "windows":
						self.set_windows()
					else:
						self.set_linux()
					active_host = intended
				self.record_cycle(mode, intended)
		finally:
			self.stop_linux_scan()


def build_parser() -> argparse.ArgumentParser:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--cycles-per-mode", type=int, default=25)
	parser.add_argument("--results", type=Path, required=True)
	parser.add_argument("--resume", action="store_true")
	parser.add_argument("--helper", type=Path, default=Path("tools/hil/totp_hil.py"))
	parser.add_argument("--elf", type=Path, default=Path("build/zephyr/zephyr.elf"))
	parser.add_argument("--pyocd-python", type=Path, default=Path(sys.executable),
			    help="Python with pyOCD installed (default: current interpreter)")
	parser.add_argument("--secret", required=True)
	parser.add_argument("--linux-host", required=True)
	parser.add_argument("--linux-key", type=Path, required=True)
	parser.add_argument("--linux-helper", required=True, help="Absolute helper path on the Linux host")
	parser.add_argument("--token-address", required=True)
	parser.add_argument("--windows-host", required=True)
	parser.add_argument("--windows-key", type=Path, required=True)
	parser.add_argument("--windows-script", required=True, help="Absolute Windows-Hil.ps1 path on the Windows host")
	parser.add_argument("--windows-capture", default=r"C:\totp-hil\capture.jsonl")
	return parser


def main() -> int:
	args = build_parser().parse_args()
	if args.cycles_per_mode < 1:
		raise SystemExit("--cycles-per-mode must be positive")
	Soak(args).execute()
	return 0


if __name__ == "__main__":
	sys.exit(main())
