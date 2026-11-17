# Dependencies and firmware provenance

Project-owned application, bootloader, and host tools are covered by LICENSE.
The independent bootloader includes no Adafruit USB implementation, Nordic SDK
implementation, CMSIS headers, or factory firmware. Its SHA-256 implementation
follows FIPS 180-4. Register addresses and the minimal MBR/S140 SVC ABI are
independently declared in `bootloader/boot.h`, `bootloader/boot.c`, and
`bootloader/svc.S`.

The Zephyr application depends on the following upstream components. Their own
licenses and notices remain in force; the project's MIT license does not replace
them. The [collected notices](THIRD_PARTY_NOTICES.txt) accompany binary releases
as `LICENSES.txt`.

| Component | License |
|---|---|
| Zephyr | Apache-2.0, with per-file notices |
| CMSIS and CMSIS 6 | Apache-2.0 |
| Nordic nrfx / MDK | BSD-3-Clause |
| Nordic Zephyr integration | LicenseRef-Nordic-5-Clause |
| FatFs | ChaN's permissive license |
| Mbed TLS and TF-PSA-Crypto | Apache-2.0, selected from the upstream dual-license option |
| Picolibc / Newlib runtime | Per-file licenses in the upstream copyright catalogs |
| GCC support routines | GPL-3.0 with GCC Runtime Library Exception 3.1 |

Each release includes `WEST_MANIFEST.yml` with exact dependency revisions and
`BUILD_INFO.json` with the source commit, tool versions, production settings and
firmware hashes. The notice collection includes upstream license catalogs;
listing a catalog does not mean every file it describes is linked into the UF2.

The independent bootloader currently pins Zephyr SDK 1.0.1's ARM GCC 14.3.0.
Compiler support routines retain the toolchain's licensing and GCC Runtime
Library Exception. The protected application uses the SDK's Picolibc runtime.

The offline recovery helper `tools/uf2/prepare_recovery.py` accepts a separately
downloaded official Adafruit nice!nano 0.9.2 HEX containing Nordic S140 6.1.1 and
checks its pinned SHA-256. That download, its board-specific recovery HEX,
and flash backups are ignored bench artifacts; they are not part of the
application UF2 or this repository. On-device recovery captures the board's
factory bootloader locally. No MBR/S140/factory firmware is distributed inside
the application UF2.

Interface references:

- [Nordic S140 6.1.1 MBR API](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.2/lib/softdevice/s140_nrf52_6.1.1/s140_nrf52_6.1.1_API/include/nrf52/nrf_mbr.h)
- [Adafruit 0.9.2 handoff and GPREGRET behavior](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.2/src/main.c)
- [Zephyr Pro Micro board documentation](https://docs.zephyrproject.org/latest/boards/others/promicro_nrf52840/doc/index.html)
- [Objective Development shared USB IDs and usage terms](https://github.com/obdev/v-usb/blob/master/usbdrv/USB-IDs-for-free.txt)
- [Nordic nRF52840 debug/protection reference](https://docs.nordicsemi.com/r/bundle/ps_nrf52840/page/dif.html)
