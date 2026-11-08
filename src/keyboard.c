/*
 * BLE HID keyboard (HID over GATT) service.
 *
 * Implements the HIDS service manually (no Zephyr HOGP helper exists in
 * 4.4.x), exposing a boot-protocol keyboard report (8 bytes: modifier +
 * reserved + 6 key codes) and notifying input reports over GATT.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include "keyboard.h"
#include "secure_zero.h"

#define HIDS_REPORT_ID 0x00
#define KBD_SEND_MAX_KEYS 16
#define KBD_WARMUP_MS 60
#define KBD_RETRY_MS 10
#define KBD_MAX_RETRIES 100
#define KBD_NOTIFY_TIMEOUT_MS 5000

/* HID Report Reference: Report ID (0 = none) + Report Type (0x01 = Input). */
static const uint8_t input_ref[] = { HIDS_REPORT_ID, 0x01 };

/* HID Report Reference for the output (LED) report: Report Type 0x02 = Output. */
static const uint8_t output_ref[] = { HIDS_REPORT_ID, 0x02 };

/* Output report value (the keyboard LED state, 1 byte). */
static uint8_t output_value;

/* Boot keyboard report map (8-byte report, no Report ID). */
static const uint8_t report_map[] = {
	0x05, 0x01, /* Usage Page (Generic Desktop) */
	0x09, 0x06, /* Usage (Keyboard) */
	0xA1, 0x01, /* Collection (Application) */
	0x05, 0x07, /*   Usage Page (Key Codes) */
	0x19, 0xE0, /*   Usage Minimum (224) */
	0x29, 0xE7, /*   Usage Maximum (231) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x01, /*   Logical Maximum (1) */
	0x75, 0x01, /*   Report Size (1) */
	0x95, 0x08, /*   Report Count (8) */
	0x81, 0x02, /*   Input (Data,Var,Abs) — modifiers */
	0x95, 0x01, /*   Report Count (1) */
	0x75, 0x08, /*   Report Size (8) */
	0x81, 0x01, /*   Input (Const) — reserved */
	0x95, 0x05, /*   Report Count (5) */
	0x75, 0x01, /*   Report Size (1) */
	0x05, 0x08, /*   Usage Page (LEDs) */
	0x19, 0x01, /*   Usage Minimum (1) */
	0x29, 0x05, /*   Usage Maximum (5) */
	0x91, 0x02, /*   Output (Data,Var,Abs) — LED report */
	0x95, 0x01, /*   Report Count (1) */
	0x75, 0x03, /*   Report Size (3) */
	0x91, 0x01, /*   Output (Const) */
	0x95, 0x06, /*   Report Count (6) */
	0x75, 0x08, /*   Report Size (8) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x65, /*   Logical Maximum (101) */
	0x05, 0x07, /*   Usage Page (Key Codes) */
	0x19, 0x00, /*   Usage Minimum (0) */
	0x29, 0x65, /*   Usage Maximum (101) */
	0x81, 0x00, /*   Input (Data,Array) — 6 key codes */
	0xC0,       /* End Collection */
};

/* Report map characteristic value: HID info (2 bytes) + report map. */
struct hids_info {
	uint16_t version; /* bcdHID */
	uint8_t code;     /* bCountryCode */
	uint8_t flags;    /* flags */
} __packed;

static const struct hids_info info = {
	.version = 0x0101,
	.code = 0x00,
	.flags = 0x01, /* normally connectable */
};

static volatile int cccd_notify; /* 1 = central subscribed to notifications */
static void (*subscribed_cb)(void);

/* Debug: last CCC value written (inspected via SWD). */
volatile int dbg_ccc_notify;

/* Debug: notification send attempts + last error (inspected via SWD). */
volatile int dbg_notify_sent;
volatile int dbg_notify_err;
volatile int dbg_output_write;
volatile int dbg_notify_completed;
volatile int dbg_notify_retries;
volatile int dbg_notify_aborts;
#if IS_ENABLED(CONFIG_TOTP_TEST_POLL)
/* Inject one submit error after N successful attempts (inspected via SWD). */
volatile int dbg_force_notify_err;
volatile int dbg_force_notify_after;
#endif

static uint8_t report_value[8];

