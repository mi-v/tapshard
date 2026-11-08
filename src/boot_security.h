/* SPDX-License-Identifier: MIT */
#ifndef TOTP_BOOT_SECURITY_H
#define TOTP_BOOT_SECURITY_H
/* Run before BLE/USB or any secret provisioning. NULL means ready. */
const char *boot_security_init(void);
#endif
