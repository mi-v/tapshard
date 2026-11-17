# Hardware-in-the-loop helpers

These helpers cover the repeatable parts of multi-host validation. Machine
addresses, SSH credentials, USB passthrough commands, test secrets and captured
results belong in ignored `*.local.json` files or `results/`. Start with the
[build guide](../../docs/BUILDING.md) and an active Python environment containing
pyOCD for SWD commands. Use unlocked images and test secrets only.

A suitable multi-host setup uses a Linux host, a Windows host or VM, and a
separate Bluetooth adapter for each. An SWD probe with nRESET connected allows
automated button gestures and state inspection. Supply your own addresses,
credentials and device identifiers; the helpers do not require a particular
machine or VM manager.

## Firmware variants

Build the normal four-bond development image:

```sh
sh tools/run-limited.sh --memory-mib 1024 --seconds 240 \
  west build -b promicro_nrf52840/nrf52840 -d build-swd -o=-j2
```

Build the capacity-limit image separately:

```sh
sh tools/run-limited.sh --memory-mib 1024 --seconds 240 \
  west build -b promicro_nrf52840/nrf52840 -d build-1bond -o=-j2 -- \
  -DCONFIG_TOTP_MAX_BONDS=1 -DCONFIG_BT_MAX_PAIRED=1
```

Both variants must report `selftest_failed = 0` after boot. Restore the normal
image before platform and soak testing.

## SWD and OTP helpers

SWD commands use pyOCD's Python API so a complete gesture stays in one debugger
session and is not stretched by process or conversational delays:

```sh
sh tools/run-limited.sh python tools/hil/totp_hil.py snapshot \
  --elf build-swd/zephyr/zephyr.elf \
  --output tools/hil/results/before.json
sh tools/run-limited.sh python tools/hil/totp_hil.py press \
  --elf build-swd/zephyr/zephyr.elf --count 2
sh tools/run-limited.sh python tools/hil/totp_hil.py press \
  --elf build-swd/zephyr/zephyr.elf --hold-ms 3200
```

Calculate the host-side expected value independently:

```sh
sh tools/run-limited.sh python tools/hil/totp_hil.py totp JBSWY3DPEHPK3PXP --digits 6 --period 30
```

Use the PTY wrapper for Bluetooth management programs that otherwise wait for a
controlling terminal:

```sh
sh tools/run-limited.sh python tools/hil/totp_hil.py pty -- bluetoothctl
sh tools/run-limited.sh python tools/hil/totp_hil.py pty -- ssh -tt HOST bluetoothctl
```

Capture one Enter-terminated token value directly from the Linux input device:

```sh
sh tools/run-limited.sh python tools/hil/totp_hil.py capture-linux \
  --output tools/hil/results/linux-capture.jsonl
```

The reader takes an exclusive `EVIOCGRAB` for the short-lived event node, so
the test code and trailing Enter do not leak into the host's focused desktop
application while the capture is active.

The default input-device name is `TapShard TOTP token`. Use `--name` to select
another input-device name explicitly.

## First reconnect after Bluetooth restart

Use a dedicated test adapter and a public test account. Retain the bond and
let the token finish typing and disconnect before each restart. Capture the
first click after restarting BlueZ, then repeat with Bluetooth off/on. Test
the desktop toggle as well as `bluetoothctl power off/on`: a desktop may use
rfkill, which exercises a different adapter path. Also capture the following
click to distinguish a first-reconnect failure from a persistent one.

Compare the entire captured code with the independent TOTP calculation, away
from a period boundary. Successful notification completions alone do not
prove that the host received the keys. Record `btmon` and input-device creation
alongside SWD snapshots; `dbg_hid_setup` counts HID metadata reads and explicit
notification-enable writes. A restored CCC can enable notifications before
the host has registered its handler or created the input device. Verify that
the first code survives that cold setup, with no explicit host-side `connect`
command before the button press.

## Waiting LED and disconnect-boundary checks

Long USB visits require bounded disk references across forced FatFs remounts.
Boot filesystem tests exercise scans, writes, formatting and failed mounts.
With `CONFIG_TOTP_TEST_POLL=y`, a boot-only stress test starts the actual RAM
disk count at 65533 and also tests initialization refusal at 65535. It restores
the saved count before BLE/USB starts. `dbg_fs_ref_stress = 2` and matching
`dbg_fs_ref_before/after = 65533` indicate success. Production omits this fault
injection. Confirm `selftest_failed = 0` and a valid FAT configuration volume
after the test; do not run filesystem self-tests on a provisioned live volume.

During a known HID-settle or TOTP-boundary wait, the ramp follows that timer;
a restarted wait restarts progress. Unknown waits retain the initial pattern.
Waiting ramps from 3 to 10 Hz at 50% duty; typing is 10 Hz at 50% duty.
Pairing is distinctly slower, at 1 Hz (500 ms off/on), starting OFF immediately
at the long-press threshold so entry is visible from steady button feedback.
Steady button feedback
covers click collection; idle and completed sends leave the LED off.

`dbg_led_mode` is off/button/pairing/waiting/typing = 0/1/2/3/4.
`dbg_led_period_ms`, `dbg_led_on_ms`,
`dbg_led_progress` (0..1000), `dbg_led_wait_restarts` and `dbg_led_edges`
allow GPIO timing to be correlated with actual OTP capture. Sampling must
use one pyOCD session with flushed gesture writes. Confirm the waiting ramp,
deadline restarts, success/failure/cancel transitions and no LED edges in idle.