enum send_phase {
	SEND_WARMUP,
	SEND_KEY_DOWN,
	SEND_KEY_UP,
	SEND_FINISH,
};

/* A single notification is ever in flight. The completion callback advances
 * this sequencer, so no sleeps or report bursts run on the system workqueue. */
static struct {
	uint8_t usages[KBD_SEND_MAX_KEYS];
	size_t count;
	size_t digits;
	size_t index;
	uint32_t inter_key_ms;
	uint32_t generation;
	uint16_t retries;
	enum send_phase phase;
	bool active;
	bool slow;
	bool in_flight;
	kbd_send_done_cb_t done_cb;
	struct k_work_delayable work;
} sender;

/* Protocol Mode value: 1 = Report protocol. */
static uint8_t protocol_mode = 1;

/* Forward declaration for the GATT table. */
static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset);
static ssize_t read_report_map(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset);
static ssize_t read_input_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				 void *buf, uint16_t len, uint16_t offset);
static void input_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);
static void sender_work_handler(struct k_work *work);

static ssize_t read_protocol_mode(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				  void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &protocol_mode,
				 sizeof(protocol_mode));
}

static ssize_t read_report_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, input_ref,
				 sizeof(input_ref));
}

static void (*output_write_cb)(void);

static ssize_t read_output_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, output_ref,
				 sizeof(output_ref));
}

static ssize_t read_output_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				  void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &output_value,
				 sizeof(output_value));
}

static ssize_t write_output_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				   const void *buf, uint16_t len, uint16_t offset,
				   uint8_t flags)
{
	if (offset + len > sizeof(output_value)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	memcpy(&output_value + offset, buf, len);
	dbg_output_write++;

	/* The host writes the LED report once its HID stack is up: treat this
	 * as the "host ready" signal. */
	if (output_write_cb) {
		output_write_cb();
	}
	return len;
}

void kbd_set_output_write_cb(void (*cb)(void))
{
	output_write_cb = cb;
}

/* HID Service (0x1812) with mandatory characteristics. */
BT_GATT_SERVICE_DEFINE(hog_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),

	/* Protocol Mode */
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_PROTOCOL_MODE,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_protocol_mode, NULL, &protocol_mode),

	/* Report Map */
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ,
			       read_report_map, NULL, NULL),

	/* Input Report (the typed TOTP codes): encrypted so they are not sent
	 * as cleartext over the air. */
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT,
			       read_input_report, NULL, &report_value),
	BT_GATT_CCC(input_ccc_changed,
		    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ,
			   read_report_ref, NULL, (void *)input_ref),

	/* Output Report (keyboard LED state) */
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
			       read_output_report, write_output_report,
			       &output_value),
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ,
			   read_output_ref, NULL, (void *)output_ref),

	/* HID Information */
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ,
			       read_info, NULL, NULL),
);

static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &info,
				 sizeof(info));
}

static ssize_t read_report_map(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, report_map,
				 sizeof(report_map));
}

static ssize_t read_input_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				 void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, report_value,
				 sizeof(report_value));
}

static void input_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	int prev = cccd_notify;

	dbg_ccc_notify = value;
	cccd_notify = (value & BT_GATT_CCC_NOTIFY) ? 1 : 0;

	if (!prev && cccd_notify && subscribed_cb) {
		subscribed_cb();
	}
}

void kbd_init(void)
{
	cccd_notify = 0;
	memset(report_value, 0, sizeof(report_value));
	memset(&sender, 0, sizeof(sender));
	k_work_init_delayable(&sender.work, sender_work_handler);
}

bool kbd_is_subscribed(void)
{
	return cccd_notify != 0;
}

void kbd_set_subscribed_cb(void (*cb)(void))
{
	subscribed_cb = cb;
}

/* Map ASCII digit to USB HID usage. */
static int char_to_usage(char c)
{
	if (c >= '1' && c <= '9') {
		return 0x1E + (c - '1'); /* '1' -> 0x1E ... '9' -> 0x26 */
	}
	if (c == '0') {
		return 0x27;
	}
	if (c >= 'a' && c <= 'z') {
		return 0x04 + (c - 'a');
	}
	if (c >= 'A' && c <= 'Z') {
		return 0x04 + (c - 'A');
	}
	if (c == '\n' || c == '\r') {
		return 0x28; /* Enter */
	}
	if (c == ' ') {
		return 0x2C; /* Space */
	}
	return -1;
}

