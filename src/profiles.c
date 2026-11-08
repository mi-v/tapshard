/* RAM-only TOTP profiles keyed by Zephyr bond identity addresses. */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/sys/util.h>

#include "profiles.h"
#include "secure_zero.h"

static struct secret_profile bonded_profiles[CONFIG_TOTP_MAX_BONDS];
static struct secret_profile limbo_profile;
static struct secret_profile *current_profile = &limbo_profile;

static bool profile_empty(const struct secret_profile *profile)
{
	return bt_addr_le_eq(&profile->addr, BT_ADDR_LE_ANY);
}

static void profile_clear(struct secret_profile *profile)
{
	secure_zero(profile, sizeof(*profile));
}

void profiles_clear_contents(struct secret_profile *profile)
{
	bt_addr_le_t addr;
	uint32_t next_revision;

	if (!profile) {
		return;
	}
	bt_addr_le_copy(&addr, &profile->addr);
	next_revision = profile->content_revision + 1U;
	profile_clear(profile);
	bt_addr_le_copy(&profile->addr, &addr);
	profile->content_revision = next_revision;
	secure_zero(&addr, sizeof(addr));
}

void profiles_init(void)
{
	secure_zero(bonded_profiles, sizeof(bonded_profiles));
	profile_clear(&limbo_profile);
	current_profile = &limbo_profile;
}

struct secret_profile *profiles_find(const bt_addr_le_t *addr)
{
	int i;

	if (!addr || bt_addr_le_eq(addr, BT_ADDR_LE_ANY)) {
		return NULL;
	}
	for (i = 0; i < ARRAY_SIZE(bonded_profiles); i++) {
		if (!profile_empty(&bonded_profiles[i]) &&
		    bt_addr_le_eq(&bonded_profiles[i].addr, addr)) {
			return &bonded_profiles[i];
		}
	}
	return NULL;
}

int profiles_add(const bt_addr_le_t *addr)
{
	struct secret_profile *profile;
	int i;

	if (!addr || bt_addr_le_eq(addr, BT_ADDR_LE_ANY)) {
		return -EINVAL;
	}
	profile = profiles_find(addr);
	if (profile) {
		return 0;
	}
	for (i = 0; i < ARRAY_SIZE(bonded_profiles); i++) {
		if (profile_empty(&bonded_profiles[i])) {
			profile_clear(&bonded_profiles[i]);
			bt_addr_le_copy(&bonded_profiles[i].addr, addr);
			return 0;
		}
	}
	return -ENOSPC;
}

void profiles_remove(const bt_addr_le_t *addr)
{
	struct secret_profile *profile = profiles_find(addr);

	if (!profile) {
		return;
	}
	if (current_profile == profile) {
		current_profile = &limbo_profile;
	}
	profile_clear(profile);
}

int profiles_set_current(const bt_addr_le_t *addr)
{
	struct secret_profile *profile = profiles_find(addr);

	if (!profile) {
		return -ENOENT;
	}
	current_profile = profile;
	return 0;
}

struct secret_profile *profiles_current(void)
{
	return current_profile;
}

bool profiles_current_addr(bt_addr_le_t *addr)
{
	if (current_profile == &limbo_profile || profile_empty(current_profile)) {
		return false;
	}
	if (addr) {
		bt_addr_le_copy(addr, &current_profile->addr);
	}
	return true;
}

struct secret_profile *profiles_limbo(void)
{
	return &limbo_profile;
}

