# TapShard

TapShard is a small, battery-powered TOTP token that types one-time codes over
Bluetooth. Pair it as a keyboard, configure your accounts through a USB drive,
and press its button when you need a code. No companion app is needed.

It runs upstream Zephyr on an nRF52840 Pro Micro / SuperMini-style board.

## Features

- HMAC-SHA1 TOTP with configurable code length, period and optional Enter.
- Up to four paired hosts, each with its own list of up to 16 accounts.
- One button: hold to pair, click once for the first account, twice for the
  second, and so on.
- A USB configuration drive: edit `config.ini` with a text editor.
- TOTP secrets kept only in RAM and omitted from configuration dumps.
- Automatic clock synchronization from fresh USB file timestamps, with adaptive
  crystal-drift calibration between USB visits.
- Low-power idle and a low-battery reminder that gradually slows code entry.
- USB firmware updates, a RAM-wiping bootloader and debug-port protection in
  the protected build.

**Keep the battery connected.** Resetting the device or losing power clears its
accounts and pairings; you will need to configure and pair it again. Keep your
original account secrets and recovery codes somewhere you can use to set it up.

## Hardware

You need a compatible nRF52840 board, a suitable battery and a momentary button.
The firmware is configured for the Zephyr `promicro_nrf52840` board target.
Check your board's pinout and factory bootloader against the
[compatibility requirements](docs/UF2.md#compatibility) before installing.

| Connection | Wiring |
|---|---|
| Button | Between **P0.10** and **P1.11** |
| Battery | Board battery terminals, following its voltage and polarity requirements |
| USB | Board's native USB port, using a data-capable cable |

P0.10 is the button input; P1.11 is driven low as its return connection. These
are nRF GPIO names, which may differ from the labels printed on the board.
The firmware disables the external VCC regulator controlled by P0.13, so the
VCC/EXTVCC header is unpowered during normal operation.

Idle current depends on the board and attached components. One battery setup
measured about 4 µA, including roughly 2 µA of external capacitor leakage.

## Get started

### 1. Build and install

Follow the [build guide](docs/BUILDING.md) to set up Zephyr and build the protected
UF2 image. Put a compatible factory board into its USB bootloader mode, then
copy `zephyr.uf2` to the **NICENANO** drive. The device restarts and appears as
**TapShard Config**. See [installation and recovery](docs/UF2.md) for details.

The protected image replaces the active bootloader while saving a recovery copy
on the device, and locks the debug port. Restoring debug access requires a full
chip erase. Use the separate bench image when developing over SWD.

### 2. Pair a host

1. Turn on Bluetooth on your computer or phone.
2. Hold the button for about **three seconds**, until the LED flashes.
3. Select **TapShard TOTP token** in the host's Bluetooth settings. The default
   pairing window is 30 seconds.

Pair additional hosts the same way. If another paired host keeps reconnecting
during setup, temporarily turn off its Bluetooth. After a device reset, forget
the old pairing on the host and pair again.

### 3. Add your accounts

Connect USB and open `config.ini` on **TapShard Config**. Add an `[account]`
section using the Base32 secret from your service's two-factor setup screen:

```ini
[device]
timezone = 0

[account]
name = Example
# Replace this demonstration secret with your own account's Base32 secret.
secret = JBSWY3DPEHPK3PXP
digits = 6
period = 30
enter = false
```

Leave `timezone = 0` for the tested Linux setup. On Windows, set the host's local
UTC offset, such as `+3` or `-5`. The
[configuration example](config.ini.example) explains the settings and ranges.
macOS behavior has not been validated.

Save the file, allow about a second for timestamp synchronization, safely eject
the drive if needed, and unplug USB to apply the configuration. To keep USB
connected for charging, focus the code-entry field and click the button: it
applies the saved configuration and requests that account's code.

The device creates a new configuration dump on every USB connection. Existing
secrets appear as `secret = keep:N`; preserve those references when editing or
reordering accounts. They are valid only for the dump that contains them.

### 4. Type a code

Focus the host's code-entry field, then click once for the first account, twice
for the second, and so on. TapShard reconnects, types the code and disconnects
again. It prefers the last-used host before allowing another paired host to
connect. If several hosts are nearby, their Bluetooth availability determines
which one can receive the code.

Each host has a separate account profile. With multiple pairings, the generated
configuration includes `host_id` and bond references for selecting a profile or
forgetting a host; see [the annotated example](config.ini.example).

## Clock and battery

Saving a file on the configuration drive provides a fresh clock timestamp. USB
power alone does not synchronize the clock. Between visits, a low-frequency
crystal keeps time while the CPU sleeps.

TapShard learns a ppm correction when the elapsed-time measurement is precise
enough to improve the current value. It uses recent USB visits so older
conditions do not dominate indefinitely. You can also edit `drift_ppm` in the
configuration. Reset loses the clock reference but preserves the saved ppm;
factory recovery clears it along with the other settings.

On low battery, typing gradually slows from 2× to 5× its normal delay across a
code. Charge the battery to restore normal speed. The threshold and typing
speed are configurable; the dump header includes the measured battery voltage.

## Troubleshooting

| Symptom | What to check |
|---|---|
| Types `0000` | No accounts are configured for the connected host. |
| Types `000` | Configuration could not be parsed, or the selected secret is invalid. Check `log.txt` on the next USB connection. |
| No text appears | Check pairing, Bluetooth availability, input focus and the selected account number. Save a file on USB to synchronize an unset clock. |
| Codes are rejected | Check the service's secret, period, digit count and the host's clock/timezone setting. |
| Only `log.txt` appears after installation | The bootloader verification gate reported an error; see [recovery](docs/UF2.md#installation-errors). |

## Firmware updates

Connect USB and press Reset **three times quickly**, with less than 750 ms
between presses. When the factory bootloader drive appears, copy the new UF2.
Then pair and configure again. Updates clear accounts, pairings and saved clock
calibration. The [UF2 guide](docs/UF2.md) covers reset behavior and recovery.

## Project status and security

TapShard is a hobby project. Linux and Windows pairing, configuration and HID
operation have been tested. Compatible boards with factory bootloaders 0.6.0
and 0.9.2 / S140 6.1.1 have been used; see the
[compatibility and validation limits](docs/UF2.md#compatibility).

Configuration contains plaintext secrets while you edit it, so use a trusted
host and pairing session. The protected build wipes RAM on reset and enables
debug-port protection. Resistance to deliberate hardware fault injection has
not been established.

Only source code is provided. The default USB ID
`1209:0001` is reserved for private testing and is not unique. Do not distribute
binaries or devices using that ID; an allocated PID is needed first. See the
[PID policy](https://pid.codes/1209/0001/).

## Building and contributing

Set up Zephyr v4.4.2 and SDK 1.0.1 with [Building TapShard](docs/BUILDING.md),
then build the protected UF2 from the application directory:

```sh
sh tools/run-limited.sh --memory-mib 1024 --seconds 240 \
  west build -b promicro_nrf52840/nrf52840/uf2 -d build-uf2 -o=-j2 -- \
  -DEXTRA_CONF_FILE=conf/uf2-production.conf
```

The [UF2 reference](docs/UF2.md) describes installation and the flash layout;
[hardware test helpers](tools/hil/README.md) cover developer testing.

Project source is [MIT licensed](LICENSE). See [third-party notices](THIRD_PARTY.md)
for dependency and firmware provenance.
