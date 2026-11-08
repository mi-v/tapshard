/* Continuous/absolute timekeeping — public API */

#ifndef TIME_H
#define TIME_H

#include <stdint.h>
#include <stdbool.h>

/* Anchor the host-local FAT time scale at the current uptime. */
void time_set(uint64_t host_local_time);

/* Anchor a timestamp at the uptime when it was first observed. This preserves
 * the time spent confirming a stable filesystem observation. */
void time_set_observed(uint64_t host_local_time, int64_t observed_uptime_ms);

/* Track a physical USB visit. Begin once on a fresh plug (not VBUS bounce),
 * end after the final confirmed timestamp. Only observations during a visit
 * calibrate ppm; its latest timestamp becomes the next visit's reference. */
void time_usb_session_begin(void);
void time_usb_session_end(void);

/* Get the current host-local time, continuously updated. */
uint64_t time_get(void);

/* Whether the time has been set (valid) yet. */
bool time_is_valid(void);

/* Set the LFCLK drift calibration (signed ppm), persisting it. Used to apply a
 * user-edited drift_ppm from the config file; auto-calibration may overwrite it
 * later. */
void time_set_drift(int32_t ppm);

/* Current drift calibration (signed ppm). */
int32_t time_get_drift(void);

/* The last host-local timestamp the clock was anchored to (0 if never set). */
uint64_t time_get_epoch_base(void);

/* Self-test. Returns 0 on success. */
int time_selftest(void);

#endif /* TIME_H */
