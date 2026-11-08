/*
 * Config parser (INI-style).
 *
 * Format:
 *
 *   # device-wide settings (all optional)
 *   [device]
 *   timezone = +7
 *   conn_settle_time = 600ms
 *   inter_key_time = 80ms
 *
 *   # one or more TOTP accounts; "name" is an optional label
 *   [account]
 *   name = github
 *   secret = JBSWY3DPEHPK3PXP
 *   digits = 6
 *   period = 30
 *   enter = false
 *
 * Key names are case-insensitive; '#' and ';' start comments (full-line or
 * trailing). Missing keys fall back to sensible defaults. A [account] without
 * a valid secret is a parse error.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <zephyr/sys/util.h>

#include "config.h"
#include "totp.h"
#include "secure_zero.h"

#define DEFAULT_SETTLE_MS    200
#define DEFAULT_INTER_KEY_MS 60
#define DEFAULT_DIGITS       6
#define DEFAULT_PERIOD       30
#define DEFAULT_LONG_PRESS_MS    3000
#define DEFAULT_CLICK_WINDOW_MS  500
#define DEFAULT_GRACE_MS         2000
#define DEFAULT_LOW_BATTERY_MV   3500

#define MAX_SETTLE_MS            60000U
#define MIN_INTER_KEY_MS         1U
#define MAX_INTER_KEY_MS         5000U
#define MIN_LONG_PRESS_MS        250U
#define MAX_LONG_PRESS_MS        60000U
#define MIN_CLICK_WINDOW_MS      50U
#define MAX_CLICK_WINDOW_MS      5000U
#define MAX_GRACE_MS             300000U
#define MIN_LOW_BATTERY_MV       1000U
#define MAX_LOW_BATTERY_MV       5500U
#define MAX_PERIOD_S             86400U
#define MAX_DRIFT_PPM            500

/* Max value length (a base32 secret for a 64-byte key is ~103 chars). */
#define CONFIG_VALUE_MAX     128

/* Reason for the last config_parse() failure ("" if none). */
char config_err_msg[80];

static int parse_err(const char *msg)
{
	strncpy(config_err_msg, msg, sizeof(config_err_msg) - 1);
	config_err_msg[sizeof(config_err_msg) - 1] = '\0';
	return -1;
}

static int parse_err_key(const char *section, const char *key)
{
	snprintf(config_err_msg, sizeof(config_err_msg),
		 "unknown [%s] key '%s'", section, key);
	return -1;
}

/* Parse one or more decimal digits and stop at the first non-digit. */
static int parse_uint_prefix(const char **cursor, uint32_t *out)
{
	const char *p = *cursor;
	uint32_t v = 0;

	if (!isdigit((unsigned char)*p)) {
		return -1;
	}
	while (isdigit((unsigned char)*p)) {
		uint32_t digit = (uint32_t)(*p - '0');

		if (v > (UINT32_MAX - digit) / 10U) {
			return -1;
		}
		v = v * 10U + digit;
		p++;
	}
	*cursor = p;
	*out = v;
	return 0;
}

int config_parse_utc_offset(const char *s, int32_t *minutes)
{
	const char *p = s;
	int sign = 1;
	uint32_t hours;
	uint32_t mins = 0;

	if (*p == '+') {
		sign = 1;
		p++;
	} else if (*p == '-') {
		sign = -1;
		p++;
	}

	if (parse_uint_prefix(&p, &hours) != 0) {
		return -1;
	}

	if (*p == ':') {
		p++;
		if (parse_uint_prefix(&p, &mins) != 0) {
			return -1;
		}
	}

	if (*p != '\0' || mins >= 60 || hours > 24 ||
	    (hours == 24 && mins != 0)) {
		return -1;
	}

	*minutes = sign * (int32_t)(hours * 60U + mins);
	return 0;
}

int config_parse_duration(const char *s, uint32_t *ms)
{
	const char *p = s;
	uint32_t v;

	if (parse_uint_prefix(&p, &v) != 0) {
		return -1;
	}

	if (*p == '\0' || strcmp(p, "ms") == 0) {
		*ms = v;
		return 0;
	}
	if (strcmp(p, "s") == 0) {
		if (v > UINT32_MAX / 1000U) {
			return -1;
		}
		*ms = v * 1000U;
		return 0;
	}
	return -1;
}