static void sender_clear(void)
{
	secure_zero(sender.usages, sizeof(sender.usages));
	sender.count = 0;
	sender.digits = 0;
	sender.index = 0;
	sender.inter_key_ms = 0;
	sender.retries = 0;
	sender.phase = SEND_WARMUP;
	sender.active = false;
	sender.slow = false;
	sender.in_flight = false;
	sender.done_cb = NULL;
	secure_zero(report_value, sizeof(report_value));
}

static void sender_finish(int err)
{
	kbd_send_done_cb_t done_cb;

	if (!sender.active) {
		return;
	}
	done_cb = sender.done_cb;
	sender_clear();
	if (err != 0) {
		dbg_notify_err = err;
		dbg_notify_aborts++;
	}
	if (done_cb) {
		done_cb(err);
	}
}

static bool transient_notify_error(int err)
{
	return err == -ENOMEM || err == -EAGAIN;
}

static void sender_send_failed(int err)
{
	sender.in_flight = false;
	dbg_notify_err = err;
	if (transient_notify_error(err) &&
	    sender.retries < KBD_MAX_RETRIES) {
		sender.retries++;
		dbg_notify_retries++;
		k_work_reschedule(&sender.work, K_MSEC(KBD_RETRY_MS));
		return;
	}
	sender_finish(err);
}

/* Average delay for one half of a character's press/release cycle. Slowdown
 * is placed after release so holding a key cannot trigger host auto-repeat.
 * Enter keeps the last character's cadence instead of extending the ramp. */
static uint32_t character_delay_ms(size_t index, size_t digits,
				   uint32_t base_ms, bool slow)
{
	if (!slow) {
		return base_ms;
	}
	if (digits < 2) {
		return 2u * base_ms;
	}
	return 2u * base_ms +
	       (uint64_t)3u * base_ms * MIN(index, digits - 1) / (digits - 1);
}

uint32_t kbd_send_duration_ms(size_t digits, bool enter, uint32_t inter_key_ms,
			      bool slow)
{
	uint32_t duration = KBD_WARMUP_MS;

	for (size_t i = 0; i < digits + (enter ? 1u : 0u); i++) {
		duration += 2u * character_delay_ms(i, digits, inter_key_ms, slow);
	}
	return duration;
}

int kbd_selftest(void)
{
	static const uint32_t expected[] = { 120, 156, 192, 228, 264, 300, 300 };

	for (size_t i = 0; i < ARRAY_SIZE(expected); i++) {
		if (character_delay_ms(i, 6, 60, true) != expected[i] ||
		    character_delay_ms(i, 6, 60, false) != 60) {
			return -1;
		}
	}
	if (character_delay_ms(0, 1, 60, true) != 120 ||
	    kbd_send_duration_ms(6, false, 60, false) != 780 ||
	    kbd_send_duration_ms(6, true, 60, false) != 900 ||
	    kbd_send_duration_ms(6, false, 60, true) != 2580 ||
	    kbd_send_duration_ms(6, true, 60, true) != 3180 ||
	    kbd_send_duration_ms(6, true, 5000, true) != 260060) {
		return -1;
	}
	return 0;
}

static void report_complete(struct bt_conn *conn, void *user_data)
{
	uint32_t generation = (uint32_t)(uintptr_t)user_data;
	uint32_t delay_ms;

	ARG_UNUSED(conn);
	if (!sender.active || generation != sender.generation) {
		return; /* completion from a cancelled send */
	}

	sender.in_flight = false;
	sender.retries = 0;
	dbg_notify_completed++;

	switch (sender.phase) {
	case SEND_WARMUP:
		sender.phase = SEND_KEY_DOWN;
		delay_ms = KBD_WARMUP_MS;
		break;
	case SEND_KEY_DOWN:
		sender.phase = SEND_KEY_UP;
		delay_ms = sender.inter_key_ms;
		break;
	case SEND_KEY_UP:
		delay_ms = 2u * character_delay_ms(sender.index, sender.digits,
						   sender.inter_key_ms, sender.slow) -
			   sender.inter_key_ms;
		sender.index++;
		sender.phase = sender.index < sender.count ?
			       SEND_KEY_DOWN : SEND_FINISH;
		break;
	case SEND_FINISH:
	default:
		sender_finish(-EIO);
		return;
	}

	k_work_reschedule(&sender.work, K_MSEC(delay_ms));
}

