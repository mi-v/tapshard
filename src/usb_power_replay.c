/* Compatibility adapter for upstream Zephyr's edge-only nRF UDC startup.
 * A preceding USB bootloader may have consumed the POWER events. Keep the
 * driver's real callback, so replay updates its private VBUS state as well as
 * the USB stack. Remove this adapter when the UDC handles initial VBUS levels.
 */
#include <zephyr/kernel.h>
#include <hal/nrf_power.h>
#include <nrfx_power.h>

#include "usb_power_replay.h"

static nrfx_power_usb_event_handler_t power_handler;

void __real_nrfx_power_usbevt_init(nrfx_power_usbevt_config_t const *config);

void __wrap_nrfx_power_usbevt_init(nrfx_power_usbevt_config_t const *config)
{
	power_handler = config->handler;
	__real_nrfx_power_usbevt_init(config);
}

void usb_power_replay_detected(void)
{
	unsigned int key = irq_lock();

	if (power_handler && nrf_power_usbregstatus_vbusdet_get(NRF_POWER)) {
		power_handler(NRFX_POWER_USB_EVT_DETECTED);
	}
	irq_unlock(key);
}

void usb_power_replay_ready(void)
{
	unsigned int key = irq_lock();

	/* This runs after usbd_enable. Do not start a disabled peripheral, or
	 * duplicate a READY event which already enabled its pull-up. */
	if (power_handler && NRF_USBD->ENABLE && !NRF_USBD->USBPULLUP &&
	    nrf_power_usbregstatus_vbusdet_get(NRF_POWER) &&
	    nrf_power_usbregstatus_outrdy_get(NRF_POWER)) {
		power_handler(NRFX_POWER_USB_EVT_READY);
	}
	irq_unlock(key);
}
