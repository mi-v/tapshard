# Building TapShard

These instructions use Linux, including Linux under WSL2, to build the firmware.
The build host can be separate from the computer used to configure and receive
codes from the token. Native Windows/macOS builds are not covered here.

Use **upstream Zephyr v4.4.2**, **Zephyr SDK 1.0.1**, and its **GCC 14.3.0 ARM
toolchain**. The independent bootloader checks the compiler version and verifies
its startup assembly, so changing compilers requires review of those checks.

## Set up a workspace

Install the host dependencies from Zephyr's
[getting started guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).
You need Python 3.12 or newer, Git, CMake, Ninja, a device-tree compiler, a host C
compiler for tests, and GNU `timeout` for the command-limit wrapper.

Create a Python environment and a workspace with the pinned Zephyr revision:

```sh
mkdir -p "$HOME/tapshard-workspace"
cd "$HOME/tapshard-workspace"
python3 -m venv .venv
. .venv/bin/activate
python -m pip install 'west==1.5.0'
west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.4.2 .
west update
west packages pip --install
west zephyr-export
west sdk install --version 1.0.1 --gnu-toolchains arm-zephyr-eabi \
  --install-dir "$HOME/.local/opt/zephyr-sdk-1.0.1"
```

Clone or copy this repository into `tapshard/` under that workspace. The layout
should contain `.west/`, `.venv/`, `zephyr/`, `modules/` and `tapshard/` as siblings.
This lets west find its build commands when invoked from the application.

In each new terminal, activate the environment and select the SDK:

```sh
cd "$HOME/tapshard-workspace/tapshard"
. ../.venv/bin/activate
export ZEPHYR_SDK_INSTALL_DIR="$HOME/.local/opt/zephyr-sdk-1.0.1"
```

If SDK 1.0.1 was already installed elsewhere, the SDK installer may reuse it;
set `ZEPHYR_SDK_INSTALL_DIR` to that existing directory instead.

## Build normal protected firmware

From the application directory:

```sh
sh tools/run-limited.sh --memory-mib 1024 --seconds 240 \
  west build -b promicro_nrf52840/nrf52840/uf2 -d build-uf2 -o=-j2 -- \
  -DEXTRA_CONF_FILE=conf/uf2-production.conf
```

Install **`build-uf2/zephyr/zephyr.uf2`** through the board's factory USB drive.
Read [installation and recovery](UF2.md) before copying it: this image installs
the wiping bootloader and enables persistent debug-port protection.

