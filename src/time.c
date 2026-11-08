/*
 * Timekeeping.
 *
 * Continuous time is derived from the system clock (the nRF RTC on the LFCLK
 * 32.768 kHz crystal). A host-local FAT time value is anchored to that
 * monotonic base by time_set_observed(); timezone conversion is intentionally
 * left to the TOTP caller. The LFCLK has a small, roughly constant frequency
 * error that makes the clock drift between anchors. Calibration compares raw
 * uptime with host time across recent USB visits, accepting a new integer ppm
 * only when it improves precision throughout the timestamp uncertainty range.
 *
 * The calibrated drift_ppm is persisted under the "totp/drift" settings key.
 */

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "time.h"

/* Conservative uncertainty in elapsed host time between two FAT timestamps,
 * including their 2 s granularity and normal filesystem observation delay.
 * This assumes fresh host timestamps, not arbitrarily delayed/stale writes. */
#define TIME_ANCHOR_UNCERTAINTY_MS 4000LL

/* Reject a drift measurement beyond this (host clock jump / stale mtime). */
#define TIME_DRIFT_MAX_PPM 500
#define TIME_CIVIL_STEP_MS (15LL * 60LL * 1000LL)
#define TIME_CIVIL_STEP_TOLERANCE_MS 5000LL

static uint64_t epoch_base;
static int64_t uptime_base_ms;
static bool valid;
static int32_t drift_ppm; /* signed LFCLK error in parts-per-million */

struct clock_anchor {
	uint64_t epoch;
	int64_t uptime_ms;
	bool valid;
};

struct drift_calibration {
	struct clock_anchor reference;
	struct clock_anchor latest;
	bool session_active;
	bool session_observed;
};

static struct drift_calibration calibration;

