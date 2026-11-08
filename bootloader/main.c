/* SPDX-License-Identifier: MIT */
#include "boot.h"

static void reset_window(void)
{
	/* TIMER2: 16 MHz / 2^4 = 1 MHz, one 750 ms reset gesture window. */
	BOOT_REG(0x4000a004) = 1; /* STOP */
	BOOT_REG(0x4000a504) = 0; /* MODE: timer */
	BOOT_REG(0x4000a508) = 3; /* BITMODE: 32-bit */
	BOOT_REG(0x4000a510) = 4;
	BOOT_REG(0x4000a00c) = 1; /* CLEAR */
	BOOT_REG(0x4000a140) = 0; /* EVENTS_COMPARE[0] */
	BOOT_REG(0x4000a540) = 750000;
	BOOT_REG(0x4000a000) = 1; /* START */
	while (!BOOT_REG(0x4000a140)) {
	}
	BOOT_REG(0x4000a004) = 1; /* STOP */
	/* Release TIMER2's power resources before application handoff. */
	BOOT_REG(0x4000a010) = 1; /* SHUTDOWN: STOP alone can retain current. */
	boot_marker_clear();
}

void boot_main(uint32_t reason, uint32_t stage)
{
	BOOT_RESETREAS = reason; /* W1C: do not let old RESETPIN qualify SW reset. */
	BOOT_GPREGRET2 = 0;
	boot_marker_clear();
#if !TOTP_BOOT_UNLOCKED
	boot_force_protect();
#endif
	if (!boot_layout_valid()) {
		for (;;) {
			__asm volatile("wfe");
		}
	}
	bool recover = false;

	if (reason == 1) {
		if (stage == BOOT_STAGE_TWO) {
			recover = true;
		} else {
			boot_marker_set(stage == BOOT_STAGE_ONE ?
					BOOT_STAGE_TWO : BOOT_STAGE_ONE);
			reset_window();
		}
	}
	if (recover) {
		/* Validate everything before deleting any persistent state. */
		if (boot_backup_valid()) {
			bool erased = true;

			for (uint32_t page = BOOT_STORAGE; page < BOOT_START;
			     page += BOOT_PAGE) {
				erased = boot_flash_erase(page) && erased;
			}
			if (erased) {
				BOOT_GPREGRET = 0x57;
				boot_mbr_prepare();
				boot_copy(BOOT_BACKUP); /* Success resets; never returns. */
			}
		}
		BOOT_GPREGRET2 = BOOT_RECOVERY_FAILED;
	}
	if (!boot_vectors_valid(BOOT_APP, BOOT_APP_END)) {
		for (;;) {
			__asm volatile("wfe");
		}
	}
	boot_mbr_prepare();
	struct boot_mbr_command init = { 2, 0, 0, 0 };

	if (boot_mbr_call(&init) || boot_sd_disable() || boot_sd_vectors(BOOT_APP)) {
		for (;;) {
			__asm volatile("wfe");
		}
	}
	boot_jump(BOOT_APP);
}
