/*
 * Battery voltage monitoring via the SAADC internal VDDH/5 input.
 *
 * On this board (Pro Micro nRF52840 / SuperMini clone) VDDH is the battery
 * rail, so VDDH/5 reflects the battery voltage (÷5). While USB is plugged the
 * charger raises VDDH toward 5 V, so samples are only cached while unplugged;
 * the last on-battery reading is kept for the config dump.
 *
 * A single SAADC sample is a few milliseconds of CPU, so a 10-minute cadence
 * adds a negligible average current to the ~3 uA sleep budget.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/sys/printk.h>

#include "battery.h"

/* First sample shortly after boot, then every 10 minutes. */
#define BATTERY_FIRST_SAMPLE_MS 5000
#define BATTERY_SAMPLE_MS       (10 * 60 * 1000)
/* Settle time after USB unplug before resampling (VDDH drops from VBUS to the
 * battery rail through the load switch). */
#define BATTERY_RESAMPLE_MS     500

/* Battery-rail divisor baked into the SAADC VDDH/5 input. */
#define VDDH_DIVISOR 5u

static const struct adc_dt_spec battery_adc =
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

static uint32_t battery_mv_cache;
static bool battery_measured;
static bool usb_present;
static uint32_t low_threshold_mv = 3500;

static struct k_work_delayable battery_work;

/* Debug (SWD): raw ADC count and the VDDH/5 input in millivolts of the last
 * sample, taken regardless of USB state so the raw value can be inspected
 * while charging. */
volatile int dbg_battery_raw;
volatile int dbg_battery_input_mv;

static void battery_sample_work(struct k_work *work)
{
	uint32_t buf = 0;
	struct adc_sequence seq = {
		.buffer = &buf,
		.buffer_size = sizeof(buf),
	};

	(void)adc_sequence_init_dt(&battery_adc, &seq);
	if (adc_read_dt(&battery_adc, &seq) == 0) {
		int32_t val_mv = (int32_t)(int16_t)buf;

		dbg_battery_raw = (int)val_mv;
		if (adc_raw_to_millivolts_dt(&battery_adc, &val_mv) == 0) {
			dbg_battery_input_mv = (int)val_mv;
			if (!usb_present) {
				battery_mv_cache =
					(uint32_t)val_mv * VDDH_DIVISOR;
				battery_measured = true;
			}
		}
	}

	k_work_reschedule(&battery_work, K_MSEC(BATTERY_SAMPLE_MS));
}

void battery_set_usb_present(bool present)
{
	usb_present = present;
}

void battery_resample(void)
{
	k_work_reschedule(&battery_work, K_MSEC(BATTERY_RESAMPLE_MS));
}

void battery_set_low_threshold(uint32_t mv)
{
	low_threshold_mv = mv;
}

uint32_t battery_mv(void)
{
	return battery_measured ? battery_mv_cache : 0;
}

bool battery_is_measured(void)
{
	return battery_measured;
}

bool battery_is_low(void)
{
	return battery_measured && battery_mv_cache < low_threshold_mv;
}

void battery_init(void)
{
	if (!adc_is_ready_dt(&battery_adc)) {
		printk("Battery ADC not ready\n");
		return;
	}
	(void)adc_channel_setup_dt(&battery_adc);

	k_work_init_delayable(&battery_work, battery_sample_work);
	k_work_reschedule(&battery_work, K_MSEC(BATTERY_FIRST_SAMPLE_MS));
}
