# UF2 installation and recovery

Use the protected UF2 image for normal operation. The
[build guide](BUILDING.md) also describes unlocked developer and measurement
images. The default USB PID is for private testing; see the
[project README](../README.md#project-status-and-security) before distribution.

## Compatibility

TapShard targets nRF52840 Pro Micro / SuperMini-style boards with a compatible
Adafruit/nice!nano factory layout. The installer checks:

- nRF52840 flash geometry and an existing S140 **v6** stack with application
  start at `0x26000`.
- Effective bootloader address `0xf4000` and MBR parameter page `0xfe000`, taking
  MBR settings into account before UICR.
- Factory vectors and Adafruit-compatible GPREGRET `0x57` USB recovery behavior.

Support is based on geometry and behavior, not a factory version allowlist.
Factory **0.6.0** and **0.9.2**, both with S140 **6.1.1**, have been used on two
boards. A different clone or factory layout may require board-specific work.
The board must also match the button and external-regulator wiring in
[the hardware description](../README.md#hardware).

| Area | Validation coverage |
|---|---|
| Linux and Windows | Pairing, configuration, encrypted HID and multi-host reconnect |
| macOS | Not validated |
| Unlocked bootloader | Full SRAM wipe before stack use; real reset gestures; exact factory recovery; backup corruption and reset-interruption checks |
| Persistent debug protection | Pin reset, factory recovery, UF2 reinstall and explicit erase/unlock on AAD0 silicon |
| Native USB path | Installation and recovery with factory 0.6.0; protected installation reaches normal configuration; no independent SWD inspection |
| Other silicon revisions | Software protection branch compile-checked |
| Power removal/brownout during flash writes | Not validated; reset interruption is a separate test |
| Deliberate fault injection | Resistance not established |

## First installation

1. Build `zephyr.uf2` with `conf/uf2-production.conf` as described in the
   [build guide](BUILDING.md#build-normal-protected-firmware).
2. Connect the board's native USB. Enter its factory bootloader using the
   board's own gesture, commonly a double press of Reset.
3. Confirm the **NICENANO** drive and copy the application `zephyr.uf2` to it.
4. Allow the automatic restarts to complete. The factory drive disappears;
   normal operation presents **TapShard Config** with `config.ini`.
5. [Pair and configure the token](../README.md#get-started).

Before accepting secrets, the application captures the complete factory
bootloader with its SHA-256 and installs the independent wiping bootloader
through Nordic MBR COPY_BL. It verifies the active image and backup, writes
persistent APPROTECT in protected builds, then restarts to confirm the stored
protection setting. Only the normal configuration path permits provisioning.

Installation enables debug-port protection. Reopening SWD requires explicit
CTRL-AP ERASEALL and restoration of the factory stack; that operation erases
flash, UICR and RAM, including the on-device backup. Keep off-board recovery
images before performing development that needs this operation.

## Updating or returning to factory USB

Connect native USB and press Reset **three times quickly**, with less than
750 ms between releases. The wiping bootloader validates its backup, erases
settings/NVS, and restores the captured factory bootloader through MBR COPY_BL.
It requests immediate factory USB mode. Copy the new application UF2 to the
factory drive and wait for normal configuration USB to return.

Accounts, pairings and saved ppm calibration are cleared by this procedure.
Debug protection remains enabled through factory recovery. The factory image
is restored from the board's own backup; the application UF2 does not distribute
MBR, SoftDevice or factory firmware.

A single or double pin reset returns to the application after its 750 ms
window. Power-on has no gesture delay. Software, watchdog and other reset
causes discard the gesture stage. All resets lose the RAM configuration.

## Installation errors

If only `log.txt` appears, read its diagnostic before attempting another
installation. The verification gate does not start normal BLE, button actions
or configuration parsing when the layout, backup or installed bootloader fails
validation.

An incomplete backup can be captured again while a compatible factory
bootloader is active. An installed wiping bootloader never backs up its own
bytes as a factory image. An unchanged valid backup is reused; a changed factory
image is captured before installation. A bad backup prevents triple-reset
restoration and preserves the active bootloader and settings for diagnosis.

If factory USB cannot be restored, SWD recovery may be needed. Use a recovery
image for the correct board and preserve its reset-pin and supply configuration.
The developer `prepare_recovery.py` helper accepts a pinned official
nice!nano 0.9.2/S140 6.1.1 HEX plus a saved UICR image; it prepares an offline
recovery artifact and never writes hardware. See [firmware provenance](../THIRD_PARTY.md).

## Flash layout

The normal address-zero SWD target is incompatible with this map. Preserve
MBR/S140, backup, settings and bootloader partitions when working with UF2.

| Flash range | Contents |
|---|---|
| 0x00000–0x25fff | Existing MBR and S140 v6 |
| 0x26000–0xdffff | Application and embedded independent bootloader |
| 0xe0000–0xe9fff | Exact 40 KiB factory bootloader backup |
| 0xea000–0xeafff | Backup metadata and final validity marker |
| 0xeb000–0xebfff | Reserved |
| 0xec000–0xf3fff | Settings/NVS |
| 0xf4000–0xfdfff | Active bootloader |
| 0xfe000–0xfefff | MBR parameter page |
| 0xff000–0xfffff | Factory bootloader settings |

The application has 255 KiB of RAM. The public 16-byte reset marker occupies
`0x2003fff0..0x2003ffff`; the independent boot stack ends below it. Assembly
validates the marker into registers, clears raw scratch registers and wipes
**all 256 KiB**, including the marker, before the first stack access. C then
regenerates fixed public marker fields with the magic committed last. Only
validated A1/A2 stages accompanied by a real pin-reset reason advance recovery.

## Security boundaries

The independent bootloader contains no USB, BLE, DFU or configuration interface.
It wipes RAM before handing control to the application or factory recovery.
Backup SHA-256 detects corruption; it does not authenticate the factory publisher.
The existing factory image, configuration host and pairing session are trusted.

Protected builds enable persistent debug protection. This design does not
authenticate application updates, prevent physical denial of service, or claim
resistance to deliberate voltage glitching or invasive hardware attacks. Host
configuration is plaintext on the volatile drive while being edited.

## Developer checks

Install pyOCD into the active environment and use an unlocked bench image with
**test secrets only**. Run host-side checks first:

```sh
sh tools/run-limited.sh python -m unittest discover -s tools/uf2 -v
```

These commands mutate the bench board and may destroy RAM or settings:

```sh
sh tools/run-limited.sh --seconds 60 python tools/uf2/bench.py wipe --reset software
sh tools/run-limited.sh --seconds 60 python tools/uf2/bench.py wipe --reset watchdog
sh tools/run-limited.sh --seconds 60 python tools/uf2/bench.py wipe --reset pin
sh tools/run-limited.sh --seconds 180 python tools/uf2/bench.py recovery
```

The helpers default to `build-uf2-bench`; pass `--build` for another directory.
Symbol tools are found from CMake metadata or `PATH`, with `--nm` as an override.
All probe sessions use `auto_unlock=false`.

Connect probe nRESET to board RST for real reset tests. Flush writes before timed
waits; after a raw reset pulse, reconnect the DP and issue reset notifications
so debug register caches match hardware. Software reset with an injected reason
is not a substitute for a pin-reset retention test. Physical pin reset clears
GPREGRET2; the public SRAM marker carries the gesture stage instead.

`pin-check` exercises single/double timeouts; `marker-check` covers malformed
markers; `nonpin` checks software/watchdog stage clearing. Unlocked builds can
stop after wiping SRAM and before stack use for inspection. Protected builds
omit that stop and its request decoder.

`invalid-backup` and `invalid-recovery` check failure handling, including NVS
preservation with a nonempty sentinel. `backup-resume` interrupts the 13 capture
checkpoints. `copy-resume --direction install` and `--direction restore` exercise
MBR copy restart through the parameter page. Use a controlled recovery setup
and preserve local captures; these checks do not establish power-cut safety.

A debugger changes System OFF into debug emulation. For the separate 60-second
quiet-idle/System-OFF image, follow the [diagnostic build instructions](BUILDING.md#development-variants)
and measure without USB or SWD attached.