The default USB identity is the shared Mass Storage pair `16c0:27df`, used under
[Objective Development's sharing terms](https://github.com/obdev/v-usb/blob/master/usbdrv/USB-IDs-for-free.txt).
The serial starts with the owner-controlled `mivlad@gmail.com:` prefix and
appends `tapshard-` plus 16 uppercase hexadecimal digits from the board's
hardware ID. Keep the prefix and the standard OS mass-storage driver when
distributing TapShard builds. Host discovery must check the serial as well as
the VID/PID; the UF2 uploader does this automatically using the selected build's
effective IDs.

For a different authorized identity, supply both
`-DCONFIG_TOTP_USB_VID=0xVVVV` and `-DCONFIG_TOTP_USB_PID=0xPPPP` alongside the
configuration fragment. These settings do not allocate an ID. Builds using the
`1209:0001` private-test pair must not be redistributed.

When changing USB IDs in an existing build, use a pristine build
(`west build -p always ...`) or explicitly set both IDs. Existing build
directories can retain previous configuration overrides.

Each build validates that the UF2 contains only application addresses. The
independent bootloader payload, compiler/source hashes and ELF are generated
under `build-uf2/security-bootloader/`; only the application UF2 is installed
through the factory drive. Factory MBR/S140 firmware is not bundled in that UF2.

## Development variants

Use a separate build directory for each variant. Replace the directory and
`EXTRA_CONF_FILE` in the command above as follows:

| Purpose | Directory | Configuration | Debug access |
|---|---|---|---|
| Normal use | `build-uf2` | `conf/uf2-production.conf` | Locked |
| SWD acceptance | `build-uf2-bench` | `conf/uf2-bench.conf` | Unlocked; test button polling enabled |
| Current measurements | `build-uf2-measurement` | `conf/uf2-measurement.conf` | Unlocked; polling and output disabled |
| Quiet idle / System OFF isolation | `build-uf2-power-diagnostic` | Both fragments below | Unlocked; no normal application operation |

The measurement image retains normal battery sampling, GPIO button interrupts
and tickless idle. Its power behavior requires disconnecting SWD and USB while
measuring the battery supply. Keep real secrets off unlocked images.

The diagnostic image sleeps quietly for 60 seconds, then enters System OFF:

```sh
sh tools/run-limited.sh --memory-mib 1024 --seconds 240 \
  west build -b promicro_nrf52840/nrf52840/uf2 \
  -d build-uf2-power-diagnostic -o=-j2 -- \
  '-DEXTRA_CONF_FILE=conf/uf2-measurement.conf;conf/uf2-power-diagnostic.conf'
```

USB configuration, Bluetooth and button operation are unavailable in that
image. To recover from System OFF, connect USB, press Reset once and wait two
seconds, then use the triple-reset recovery gesture and install normal firmware.

For an SWD application starting at address zero:

```sh
sh tools/run-limited.sh --memory-mib 1024 --seconds 240 \
  west build -b promicro_nrf52840/nrf52840 -d build-swd -o=-j2
```

This is a development image with demonstration accounts/time and button polling.
Its layout is incompatible with UF2 installations. Do not flash its HEX over a
board whose factory MBR/S140 stack must be preserved.

## Firmware behavior reference

### HID readiness

`conn_settle_time` controls settling after the host subscribes to keyboard
notifications. The fallback has a 500 ms minimum. HID metadata reads and explicit
notification-enable writes restart settling; a keyboard LED output report can
end it early. A pending readiness wait is bounded by the configured settling
interval (at least 500 ms) plus 10 seconds. Ordinary host activity does not
extend that deadline. See the [HIL guide](../tools/hil/README.md) for checks.

### Clock synchronization and calibration

The clock anchor is the newest modification time among new or changed regular
files in the USB volume's root directory. It must be within
2020-01-01..2100-01-01, newer than the session's previous anchor, and unchanged
in two polls. The anchor uses the first poll's uptime so confirmation does not
add a clock error. `timezone` converts the timestamp to UTC when generating a
code. Fresh host timestamps and a correct host clock are required.

Calibration compares raw uptime with elapsed host time across USB visits.
There is no minimum waiting period: a new rounded integer correction is accepted
only when it strictly improves both ends of a conservative +/-4-second elapsed
time uncertainty interval. The limit is +/-500 ppm; implausible rates and
apparent civil-time jumps restart the reference.

The calibration reference stays fixed during a USB visit. On unplug, the latest
accepted anchor becomes the next reference; a visit without an anchor retains
the previous reference. A manual change to the current ppm value invalidates
the reference and starts a new measurement window. References are RAM-only;
the signed 32-bit `totp/drift` setting persists across normal resets and is
cleared by factory recovery.

## Host tests

```sh
sh tools/run-limited.sh python -m unittest discover -s tools/uf2 -v
```

These tests cover UF2 boundaries, factory backup validation, SHA-256, command
limits and host tool discovery. Firmware self-tests also run at boot; developer
images expose their result as `selftest_failed` over SWD.

`tools/run-limited.sh` defaults to 512 MiB of address space per process and a
120-second wall-clock timeout. Builds use 1 GiB and two compile jobs. Its timeout
terminates descendants as well as the initial process.

## Release artifacts

Firmware releases contain the protected `tapshard-VERSION.uf2`, `SHA256SUMS`,
`BUILD_INFO.json`, `WEST_MANIFEST.yml` and `LICENSES.txt`. Download all five files
to verify the package with `sha256sum -c SHA256SUMS`. Only the UF2 is copied to
the board's factory bootloader drive.

The build metadata identifies the source commit, tool versions, production
settings and image hashes. The frozen west manifest records the exact Zephyr
and module revisions used by the build; optional modules are omitted. ELF,
linker maps and local hardware captures are not release assets. The UF2 contains
flash data only, without the ELF's debugger sections or factory MBR/S140 images.

To use the frozen dependencies, place `WEST_MANIFEST.yml` in the application
source directory, named `tapshard`, before initializing a new workspace:

```sh
west init -l --mf WEST_MANIFEST.yml tapshard
west update
```

Run these commands from the parent of `tapshard/`, using the source commit named
in `BUILD_INFO.json`. Install dependencies and select the SDK as described above,
then run the protected build command.

## SWD debugging

Install pyOCD into your active development environment:

```sh
python -m pip install pyocd
```

For an unlocked SWD-layout board, use the matching HEX:

```sh
sh tools/run-limited.sh pyocd flash -t nrf52840 -O auto_unlock=false \
  build-swd/zephyr/zephyr.hex
sh tools/run-limited.sh pyocd reset -t nrf52840 -O auto_unlock=false
```

Always set `auto_unlock=false`, including for reads. Automatic unlocking erases
the entire chip. On UF2 boards, preserve the [partition map](UF2.md#flash-layout)
and use the matching application-only artifact.

To inspect ELF symbols without touching hardware:

```sh
sh tools/run-limited.sh python tools/hil/totp_hil.py symbols \
  --elf build-uf2-bench/zephyr/zephyr.elf --symbol selftest_failed
```

The helpers find `nm` using the build's `CMAKE_NM` entry, then `PATH`. An explicit
`--nm /path/to/arm-zephyr-eabi-nm` overrides discovery. The same logic works for
the independent bootloader ELF.

Use the [HIL guide](../tools/hil/README.md) for snapshots, gestures and host
captures. Destructive unlocked-bootloader checks are documented in the
[UF2 reference](UF2.md#developer-checks).
