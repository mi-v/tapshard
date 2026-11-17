# TapShard

TapShard is a small, battery-powered TOTP token that types one-time codes over
Bluetooth. Pair it as a keyboard, configure your accounts through a USB drive,
and press its button when you need a code. No companion app is needed.

It runs upstream Zephyr on an nRF52840 Pro Micro / SuperMini-style board.

Ready-to-flash firmware: [v0.1.0](https://github.com/mi-v/tapshard/releases/tag/v0.1.0).

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

The [enclosure 3D model and build instructions](https://www.printables.com/model/1906498-tapshard-totp-token)
are available on Printables.

| Connection | Wiring |
|---|---|
| Button | Between **P0.10** and **P1.11** |
| Battery | Board battery terminals, following its voltage and polarity requirements |
| USB | Board's native USB port, using a data-capable cable |

P0.10 is the button input; P1.11 is driven low as its return connection. These
are nRF GPIO names, which may differ from the labels printed on the board.
The firmware disables the external VCC regulator controlled by P0.13, so the
VCC/EXTVCC header is unpowered during normal operation.

Idle current depends on the board and attached components. For measurements,
disconnect USB and SWD and use the [measurement build](docs/BUILDING.md#development-variants).

## Get started

### 1. Install firmware

Download the protected [tapshard-v0.1.0.uf2](https://github.com/mi-v/tapshard/releases/download/v0.1.0/tapshard-v0.1.0.uf2),
or follow the [build guide](docs/BUILDING.md) to build it yourself. Put a compatible
factory board into its USB bootloader mode, then copy the UF2 to its bootloader
drive, commonly named **NICENANO**. The device restarts and appears as
**TapShard Config**. See
[installation and recovery](docs/UF2.md) for details.

The protected image replaces the active bootloader while saving a recovery copy
on the device, and locks the debug port. Restoring debug access requires a full
chip erase. Use the separate bench image when developing over SWD.

### 2. Pair a host

1. Turn on Bluetooth on your computer or phone.
2. Hold the button for about **three seconds**, until the steady LED turns off
   and begins a slow blink.
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

Set `timezone` to the UTC offset of the file timestamps written to the USB
drive. On Linux, start with `timezone = 0`; FAT timestamp handling depends on
mount settings. If your system writes local-time timestamps, use your current
local UTC offset instead. On Windows, use your current local UTC offset, such
as `+3` or `-5`. The [configuration example](config.ini.example) explains the
settings and ranges.

Save the file, allow about a second for timestamp synchronization, safely eject
the drive if needed, and unplug USB to apply the configuration. To keep USB
connected for charging, focus the code-entry field and click the button: it
applies the saved configuration and requests that account's code.

The device creates a new configuration dump on every USB connection. Existing
secrets appear as `secret = keep:N`; preserve those references when editing or
reordering accounts. They are valid only for the dump that contains them.

You can configure accounts before pairing. They go to the first host you pair,
even if you pair before saving the file. Each host has its own accounts;
device settings are shared. Status comments update on the next USB connection.

### 4. Type a code

Focus the host's code-entry field, then click once for the first account, twice
for the second, and so on. TapShard reconnects, types the code and disconnects
again. It gives the most recently connected host a five-second preference
window before allowing another paired host to connect. If several hosts are
nearby, their Bluetooth availability determines which one can receive the code.

Each host has a separate account profile. With multiple pairings, the generated
configuration includes `host_id` and bond references for selecting a profile or
forgetting a host; see [the annotated example](config.ini.example).

## Clock and battery

Saving `config.ini` synchronizes the clock; drift correction is automatic.
USB power alone does not synchronize it. After a reset, save the file again to
set the clock. The learned drift correction survives reset; factory recovery
clears it.

When battery voltage is low, code typing slows down to indicate it. Adjust the
threshold with `low_battery_mv`.

## Troubleshooting

| Symptom | What to check |
|---|---|
| Types `0000` | No accounts are configured for the connected host. |
| Types `000` | Configuration could not be parsed or applied, or the selected account has `secret = error`. After unplugging, check `log.txt` on the next USB connection for configuration errors. |
| No text appears | Check pairing, Bluetooth availability, input focus and the selected account number. Save a file on USB to synchronize an unset clock. |
| Codes are rejected | Check the service's secret, period, digit count and the host's clock/timezone setting. |
| Only `log.txt` appears after installation | The bootloader verification gate reported an error; see [recovery](docs/UF2.md#installation-errors). |

## Firmware updates

Connect USB and press Reset **three times quickly**, with less than 750 ms
between releases. When the factory bootloader drive appears, copy the new UF2.
Then pair and configure again. Updates clear accounts, pairings and saved clock
calibration. The [UF2 guide](docs/UF2.md) covers reset behavior and recovery.

## Project status and security

TapShard is a hobby project. Your host needs BLE HID keyboard support and USB
Mass Storage support for configuration. Check the board and factory-bootloader
[compatibility requirements](docs/UF2.md#compatibility) before installing.

Configuration contains plaintext secrets while you edit it, so use a trusted
host and pairing session. The protected build wipes RAM on reset and enables
debug-port protection. These measures do not guarantee resistance to deliberate
hardware fault injection.

TapShard uses a shared USB ID, so USB diagnostic tools may show
**Van Ooijen Technische Informatica** as its vendor.
The configuration drive is **TapShard Config**. See the
[build guide](docs/BUILDING.md#build-normal-protected-firmware) for USB identity
details and sharing terms.

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