int profiles_attach_limbo(const bt_addr_le_t *addr)
{
	struct secret_profile *profile;
	bt_addr_le_t saved_addr;
	int err;

	err = profiles_add(addr);
	if (err != 0) {
		return err;
	}
	profile = profiles_find(addr);
	if (!profile) {
		return -ENOENT;
	}

	/* A newly paired identity gets the pre-pairing configuration. Never
	 * overwrite an already populated profile during a re-pair. */
	if (limbo_profile.have_config || limbo_profile.config_error ||
	    limbo_profile.slot_count > 0) {
		if (profile->have_config || profile->config_error ||
		    profile->slot_count > 0) {
			/* Re-pairing an existing identity must not overwrite either
			 * profile. Leave limbo available for a genuinely new host. */
			current_profile = profile;
			return 0;
		}
		bt_addr_le_copy(&saved_addr, &profile->addr);
		memcpy(profile, &limbo_profile, sizeof(*profile));
		bt_addr_le_copy(&profile->addr, &saved_addr);
		profile_clear(&limbo_profile);
	}
	current_profile = profile;
	return 0;
}

int profiles_count(void)
{
	int count = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(bonded_profiles); i++) {
		if (!profile_empty(&bonded_profiles[i])) {
			count++;
		}
	}
	return count;
}

struct secret_profile *profiles_only(void)
{
	struct secret_profile *only = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(bonded_profiles); i++) {
		if (profile_empty(&bonded_profiles[i])) {
			continue;
		}
		if (only) {
			return NULL;
		}
		only = &bonded_profiles[i];
	}
	return only;
}

struct secret_profile *profiles_at(int index)
{
	int seen = 0;
	int i;

	if (index < 0) {
		return NULL;
	}
	for (i = 0; i < ARRAY_SIZE(bonded_profiles); i++) {
		if (profile_empty(&bonded_profiles[i])) {
			continue;
		}
		if (seen++ == index) {
			return &bonded_profiles[i];
		}
	}
	return NULL;
}

struct bond_snapshot {
	bt_addr_le_t addrs[CONFIG_BT_MAX_PAIRED];
	int count;
};

static void capture_bond(const struct bt_bond_info *info, void *user_data)
{
	struct bond_snapshot *snapshot = user_data;

	if (snapshot->count >= ARRAY_SIZE(snapshot->addrs)) {
		return;
	}
	bt_addr_le_copy(&snapshot->addrs[snapshot->count++], &info->addr);
}

static bool snapshot_has(const struct bond_snapshot *snapshot,
			 const bt_addr_le_t *addr)
{
	int i;

	for (i = 0; i < snapshot->count; i++) {
		if (bt_addr_le_eq(&snapshot->addrs[i], addr)) {
			return true;
		}
	}
	return false;
}

bool profiles_is_bonded(const bt_addr_le_t *addr)
{
	struct bond_snapshot snapshot = { 0 };
	bool found;

	if (!addr) {
		return false;
	}
	bt_foreach_bond(BT_ID_DEFAULT, capture_bond, &snapshot);
	found = snapshot_has(&snapshot, addr);
	secure_zero(&snapshot, sizeof(snapshot));
	return found;
}

int profiles_sync_bonds(void)
{
	struct bond_snapshot snapshot = { 0 };
	int i;
	int err;

	bt_foreach_bond(BT_ID_DEFAULT, capture_bond, &snapshot);
	for (i = 0; i < ARRAY_SIZE(bonded_profiles); i++) {
		if (!profile_empty(&bonded_profiles[i]) &&
		    !snapshot_has(&snapshot, &bonded_profiles[i].addr)) {
			profiles_remove(&bonded_profiles[i].addr);
		}
	}
	for (i = 0; i < snapshot.count; i++) {
		err = profiles_add(&snapshot.addrs[i]);
		if (err != 0) {
			secure_zero(&snapshot, sizeof(snapshot));
			return err;
		}
	}
	secure_zero(&snapshot, sizeof(snapshot));
	return 0;
}

