/* Button gesture detection — public API */

#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>

/* Initialize the button (virtual-ground + sense pins, interrupts, timers). */
void button_init(void);

/*
 * Called (from workqueue context) when a stable press is detected.
 * Implemented by the application. Used to wake the radio early.
 */
void button_pressed(void);

/*
 * Called (from workqueue context) when a click sequence completes.
 * @count is the number of clicks in the click window.
 * Implemented by the application.
 */
void button_clicked(int count);

/*
 * Called (from workqueue context) when a long press is detected.
 * Implemented by the application.
 */
void button_long_pressed(void);

/* Notify the button module of the pairing state (so a short press in
 * pairing mode cancels immediately instead of waiting for the click window). */
void button_set_pairing(bool pairing);

/* Set the gesture timings (click window and long-press hold, in ms). Values of
 * 0 leave the current value unchanged. */
void button_set_timings(uint32_t click_window_ms, uint32_t long_press_ms);

#endif /* BUTTON_H */
