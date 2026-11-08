/* BLE HID keyboard (HID over GATT) — public API */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Initialize the HID service and prepare the input report. */
void kbd_init(void);

/* True if a central has subscribed to notifications on the input report. */
bool kbd_is_subscribed(void);

/* Register a callback invoked (workqueue context) when a central subscribes
 * to notifications. Used by the app to know the host is ready for reports. */
void kbd_set_subscribed_cb(void (*cb)(void));

/* Register a callback invoked when the host writes the output (LED) report.
 * Used as the "host HID stack is up" signal to start typing sooner. */
void kbd_set_output_write_cb(void (*cb)(void));

/* Completion for one asynchronous code send. @err is zero on success and a
 * negative errno if reports could not be delivered. */
typedef void (*kbd_send_done_cb_t)(int err);

/* Queue a zero warm-up report followed by press/release reports for @digits
 * and optional Enter. Only one send may be active. Report completion drives
 * progress; this function never sleeps. With @slow, character cadence ramps
 * from 2x to 5x delay using longer gaps after release; key holds stay normal
 * to avoid auto-repeat. Optional Enter keeps the final cadence. */
int kbd_send_code(const char *digits, bool enter, uint32_t inter_key_ms,
		   bool slow, kbd_send_done_cb_t done_cb);

/* Scheduled duration, including warm-up and the final release delay. Uses the
 * same delay calculation as the sender for the TOTP expiry budget. */
uint32_t kbd_send_duration_ms(size_t digits, bool enter, uint32_t inter_key_ms,
			      bool slow);

/* Timing checks, run alongside the other boot self-tests. */
int kbd_selftest(void);

/* Silently cancel an active send during connection/state teardown. */
void kbd_cancel_send(void);

#endif /* KEYBOARD_H */