void profiles_record_use(struct secret_profile *profile, int slot_idx)
{
	int i;

	if (!profile || slot_idx < 0 || slot_idx >= profile->slot_count) {
		return;
	}

	/* 15/16 event-based decay has a half-life of about 11 successful sends
	 * on this host. Round the decay up so small stale scores reach zero. */
	for (i = 0; i < profile->slot_count; i++) {
		uint16_t score = profile->usage_score[i];

		profile->usage_score[i] = score - ((score + 15U) / 16U);
	}
	if (profile->usage_score[slot_idx] > UINT16_MAX - 4096U) {
		profile->usage_score[slot_idx] = UINT16_MAX;
	} else {
		profile->usage_score[slot_idx] += 4096U;
	}
}

int profiles_selftest(void)
{
	bt_addr_le_t addrs[CONFIG_TOTP_MAX_BONDS + 1];
	struct secret_profile zero_profile = { 0 };
	struct secret_profile *removed;
	int remove_idx = CONFIG_TOTP_MAX_BONDS / 2;
	int i;
	int err = -1;

	profiles_init();
	secure_zero(addrs, sizeof(addrs));
	for (i = 0; i < ARRAY_SIZE(addrs); i++) {
		addrs[i].type = (i & 1) ? BT_ADDR_LE_RANDOM : BT_ADDR_LE_PUBLIC;
		addrs[i].a.val[0] = (uint8_t)(i + 1);
		addrs[i].a.val[5] = (i & 1) ? 0xc0 : 0x00;
	}
	for (i = 0; i < CONFIG_TOTP_MAX_BONDS; i++) {
		if (profiles_add(&addrs[i]) != 0) {
			goto out;
		}
		profiles_find(&addrs[i])->have_config = true;
		profiles_find(&addrs[i])->slot_count = 1;
		profiles_find(&addrs[i])->slots[0].secret[0] =
			(uint8_t)(0xa0 + i);
		profiles_find(&addrs[i])->slots[0].secret_len = 1;
	}
	if (profiles_count() != CONFIG_TOTP_MAX_BONDS ||
	    profiles_add(&addrs[CONFIG_TOTP_MAX_BONDS]) != -ENOSPC) {
		goto out;
	}

	removed = profiles_find(&addrs[remove_idx]);
	if (!removed || profiles_set_current(&addrs[remove_idx]) != 0) {
		goto out;
	}
	profiles_remove(&addrs[remove_idx]);
	if (!profile_empty(removed) ||
	    memcmp(removed, &zero_profile, sizeof(*removed)) != 0 ||
	    profiles_current() != profiles_limbo() ||
	    profiles_count() != CONFIG_TOTP_MAX_BONDS - 1) {
		goto out;
	}
	if (profiles_add(&addrs[CONFIG_TOTP_MAX_BONDS]) != 0 ||
	    profiles_find(&addrs[CONFIG_TOTP_MAX_BONDS]) != removed ||
	    removed->have_config || removed->slot_count != 0 ||
	    profiles_count() != CONFIG_TOTP_MAX_BONDS) {
		goto out;
	}

	/* The limbo profile transfers only to an empty newly bonded profile. */
	profiles_init();
	profiles_limbo()->have_config = true;
	profiles_limbo()->slot_count = 1;
	profiles_limbo()->slots[0].secret[0] = 0xa5;
	profiles_limbo()->slots[0].secret_len = 1;
	if (profiles_attach_limbo(&addrs[0]) != 0 || profiles_count() != 1 ||
	    profiles_current() != profiles_find(&addrs[0]) ||
	    profiles_current()->slots[0].secret[0] != 0xa5 ||
	    profiles_limbo()->have_config) {
		goto out;
	}
	profiles_record_use(profiles_current(), 0);
	profiles_record_use(profiles_current(), 0);
	if (profiles_current()->usage_score[0] != 7936U) {
		goto out;
	}
	profiles_remove(&addrs[0]);
	if (profiles_count() != 0 || profiles_current() != profiles_limbo()) {
		goto out;
	}
	err = 0;
out:
	profiles_init();
	secure_zero(addrs, sizeof(addrs));
	secure_zero(&zero_profile, sizeof(zero_profile));
	return err;
}
