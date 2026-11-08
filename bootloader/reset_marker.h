/* SPDX-License-Identifier: MIT
 * Public reset bookkeeping, reserved by both application and boot linkers.
 * Constants are shared with the stackless assembly decoder.
 */
#ifndef TOTP_RESET_MARKER_H
#define TOTP_RESET_MARKER_H
#define BOOT_RAM_START 0x20000000
#define BOOT_RAM_END 0x20040000
#define BOOT_APP_RAM_END 0x2003fc00
#define BOOT_MARKER_ADDR 0x2003fff0
#define BOOT_MARKER_SIZE 16
#define BOOT_MARKER_MAGIC 0x31545352
#define BOOT_STAGE_ONE 0xa1
#define BOOT_STAGE_TWO 0xa2
/* Only unlocked bench builds recognize this request to stop after the wipe. */
#define BOOT_BENCH_WIPE_MAGIC 0x45504957
#endif
