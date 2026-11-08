/* SPDX-License-Identifier: MIT */
#ifndef TOTP_BOOT_H
#define TOTP_BOOT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "reset_marker.h"

#define BOOT_APP 0x26000u
#define BOOT_APP_END 0xe0000u
#define BOOT_BACKUP 0xe0000u
#define BOOT_METADATA 0xea000u
#define BOOT_STORAGE 0xec000u
#define BOOT_START 0xf4000u
#define BOOT_SIZE 0xa000u
#define BOOT_PARAMS 0xfe000u
#define BOOT_PAGE 0x1000u
#define BOOT_META_MAGIC 0x504f5442u
#define BOOT_META_VALID 0x56414c49u
#define BOOT_ID_MAGIC 0x534f5450u
#define BOOT_RECOVERY_FAILED 0xe1u
#define BOOT_REG(address) (*(volatile uint32_t *)(uintptr_t)(address))
#define BOOT_RESETREAS BOOT_REG(0x40000400u)
#define BOOT_GPREGRET BOOT_REG(0x4000051cu)
#define BOOT_GPREGRET2 BOOT_REG(0x40000520u)
#define BOOT_APPROTECT BOOT_REG(0x10001208u)

struct boot_metadata {
	uint32_t magic;
	uint32_t version;
	uint32_t source;
	uint32_t length;
	uint8_t sha256[32];
	uint32_t valid;
};

/* Minimal, independently declared Nordic MBR SVC ABI: command plus union. */
struct boot_mbr_command {
	uint32_t command;
	uint32_t source;
	uint32_t words;
	uint32_t unused;
};

bool boot_layout_valid(void);
bool boot_vectors_valid(uint32_t start, uint32_t end);
bool boot_factory_valid(const void *image);
bool boot_metadata_valid(const struct boot_metadata *metadata);
bool boot_factory_backup_valid(const void *image, const struct boot_metadata *metadata);
bool boot_backup_valid(void);
bool boot_equal(const void *a, const void *b, size_t size);
bool boot_flash_erase(uint32_t address);
bool boot_flash_write(uint32_t address, const void *data, size_t size);
uint32_t boot_mbr_call(struct boot_mbr_command *command);
uint32_t boot_sd_disable(void);
uint32_t boot_sd_vectors(uint32_t address);
void boot_jump(uint32_t address) __attribute__((noreturn));
void boot_reset(void) __attribute__((noreturn));
uint32_t boot_copy(uint32_t source);
void boot_mbr_prepare(void);
void boot_force_protect(void);
void boot_marker_clear(void);
void boot_marker_set(uint32_t stage);
#endif