int config_parse_bool(const char *s, bool *value)
{
	if (strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 ||
	    strcmp(s, "on") == 0 || strcmp(s, "1") == 0) {
		*value = true;
		return 0;
	}
	if (strcmp(s, "false") == 0 || strcmp(s, "no") == 0 ||
	    strcmp(s, "off") == 0 || strcmp(s, "0") == 0) {
		*value = false;
		return 0;
	}
	return -1;
}

static int parse_uint(const char *s, uint32_t *out)
{
	const char *p = s;
	uint32_t v;

	if (parse_uint_prefix(&p, &v) != 0 || *p != '\0') {
		return -1;
	}
	*out = v;
	return 0;
}

static int parse_int(const char *s, int32_t *out)
{
	const char *p = s;
	int sign = 1;
	uint32_t v = 0;

	if (*p == '-') {
		sign = -1;
		p++;
	} else if (*p == '+') {
		p++;
	}
	if (parse_uint(p, &v) != 0) {
		return -1;
	}
	if ((sign > 0 && v > INT32_MAX) ||
	    (sign < 0 && v > (uint32_t)INT32_MAX + 1U)) {
		return -1;
	}
	if (sign < 0 && v == (uint32_t)INT32_MAX + 1U) {
		*out = INT32_MIN;
	} else {
		*out = sign * (int32_t)v;
	}
	return 0;
}

static int parse_hex64(const char *s, uint64_t *out)
{
	uint64_t value = 0;
	int i;

	if (strlen(s) != 16) {
		return -1;
	}
	for (i = 0; i < 16; i++) {
		unsigned char c = (unsigned char)s[i];
		uint8_t digit;

		if (c >= '0' && c <= '9') {
			digit = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			digit = c - 'a' + 10;
		} else if (c >= 'A' && c <= 'F') {
			digit = c - 'A' + 10;
		} else {
			return -1;
		}
		value = (value << 4) | digit;
	}
	*out = value;
	return 0;
}

static int ascii_lower(int c)
{
	if (c >= 'A' && c <= 'Z') {
		return c + ('a' - 'A');
	}
	return c;
}

/* Case-insensitive compare of a bounded [start,end) span against a literal. */
static bool span_is(const char *start, const char *end, const char *lit)
{
	size_t n = (size_t)(end - start);

	if (strlen(lit) != n) {
		return false;
	}
	while (n--) {
		if (ascii_lower((unsigned char)*start) !=
		    ascii_lower((unsigned char)*lit)) {
			return false;
		}
		start++;
		lit++;
	}
	return true;
}

static int parse_bond_ref(const char *s, uint8_t *ref)
{
	uint32_t value;

	if (strlen(s) < 6 || !span_is(s, s + 5, "bond_") ||
	    parse_uint(s + 5, &value) != 0 || value == 0 ||
	    value > CONFIG_TOTP_MAX_BONDS) {
		return -1;
	}
	*ref = (uint8_t)value;
	return 0;
}

void config_device_defaults(struct totp_device *device)
{
	memset(device, 0, sizeof(*device));
	device->settle_ms = DEFAULT_SETTLE_MS;
	device->inter_key_ms = DEFAULT_INTER_KEY_MS;
	device->long_press_ms = DEFAULT_LONG_PRESS_MS;
	device->click_window_ms = DEFAULT_CLICK_WINDOW_MS;
	device->grace_ms = DEFAULT_GRACE_MS;
	device->low_battery_mv = DEFAULT_LOW_BATTERY_MV;
}

/* Copy the trimmed span [start,end) into out without silent truncation. */
static int copy_trimmed(char *out, size_t out_size,
			const char *start, const char *end)
{
	while (start < end && isspace((unsigned char)*start)) {
		start++;
	}
	while (end > start && isspace((unsigned char)end[-1])) {
		end--;
	}
	if ((size_t)(end - start) >= out_size) {
		return -1;
	}
	memcpy(out, start, (size_t)(end - start));
	out[end - start] = '\0';
	return 0;
}

