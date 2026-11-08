/* RAM-only TOTP profiles keyed by bonded BLE peer identity. */

#ifndef PROFILES_H
#define PROFILES_H

#include <stdbool.h>

#include <zephyr/bluetooth/addr.h>

#include "config.h"

struct secret_profile {
	bt_addr_le_t addr;
	struct totp_slot slots[CONFIG_MAX_SLOTS];
	uint16_t usage_score[CONFIG_MAX_SLOTS];
	uint32_t content_revision;
	int slot_count;
	bool have_config;
	bool config_error;
};

/* Clear every RAM profile and select the unbonded limbo profile. */
void profiles_init(void);

/* Reconcile RAM address records with Zephyr's loaded bond database while
 * preserving profiles whose identities still exist. */
int profiles_sync_bonds(void);
bool profiles_is_bonded(const bt_addr_le_t *addr);

/* Add/remove a bond identity record. Removal securely clears its profile. */
int profiles_add(const bt_addr_le_t *addr);
void profiles_remove(const bt_addr_le_t *addr);
void profiles_clear_contents(struct secret_profile *profile);

/* Find/select a bonded profile by identity. */
struct secret_profile *profiles_find(const bt_addr_le_t *addr);
int profiles_set_current(const bt_addr_le_t *addr);
struct secret_profile *profiles_current(void);
bool profiles_current_addr(bt_addr_le_t *addr);

/* The limbo profile holds configuration made before the first bond. */
struct secret_profile *profiles_limbo(void);
int profiles_attach_limbo(const bt_addr_le_t *addr);

/* Bonded-profile enumeration helpers. */
int profiles_count(void);
struct secret_profile *profiles_only(void);
struct secret_profile *profiles_at(int index);

/* Record one completely successful OTP send for ranking dump labels. */
void profiles_record_use(struct secret_profile *profile, int slot_idx);

/* Pure RAM/address helper self-test. */
int profiles_selftest(void);

#endif /* PROFILES_H */
