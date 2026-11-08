# AGENTS.md

Guidance for AI agents working in this repository. If an ignored
`AGENTS.local.md` is present, read it for workspace-specific context before
hardware work. Public builds must not depend on that file.

## Project

TapShard is a battery-powered BLE HID TOTP token on nRF52840, using upstream
Zephyr rather than Nordic NCS. USB Mass Storage exposes a RAM configuration
volume; plugging in dumps `config.ini`, and a plugged-in click or physical
unplug applies the edited configuration. Unplug then erases and reformats the
entire RAM disk. Secrets and per-host profiles are RAM-only.

USB manufacturer/product: `TapShard`; storage vendor/model: `TapShard` / `Config`;
BLE name: `TapShard TOTP token`; USB serial: `totp-01`. Factory recovery identities
are independent of application branding.

## Layout

- `src/main.c` — app state machine, pairing/snatch, `config_sync`/`time_sync`,
  config dump, button callbacks, LED.
- `src/keyboard.c/.h` — HID-over-GATT (hand-rolled HIDS; no Zephyr HOGP),
  completion-driven non-blocking report sequencer.
- `src/totp.c/.h` — HMAC-SHA1 (PSA), RFC 4648 base32, RFC 6238 + selftest.
- `src/config.c/.h` — INI parser, bounded values, dump-local `keep:N` refs.
- `src/config_fs.c/.h` — FatFs read/write/wipe, stable snapshots, session-local
  mtime changes, `get_fattime`.
- `src/profiles.c/.h` — four per-bond RAM profiles, limbo profile, bond lookup,
  usage ranking.
- `src/secure_zero.h` — compiler-resistant clearing of sensitive RAM buffers.
- `src/time.c/.h` — monotonic clock + LFCLK drift calibration (settings).
- `src/button.c/.h` — click/long-press gesture detection.
- `src/battery.c/.h` — SAADC `VDDH/5` battery voltage sampling.
- `src/usb_msc.c/.h` — USB MSC over a RAM disk.
- `src/usb_power_replay.c/.h` — nRF UDC VBUS replay after bootloader handoff.
- `src/boot_security.c/.h` — pre-provisioning UF2 install/backup/protection gate.
- `bootloader/` — independent SRAM-wiping bootloader, fixed layout, SHA-256,
  MBR handoff, and triple-reset factory restore.
- `tools/uf2/` — bootloader build, UF2 bounds validation, recovery preparation,
  native checks, and destructive unlocked-bench acceptance helpers.
- `tools/hil/` — SWD gestures/counter snapshots, Linux HID capture, independent
  TOTP calculation, cycle assertions, and Windows PowerShell helpers.
- `config.ini.example` — documented INI format (keep in sync with `config.c`).
- `Kconfig` — `TOTP_DEBUG`, `TOTP_TEST_POLL`, `TOTP_BATTERY_MONITOR`, and the
  app-visible `TOTP_MAX_BONDS` capacity.

## Build and verification

Use [the build guide](docs/BUILDING.md) for the pinned Zephyr/SDK setup, build
variants, host tests and SWD commands. Use [the UF2 reference](docs/UF2.md) for
compatibility, partitions, recovery and validation limits. HIL procedures are
in [tools/hil/README.md](tools/hil/README.md).

Keep routine SWD development unlocked. The protected configuration is
`conf/uf2-production.conf`; testing protection on a development board requires
an explicit lock/erase/restore cycle and verified off-board recovery images.
The private-test PID `1209:0001` must not be used for distributed binaries or
devices. Preserve the compiler-version and boot startup checks.

All host scripts need memory/time bounds: `sh tools/run-limited.sh command ...`
defaults to 512 MiB per process and 120 seconds. Builds use 1 GiB, 240 seconds
and two compile jobs. Timeout terminates descendants; ensure a previous runaway
process has stopped before retrying. Preserve ignored recovery backups and
captures when cleaning the tracked source tree.

Firmware boot tests cover configuration, TOTP, time, FAT snapshots, profiles,
HID pacing and application integration. Their result is `selftest_failed`.
Host tests run with `python -m unittest discover -s tools/uf2 -v`; use the limit
wrapper. Exercise changed behavior without repeating unrelated hardware tests.

## SWD observability

The HIL `symbols` command reads an ELF without hardware. `snapshot` reads
scalars from an unlocked target using pyOCD with `auto_unlock=false`.
Useful hooks include `dbg_state`, `dbg_idle_reason`, `dbg_notify_sent/err`,
`dbg_battery_raw`, `dbg_battery_input_mv`, `dbg_usb_vbus_ready/removed`,
`dbg_usb_enable_err/disable_err`, `dbg_disk_wipe_count/err`,
`dbg_notify_completed/retries/aborts`, and `dbg_adv_preferred/fallback`.

