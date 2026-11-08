/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <zephyr/kernel.h>

#include "boot_security.h"
#include "boot.h"
#include "sha256.h"

extern const uint8_t totp_boot_payload[BOOT_SIZE];
extern const uint8_t totp_boot_digest[32];

/* Bench observability: 1 layout, 2 backup, 3 MBR copy, 4 active verify,
 * 5 persistent lock, 6 ready. Failures are negative. No secret values. */
volatile int dbg_boot_stage;
#if CONFIG_TOTP_UF2_BENCH
/* Transaction checkpoints for destructive SWD fault injection, bench only. */
volatile int dbg_boot_backup_page;
#define BACKUP_CHECKPOINT(step) (dbg_boot_backup_page = (step))
#else
#define BACKUP_CHECKPOINT(step) do { } while (0)
#endif

static bool capture_factory(void)
{
	struct boot_metadata metadata = {
		.magic = BOOT_META_MAGIC, .version = 1,
		.source = BOOT_START, .length = BOOT_SIZE,
		.valid = UINT32_MAX,
	};
	unsigned int key = irq_lock();
	bool good = boot_flash_erase(BOOT_METADATA);

	BACKUP_CHECKPOINT(0);

	/* Invalidate the old metadata before touching any backup payload page. */
	for (uint32_t offset = 0; good && offset < BOOT_SIZE; offset += BOOT_PAGE) {
		good = boot_flash_erase(BOOT_BACKUP + offset) &&
			boot_flash_write(BOOT_BACKUP + offset,
				(const void *)(BOOT_START + offset), BOOT_PAGE);
		BACKUP_CHECKPOINT(1 + offset / BOOT_PAGE);
	}
	irq_unlock(key);
	if (!good) {
		return false;
	}
	boot_sha256((const void *)BOOT_START, BOOT_SIZE, metadata.sha256);
	/* Check copied bytes against the active image before committing metadata. */
	uint8_t copied_digest[32];

	boot_sha256((const void *)BOOT_BACKUP, BOOT_SIZE, copied_digest);
	if (!boot_factory_valid((const void *)BOOT_BACKUP) ||
	    !boot_equal(copied_digest, metadata.sha256, sizeof(copied_digest))) {
		return false;
	}
	key = irq_lock();
	good = boot_flash_write(BOOT_METADATA, &metadata,
				offsetof(struct boot_metadata, valid));
	BACKUP_CHECKPOINT(11);
	if (good) {
		uint32_t valid = BOOT_META_VALID;

		good = boot_flash_write(BOOT_METADATA +
				offsetof(struct boot_metadata, valid), &valid, sizeof(valid));
		BACKUP_CHECKPOINT(12);
	}
	irq_unlock(key);
	return good && boot_backup_valid();
}

static uint32_t install(void)
{
	/* COPY_BL is entered through the real MBR SVC vector, before the radio or
	 * USB starts. Restore Zephyr's interrupt state if MBR rejects the request. */
	unsigned int key = irq_lock();
	uint32_t vtor = BOOT_REG(0xe000ed08);
	uint32_t forwarding = BOOT_REG(0x20000000);
	uint32_t enabled[2] = { BOOT_REG(0xe000e100), BOOT_REG(0xe000e104) };
	uint32_t systick = BOOT_REG(0xe000e010);

	boot_mbr_prepare();
	uint32_t result = boot_copy((uint32_t)(uintptr_t)totp_boot_payload);

	__asm volatile("cpsid i" ::: "memory");
	BOOT_REG(0xe000ed08) = vtor;
	BOOT_REG(0x20000000) = forwarding;
	BOOT_REG(0xe000e100) = enabled[0];
	BOOT_REG(0xe000e104) = enabled[1];
	BOOT_REG(0xe000e010) = systick;
	__asm volatile("dsb\n isb" ::: "memory");
	irq_unlock(key);
	return result;
}

const char *boot_security_init(void)
{
	uint8_t digest[32];

	dbg_boot_stage = 1;
	BACKUP_CHECKPOINT(-1);
	if (!boot_layout_valid()) {
		dbg_boot_stage = -1;
		return "Unsupported flash/MBR/S140 layout. No secrets accepted.\n";
	}
	if (BOOT_GPREGRET2 == BOOT_RECOVERY_FAILED) {
		BOOT_GPREGRET2 = 0;
		dbg_boot_stage = -2;
		return "Recovery backup is invalid or restore failed. No secrets accepted.\n";
	}
	boot_sha256(totp_boot_payload, BOOT_SIZE, digest);
	if (!boot_equal(digest, totp_boot_digest, sizeof(digest)) ||
	    ((uintptr_t)totp_boot_payload & (BOOT_PAGE - 1))) {
		dbg_boot_stage = -3;
		return "Embedded security bootloader is invalid. No secrets accepted.\n";
	}
	boot_sha256((const void *)BOOT_START, BOOT_SIZE, digest);
	bool installed = boot_equal(digest, totp_boot_digest, sizeof(digest));

	dbg_boot_stage = 2;
	if (!installed && !boot_factory_valid((const void *)BOOT_START)) {
		dbg_boot_stage = -4;
		return "Active bootloader is not a compatible factory image. No secrets accepted.\n";
	}
	bool backup_valid = boot_backup_valid();
	bool capture_needed = !backup_valid ||
		!boot_equal(digest, ((const struct boot_metadata *)BOOT_METADATA)->sha256,
			    sizeof(digest));

	/* If a user updated the factory loader, capture that current image. An
	 * already-installed wiping loader may only use its existing valid backup. */
	if ((installed && !backup_valid) ||
	    (!installed && capture_needed && !capture_factory())) {
		dbg_boot_stage = -5;
		return "Factory backup is invalid. No secrets accepted.\n";
	}
	if (!installed) {
		dbg_boot_stage = 3;
		install(); /* Success restarts through the new bootloader. */
		dbg_boot_stage = -6;
		return "MBR rejected security bootloader installation. No secrets accepted.\n";
	}
	dbg_boot_stage = 4;
#if !CONFIG_TOTP_UF2_BENCH
	dbg_boot_stage = 5;
	if ((BOOT_APPROTECT & 0xffu) != 0) {
		uint32_t locked = BOOT_APPROTECT & ~0xffu;
		unsigned int key = irq_lock();
		bool good = boot_flash_write(0x10001208, &locked, sizeof(locked));

		irq_unlock(key);
		if (!good || (BOOT_APPROTECT & 0xffu) != 0) {
			dbg_boot_stage = -7;
			return "Persistent debug protection failed. No secrets accepted.\n";
		}
		boot_reset(); /* Confirm persistent protection on a fresh boot. */
	}
	boot_force_protect();
#endif
	dbg_boot_stage = 6;
	return NULL;
}
