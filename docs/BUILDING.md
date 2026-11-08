# Building TapShard

The supported build path is Linux, including Linux under WSL2. Normal device
use is tested on Linux and Windows; native Windows/macOS builds are not covered
by these instructions.

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

The default PID `1209:0001` is for private testing. Source builds for personal
testing use that default; binary/device distribution requires an allocated PID.
An allocated PID can be supplied as `-DCONFIG_TOTP_USB_PID=0xNNNN` alongside the
configuration fragment. See [the allocation policy](https://pid.codes/1209/0001/).

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