`CONFIG_TOTP_TEST_POLL=y` enables `dbg_force_press` and notification-error
injection (`dbg_force_notify_err`, `dbg_force_notify_after`). It adds periodic
wakeups, so disable it for current measurements and normal use. UF2 builds
expose `dbg_boot_stage` (6 means ready; negative values mean diagnostic-only
MSC). Unlocked bench builds add `dbg_boot_backup_page` for capture interruption.

In the power diagnostic, `dbg_power_phase` is 1 during quiet idle and 2 before
System OFF. SWD makes System OFF use debug emulation; measure without SWD.

## Implementation constraints

- Button wiring is P0.10 (sense input with pull-up) to P1.11 (output-low
  virtual ground), as defined in the shared board overlay.
- Button edge detection must use GPIO SENSE/PORT via the port's
  `sense-edge-mask`. A GPIOTE IN channel keeps the high-frequency peripheral
  clock running in idle. A build assertion ties the button pin to that mask;
  update both when moving the button.
- The unused external VCC regulator is disabled by a GPIO hog on P0.13,
  driven physically low before main(), including the power diagnostic.
  Its board pull-up otherwise leaves it enabled even in System OFF. This
  controls the external VCC rail, not the MCU's internal regulators or supply.
- Flush pyOCD writes before timed waits in electrical/SWD gesture tests.
  Buffered writes can otherwise reach hardware only when the next read occurs.
- `CONFIG_BOARD_HAS_NRF5_BOOTLOADER=n` remains required. The normal SWD target
  starts at 0x0. The separate `/uf2` target uses `USE_DT_CODE_PARTITION` and
  starts at 0x26000; its MBR/S140/backup/NVS/bootloader map must be preserved.
- Never let pyOCD auto-unlock a protected target: that performs a mass erase.
  Always use `auto_unlock=false`, including read-only tools.
- Locked SWD reads can return zeros or stale bus values. Check CTRL-AP lock
  status and AHB access availability; returned zeros do not prove SRAM was wiped.
- Physical pin reset clears GPREGRET2. Reset stages instead use a validated
  16-byte public marker at 0x2003fff0. Assembly decodes it before the full
  256 KiB wipe, clears raw scratch registers, and passes only stage 0/A1/A2 to
  C. C regenerates fixed public fields with the magic committed last.
- App RAM is 255 KiB: Zephyr's ARM linker uses KiB units, so its final KiB is
  reserved. The independent boot stack ends at 0x2003fff0. Never skip the
  marker during the wipe, allocate app data there, or let the boot stack touch it.
- Only unlocked bootloaders recognize a validated bench wipe-stop request in
  that marker area. They spin at `boot_wipe_wait` after erasing all SRAM and
  before stack use, allowing SWD to reconnect after physical reset. Protected
  builds omit the request decoder and stop entirely.
- Real nRESET also resets debug state. After a raw probe pulse, reconnect the
  DP and send pyOCD PRE_RESET/POST_RESET notifications to invalidate MEM-AP
  CSW caches; otherwise reads may silently use the reset byte access width.
- Shut down TIMER2 after the bootloader's reset window, using its SHUTDOWN
  task as well as STOP. Leaving the stopped timer powered can raise idle draw
  after a pin reset even though USB wake skips that window.
- UF2 provisioning runs only after factory backup and complete active
  bootloader verification. A gate error exposes diagnostic-only MSC and never
  initializes BLE, button actions, or config parsing. Backup validity is written
  last; installed custom bootloaders must never be captured as factory backups.
- Factory recovery validates S140-v6 geometry/addresses and factory vectors,
  independently of factory version. Capture records a complete-image SHA-256;
  recovery verifies it against the saved metadata. This is corruption detection,
  not publisher authentication. Reject our boot ID across versions as a factory
  candidate. Reuse an unchanged valid backup; recapture a changed factory image.
  Adafruit-compatible GPREGRET 0x57 recovery behavior is still required.
  The independent bootloader contains no USB/BLE and restores using MBR COPY_BL,
  preserving debug protection. Its first stack access follows a full SRAM wipe.
- Bootloader handoff can consume POWER's initial VBUS edge. The adapter wraps
  `nrfx_power_usbevt_init` and replays the current level through the UDC's actual
  callback; setting POWER event registers from software does not replay it.
- `settings_load()` must run in `bt_ready` **before** `bt_le_adv_start`.
- A real boot clears all restored BLE bonds after `settings_load()`: secrets and
  usage metadata are RAM-only, so retaining flash-backed trust without them
  would leave stale empty hosts. System On sleep does not clear bonds.
