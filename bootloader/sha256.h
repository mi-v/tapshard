/* SPDX-License-Identifier: MIT */
#ifndef TOTP_SHA256_H
#define TOTP_SHA256_H
#include <stddef.h>
#include <stdint.h>
void boot_sha256(const void *data, size_t length, uint8_t digest[32]);
#endif
