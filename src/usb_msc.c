/*
 * USB Mass Storage device — presents the config disk to the host.
 *
 * Uses the new (usbd) USB device stack with a fixed identity:
 *   VID 0x1209, configurable PID, serial "totp-01".
 * The default PID 0x0001 is shared, private testing only: it is not unique
 * and must not be used on redistributed binaries or manufactured devices.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>
#include <zephyr/usb/class/usbd_msc.h>

#include "usb_msc.h"
#include "config_fs.h"
#include "usb_power_replay.h"

#define USB_VID 0x1209
#define USB_PID CONFIG_TOTP_USB_PID

/* Device context on the default USB controller (usbd). */
USBD_DEVICE_DEFINE(totp_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   USB_VID, USB_PID);

/* String descriptors. */
USBD_DESC_LANG_DEFINE(totp_lang);
USBD_DESC_MANUFACTURER_DEFINE(totp_mfr, "TapShard");
USBD_DESC_PRODUCT_DEFINE(totp_product, "TapShard");
USBD_DESC_STRING_DEFINE(totp_sn, "totp-01", USBD_DUT_STRING_SERIAL_NUMBER);
USBD_DESC_CONFIG_DEFINE(totp_fs_cfg, "FS Configuration");

/* Full-speed configuration (bus-powered, 100 mA). */
USBD_CONFIGURATION_DEFINE(totp_fs_config, 0, 100, &totp_fs_cfg);

/* Mass Storage logical unit backed by the RAM disk. */
USBD_DEFINE_MSC_LUN(config, "RAM", "TapShard", "Config", "0.01");

static bool connected;
static bool vbus_ready;
static void (*vbus_cb)(bool present);

/* Inspect through SWD when a USB transition fails; no periodic wakeups. */
volatile int dbg_usb_enable_err;
volatile int dbg_usb_disable_err;

static void usb_msc_msg_cb(struct usbd_context *const ctx,
			   const struct usbd_msg *const msg)
{
	switch (msg->type) {
	case USBD_MSG_VBUS_READY:
		if (vbus_ready) {
			break;
		}
		vbus_ready = true;
		/* Notify the app first so it can dump config.ini before the
		 * device is enabled and the host mounts (otherwise the host could
		 * enumerate a volume that is still being written). */
		if (vbus_cb) {
			vbus_cb(true);
		}
		dbg_usb_enable_err = usbd_enable(&totp_usbd);
		if (dbg_usb_enable_err == 0) {
			usb_power_replay_ready();
		}
		break;
	case USBD_MSG_VBUS_REMOVED:
		vbus_ready = false;
		connected = false;
		dbg_usb_disable_err = usbd_disable(&totp_usbd);
		if (vbus_cb) {
			vbus_cb(false);
		}
		break;
	case USBD_MSG_CONFIGURATION:
		connected = true;
		break;
	case USBD_MSG_RESET:
	case USBD_MSG_UDC_ERROR:
		connected = false;
		break;
	default:
		break;
	}
}

bool usb_msc_is_connected(void)
{
	return connected;
}

void usb_msc_set_vbus_cb(void (*cb)(bool present))
{
	vbus_cb = cb;
}

static int usb_msc_setup(void)
{
	int err;

	err = usbd_add_descriptor(&totp_usbd, &totp_lang);
	if (err) {
		return err;
	}
	err = usbd_add_descriptor(&totp_usbd, &totp_mfr);
	if (err) {
		return err;
	}
	err = usbd_add_descriptor(&totp_usbd, &totp_product);
	if (err) {
		return err;
	}
	err = usbd_add_descriptor(&totp_usbd, &totp_sn);
	if (err) {
		return err;
	}

	err = usbd_add_configuration(&totp_usbd, USBD_SPEED_FS,
				     &totp_fs_config);
	if (err) {
		return err;
	}

	err = usbd_register_all_classes(&totp_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		return err;
	}

	err = usbd_msg_register_cb(&totp_usbd, usb_msc_msg_cb);
	if (err) {
		return err;
	}

	return 0;
}

int usb_msc_init(void)
{
	int err;

	/* A format alone need not overwrite old data clusters. Physically erase
	 * the volatile disk first so a reset cannot expose a prior config. */
	err = config_fs_wipe();
	if (err != 0) {
		printk("RAM disk wipe/format failed (err %d)\n", err);
		return err;
	}

	err = usb_msc_setup();
	if (err) {
		return err;
	}

	err = usbd_init(&totp_usbd);
	if (err) {
		return err;
	}

	/* Restore the current level after a USB bootloader consumed its edges.
	 * This keeps the normal dump-before-enable callback and UDC state. */
	usb_power_replay_detected();

	/* With no VBUS the USB PHY stays off until a physical plug event. */

	return 0;
}