- PSA crypto needs the system heap (`CONFIG_HEAP_MEM_POOL_SIZE`) **and**
  `CONFIG_MAIN_STACK_SIZE=8192` (HMAC is stack-heavy).
- HID: Report Reference descriptor must be exactly 2 bytes `{id, type}`; no
  Report ID in the 8-byte boot report map; report/CCC perms are `*_ENCRYPT`;
  real read callbacks (not `NULL`).
- HID sends are serialized through `bt_gatt_notify_cb`: wait for completion,
  then apply the configured delay. Retry transient `-ENOMEM`/`-EAGAIN`; abort
  and disconnect on permanent error or completion timeout so no key stays down.
- On low battery with USB unplugged, typing delays ramp from 2x on the first
  character to 5x on the last; Enter keeps the final cadence. Keep key holds
  at normal duration and put the extra delay after release to avoid host
  auto-repeat. The sender and TOTP expiry budget share the same timing
  calculation. Each send captures its pacing for the entire code.
- SMP is Just Works (`BT_SMP_ENFORCE_MITM=n`) + `ALLOW_UNAUTH_OVERWRITE=y`.
- Raised `CONFIG_BT_BUF_ACL_TX_COUNT`/`ATT_TX_COUNT`/etc. for the HID report burst.
- FatFs: `CONFIG_FS_FATFS_HAS_RTC=y` with `get_fattime()` returning **1980-01-01**
  (below `TIME_SANE_MIN`) so device-written files never become a clock anchor;
  `CONFIG_FS_FATFS_LFN=y` so `config.ini` keeps its lowercase name.
- `CONFIG_FS_FATFS_NORTC` is not used; with `HAS_RTC`, `get_fattime()` is required.
- Dumped secrets use `keep:N`, resolved against the protected profile snapshot
  that produced that USB session's dump. Do not resolve them by destination
  position or against a profile changed by a live apply.
- With at least two bonds, the dump emits session-local `bond_N` references,
  `bond_snapshot`, `edit_bonds = false`, repeated `keep_bond`, and `host_id`.
  Validate the snapshot before using any bond reference. Bond editing is
  opt-in; bonds paired after the snapshot are never removed by that snapshot.
- Select a host profile only after successful link security and confirmation
  that the resolved identity exists in Zephyr's real bond database. Pairing is
  always unfiltered. Reconnect first uses ordinary connectable advertising with
  the controller accept list restricted to the last host for 5 seconds, then
  restarts unfiltered advertising so any retained host may connect.
- With `CONFIG_BT_MAX_CONN=1`, a disconnect callback still owns the sole
  connection object. Defer restarting connectable advertising after a
  disconnect until `bt_conn_cb.recycled`; starting it in the callback fails
  with `-ENOMEM`. The filtered-to-unfiltered timer has no connection object and
  restarts advertising directly.
- Preserve a click as a raw slot request until the secured peer profile is
  selected. A fallback may connect a different host than the preferred one, so
  classifying `000`/`0000` or slot validity at button time uses stale state.
- `CONFIG_TOTP_MAX_BONDS` must equal `CONFIG_BT_MAX_PAIRED`. Normal builds use
  four; reduced-capacity HIL builds lower both together.
- A simple zero-/one-bond USB dump remains bound to its protected dump profile
  if another host is paired during that mounted session. Multi-bond dumps still
  require the session-bound `host_id`.
- A profile with no accounts always dumps the commented account template,
  including after a valid settings-only configuration has been applied.

## Clock invariants

- Time anchor = newest sane mtime among files changed in the current USB
  session, confirmed in two polls and applied at the first poll's uptime; the
  dump's own mtime is 1980 and is part of the pre-enumeration baseline.
- Ppm calibration has no minimum waiting period. It compares host elapsed time
  with raw uptime and changes the rounded integer correction only when it is
  strictly better at both ends of a conservative +/-4 s elapsed-time uncertainty
  interval. Keep the +/-500 ppm limit and civil-time-step guard. This assumes
  fresh host timestamps and does not predict future temperature changes.
- USB visit begin/end delimit a fixed calibration reference during a visit;
  end follows final timestamp confirmation and promotes its latest accepted
  anchor even if ppm did not change. Empty visits retain the reference, and
  VBUS bounce does not begin a new visit. Manual ppm changes invalidate the
  reference and pending promotion. Calibration references are RAM-only; the
  saved `totp/drift` setting remains a signed 32-bit ppm. Clock self-tests use
  a pure estimator and do not write settings or alter live calibration state.

## Code style

Match the existing code: C99, tabs, K&R braces, descriptive `/* */` comments in
English. Keep INI documentation synchronized with `config.c` and follow the
comment style in `config.ini.example`.
