# Dependencies and firmware provenance

Project-owned application, bootloader, and host tools are covered by LICENSE.
The independent bootloader includes no Adafruit USB implementation, Nordic SDK
implementation, CMSIS headers, or factory firmware. Its SHA-256 implementation
follows FIPS 180-4. Register addresses and the minimal MBR/S140 SVC ABI are
independently declared in `bootloader/boot.h`, `bootloader/boot.c`, and
`bootloader/svc.S`.

The Zephyr application depends on Zephyr and its west modules, including Nordic
HAL/CMSIS, FatFs, and PSA crypto. Their own licenses and notices remain in force;
the project's MIT license does not replace them. Reproduce the workspace's
west manifest when preparing a release and include the notices for the actual
linked dependency versions.

The independent bootloader currently pins Zephyr SDK 1.0.1's ARM GCC 14.3.0.
Compiler support routines, if linked, retain the toolchain's licensing and GCC
Runtime Library Exception. The application uses Zephyr's configured C runtime.

Bench recovery uses the separately downloaded official Adafruit nice!nano 0.9.2
HEX containing Nordic S140 6.1.1. It is verified by SHA-256 in
`tools/uf2/prepare_recovery.py`. That download, its board-specific recovery HEX,
and flash backups are ignored bench artifacts; they are not part of the
application UF2 or this repository. On-device recovery captures the board's
factory bootloader locally. No MBR/S140/factory firmware is distributed inside
the application UF2.

Interface references:

- [Nordic S140 6.1.1 MBR API](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.2/lib/softdevice/s140_nrf52_6.1.1/s140_nrf52_6.1.1_API/include/nrf52/nrf_mbr.h)
- [Adafruit 0.9.2 handoff and GPREGRET behavior](https://github.com/adafruit/Adafruit_nRF52_Bootloader/blob/0.9.2/src/main.c)
- [Zephyr Pro Micro board documentation](https://docs.zephyrproject.org/latest/boards/others/promicro_nrf52840/doc/index.html)
- [Private testing PID policy](https://pid.codes/1209/0001/)
- [Nordic nRF52840 debug/protection reference](https://docs.nordicsemi.com/r/bundle/ps_nrf52840/page/dif.html)
