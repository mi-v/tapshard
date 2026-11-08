/* Config parsing — public API */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CONFIG_MAX_SLOTS 16
#define CONFIG_NAME_MAX  32

/* Device-wide settings. */
struct totp_device {
	int32_t timezone_min;   /* signed minutes ("timezone") */
	uint32_t settle_ms;     /* host-ready settle after subscribe */
	uint32_t inter_key_ms;  /* per-key press/release delay */
	int32_t drift_ppm;      /* LFCLK drift calibration ("drift_ppm") */
	uint32_t long_press_ms; /* hold time for pairing ("long_press_ms") */
	uint32_t click_window_ms; /* multi-click window ("click_window_ms") */
	uint32_t grace_ms;      /* stay-connected grace after typing ("grace_ms") */
	uint32_t low_battery_mv; /* low-battery threshold ("low_battery_mv") */
};

/* One TOTP slot (base32 secret decoded to bytes). */
struct totp_slot {
	char name[CONFIG_NAME_MAX];
	uint8_t secret[64];
	size_t secret_len;
	uint8_t digits;
	uint32_t period;
	bool enter;             /* press Enter after typing the code */
	uint8_t secret_ref;     /* 1-based "keep:N" dump-snapshot reference */
	bool secret_error;      /* secret unresolved/broken: report, no code */
};

/* Parsed configuration. */
struct totp_config {
	struct totp_device device;
	uint64_t bond_snapshot;
	uint8_t host_id;        /* 1-based dump-local bond_N target */
	uint8_t keep_bonds;     /* bit N-1 retains dump-local bond_N */
	bool have_bond_snapshot;
	bool have_host_id;
	bool edit_bonds;
	bool have_edit_bonds;
	struct totp_slot slots[CONFIG_MAX_SLOTS];
	int slot_count;
};

/* Fill device-wide settings with the parser defaults. */
void config_device_defaults(struct totp_device *device);

/* Parse the config text into @cfg. Returns 0 on success, negative on error.
 * On failure, a human-readable reason is written to config_err_msg. */
int config_parse(const char *buf, size_t len, struct totp_config *cfg);

/* Reason for the last config_parse() failure ("" if none). */
extern char config_err_msg[80];

/* Parse a duration ("600ms", "2s", or a bare integer as ms) into milliseconds.
 * Returns 0 on success, negative on error. */
int config_parse_duration(const char *s, uint32_t *ms);

/* Parse a boolean ("true/false/yes/no/1/0/on/off"). Returns 0 on success. */
int config_parse_bool(const char *s, bool *value);

/* Parse a partial utc_offset string ("+3", "+3:30", "-5", "-4:45")
 * into signed minutes. Returns 0 on success, negative on error. */
int config_parse_utc_offset(const char *s, int32_t *minutes);

/* Self-test. Returns 0 on success. */
int config_selftest(void);

#endif /* CONFIG_H */
