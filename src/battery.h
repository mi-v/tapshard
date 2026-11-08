/* Battery voltage monitoring — public API */

#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>
#include <stdbool.h>

/* Initialize and start periodic battery sampling. */
void battery_init(void);

/* Notify of the USB power state: samples taken while USB is present are
 * inflated (VDDH rises toward 5 V), so they are not cached. */
void battery_set_usb_present(bool present);

/* Schedule a fresh sample shortly (used on USB unplug so the cached voltage
 * reflects the battery right away rather than up to one sample interval later). */
void battery_resample(void);

/* Set the low-battery threshold (mV). */
void battery_set_low_threshold(uint32_t mv);

/* Last cached on-battery voltage in mV, or 0 if never measured. */
uint32_t battery_mv(void);

/* True once a valid on-battery sample has been taken. */
bool battery_is_measured(void);

/* True if the battery is measured and below the low threshold. */
bool battery_is_low(void);

#endif /* BATTERY_H */