int config_parse(const char *buf, size_t len, struct totp_config *cfg)
{
	enum { SEC_NONE, SEC_DEVICE, SEC_ACCOUNT } section = SEC_NONE;
	const char *p = buf;
	const char *end = buf + len;
	bool secret_seen[CONFIG_MAX_SLOTS] = { false };
	int i;

	secure_zero(cfg, sizeof(*cfg));
	config_device_defaults(&cfg->device);
	config_err_msg[0] = '\0';

	while (p < end) {
		const char *line = p;
		const char *line_end;
		char key[CONFIG_NAME_MAX];
		char val[CONFIG_VALUE_MAX];
		struct totp_slot *slot;

#define FAIL_PARSE(message) do { \
	int fail_err = parse_err(message); \
	secure_zero(key, sizeof(key)); \
	secure_zero(val, sizeof(val)); \
	return fail_err; \
} while (0)
#define FAIL_PARSE_KEY(section_name, key_name) do { \
	int fail_err = parse_err_key(section_name, key_name); \
	secure_zero(key, sizeof(key)); \
	secure_zero(val, sizeof(val)); \
	return fail_err; \
} while (0)

		while (p < end && *p != '\n') {
			p++;
		}
		line_end = p;
		if (p < end) {
			p++; /* skip '\n' */
		}
		if (line_end > line && line_end[-1] == '\r') {
			line_end--;
		}

		while (line < line_end && isspace((unsigned char)*line)) {
			line++;
		}

		/* Blank or comment. */
		if (line >= line_end || *line == '#' || *line == ';') {
			continue;
		}

		/* Section header. */
		if (*line == '[') {
			const char *name = line + 1;
			const char *name_end = name;

			while (name_end < line_end && *name_end != ']') {
				name_end++;
			}
			if (name_end >= line_end) {
				FAIL_PARSE("unterminated [section]");
			}

			if (span_is(name, name_end, "device")) {
				section = SEC_DEVICE;
			} else if (span_is(name, name_end, "account")) {
				if (cfg->slot_count >= CONFIG_MAX_SLOTS) {
					FAIL_PARSE("too many accounts");
				}
				slot = &cfg->slots[cfg->slot_count];
				memset(slot, 0, sizeof(*slot));
				slot->digits = DEFAULT_DIGITS;
				slot->period = DEFAULT_PERIOD;
				slot->enter = false;
				snprintf(slot->name, sizeof(slot->name),
					 "account%d", cfg->slot_count + 1);
				cfg->slot_count++;
				section = SEC_ACCOUNT;
			} else {
				FAIL_PARSE("unknown section");
			}
			continue;
		}

		/* key = value. Locate a trailing comment before copying the value,
		 * so long descriptive comments do not consume the value buffer. */
		{
			const char *eq = line;
			const char *value_end;

			while (eq < line_end && *eq != '=') {
				eq++;
			}
			if (eq >= line_end) {
				FAIL_PARSE("line without '='");
			}
			if (copy_trimmed(key, sizeof(key), line, eq) != 0) {
				FAIL_PARSE("config key is too long");
			}
			value_end = eq + 1;
			while (value_end < line_end && *value_end != '#' &&
			       *value_end != ';') {
				value_end++;
			}
			if (copy_trimmed(val, sizeof(val), eq + 1, value_end) != 0) {
				FAIL_PARSE("config value is too long");
			}
		}

		switch (section) {
		case SEC_DEVICE:
			if (span_is(key, key + strlen(key), "timezone")) {
				if (config_parse_utc_offset(val,
						&cfg->device.timezone_min) != 0) {
					FAIL_PARSE("bad timezone value");
				}
			} else if (span_is(key, key + strlen(key),
					   "bond_snapshot")) {
				if (cfg->have_bond_snapshot ||
				    parse_hex64(val, &cfg->bond_snapshot) != 0) {
					FAIL_PARSE("bad or duplicate bond_snapshot");
				}
				cfg->have_bond_snapshot = true;
			} else if (span_is(key, key + strlen(key),
					   "edit_bonds")) {
				if (cfg->have_edit_bonds ||
				    config_parse_bool(val, &cfg->edit_bonds) != 0) {
					FAIL_PARSE("bad or duplicate edit_bonds");
				}
				cfg->have_edit_bonds = true;
			} else if (span_is(key, key + strlen(key), "keep_bond")) {
				uint8_t ref;

				if (parse_bond_ref(val, &ref) != 0 ||
				    (cfg->keep_bonds & BIT(ref - 1)) != 0) {
					FAIL_PARSE("bad or duplicate keep_bond");
				}
				cfg->keep_bonds |= BIT(ref - 1);
			} else if (span_is(key, key + strlen(key), "host_id")) {
				if (cfg->have_host_id ||
				    parse_bond_ref(val, &cfg->host_id) != 0) {
					FAIL_PARSE("bad host_id");
				}
				cfg->have_host_id = true;
			} else if (span_is(key, key + strlen(key),
					   "conn_settle_time")) {
				if (config_parse_duration(val,
						&cfg->device.settle_ms) != 0 ||
				    cfg->device.settle_ms > MAX_SETTLE_MS) {
					FAIL_PARSE("bad conn_settle_time");
				}
			} else if (span_is(key, key + strlen(key),
					   "inter_key_time")) {
				if (config_parse_duration(val,
						&cfg->device.inter_key_ms) != 0 ||
				    cfg->device.inter_key_ms < MIN_INTER_KEY_MS ||
				    cfg->device.inter_key_ms > MAX_INTER_KEY_MS) {
					FAIL_PARSE("bad inter_key_time");
				}
			} else if (span_is(key, key + strlen(key), "drift_ppm")) {
				if (parse_int(val, &cfg->device.drift_ppm) != 0 ||
				    cfg->device.drift_ppm < -MAX_DRIFT_PPM ||
				    cfg->device.drift_ppm > MAX_DRIFT_PPM) {
					FAIL_PARSE("bad drift_ppm");
				}
			} else if (span_is(key, key + strlen(key),
					   "long_press_ms")) {
				if (config_parse_duration(val,
						&cfg->device.long_press_ms) != 0 ||
				    cfg->device.long_press_ms < MIN_LONG_PRESS_MS ||
				    cfg->device.long_press_ms > MAX_LONG_PRESS_MS) {
					FAIL_PARSE("bad long_press_ms");
				}
			} else if (span_is(key, key + strlen(key),
					   "click_window_ms")) {
				if (config_parse_duration(val,
						&cfg->device.click_window_ms) != 0 ||
				    cfg->device.click_window_ms < MIN_CLICK_WINDOW_MS ||
				    cfg->device.click_window_ms > MAX_CLICK_WINDOW_MS) {
					FAIL_PARSE("bad click_window_ms");
				}
			} else if (span_is(key, key + strlen(key), "grace_ms")) {
				if (config_parse_duration(val,
						&cfg->device.grace_ms) != 0 ||
				    cfg->device.grace_ms > MAX_GRACE_MS) {
					FAIL_PARSE("bad grace_ms");
				}
			} else if (span_is(key, key + strlen(key),
					   "low_battery_mv")) {
				if (parse_uint(val, &cfg->device.low_battery_mv) != 0 ||
				    cfg->device.low_battery_mv < MIN_LOW_BATTERY_MV ||
				    cfg->device.low_battery_mv > MAX_LOW_BATTERY_MV) {
					FAIL_PARSE("bad low_battery_mv");
				}
			} else {
				FAIL_PARSE_KEY("device", key);
			}
			break;
		case SEC_ACCOUNT:
			slot = &cfg->slots[cfg->slot_count - 1];
			if (span_is(key, key + strlen(key), "name")) {
				if (strlen(val) >= sizeof(slot->name)) {
					FAIL_PARSE("account name is too long");
				}
				strcpy(slot->name, val);
			} else if (span_is(key, key + strlen(key), "secret")) {
				if (secret_seen[cfg->slot_count - 1]) {
					FAIL_PARSE("duplicate account secret");
				}
				secret_seen[cfg->slot_count - 1] = true;
				if (span_is(val, val + strlen(val), "keep")) {
					FAIL_PARSE("bare secret keep is not allowed");
				} else if (strlen(val) >= 5 &&
					   span_is(val, val + 5, "keep:")) {
					uint32_t ref;

					if (parse_uint(val + 5, &ref) != 0 || ref == 0 ||
					    ref > CONFIG_MAX_SLOTS) {
						FAIL_PARSE("bad secret keep reference");
					}
					slot->secret_ref = (uint8_t)ref;
				} else if (span_is(val, val + strlen(val),
						   "error")) {
					slot->secret_error = true;
				} else {
					uint8_t decoded[64];
					int dlen = base32_decode(val, decoded,
								 sizeof(decoded));

					if (dlen <= 0) {
						secure_zero(decoded, sizeof(decoded));
						FAIL_PARSE("bad or empty secret");
					}
					memcpy(slot->secret, decoded, (size_t)dlen);
					slot->secret_len = (size_t)dlen;
					secure_zero(decoded, sizeof(decoded));
				}
			} else if (span_is(key, key + strlen(key), "digits")) {
				uint32_t d;

				if (parse_uint(val, &d) != 0 || d == 0 || d > 9) {
					FAIL_PARSE("bad digits (must be 1-9)");
				}
				slot->digits = (uint8_t)d;
			} else if (span_is(key, key + strlen(key), "period")) {
				uint32_t pr;

				if (parse_uint(val, &pr) != 0 || pr == 0 ||
				    pr > MAX_PERIOD_S) {
					FAIL_PARSE("bad period");
				}
				slot->period = pr;
			} else if (span_is(key, key + strlen(key), "enter")) {
				if (config_parse_bool(val, &slot->enter) != 0) {
					FAIL_PARSE("bad enter (true/false)");
				}
			} else {
				FAIL_PARSE_KEY("account", key);
			}
			break;
		default:
			/* key outside any section */
			FAIL_PARSE("key outside any section");
		}
		secure_zero(key, sizeof(key));
		secure_zero(val, sizeof(val));
	}