For disconnect-boundary regression, send a public test OTP and time another
single press around the end of `grace_ms`, including a press just before the
deadline whose click window ends afterwards. Check that each gesture emits
exactly one complete independently verified OTP. A press before teardown
keeps the link; a press during teardown waits for the connection object to be
recycled before reconnecting. Also check a fresh click after full disconnect,
pairing cancellation and an idle LED with no timer activity.

Also suppress subscription readiness after a completed send, then click again
during grace. Repeat with a click during the post-pairing grace before readiness.
Both pending requests must disconnect after the configured settling interval
(at least 500 ms) plus 10 seconds, with `dbg_idle_reason = 12`. Host metadata
activity must not restart that deadline. Restoring readiness before it expires
must allow exactly one complete code, and the deadline must not truncate a
legitimate long settling interval or a subsequent TOTP-boundary wait.
The boot scheduling test also covers a pending config read at 500 ms followed
by application of a 60 s settling value: no HID deadline is armed during config
loading, and the resulting deadline allows the full new interval plus 10 s.
It also checks that an explicit settling increase after queuing a slot extends
the deadline, while ordinary re-evaluation or a decrease does not truncate it.

## Windows helpers

Copy `Capture-Totp.ps1` and `Windows-Hil.ps1` to the Windows test host or VM.
Start the capture script in the interactive desktop; its low-level keyboard
hook records digit sequences even when its console is not focused:

```powershell
powershell -ExecutionPolicy Bypass -File .\Capture-Totp.ps1 `
  -LogPath C:\totp-hil\capture.jsonl
```

`Windows-Hil.ps1` supports adapter enable/disable, UTC-offset discovery, flushed
file writes, a recursive hidden-file/mtime inventory, and relevant System event
collection. Adapter commands default to a TP-Link UB500 (`2357:0604`); pass
`-Vid` and `-ProductId` to select another adapter. Run adapter state changes
from an elevated PowerShell session.

## Cycle records

Generate the fixed 100-cycle ordering, capture SWD snapshots before and after a
cycle, then validate the expected preferred/fallback and HID deltas:

```sh
sh tools/run-limited.sh python tools/hil/totp_hil.py soak-plan \
  --output tools/hil/results/soak-plan.json
sh tools/run-limited.sh python tools/hil/totp_hil.py assert-cycle \
  --cycle 1 --mode windows-preferred --reports 15 \
  --before tools/hil/results/before.json \
  --after tools/hil/results/after.json \
  --expected 123456 --capture tools/hil/results/windows-capture.jsonl \
  --capture-before-size "$WINDOWS_CAPTURE_SIZE" \
  --other-capture tools/hil/results/linux-capture.jsonl \
  --other-before-size "$LINUX_CAPTURE_SIZE" \
  --results tools/hil/results/cycles.jsonl
```

Record both capture-file byte sizes immediately before the cycle. The validator
requires a new exact value on the intended host and no growth on the other, so a
matching value left over from an earlier TOTP period cannot produce a false pass.

Stop on the first failed assertion. Preserve both host logs, Windows Bluetooth
events, the two capture logs, SWD snapshots, and the exact firmware hashes.
Before every pairing step that needs another person, stop and confirm that they
are ready; no confirmation is needed for automated steps.

The orchestrator defaults to the current Python interpreter; pass
`--pyocd-python` when pyOCD is installed in another environment. Symbol tools
are resolved from the selected ELF's CMake build metadata or `PATH`; SWD helper
commands also accept `--nm`.

Once both hosts are paired and contain the same explicit test fixture, the
orchestrator runs equal preferred/fallback groups and writes a per-cycle record:

```sh
sh tools/run-limited.sh --seconds 7200 python tools/hil/multihost_soak.py \
  --elf build-swd/zephyr/zephyr.elf \
  --results tools/hil/results/soak-RUN \
  --secret TEST_BASE32_SECRET \
  --token-address TOKEN_BT_ADDRESS \
  --linux-host USER@LINUX_HOST --linux-key LINUX_KEY \
  --linux-helper /absolute/path/on/linux/totp_hil.py \
  --windows-host USER@WINDOWS_HOST --windows-key WINDOWS_KEY \
  --windows-script 'C:\totp-hil\Windows-Hil.ps1'
```

The Windows capture hook must run in the physical console session, not through
RDP. During Linux-host cycles, ensure a VM console viewer does not forward the
host's keyboard events into the Windows guest and create a false cross-host
capture. Copy the helper to the Linux path supplied with `--linux-helper`, and
install a narrowly scoped udev rule allowing the SSH test user to read the
token's hotplugged input node. Host-only TOTP/PTY/capture commands work from a
standalone copy of the helper; ELF/SWD commands need the repository's shared
toolchain helper. Supply the Windows script location with
`--windows-script`; neither remote path has a machine-specific default. The
runner controls adapter availability and keeps BlueZ discovery active for
reconnect tests. Linux preferred-host cycles require automatic reconnect. During
Linux fallback cycles, the runner first allows automatic reconnect, then issues
an explicit BlueZ connect if repeated rapid adapter cycling has put the host's HID reconnect
policy into backoff. Each cycle records whether the connection was automatic or
explicit. The default of 25 iterations per mode is 100 cycles.

The runner stops on the first failure. After diagnosing a bench/infrastructure
failure, pass `--resume` with the same result directory; it validates the
existing contiguous passing records and continues at the next scheduled mode.