static void sender_work_handler(struct k_work *work)
{
	struct bt_gatt_notify_params params;
	int err;

	ARG_UNUSED(work);
	if (!sender.active) {
		return;
	}
	if (sender.in_flight) {
		/* Zephyr deliberately omits the completion callback when its ATT
		 * transmit metadata reports an error. Disconnect to release any key
		 * the host may still consider pressed, then let a click retry. */
		sender_finish(-ETIMEDOUT);
		return;
	}
	if (sender.phase == SEND_FINISH) {
		sender_finish(0);
		return;
	}
	if (!cccd_notify) {
		sender_finish(-ENOTCONN);
		return;
	}

	memset(report_value, 0, sizeof(report_value));
	if (sender.phase == SEND_KEY_DOWN) {
		report_value[2] = sender.usages[sender.index];
	}

	memset(&params, 0, sizeof(params));
	params.attr = &hog_svc.attrs[6];
	params.data = report_value;
	params.len = sizeof(report_value);
	params.func = report_complete;
	params.user_data = (void *)(uintptr_t)sender.generation;
	sender.in_flight = true;
	dbg_notify_sent++;
#if IS_ENABLED(CONFIG_TOTP_TEST_POLL)
	if (dbg_force_notify_err != 0 && dbg_force_notify_after <= 0) {
		err = dbg_force_notify_err;
		dbg_force_notify_err = 0;
	} else {
		if (dbg_force_notify_after > 0) {
			dbg_force_notify_after--;
		}
		err = bt_gatt_notify_cb(NULL, &params);
	}
#else
	err = bt_gatt_notify_cb(NULL, &params);
#endif
	if (err != 0) {
		sender_send_failed(err);
	} else {
		/* A disconnect cancels the sender explicitly. This watchdog covers
		 * lower-layer completion loss while the connection remains present. */
		k_work_reschedule(&sender.work,
				  K_MSEC(KBD_NOTIFY_TIMEOUT_MS));
	}
}

int kbd_send_code(const char *digits, bool enter, uint32_t inter_key_ms,
		   bool slow, kbd_send_done_cb_t done_cb)
{
	size_t count = 0;
	const char *p;

	if (sender.active) {
		return -EBUSY;
	}
	if (!cccd_notify) {
		return -ENOTCONN;
	}
	for (p = digits; *p; p++) {
		int usage;

		if (count >= KBD_SEND_MAX_KEYS) {
			secure_zero(sender.usages, sizeof(sender.usages));
			return -E2BIG;
		}
		usage = char_to_usage(*p);
		if (usage < 0) {
			secure_zero(sender.usages, sizeof(sender.usages));
			return -EINVAL;
		}
		sender.usages[count++] = (uint8_t)usage;
	}
	if (enter) {
		if (count >= KBD_SEND_MAX_KEYS) {
			secure_zero(sender.usages, sizeof(sender.usages));
			return -E2BIG;
		}
		sender.usages[count++] = 0x28;
	}
	if (count == 0) {
		return -EINVAL;
	}

	sender.count = count;
	sender.digits = count - (enter ? 1u : 0u);
	sender.index = 0;
	sender.inter_key_ms = inter_key_ms;
	sender.slow = slow;
	sender.retries = 0;
	sender.phase = SEND_WARMUP;
	sender.done_cb = done_cb;
	sender.in_flight = false;
	sender.generation++;
	if (sender.generation == 0) {
		sender.generation++;
	}
	sender.active = true;
	dbg_notify_err = 0;
	k_work_reschedule(&sender.work, K_NO_WAIT);
	return 0;
}

void kbd_cancel_send(void)
{
	if (!sender.active) {
		return;
	}
	sender.generation++;
	k_work_cancel_delayable(&sender.work);
	sender_clear();
}