#undef FAIL_PARSE
#undef FAIL_PARSE_KEY

	/* Every account must have a secret (or a keep/error placeholder). */
	for (i = 0; i < cfg->slot_count; i++) {
		if (cfg->slots[i].secret_len == 0 &&
		    cfg->slots[i].secret_ref == 0 && !cfg->slots[i].secret_error) {
			return parse_err("account missing secret");
		}
	}

	return 0;
}

int config_selftest(void)
{
#if CONFIG_TOTP_MAX_BONDS >= 2
	static const char text[] =
		"# device\n"
		"[device]\n"
		"timezone = +3:30\n"
		"bond_snapshot = 7c19e2a4d6314f08 # a deliberately long "
		"descriptive comment that exceeds the value buffer without "
		"affecting parsing\n"
		"edit_bonds = false\n"
		"keep_bond = bond_1\n"
		"keep_bond = bond_2\n"
		"host_id = bond_2\n"
		"conn_settle_time = 600ms\n"
		"inter_key_time = 80ms\n"
		"drift_ppm = -25\n"
		"long_press_ms = 2s\n"
		"click_window_ms = 400\n"
		"grace_ms = 1s\n"
		"low_battery_mv = 2200\n"
		"\n"
		"[account]\n"
		"name = Google\n"
		"secret = GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\n"
		"digits = 6\n"
		"period = 30\n"
		"enter = false\n"
		"\n"
		"[account]\n"
		"secret = GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\n"
		"digits = 8\n"
		"period = 60\n"
		"\n";
#define EXPECTED_KEEP_BONDS (BIT(0) | BIT(1))
#define EXPECTED_HOST_ID 2
#else
	static const char text[] =
		"# device\n"
		"[device]\n"
		"timezone = +3:30\n"
		"bond_snapshot = 7c19e2a4d6314f08 # a deliberately long "
		"descriptive comment that exceeds the value buffer without "
		"affecting parsing\n"
		"edit_bonds = false\n"
		"keep_bond = bond_1\n"
		"host_id = bond_1\n"
		"conn_settle_time = 600ms\n"
		"inter_key_time = 80ms\n"
		"drift_ppm = -25\n"
		"long_press_ms = 2s\n"
		"click_window_ms = 400\n"
		"grace_ms = 1s\n"
		"low_battery_mv = 2200\n"
		"\n"
		"[account]\n"
		"name = Google\n"
		"secret = GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\n"
		"digits = 6\n"
		"period = 30\n"
		"enter = false\n"
		"\n"
		"[account]\n"
		"secret = GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\n"
		"digits = 8\n"
		"period = 60\n"
		"\n";
#define EXPECTED_KEEP_BONDS BIT(0)
#define EXPECTED_HOST_ID 1
#endif
	static const char keep_text[] =
		"[account]\n"
		"secret = keep:2\n"
		"\n"
		"[account]\n"
		"secret = error\n"
		"\n";
	static const char bare_keep_text[] =
		"[account]\n"
		"secret = keep\n";
	static const char out_of_range_text[] =
		"[device]\n"
		"inter_key_time = 0\n";
	static const char duplicate_bond_text[] =
		"[device]\n"
		"keep_bond = bond_1\n"
		"keep_bond = bond_1\n";
	static const char bad_snapshot_text[] =
		"[device]\n"
		"bond_snapshot = 1234\n";
	struct totp_config cfg;
	int32_t off;
	uint32_t ms;
	bool b;

	if (config_parse_utc_offset("+3", &off) != 0 || off != 180) {
		return -1;
	}
	if (config_parse_utc_offset("-5", &off) != 0 || off != -300) {
		return -1;
	}
	if (config_parse_utc_offset("-4:45", &off) != 0 || off != -285) {
		return -1;
	}
	if (config_parse_utc_offset("+3:30", &off) != 0 || off != 210) {
		return -1;
	}
	if (config_parse_utc_offset("+24:01", &off) == 0 ||
	    config_parse_utc_offset("999999999999", &off) == 0) {
		return -1;
	}

	if (config_parse_duration("600ms", &ms) != 0 || ms != 600) {
		return -1;
	}
	if (config_parse_duration("2s", &ms) != 0 || ms != 2000) {
		return -1;
	}
	if (config_parse_duration("500", &ms) != 0 || ms != 500) {
		return -1;
	}
	if (config_parse_duration("1x", &ms) == 0) {
		return -1; /* should fail */
	}
	if (config_parse_duration("4294967296", &ms) == 0 ||
	    config_parse_duration("4294968s", &ms) == 0) {
		return -1;
	}

	if (config_parse_bool("false", &b) != 0 || b != false) {
		return -1;
	}
	if (config_parse_bool("yes", &b) != 0 || b != true) {
		return -1;
	}
	if (config_parse_bool("1", &b) != 0 || b != true) {
		return -1;
	}
	if (config_parse_bool("maybe", &b) == 0) {
		return -1; /* should fail */
	}

	if (config_parse(text, sizeof(text) - 1, &cfg) != 0) {
		return -1;
	}
	if (cfg.device.timezone_min != 210) {
		return -1;
	}
	if (!cfg.have_bond_snapshot ||
	    cfg.bond_snapshot != 0x7c19e2a4d6314f08ULL ||
	    !cfg.have_edit_bonds || cfg.edit_bonds ||
	    cfg.keep_bonds != EXPECTED_KEEP_BONDS ||
	    !cfg.have_host_id || cfg.host_id != EXPECTED_HOST_ID) {
		return -1;
	}
	if (cfg.device.settle_ms != 600) {
		return -1;
	}
	if (cfg.device.inter_key_ms != 80) {
		return -1;
	}
	if (cfg.device.drift_ppm != -25) {
		return -1;
	}
	if (cfg.device.long_press_ms != 2000) {
		return -1;
	}
	if (cfg.device.click_window_ms != 400) {
		return -1;
	}
	if (cfg.device.grace_ms != 1000) {
		return -1;
	}
	if (cfg.device.low_battery_mv != 2200) {
		return -1;
	}
	if (cfg.slot_count != 2) {
		return -1;
	}
	if (strcmp(cfg.slots[0].name, "Google") != 0 ||
	    cfg.slots[0].digits != 6 || cfg.slots[0].period != 30 ||
	    cfg.slots[0].enter != false ||
	    cfg.slots[0].secret_len != 20) {
		return -1;
	}
	if (strcmp(cfg.slots[1].name, "account2") != 0 ||
	    cfg.slots[1].digits != 8 || cfg.slots[1].period != 60 ||
	    cfg.slots[1].enter != false ||
	    cfg.slots[1].secret_len != 20) {
		return -1;
	}

	/* "keep" / "error" secret placeholders parse without a real secret. */
	if (config_parse(keep_text, sizeof(keep_text) - 1, &cfg) != 0) {
		return -1;
	}
	if (cfg.slot_count != 2 ||
	    cfg.slots[0].secret_ref != 2 || cfg.slots[0].secret_len != 0 ||
	    !cfg.slots[1].secret_error || cfg.slots[1].secret_len != 0) {
		return -1;
	}
	if (config_parse(bare_keep_text, sizeof(bare_keep_text) - 1, &cfg) == 0 ||
	    config_parse(out_of_range_text, sizeof(out_of_range_text) - 1,
			 &cfg) == 0 ||
	    config_parse(duplicate_bond_text, sizeof(duplicate_bond_text) - 1,
			 &cfg) == 0 ||
	    config_parse(bad_snapshot_text, sizeof(bad_snapshot_text) - 1,
			 &cfg) == 0) {
		return -1;
	}

#undef EXPECTED_KEEP_BONDS
#undef EXPECTED_HOST_ID

	return 0;
}
