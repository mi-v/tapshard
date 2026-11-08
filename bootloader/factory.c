/* SPDX-License-Identifier: MIT
 * Validate a captured factory image by layout and its recorded digest, not by
 * release version. The already-installed factory bootloader is trusted code;
 * its digest detects a damaged backup, not the publisher's authenticity.
 */
#include "boot.h"
#include "sha256.h"

bool boot_equal(const void *a, const void *b, size_t size)
{
	const uint8_t *x = a, *y = b;

	for (size_t i = 0; i < size; i++) {
		if (x[i] != y[i]) {
			return false;
		}
	}
	return true;
}

static uint32_t word(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
		(uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

bool boot_factory_valid(const void *image)
{
	const uint8_t *bytes = image;
	uint32_t stack = word(bytes), reset = word(bytes + 4);

	/* Factory stacks may use all RAM; only our application reserves the marker.
	 * Reject our loader's identity regardless of its ABI version or build hash:
	 * a lost backup must never be recreated from the wiping loader itself. */
	return stack > BOOT_RAM_START && stack <= BOOT_RAM_END && !(stack & 7) &&
		(reset & 1) && (reset & ~1u) >= BOOT_START &&
		(reset & ~1u) < BOOT_START + BOOT_SIZE &&
		word(bytes + 0x100) != BOOT_ID_MAGIC;
}

bool boot_metadata_valid(const struct boot_metadata *metadata)
{
	return metadata->magic == BOOT_META_MAGIC && metadata->version == 1 &&
		metadata->source == BOOT_START && metadata->length == BOOT_SIZE &&
		metadata->valid == BOOT_META_VALID;
}

bool boot_factory_backup_valid(const void *image, const struct boot_metadata *metadata)
{
	uint8_t digest[32];

	if (!boot_metadata_valid(metadata) || !boot_factory_valid(image)) {
		return false;
	}
	boot_sha256(image, BOOT_SIZE, digest);
	return boot_equal(digest, metadata->sha256, sizeof(digest));
}
