/* USB Mass Storage (config disk) — public API */

#ifndef USB_MSC_H
#define USB_MSC_H

#include <stdbool.h>

/* Initialize the USB Mass Storage device. The device is *not* enabled until
 * VBUS is detected (low power: USB PHY stays off while unplugged). */
int usb_msc_init(void);

/* Whether a host is currently connected (device configured). */
bool usb_msc_is_connected(void);

/* Register a callback invoked on VBUS state change (true = plugged in).
 * Called from the USB stack context; keep it short and defer real work. */
void usb_msc_set_vbus_cb(void (*cb)(bool present));

#endif /* USB_MSC_H */
