/* SPDX-License-Identifier: MIT
 * Fixed nRF52840/S140-v6 layout and Nordic MBR register/SVC interface.
 * No Nordic SDK implementation or factory firmware is incorporated here.
 */
#include "boot.h"
#include "sha256.h"

static uint32_t effective(uint32_t mbr, uint32_t uicr)
{
	uint32_t value;

	/* Addresses 0xff8/0xffc are real flash, below GCC's assumed null page. */
	__asm volatile("ldr %0,[%1]" : "=r"(value) : "r"(mbr) : "memory");
	return value == UINT32_MAX ? BOOT_REG(uicr) : value;
}

bool boot_layout_valid(void)
{
	return BOOT_REG(0x10000100) == 0x52840 &&
		BOOT_REG(0x10000010) == BOOT_PAGE &&
		BOOT_REG(0x10000014) == 256 &&
		BOOT_REG(0x3004) == 0x51b1e5db &&
		BOOT_REG(0x3008) == BOOT_APP &&
		BOOT_REG(0x3014) / 1000000 == 6 &&
		effective(0xff8, 0x10001014) == BOOT_START &&
		effective(0xffc, 0x10001018) == BOOT_PARAMS;
}

bool boot_vectors_valid(uint32_t start, uint32_t end)
{
	uint32_t stack = BOOT_REG(start), reset = BOOT_REG(start + 4);

	return stack > BOOT_RAM_START && stack <= BOOT_APP_RAM_END && !(stack & 7) &&
		(reset & 1) && (reset & ~1u) >= start && (reset & ~1u) < end;
}

bool boot_backup_valid(void)
{
	return boot_factory_backup_valid((const void *)BOOT_BACKUP,
					(const struct boot_metadata *)BOOT_METADATA);
}

static void nvmc_mode(uint32_t mode)
{
	BOOT_REG(0x4001e504) = mode;
	while (!BOOT_REG(0x4001e400)) {
	}
	__asm volatile("dsb\n isb" ::: "memory");
}

bool boot_flash_erase(uint32_t address)
{
	/* Never directly erase application, active bootloader, MBR or UICR. */
	if (address < BOOT_BACKUP || address >= BOOT_START ||
	    (address & (BOOT_PAGE - 1))) {
		return false;
	}
	nvmc_mode(2);
	BOOT_REG(0x4001e508) = address;
	while (!BOOT_REG(0x4001e400)) {
	}
	nvmc_mode(0);
	for (uint32_t i = 0; i < BOOT_PAGE; i += 4) {
		if (BOOT_REG(address + i) != UINT32_MAX) {
			return false;
		}
	}
	return true;
}

bool boot_flash_write(uint32_t address, const void *data, size_t size)
{
	if ((address & 3) || (size & 3) || !size || size > BOOT_SIZE ||
	    !((address >= BOOT_BACKUP && address <= BOOT_METADATA + BOOT_PAGE - size) ||
	      (address == 0x10001208 && size == 4))) {
		return false;
	}
	const uint8_t *source = data;

	nvmc_mode(1);
	for (size_t i = 0; i < size; i += 4) {
		uint32_t word = (uint32_t)source[i] | (uint32_t)source[i + 1] << 8 |
			(uint32_t)source[i + 2] << 16 | (uint32_t)source[i + 3] << 24;

		BOOT_REG(address + i) = word;
		while (!BOOT_REG(0x4001e400)) {
		}
	}
	nvmc_mode(0);
	return boot_equal((const void *)(uintptr_t)address, data, size);
}

void boot_reset(void)
{
	__asm volatile("dsb" ::: "memory");
	BOOT_REG(0xe000ed0c) = 0x05fa0004;
	for (;;) {
		__asm volatile("nop");
	}
}

void boot_force_protect(void)
{
	/* nRF52840 configuration 249: the software protection branch exists on
	 * silicon revision 5 and later. Older revisions use persistent UICR only. */
	if (BOOT_REG(0x10000130) == 8 && BOOT_REG(0x10000134) >= 5) {
		BOOT_REG(0x40000550) = 0;
	}
}

void boot_marker_clear(void)
{
	/* Invalidate first; a reset during any subsequent store fails validation. */
	BOOT_REG(BOOT_MARKER_ADDR) = 0;
	BOOT_REG(BOOT_MARKER_ADDR + 4) = 0;
	BOOT_REG(BOOT_MARKER_ADDR + 8) = 0;
	BOOT_REG(BOOT_MARKER_ADDR + 12) = 0;
	__asm volatile("dsb" ::: "memory");
}

void boot_marker_set(uint32_t stage)
{
	boot_marker_clear();
	if (stage != BOOT_STAGE_ONE && stage != BOOT_STAGE_TWO) {
		return;
	}
	BOOT_REG(BOOT_MARKER_ADDR + 4) = stage;
	BOOT_REG(BOOT_MARKER_ADDR + 8) = ~stage;
	BOOT_REG(BOOT_MARKER_ADDR + 12) = ~((uint32_t)BOOT_MARKER_MAGIC);
	__asm volatile("dsb" ::: "memory");
	BOOT_REG(BOOT_MARKER_ADDR) = BOOT_MARKER_MAGIC; /* Commit last. */
	__asm volatile("dsb" ::: "memory");
}

void boot_mbr_prepare(void)
{
	__asm volatile("cpsid i" ::: "memory");
	BOOT_REG(0xe000e010) = 0; /* SysTick */
	BOOT_REG(0xe000e180) = UINT32_MAX;
	BOOT_REG(0xe000e184) = UINT32_MAX;
	BOOT_REG(0xe000e280) = UINT32_MAX;
	BOOT_REG(0xe000e284) = UINT32_MAX;
	BOOT_REG(0xe000ed04) = (1u << 27) | (1u << 25); /* PendSV/SysTick */
	BOOT_REG(0x20000000) = BOOT_START; /* MBR IRQ forwarding after SRAM wipe */
	BOOT_REG(0xe000ed08) = 0; /* MBR owns SVC, including COPY_BL. */
	__asm volatile("dsb\n isb\n cpsie i" ::: "memory");
}

uint32_t boot_copy(uint32_t source)
{
	if ((source & (BOOT_PAGE - 1)) ||
	    !((source >= BOOT_APP && source <= BOOT_APP_END - BOOT_SIZE) ||
	      source == BOOT_BACKUP)) {
		return UINT32_MAX;
	}
	struct boot_mbr_command command = { 0, source, BOOT_SIZE / 4, 0 };

	return boot_mbr_call(&command);
}