static int totp_settings_set(const char *key, size_t len,
			     settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(key, "drift") != 0) {
		return -ENOENT;
	}
	if (len != sizeof(drift_ppm)) {
		return -EINVAL;
	}
	if (read_cb(cb_arg, &drift_ppm, sizeof(drift_ppm)) !=
	    sizeof(drift_ppm)) {
		return -EIO;
	}
	if (drift_ppm < -TIME_DRIFT_MAX_PPM || drift_ppm > TIME_DRIFT_MAX_PPM) {
		drift_ppm = 0;
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(totp_time, "totp", NULL, totp_settings_set,
			       NULL, NULL);

static void time_save_drift(void)
{
	settings_save_one("totp/drift", &drift_ppm, sizeof(drift_ppm));
}

/* FAT timestamps are host-local. A timezone or DST change therefore appears
 * as a near-exact multiple of 15 minutes and must not become LFCLK drift. */
static bool looks_like_civil_time_step(int64_t error_ms)
{
	int64_t magnitude = error_ms < 0 ? -error_ms : error_ms;
	int64_t remainder;

	if (magnitude < TIME_CIVIL_STEP_MS) {
		return false;
	}
	remainder = magnitude % TIME_CIVIL_STEP_MS;
	return remainder <= TIME_CIVIL_STEP_TOLERANCE_MS ||
	       TIME_CIVIL_STEP_MS - remainder <= TIME_CIVIL_STEP_TOLERANCE_MS;
}

/* Positive ppm means the raw clock runs fast. Share the exact integer
 * conversion between the clock and the estimator's error comparisons. */
static int64_t corrected_elapsed(int64_t elapsed_ms, int32_t ppm)
{
	return elapsed_ms - elapsed_ms * (int64_t)ppm / 1000000LL;
}

enum calibration_result {
	CAL_RETAIN,
	CAL_APPLY,
	CAL_RESEED,
};

static int64_t magnitude(int64_t value)
{
	return value < 0 ? -value : value;
}

/* Pure estimator: no settings writes or changes to the live clock. The span
 * bound prevents arithmetic overflow; it is over 292 years, not a minimum
 * observation interval. Actual accepted FAT dates span at most 80 years. */
static enum calibration_result estimate_drift(int64_t raw_ms, int64_t host_ms,
					     int32_t current, int32_t *candidate)
{
	int64_t low;
	int64_t high;
	int64_t error;
	int64_t numerator;
	int64_t ppm;
	int64_t before;
	int64_t after;

	if (raw_ms <= 0 || host_ms <= 0 || raw_ms > INT64_MAX / 1000000LL ||
	    host_ms > INT64_MAX / 1000000LL) {
		return CAL_RESEED;
	}
	error = raw_ms - host_ms;
	if (looks_like_civil_time_step(error)) {
		return CAL_RESEED;
	}
	low = host_ms - TIME_ANCHOR_UNCERTAINTY_MS;
	high = host_ms + TIME_ANCHOR_UNCERTAINTY_MS;
	if (high < corrected_elapsed(raw_ms, TIME_DRIFT_MAX_PPM) ||
	    low > corrected_elapsed(raw_ms, -TIME_DRIFT_MAX_PPM)) {
		return CAL_RESEED;
	}

	/* The plausibility check also bounds error before multiplication.
	 * Rounding is signed nearest, with half-way cases away from zero. */
	numerator = error * 1000000LL;
	ppm = (numerator + (numerator < 0 ? -raw_ms / 2 : raw_ms / 2)) / raw_ms;
	if (ppm < -TIME_DRIFT_MAX_PPM || ppm > TIME_DRIFT_MAX_PPM || ppm == current) {
		return CAL_RETAIN;
	}
	before = corrected_elapsed(raw_ms, current);
	after = corrected_elapsed(raw_ms, (int32_t)ppm);
	/* Both endpoints must improve strictly. In one dimension this guarantees
	 * improvement everywhere between them, including integer ppm rounding. */
	if (magnitude(after - low) >= magnitude(before - low) ||
	    magnitude(after - high) >= magnitude(before - high)) {
		return CAL_RETAIN;
	}
	*candidate = (int32_t)ppm;
	return CAL_APPLY;
}

static void calibration_begin(struct drift_calibration *state)
{
	if (!state->session_active) {
		state->session_active = true;
		state->session_observed = false;
	}
}

static void calibration_end(struct drift_calibration *state)
{
	if (state->session_active && state->session_observed) {
		state->reference = state->latest;
	}
	state->session_active = false;
	state->session_observed = false;
}

static void calibration_reset(struct drift_calibration *state)
{
	state->reference.valid = false;
	state->latest.valid = false;
	state->session_observed = false;
}

static int32_t calibration_observe(struct drift_calibration *state,
				   uint64_t host_time, int64_t uptime_ms, int32_t current)
{
	enum calibration_result result;
	int32_t candidate = current;
	int64_t host_ms;

	if (!state->session_active) {
		return current;
	}
	state->latest = (struct clock_anchor) {
		.epoch = host_time, .uptime_ms = uptime_ms, .valid = true,
	};
	state->session_observed = true;
	if (!state->reference.valid || host_time <= state->reference.epoch ||
	    state->reference.uptime_ms < 0 ||
	    uptime_ms <= state->reference.uptime_ms ||
	    host_time - state->reference.epoch > (uint64_t)INT64_MAX / 1000ULL) {
		state->reference = state->latest;
		return current;
	}
	host_ms = (int64_t)(host_time - state->reference.epoch) * 1000LL;
	result = estimate_drift(uptime_ms - state->reference.uptime_ms, host_ms,
				current, &candidate);
	if (result == CAL_RESEED) {
		state->reference = state->latest;
	}
	return candidate;
}

void time_usb_session_begin(void)
{
	calibration_begin(&calibration);
}

void time_usb_session_end(void)
{
	calibration_end(&calibration);
}

void time_set_observed(uint64_t host_local_time, int64_t observed_uptime_ms)
{
	int32_t candidate;

	epoch_base = host_local_time;
	uptime_base_ms = observed_uptime_ms;
	valid = true;
	candidate = calibration_observe(&calibration, host_local_time,
					observed_uptime_ms, drift_ppm);
	if (candidate != drift_ppm) {
		drift_ppm = candidate;
		time_save_drift();
	}
}

void time_set(uint64_t host_local_time)
{
	time_set_observed(host_local_time, k_uptime_get());
}

uint64_t time_get(void)
{
	int64_t now_ms;
	int64_t elapsed_ms;
	int64_t corrected_ms;

	if (!valid) {
		return 0;
	}

	now_ms = k_uptime_get();
	if (now_ms <= uptime_base_ms) {
		return epoch_base;
	}
	elapsed_ms = now_ms - uptime_base_ms;

	corrected_ms = corrected_elapsed(elapsed_ms, drift_ppm);

	return epoch_base + (uint64_t)(corrected_ms / 1000);
}

bool time_is_valid(void)
{
	return valid;
}

void time_set_drift(int32_t ppm)
{
	if (ppm < -TIME_DRIFT_MAX_PPM || ppm > TIME_DRIFT_MAX_PPM) {
		ppm = 0;
	}
	if (ppm == drift_ppm) {
		return;
	}
	drift_ppm = ppm;
	/* A manual change starts a new window at the next real host observation.
	 * Do not let session end revive an observation preceding the edit. */
	calibration_reset(&calibration);
	time_save_drift();
}

int32_t time_get_drift(void)
{
	return drift_ppm;
}

uint64_t time_get_epoch_base(void)
{
	return epoch_base;
}

static int calibration_selftest(void)
{
	static const struct {
		int64_t raw_ms;
		int64_t host_ms;
		int32_t current;
		enum calibration_result result;
		int32_t candidate;
	} cases[] = {
		/* 100 ppm is resolvable in a day, but not half a day. */
		{ 86408640, 86400000, 0, CAL_APPLY, 100 },
		{ 86391360, 86400000, 0, CAL_APPLY, -100 },
		{ 43204320, 43200000, 0, CAL_RETAIN, 0 },
		{ 21610800, 21600000, 0, CAL_APPLY, 500 },
		{ 21589200, 21600000, 0, CAL_APPLY, -500 },
		{ 86408640, 86400000, 100, CAL_RETAIN, 100 },
		/* Strict improvement: equality at one endpoint is insufficient. */
		{ 1000000000, 999992000, 0, CAL_RETAIN, 0 },
		{ 1000000000, 999991999, 0, CAL_APPLY, 8 },
		/* Half-ppm candidates round away from zero. */
		{ 20000000000LL, 19999990000LL, -2, CAL_APPLY, 1 },
		{ 20000000000LL, 20000010000LL, 2, CAL_APPLY, -1 },
		/* A small seasonal change may be inconclusive at 90 days. */
		{ 7775992224LL, 7776000000LL, 0, CAL_RETAIN, 0 },
		{ 15551984448LL, 15552000000LL, 0, CAL_APPLY, -1 },
		/* Jumps and clearly implausible rates restart the reference. */
		{ 86400000, 90000000, 17, CAL_RESEED, 17 },
		{ 86451840, 86400000, 17, CAL_RESEED, 17 },
		{ 0, 1000, 17, CAL_RESEED, 17 },
		{ 1000, 0, 17, CAL_RESEED, 17 },
		{ INT64_MAX, 1000, 17, CAL_RESEED, 17 },
		/* Short intervals with wide uncertainty are not discontinuities. */
		{ 1000, 2000, 17, CAL_RETAIN, 17 },
	};
	struct drift_calibration state = { 0 };
	const uint64_t start = 1700000000ULL;
	int32_t candidate;
	int32_t ppm;
	int64_t raw;

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		candidate = cases[i].current;
		if (estimate_drift(cases[i].raw_ms, cases[i].host_ms, cases[i].current,
				   &candidate) != cases[i].result ||
		    candidate != cases[i].candidate) {
			return -1;
		}
	}
	/* Timestamp uncertainty alone must never replace an accurate correction. */
	for (int64_t days = 1; days <= 180; days *= 3) {
		for (int64_t noise = -4000; noise <= 4000; noise += 1000) {
			for (int32_t rate = -100; rate <= 100; rate += 100) {
				raw = days * 86400000LL;
				candidate = rate;
				if (estimate_drift(raw, corrected_elapsed(raw, rate) + noise,
						   rate, &candidate) == CAL_APPLY) {
					return -1;
				}
			}
		}
	}
	/* A debug/non-USB clock set is not a calibration observation. */
	if (calibration_observe(&state, start, 1000, 0) != 0 || state.reference.valid) {
		return -1;
	}
	calibration_begin(&state);
	calibration_observe(&state, start, 1000, 0);
	if (calibration_observe(&state, start + 43200, 43205320, 0) != 0 ||
	    state.reference.epoch != start) {
		return -1;
	}
	ppm = calibration_observe(&state, start + 86400, 86409640, 0);
	if (ppm != 100 || state.reference.epoch != start) {
		return -1;
	}
	/* A duplicate begin (VBUS bounce) must not clear the pending observation. */
	calibration_begin(&state);
	raw = 86419641;
	if (calibration_observe(&state, start + 86410, raw, ppm) != ppm ||
	    state.reference.epoch != start) {
		return -1;
	}
	calibration_end(&state);
	if (state.reference.epoch != start + 86410 || state.reference.uptime_ms != raw) {
		return -1;
	}
	/* Empty visits keep the previous reference. */
	calibration_begin(&state);
	calibration_end(&state);
	if (state.reference.epoch != start + 86410) {
		return -1;
	}
	/* Once a summer visit ends, the next interval measures winter conditions
	 * without averaging in the earlier reference. */
	calibration_begin(&state);
	raw += 7776124416LL; /* 90 host days, about +16 raw ppm. */
	ppm = calibration_observe(&state, start + 86410 + 7776000, raw, 20);
	if (ppm != 16 || state.reference.epoch != start + 86410) {
		return -1;
	}
	calibration_end(&state);
	if (state.reference.epoch != start + 86410 + 7776000) {
		return -1;
	}
	/* A manual edit must not revive the old pending anchor at session end. */
	calibration_begin(&state);
	calibration_observe(&state, start + 86410 + 7776010, raw + 10000, ppm);
	calibration_reset(&state);
	calibration_end(&state);
	if (state.reference.valid) {
		return -1;
	}
	calibration_begin(&state);
	if (calibration_observe(&state, start, 1000, 17) != 17 ||
	    state.reference.epoch != start) {
		return -1;
	}
	/* Forward civil-time and backward host-clock jumps leave ppm intact. */
	if (calibration_observe(&state, start + 90000, 86401000, 17) != 17 ||
	    state.reference.epoch != start + 90000 ||
	    calibration_observe(&state, start + 89900, 86402000, 17) != 17 ||
	    state.reference.epoch != start + 89900) {
		return -1;
	}
	return 0;
}

int time_selftest(void)
{
	uint64_t saved_epoch = epoch_base;
	int64_t saved_uptime = uptime_base_ms;
	bool saved_valid = valid;
	int32_t saved_drift = drift_ppm;
	struct drift_calibration saved_calibration = calibration;
	int64_t now = k_uptime_get();
	uint64_t t;
	int result = calibration_selftest();

	/* Pretend the timestamp was observed 1.5 seconds ago. */
	valid = false;
	memset(&calibration, 0, sizeof(calibration));
	drift_ppm = 0;
	time_set_observed(1700000000ULL, now - 1500);
	if (!time_is_valid()) {
		result = -1;
	}
	t = time_get();
	if (t < 1700000001ULL || t > 1700000000ULL + 10ULL) {
		result = -1;
	}
	if (!looks_like_civil_time_step(-3600000LL) ||
	    looks_like_civil_time_step(1000LL)) {
		result = -1;
	}

	/* Restore the pre-test state so the self-test does not leave the clock
	 * "valid" with a bogus epoch. */
	epoch_base = saved_epoch;
	uptime_base_ms = saved_uptime;
	valid = saved_valid;
	drift_ppm = saved_drift;
	calibration = saved_calibration;

	return result;
}
