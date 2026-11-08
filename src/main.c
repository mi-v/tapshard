/*
 * TOTP BLE-HID token — application entry point.
 *
 * State machine:
 *
 *   IDLE ──press──▶ CONNECTING ──connect──▶ CONNECTED ──ready+pending──▶ SENDING
 *     ▲              │  timeout                │  ▲                        │ typed
 *     │              └──▶ IDLE                 │  │ re-click               ▼
 *     │                                        │  └──────────────────▶ GRACE ──timeout──▶ IDLE
 *     │                                        │
 *     └────long press──▶ PAIRING ──pair ok──▶ CONNECTED ──grace──▶ IDLE
 *                         │ click / timeout       snatch
 *                         └──▶ IDLE                 └──▶ PAIRING
 *
 * Two orthogonal concerns are kept separate:
 *
 *   - link readiness  — "can the host receive reports right now?" This is
 *     `subscribed (CCC notify) && (settle elapsed || host wrote the LED report)`.
 *     The settle happens once per connection, started on subscribe and
 *     fast-forwarded by the output (LED) report write. Typing is immediate once
 *     the link is ready — there is no per-type delay.
 *
 *   - pending request — "is there a code waiting to be typed?"
 *
 * `try_send()` is the single evaluation point, called from every event that can
 * change either concern.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#if IS_ENABLED(CONFIG_TOTP_POWER_DIAGNOSTIC)
#include <zephyr/sys/poweroff.h>
#endif

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>

#include "keyboard.h"
#include "totp.h"
#include "config.h"
#include "config_fs.h"
#include "usb_msc.h"
#include "time.h"
#include "button.h"
#include "battery.h"
#include "profiles.h"
#include "secure_zero.h"
#if IS_ENABLED(CONFIG_TOTP_SECURITY_BOOTLOADER)
#include "boot_security.h"
#endif

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

/* Firmware version, shown in the dumped config.ini header. */
#define FW_VERSION "0.1.0"

/* --- Timing --- */
#if IS_ENABLED(CONFIG_TOTP_DEBUG)
#define CONNECT_TIMEOUT_S   60
#else
#define CONNECT_TIMEOUT_S   10
#endif
#define PAIRING_TIMEOUT_S   30
/* Directed advertising is not handled consistently by host background scan
 * policies. Prefer the last host with a filtered ordinary advertisement
 * before making every retained bond eligible. */
#define PREFERRED_FALLBACK_MS 5000
/* Stay connected after a genuine pairing so the host's UI/HID stack finishes
 * before we drop and sleep. */
#define PAIR_DISCONNECT_MS  2000
/* How long after security_changed to wait for pairing_complete before treating
 * a secured connection during pairing as an old bonded host snatch. */
#define PAIR_SNATCH_MS      2000
/* While USB is connected the host may write files; poll this often for stable,
 * session-local mtime changes that can re-anchor the clock. USB powers the
 * device, so this is free of battery cost. */
#define CONFIG_POLL_MS      100
#define CONFIG_STABLE_RETRY_MS 100
#define CONFIG_STABLE_RETRIES  20
/* Mechanical USB removal can briefly bounce VBUS. Keep the current session
 * alive long enough to avoid re-dumping over the host's final config write. */
#define USB_REMOVE_DEBOUNCE_MS 200
/* Do not start typing a TOTP that is too close to expiring. This margin also
 * covers the one-second granularity of time_get(). */
#define TOTP_SEND_MARGIN_MS 1500
/* Plausible range for a file mtime used as a clock anchor (rejects a garbage
 * value from a torn directory-sector read). */
#define TIME_SANE_MIN       1577836800ULL /* 2020-01-01 */
#define TIME_SANE_MAX       4102444800ULL /* 2100-01-01 */

/* LED (board default, P0.15) */
#define LED_NODE DT_NODELABEL(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

enum app_state {
	STATE_IDLE = 0,      /* radio off, sleeping */
	STATE_CONNECTING = 1,/* fast advertising, waiting for host */
	STATE_CONNECTED = 2, /* link up, awaiting "ready" (subscribe + settle) */
	STATE_SENDING = 3,   /* ready + pending code, emitting reports */
	STATE_GRACE = 4,     /* code sent, waiting to disconnect */
	STATE_PAIRING = 5,   /* discoverable, never sleeps */
};

/* Why the device returned to IDLE (inspected via SWD). */
enum idle_reason {
	IDLE_CONN_FAILED = 1,
	IDLE_DISCONNECTED = 2,
	IDLE_CONNECT_TIMEOUT = 3,
	IDLE_GRACE = 4,
	IDLE_PAIRING_CANCELLED = 5,
	IDLE_PAIRING_GRACE = 6,
	IDLE_INVALID_SLOT = 8,
	IDLE_INVALID_CLOCK = 9,
	IDLE_SEND_FAILED = 10,
	IDLE_CONFIG_UNSTABLE = 11,
};

static enum app_state state = STATE_IDLE;
volatile int dbg_state = STATE_IDLE;

static struct bt_conn *cur_conn;

/* Device-wide settings are independent from per-bond secret profiles. */
static struct totp_device device_settings;

static struct secret_profile dump_profile;
static bool dump_profile_valid;

struct bond_dump_snapshot {
	bt_addr_le_t addrs[CONFIG_TOTP_MAX_BONDS];
	uint64_t token;
	int count;
	bool valid;
};
static struct bond_dump_snapshot dump_bonds;

static void clear_dump_secrets(void)
{
	secure_zero(&dump_profile, sizeof(dump_profile));
	dump_profile_valid = false;
}

static void clear_dump_state(void)
{
	clear_dump_secrets();
	secure_zero(&dump_bonds, sizeof(dump_bonds));
}

/* Time anchors are confirmed across two directory polls. The timestamp is
 * applied at first_seen_ms so confirmation latency does not slow the clock. */
struct time_candidate {
	struct config_fs_change change;
	int64_t first_seen_ms;
	bool valid;
};
static struct time_candidate time_candidate;
static uint64_t session_anchor_mtime;
static char last_anchor_name[CONFIG_FS_ANCHOR_NAME_MAX];

/* The drift_ppm value written to config.ini at the last dump. On read-back a
 * differing value means the user edited it, and only then is it applied (so a
 * stale dump does not clobber a fresh auto-calibration). */
static int32_t dump_drift_ppm;

/* Self-test result: 0 = pass, non-zero = fail (inspected via SWD). */
static volatile int selftest_failed = 1;

/* A request waiting for the link and, for a real account, a safe point in its
 * TOTP period. The code itself is generated immediately before transmission. */
enum pending_kind {
	PENDING_NONE,
	PENDING_SLOT,
	PENDING_CONFIG_ERROR,
	PENDING_UNCONFIGURED,
	PENDING_CONFIG_READ,
};

struct pending_req {
	enum pending_kind kind;
	int slot_idx;
};
static struct pending_req pending;

/* The account whose asynchronous HID transfer is currently in flight. */
static struct secret_profile *sending_profile;
static int sending_slot = -1;
static uint32_t sending_revision;

/* Temporary test: last generated code (inspected via SWD). */
#if IS_ENABLED(CONFIG_TOTP_DEBUG)
static char last_code[16];
#endif

/* SWD hook: non-zero re-loads the hardcoded test config on next click. */
#if IS_ENABLED(CONFIG_TOTP_DEBUG)
volatile int dbg_reload_config;
#endif

/* Link readiness: settled (settle elapsed OR host wrote the LED report). */
static bool link_settled;

/* Debug state (inspected via SWD). */
static volatile int bt_ready_called;
static volatile int adv_result = -1;
static volatile int dbg_connected;
static volatile int dbg_sec_level = -99;
static volatile int dbg_sec_changed_err = -99;
static volatile int dbg_conn_interval = -99; /* microseconds */
static volatile int dbg_disconnect_reason = -99;
static volatile int dbg_idle_reason = -99;
static volatile int dbg_enc_key_size = -99;
static volatile int dbg_pairing_count;
static volatile int dbg_pairing_bonded = -99;
volatile int dbg_adv_preferred;
volatile int dbg_adv_fallback;

static struct k_work_delayable connect_timeout_work;
static struct k_work_delayable preferred_fallback_work;
static struct k_work_delayable pairing_timeout_work;
static struct k_work_delayable settle_work;
static struct k_work_delayable grace_work;
static struct k_work adv_work;
static struct k_work_delayable config_poll_work;
static struct k_work_delayable config_retry_work;
static struct k_work_delayable led_work;
static struct k_work_delayable pairing_disconnect_work;
static struct k_work_delayable pairing_finish_work;
static struct k_work_delayable totp_wait_work;
static atomic_t adv_restart_pending;

static bool usb_vbus_present;
static bool usb_session_active;
static bool preferred_advertising;
static int config_retry_count;
volatile int dbg_usb_vbus_ready;
volatile int dbg_usb_vbus_removed;

static void try_send(void);
static void queue_request(int slot_idx);
static bool classify_pending(void);
static void resolve_pending_config(void);

static const uint16_t appearance = CONFIG_BT_DEVICE_APPEARANCE;

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static void set_state(enum app_state s)
{
	state = s;
	dbg_state = s;
	printk("state -> %d\n", (int)s);
}

/* True when the host can receive reports: subscribed and settled. */
static bool link_ready(void)
{
	return kbd_is_subscribed() && link_settled;
}

static bool pending_valid(void)
{
	return pending.kind != PENDING_NONE;
}

static void pending_clear(void)
{
	pending.kind = PENDING_NONE;
	pending.slot_idx = -1;
}

static void sending_clear(void)
{
	sending_profile = NULL;
	sending_slot = -1;
	sending_revision = 0;
}

/* Blink the LED while in pairing mode (visual feedback); off otherwise. */
static void led_work_handler(struct k_work *work)
{
	if (state == STATE_PAIRING) {
		gpio_pin_toggle_dt(&led);
		k_work_reschedule(&led_work, K_MSEC(200));
	}
}

static void advertise(bool discoverable, bool prefer_host)
{
	struct bt_le_adv_param param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_CONN,
		BT_GAP_ADV_FAST_INT_MIN_1, BT_GAP_ADV_FAST_INT_MAX_1, NULL);
	bt_addr_le_t peer;
	struct bt_data ad[3];
	size_t ad_len = 0;
	bool preferred = false;
	int filter_err;
	int err;

	/* Always stop before changing the controller filter accept list. */
	bt_le_adv_stop();
	filter_err = bt_le_filter_accept_list_clear();
	if (filter_err != 0) {
		printk("Accept-list clear failed (err %d)\n", filter_err);
	}

	/* During the preference window, advertise normally so host background
	 * scanning policies see us, but accept a connection only from the last
	 * secured host. Identity-address entries also match resolved peer RPAs. */
	if (!discoverable) {
		param.options |= BT_LE_ADV_OPT_USE_IDENTITY;
		if (prefer_host && filter_err == 0 &&
		    profiles_current_addr(&peer)) {
			filter_err = bt_le_filter_accept_list_add(&peer);
			if (filter_err == 0) {
				param.options |= BT_LE_ADV_OPT_FILTER_CONN;
				preferred = true;
			} else {
				printk("Accept-list add failed (err %d)\n", filter_err);
				dbg_adv_fallback++;
			}
		}
	}

	ad[ad_len++] = (struct bt_data)BT_DATA_BYTES(BT_DATA_FLAGS,
		discoverable ? (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR) :
			       (BT_LE_AD_NO_BREDR));
	ad[ad_len++] = (struct bt_data)BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL));
	ad[ad_len++] = (struct bt_data)BT_DATA(BT_DATA_GAP_APPEARANCE,
		&appearance, sizeof(appearance));

	err = bt_le_adv_start(&param, ad, ad_len, sd, ARRAY_SIZE(sd));
	if (err != 0 && preferred) {
		/* A controller/filter failure must not prevent an ordinary
		 * reconnect attempt. Stop the failed role before editing the list. */
		bt_le_adv_stop();
		param.options &= ~BT_LE_ADV_OPT_FILTER_CONN;
		preferred = false;
		dbg_adv_fallback++;
		filter_err = bt_le_filter_accept_list_clear();
		if (filter_err != 0) {
			printk("Accept-list recovery failed (err %d)\n", filter_err);
		}
		err = bt_le_adv_start(&param, ad, ad_len, sd, ARRAY_SIZE(sd));
	}

	adv_result = err;
	preferred_advertising = err == 0 && preferred;
	if (preferred_advertising) {
		dbg_adv_preferred++;
		k_work_reschedule(&preferred_fallback_work,
				  K_MSEC(PREFERRED_FALLBACK_MS));
	}
	if (err) {
		printk("Advertising failed (err %d)\n", err);
	}
}

/* Stop advertising and return to sleep, disconnecting if still linked. */
static void go_idle(enum idle_reason reason)
{
	dbg_idle_reason = reason;
	pending_clear();
	kbd_cancel_send();
	sending_clear();
	link_settled = false;
	k_work_cancel_delayable(&connect_timeout_work);
	k_work_cancel_delayable(&preferred_fallback_work);
	k_work_cancel_delayable(&pairing_timeout_work);
	k_work_cancel_delayable(&settle_work);
	k_work_cancel_delayable(&grace_work);
	k_work_cancel_delayable(&totp_wait_work);
	k_work_cancel_delayable(&config_retry_work);
	config_fs_stable_reset();
	k_work_cancel_delayable(&led_work);
	k_work_cancel_delayable(&pairing_disconnect_work);
	k_work_cancel_delayable(&pairing_finish_work);
	gpio_pin_set_dt(&led, 0);
	bt_le_adv_stop();
	preferred_advertising = false;
	atomic_clear(&adv_restart_pending);
	set_state(STATE_IDLE);
	if (cur_conn) {
		bt_conn_disconnect(cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	printk("-> IDLE (sleeping)\n");
}

/* Common teardown when leaving pairing mode (success, cancel or timeout). */
static void stop_pairing_ui(void)
{
	button_set_pairing(false);
	k_work_cancel_delayable(&pairing_timeout_work);
	k_work_cancel_delayable(&led_work);
	gpio_pin_set_dt(&led, 0);
}

static void enter_pairing(void)
{
	pending_clear();
	kbd_cancel_send();
	link_settled = false;
	k_work_cancel_delayable(&connect_timeout_work);
	k_work_cancel_delayable(&preferred_fallback_work);
	k_work_cancel_delayable(&settle_work);
	k_work_cancel_delayable(&grace_work);
	k_work_cancel_delayable(&totp_wait_work);
	k_work_cancel_delayable(&config_retry_work);
	config_fs_stable_reset();
	k_work_cancel_delayable(&pairing_disconnect_work);
	k_work_cancel_delayable(&pairing_finish_work);
	atomic_clear(&adv_restart_pending);
	button_set_pairing(true);
	set_state(STATE_PAIRING);
	k_work_reschedule(&pairing_timeout_work, K_SECONDS(PAIRING_TIMEOUT_S));
	k_work_reschedule(&led_work, K_MSEC(200));

	/* Existing bonds remain valid. If one snatches this discoverable
	 * advertisement it is disconnected below and pairing resumes. */
	if (cur_conn) {
		bt_conn_disconnect(cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	} else {
		advertise(true, false);
	}
	printk("Pairing mode entered\n");
}

/* Pairing succeeded: stay connected so the host can confirm the pairing
 * before we go idle. Advertising was already stopped by the connection, so
 * do NOT call bt_le_adv_stop() here (it would block the SMP/RX context). */
static void finish_pairing(void)
{
	stop_pairing_ui();
	k_work_cancel_delayable(&pairing_finish_work);
	set_state(STATE_CONNECTED);
	printk("Pairing finished (connected)\n");
}

/* Cancel pairing (button click or timeout): disconnect and sleep. */
static void cancel_pairing(void)
{
	stop_pairing_ui();
	go_idle(IDLE_PAIRING_CANCELLED);
	printk("Pairing cancelled\n");
}

static void bt_ready(int err)
{
	bt_ready_called = 1;
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		if (usb_msc_init() != 0) {
			printk("USB MSC init failed\n");
		}
		return;
	}
	printk("Bluetooth initialized\n");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}
	/* Secrets and usage metadata do not survive a real firmware boot. Do not
	 * retain flash-backed trust records with no corresponding RAM profile. */
	err = bt_unpair(BT_ID_DEFAULT, NULL);
	if (err != 0) {
		printk("Boot bond cleanup failed (err %d)\n", err);
	}
	if (profiles_sync_bonds() != 0) {
		printk("Bond/profile sync failed\n");
	}
	if (usb_msc_init() != 0) {
		printk("USB MSC init failed\n");
	}

	/* Stay in IDLE (radio off) until the button is pressed. */
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connection failed (err 0x%02x)\n", err);
		if (state == STATE_CONNECTING) {
			go_idle(IDLE_CONN_FAILED);
		}
		return;
	}

	dbg_connected++;
	k_work_cancel_delayable(&preferred_fallback_work);
	preferred_advertising = false;
	atomic_clear(&adv_restart_pending);
	cur_conn = bt_conn_ref(conn);

	{
		struct bt_conn_info info;

		if (bt_conn_get_info(conn, &info) == 0) {
			dbg_conn_interval = info.le.interval_us; /* microseconds */
		}
	}
	printk("Connected\n");

	k_work_cancel_delayable(&connect_timeout_work);

	if (state == STATE_CONNECTING) {
		set_state(STATE_CONNECTED);
	}

	/* Do not send a Security Request here: BlueZ's HID profile (hog) already
	 * initiates encryption for HID devices, and our own Security Request
	 * produces "unexpected SMP command 0x0b" and a failed CCC write. */
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	dbg_disconnect_reason = reason;
	printk("Disconnected (reason 0x%02x)\n", reason);

	if (cur_conn) {
		bt_conn_unref(cur_conn);
		cur_conn = NULL;
	}

	/* The link is gone: any readiness is void. */
	link_settled = false;
	k_work_cancel_delayable(&settle_work);

	if (state == STATE_PAIRING) {
		/* The callback still owns a connection object. Keep pairing, but
		 * defer discoverable advertising until the object is recycled. */
		atomic_set(&adv_restart_pending, 1);
	} else if (state != STATE_IDLE) {
		go_idle(IDLE_DISCONNECTED);
	}
}

static void adv_work_handler(struct k_work *work)
{
	if (state == STATE_PAIRING || state == STATE_CONNECTING) {
		advertise(state == STATE_PAIRING, false);
	}
}

static void recycled(void)
{
	if (atomic_cas(&adv_restart_pending, 1, 0)) {
		k_work_submit(&adv_work);
	}
}

static void pairing_timeout_handler(struct k_work *work)
{
	if (state == STATE_PAIRING) {
		printk("Pairing timeout\n");
		cancel_pairing();
	}
}

static void connect_timeout_handler(struct k_work *work)
{
	if (state == STATE_CONNECTING) {
		printk("Connect timeout\n");
		go_idle(IDLE_CONNECT_TIMEOUT);
	}
}

static void preferred_fallback_handler(struct k_work *work)
{
	if (state != STATE_CONNECTING || !preferred_advertising) {
		return;
	}

	/* Stop the accept-list-filtered preference window and continue with
	 * unfiltered connectable advertising, where any retained bond may
	 * reconnect. */
	preferred_advertising = false;
	dbg_adv_fallback++;
	advertise(false, false);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	const bt_addr_le_t *peer = bt_conn_get_dst(conn);
	bool known_bond = level >= BT_SECURITY_L2 && peer &&
			  profiles_is_bonded(peer);

	dbg_sec_changed_err = err;
	dbg_sec_level = level;
	dbg_enc_key_size = bt_conn_enc_key_size(conn);

	if (err) {
		printk("Pairing failed (err %d)\n", err);
		return;
	}
	printk("Pairing success (level %u)\n", level);

	/* Defer a known-host snatch so a genuine re-pair's pairing_complete can
	 * cancel it. Never let a snatch change the preferred/current profile. */
	if (state == STATE_PAIRING && known_bond) {
		k_work_reschedule(&pairing_finish_work, K_MSEC(PAIR_SNATCH_MS));
	} else if (state != STATE_PAIRING && known_bond) {
		if (!profiles_find(peer)) {
			profiles_add(peer);
		}
		profiles_set_current(peer);
	}
}

static void pairing_finish_work_handler(struct k_work *work)
{
	if (state == STATE_PAIRING) {
		/* A bonded host reconnected ("snatch"). Stay discoverable after its
		 * disconnect callback re-submits advertising. */
		if (cur_conn) {
			bt_conn_disconnect(cur_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		}
		printk("Snatched; continuing pairing\n");
	}
}

/* Fired only on a genuine pairing (bond distributed), not a bonded reconnect. */
static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	const bt_addr_le_t *peer = bt_conn_get_dst(conn);
	bool dump_follows_limbo = dump_profile_valid &&
				 bt_addr_le_eq(&dump_profile.addr, BT_ADDR_LE_ANY);
	int err;

	dbg_pairing_count++;
	dbg_pairing_bonded = bonded;
	printk("Pairing complete (bonded %d)\n", bonded);

	if (bonded && state == STATE_PAIRING) {
		k_work_cancel_delayable(&pairing_finish_work);
		err = peer ? profiles_attach_limbo(peer) : -EINVAL;
		if (err != 0) {
			printk("Profile attach failed (err %d)\n", err);
		} else if (dump_follows_limbo) {
			/* keep:N in a pre-pairing USB dump follows the limbo contents
			 * when that same profile is attached to its first bond. */
			bt_addr_le_copy(&dump_profile.addr, peer);
		}
		finish_pairing();
		printk("Paired; profile selected\n");

		/* Brief grace so the host's pairing UI registers "paired", then
		 * disconnect and sleep. Typing happens on a later button press,
		 * which reconnects via the bond. */
		k_work_reschedule(&pairing_disconnect_work, K_MSEC(PAIR_DISCONNECT_MS));
	}
}

static void bond_deleted(uint8_t id, const bt_addr_le_t *peer)
{
	ARG_UNUSED(id);
	if (dump_profile_valid && bt_addr_le_eq(&dump_profile.addr, peer)) {
		clear_dump_secrets();
	}
	profiles_remove(peer);
}

static void pairing_disconnect_handler(struct k_work *work)
{
	if (state == STATE_CONNECTED) {
		go_idle(IDLE_PAIRING_GRACE);
	}
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.bond_deleted = bond_deleted,
};

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
};

/* --- config.ini dump (written on every USB plug, before the host mounts) --- */

#define BOND_SUMMARY_ACCOUNTS 4
static char dump_buf[CONFIG_FS_TEXT_SIZE];
static bool dump_overflow;

BUILD_ASSERT(CONFIG_BT_MAX_PAIRED == CONFIG_TOTP_MAX_BONDS,
	     "config bond references must match the Bluetooth bond pool");

/* Append a formatted line to the dump buffer. */
static void dump_append(size_t *pos, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (dump_overflow || *pos >= sizeof(dump_buf) - 1) {
		dump_overflow = true;
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(dump_buf + *pos, sizeof(dump_buf) - *pos, fmt, ap);
	va_end(ap);
	if (n < 0) {
		dump_overflow = true;
		return;
	}
	if ((size_t)n >= sizeof(dump_buf) - *pos) {
		*pos = sizeof(dump_buf) - 1;
		dump_overflow = true;
		return;
	}
	*pos += (size_t)n;
}

/* Format a signed minute offset as "+H", "+H:MM", "-H:MM" or "0". */
static void format_offset(int32_t minutes, char *out, size_t out_size)
{
	uint32_t a;
	uint32_t h;
	uint32_t m;

	if (minutes == 0) {
		snprintf(out, out_size, "0");
		return;
	}
	a = (uint32_t)(minutes < 0 ? -minutes : minutes);
	h = a / 60;
	m = a % 60;
	if (m == 0) {
		snprintf(out, out_size, "%s%u", minutes < 0 ? "-" : "+", h);
	} else {
		snprintf(out, out_size, "%s%u:%02u", minutes < 0 ? "-" : "+",
			 h, m);
	}
}

static void capture_dump_bonds(int bonds)
{
	int i;

	secure_zero(&dump_bonds, sizeof(dump_bonds));
	if (bonds < 2 || bonds > ARRAY_SIZE(dump_bonds.addrs)) {
		return;
	}
	for (i = 0; i < bonds; i++) {
		struct secret_profile *profile = profiles_at(i);

		if (!profile) {
			secure_zero(&dump_bonds, sizeof(dump_bonds));
			return;
		}
		bt_addr_le_copy(&dump_bonds.addrs[i], &profile->addr);
	}
	dump_bonds.count = bonds;
	dump_bonds.token = sys_rand64_get();
	dump_bonds.valid = true;
}

static int dump_bond_index(const bt_addr_le_t *addr)
{
	int i;

	if (!dump_bonds.valid || !addr) {
		return -1;
	}
	for (i = 0; i < dump_bonds.count; i++) {
		if (bt_addr_le_eq(addr, &dump_bonds.addrs[i])) {
			return i;
		}
	}
	return -1;
}

static void dump_snapshot_header(size_t *pos)
{
	if (time_is_valid()) {
		time_t seconds = (time_t)time_get();
		struct tm local;

		if (gmtime_r(&seconds, &local)) {
			dump_append(pos,
				    "bond_snapshot = %016llx  # taken at %04d.%02d.%02d %02d:%02d local\n",
				    (unsigned long long)dump_bonds.token,
				    local.tm_year + 1900, local.tm_mon + 1,
				    local.tm_mday, local.tm_hour, local.tm_min);
			return;
		}
	}
	dump_append(pos,
		    "bond_snapshot = %016llx  # taken while clock was not synced\n",
		    (unsigned long long)dump_bonds.token);
}

static void usage_order(const struct secret_profile *profile,
			int order[CONFIG_MAX_SLOTS])
{
	int i;

	for (i = 0; i < profile->slot_count; i++) {
		int slot = i;
		int j = i;

		while (j > 0) {
			int previous = order[j - 1];

			if (profile->usage_score[previous] >
			    profile->usage_score[slot] ||
			    (profile->usage_score[previous] ==
			     profile->usage_score[slot] && previous < slot)) {
				break;
			}
			order[j] = previous;
			j--;
		}
		order[j] = slot;
	}
}

static void dump_keep_bond(size_t *pos, int bond_idx,
			   const struct secret_profile *profile)
{
	char addr[BT_ADDR_STR_LEN];
	int order[CONFIG_MAX_SLOTS];
	int shown;
	int i;

	bt_addr_to_str(&profile->addr.a, addr, sizeof(addr));
	dump_append(pos, "keep_bond = bond_%d  # %s; accounts:",
		    bond_idx + 1, addr);
	if (!profile->have_config || profile->slot_count == 0) {
		dump_append(pos, " none\n");
		return;
	}
	usage_order(profile, order);
	shown = MIN(profile->slot_count, BOND_SUMMARY_ACCOUNTS);
	for (i = 0; i < shown; i++) {
		dump_append(pos, "%s%s", i == 0 ? " " : ", ",
			    profile->slots[order[i]].name);
	}
	if (profile->slot_count > shown) {
		dump_append(pos, ", +%d more", profile->slot_count - shown);
	}
	dump_append(pos, "\n");
}

/* Serialize the current state into config.ini so the host can read and edit it.
 * Non-secret fields are emitted verbatim; secrets are replaced by the "keep"
 * placeholder (or "error" for a broken slot). An unconfigured device emits a
 * template with a commented [account] example whenever this profile is empty. */
static size_t config_dump_text(void)
{
	struct secret_profile *profile = profiles_current();
	size_t pos = 0;
	int bonds = profiles_count();
	int i;
	char tz[16];
	int current_bond;

	/* keep:N references are resolved against exactly the profile represented
	 * by this dump, even if a live config changes the running slot order. */
	clear_dump_state();
	memcpy(&dump_profile, profile, sizeof(dump_profile));
	dump_profile_valid = true;
	capture_dump_bonds(bonds);
	dump_overflow = false;
	dump_buf[0] = '\0';

	dump_append(&pos, "# TapShard configuration\n");
	dump_append(&pos, "# Regenerated by the device on every USB plug. Edit and unplug to apply.\n");
	dump_append(&pos, "# firmware = %s\n", FW_VERSION);
	dump_append(&pos, "# clock = %s\n", time_is_valid() ? "synced" : "NOT SYNCED");
	if (time_is_valid()) {
		dump_append(&pos, "# last_anchor_local = %llu\n",
			    (unsigned long long)time_get_epoch_base());
		if (last_anchor_name[0] != '\0') {
			dump_append(&pos, "# last_anchor_file = %s\n",
				    last_anchor_name);
		}
	}
	dump_append(&pos, "# bonded = %s\n", bonds > 0 ? "yes" : "no");
	dump_append(&pos, "# bonds = %d\n", bonds);
#if IS_ENABLED(CONFIG_TOTP_BATTERY_MONITOR)
	if (battery_is_measured()) {
		dump_append(&pos, "# battery = %umV\n",
			    (unsigned)battery_mv());
		dump_append(&pos, "# battery_low = %s\n",
			    battery_is_low() ? "yes" : "no");
	} else {
		dump_append(&pos, "# battery = n/a\n");
	}
#endif
	dump_append(&pos, "\n");

	dump_append(&pos, "[device]\n");
	format_offset(device_settings.timezone_min, tz, sizeof(tz));
	dump_append(&pos, "timezone = %s\n", tz);
	if (dump_bonds.valid) {
		dump_append(&pos, "\n");
		dump_append(&pos, "# Bond editing is opt-in. Delete keep_bond lines, then set true.\n");
		dump_snapshot_header(&pos);
		dump_append(&pos, "edit_bonds = false\n");
		for (i = 0; i < dump_bonds.count; i++) {
			dump_keep_bond(&pos, i, profiles_at(i));
		}
		current_bond = dump_bond_index(&profile->addr);
		if (current_bond >= 0) {
			dump_append(&pos, "host_id = bond_%d\n", current_bond + 1);
		} else {
			dump_append(&pos, "# host_id = REQUIRED: choose bond above\n");
		}
		dump_append(&pos, "\n");
	}
	dump_append(&pos, "conn_settle_time = %ums\n",
		    (unsigned)device_settings.settle_ms);
	dump_append(&pos, "inter_key_time = %ums\n",
		    (unsigned)device_settings.inter_key_ms);
	dump_append(&pos, "drift_ppm = %d\n", (int)time_get_drift());
	dump_append(&pos, "long_press_ms = %u\n",
		    (unsigned)device_settings.long_press_ms);
	dump_append(&pos, "click_window_ms = %u\n",
		    (unsigned)device_settings.click_window_ms);
	dump_append(&pos, "grace_ms = %u\n",
		    (unsigned)device_settings.grace_ms);
	dump_append(&pos, "low_battery_mv = %u\n",
		    (unsigned)device_settings.low_battery_mv);
	dump_append(&pos, "\n");

	if (profile->have_config && profile->slot_count > 0) {
		for (i = 0; i < profile->slot_count; i++) {
			struct totp_slot *slot = &profile->slots[i];

			dump_append(&pos, "[account]\n");
			dump_append(&pos, "name = %s\n", slot->name);
			if (slot->secret_error) {
				dump_append(&pos, "secret = error\n");
			} else {
				dump_append(&pos, "secret = keep:%d\n", i + 1);
			}
			dump_append(&pos, "digits = %u\n", (unsigned)slot->digits);
			dump_append(&pos, "period = %u\n", (unsigned)slot->period);
			dump_append(&pos, "enter = %s\n",
				    slot->enter ? "true" : "false");
			dump_append(&pos, "\n");
		}
	} else {
		dump_append(&pos, "# Add one [account] section per TOTP account.\n");
		dump_append(&pos, "# 1 click = first account, 2 = second, ...\n");
		dump_append(&pos, "# [account]\n");
		dump_append(&pos, "# name = example\n");
		dump_append(&pos, "# secret = YOUR_BASE32_SECRET\n");
		dump_append(&pos, "# digits = 6\n");
		dump_append(&pos, "# period = 30\n");
		dump_append(&pos, "# enter = false\n");
	}

	/* Remember what drift_ppm we wrote, to detect a user edit on read-back. */
	dump_drift_ppm = time_get_drift();
	return pos;
}

static void config_dump(void)
{
	config_dump_text();
	if (dump_overflow) {
		printk("config dump exceeds %u bytes\n",
		       (unsigned)sizeof(dump_buf));
	} else if (config_fs_write_config(dump_buf) != 0) {
		printk("config dump failed\n");
	}
}

static int validate_bond_config(const struct totp_config *cfg)
{
	uint8_t valid_refs;
	bool uses_refs = cfg->have_host_id || cfg->keep_bonds != 0;
	int i;

	if (!uses_refs && !cfg->edit_bonds) {
		return 0;
	}
	if (!cfg->have_bond_snapshot || !dump_bonds.valid ||
	    cfg->bond_snapshot != dump_bonds.token) {
		snprintf(config_err_msg, sizeof(config_err_msg),
			 "bond snapshot is stale or missing");
		return -EINVAL;
	}
	valid_refs = BIT(dump_bonds.count) - 1U;
	if ((cfg->keep_bonds & ~valid_refs) != 0 ||
	    (cfg->have_host_id && cfg->host_id > dump_bonds.count)) {
		snprintf(config_err_msg, sizeof(config_err_msg),
			 "bond reference is not in this snapshot");
		return -EINVAL;
	}
	if (cfg->edit_bonds) {
		for (i = 0; i < dump_bonds.count; i++) {
			if ((cfg->keep_bonds & BIT(i)) != 0 &&
			    !profiles_find(&dump_bonds.addrs[i])) {
				snprintf(config_err_msg, sizeof(config_err_msg),
					 "bond_%d is no longer bonded", i + 1);
				return -EINVAL;
			}
		}
	}
	return 0;
}

static int config_target(const struct totp_config *cfg,
			 struct secret_profile **target, bool *delete_target)
{
	int bonds = profiles_count();

	*target = NULL;
	*delete_target = false;
	if (cfg->have_host_id) {
		uint8_t mask = BIT(cfg->host_id - 1);

		*target = profiles_find(&dump_bonds.addrs[cfg->host_id - 1]);
		*delete_target = cfg->edit_bonds && (cfg->keep_bonds & mask) == 0;
		if (!*target && !*delete_target) {
			snprintf(config_err_msg, sizeof(config_err_msg),
				 "host_id is no longer bonded");
			return -EINVAL;
		}
		return 0;
	}
	if (bonds == 0) {
		*target = profiles_limbo();
		return 0;
	}
	if (bonds == 1) {
		*target = profiles_only();
		return 0;
	}
	/* A simple dump made with zero/one bond remains unambiguous if another
	 * host is paired while that USB session is still mounted. The protected
	 * dump identity follows limbo on its first pairing; never retarget these
	 * edits merely because a different host connected in the meantime. */
	if (!dump_bonds.valid && dump_profile_valid) {
		*target = profiles_find(&dump_profile.addr);
		if (*target) {
			return 0;
		}
	}
	snprintf(config_err_msg, sizeof(config_err_msg),
		 "host_id is required with multiple bonds");
	return -EINVAL;
}

static bool same_profile_identity(const struct secret_profile *a,
				  const struct secret_profile *b)
{
	return bt_addr_le_eq(&a->addr, &b->addr);
}

static bool same_slot_secret(const struct totp_slot *a,
			     const struct totp_slot *b)
{
	return a->secret_len > 0 && a->secret_len == b->secret_len &&
	       !a->secret_error && !b->secret_error &&
	       memcmp(a->secret, b->secret, a->secret_len) == 0;
}

static void preserve_usage_scores(const struct secret_profile *target,
				  const struct totp_config *cfg,
				  const uint8_t source_ref[CONFIG_MAX_SLOTS],
				  uint16_t scores[CONFIG_MAX_SLOTS])
{
	uint16_t used = 0;
	int i;

	for (i = 0; i < cfg->slot_count; i++) {
		int match = -1;
		int j;

		/* Prefer the exact named account, then any unused slot carrying the
		 * same secret. The latter preserves usage across a rename. */
		for (j = 0; j < target->slot_count; j++) {
			if ((used & BIT(j)) == 0 &&
			    strcmp(cfg->slots[i].name, target->slots[j].name) == 0 &&
			    same_slot_secret(&cfg->slots[i], &target->slots[j])) {
				match = j;
				break;
			}
		}
		if (match < 0) {
			for (j = 0; j < target->slot_count; j++) {
				if ((used & BIT(j)) == 0 &&
				    same_slot_secret(&cfg->slots[i], &target->slots[j])) {
					match = j;
					break;
				}
			}
		}
		if (match >= 0) {
			scores[i] = target->usage_score[match];
			used |= BIT(match);
		} else if (source_ref[i] > 0) {
			scores[i] = dump_profile.usage_score[source_ref[i] - 1];
		}
	}
}

/* Parse config.ini into the running config. Called on unplug (write is
 * guaranteed complete) and on click while USB is still connected. Distinguishes
 * "no file" (unconfigured) from a file that failed to parse ("000"). */
static int config_sync(bool require_stable)
{
	struct totp_config cfg;
	struct secret_profile *target = profiles_current();
	uint16_t usage_score[CONFIG_MAX_SLOTS] = { 0 };
	uint8_t source_ref[CONFIG_MAX_SLOTS] = { 0 };
	bool delete_target;
	int err;
	int i;

	err = require_stable ? config_fs_read_stable(&cfg) :
			       config_fs_read(&cfg);
	if (err == -EAGAIN) {
		secure_zero(&cfg, sizeof(cfg));
		secure_zero(source_ref, sizeof(source_ref));
		return err;
	}
	if (err == -ENOENT) {
		/* No config.ini: unconfigured (not an error). */
		profiles_clear_contents(target);
		secure_zero(&cfg, sizeof(cfg));
		secure_zero(source_ref, sizeof(source_ref));
		return 0;
	}
	if (err != 0) {
		/* config.ini present but unreadable/invalid. */
		if (err == -EFBIG) {
			snprintf(config_err_msg, sizeof(config_err_msg),
				 "config.ini is too large");
		} else if (err != -EINVAL) {
			snprintf(config_err_msg, sizeof(config_err_msg),
				 "config.ini read/media failure");
		}
		target->config_error = true;
		printk("Config error: %s\n", config_err_msg[0] ? config_err_msg :
		       "read/media failure");
		secure_zero(&cfg, sizeof(cfg));
		secure_zero(source_ref, sizeof(source_ref));
		return err;
	}
	err = validate_bond_config(&cfg);
	if (err == 0) {
		err = config_target(&cfg, &target, &delete_target);
	}
	if (err != 0) {
		profiles_current()->config_error = true;
		secure_zero(&cfg, sizeof(cfg));
		secure_zero(usage_score, sizeof(usage_score));
		secure_zero(source_ref, sizeof(source_ref));
		return -EINVAL;
	}

	/* Resolve keep:N against the protected profile snapshot that produced the
	 * current dump. Reordering accounts changes destinations, not sources. */
	for (i = 0; !delete_target && i < cfg.slot_count; i++) {
		struct totp_slot *slot = &cfg.slots[i];
		int ref_idx;

		if (slot->secret_ref == 0) {
			continue;
		}
		ref_idx = slot->secret_ref - 1;
		source_ref[i] = slot->secret_ref;
		slot->secret_ref = 0;
		if (!dump_profile_valid ||
		    !same_profile_identity(target, &dump_profile)) {
			snprintf(config_err_msg, sizeof(config_err_msg),
				 "secret keep:%d crosses host profiles", ref_idx + 1);
			target->config_error = true;
			secure_zero(&cfg, sizeof(cfg));
			secure_zero(usage_score, sizeof(usage_score));
			secure_zero(source_ref, sizeof(source_ref));
			return -EINVAL;
		}
		if (ref_idx >= dump_profile.slot_count ||
		    dump_profile.slots[ref_idx].secret_len == 0 ||
		    dump_profile.slots[ref_idx].secret_error) {
			snprintf(config_err_msg, sizeof(config_err_msg),
				 "secret keep:%d has no source", ref_idx + 1);
			target->config_error = true;
			secure_zero(&cfg, sizeof(cfg));
			secure_zero(usage_score, sizeof(usage_score));
			secure_zero(source_ref, sizeof(source_ref));
			return -EINVAL;
		}
		memcpy(slot->secret, dump_profile.slots[ref_idx].secret,
		       dump_profile.slots[ref_idx].secret_len);
		slot->secret_len = dump_profile.slots[ref_idx].secret_len;
	}
	if (!delete_target) {
		preserve_usage_scores(target, &cfg, source_ref, usage_score);
	}

	/* Apply a user-edited drift_ppm only (a stale dump value would clobber a
	 * fresh auto-calibration). */
	if (cfg.device.drift_ppm != dump_drift_ppm) {
		time_set_drift(cfg.device.drift_ppm);
	}

	device_settings = cfg.device;
	if (!delete_target) {
		profiles_clear_contents(target);
		memcpy(target->slots, cfg.slots, sizeof(cfg.slots));
		memcpy(target->usage_score, usage_score, sizeof(usage_score));
		target->slot_count = cfg.slot_count;
		target->have_config = true;
	}
	button_set_timings(cfg.device.click_window_ms,
			   cfg.device.long_press_ms);
#if IS_ENABLED(CONFIG_TOTP_BATTERY_MONITOR)
	battery_set_low_threshold(cfg.device.low_battery_mv);
#endif
	if (cfg.edit_bonds) {
		int first_unpair_err = 0;
		int first_unpair_idx = -1;

		for (i = 0; i < dump_bonds.count; i++) {
			if ((cfg.keep_bonds & BIT(i)) != 0) {
				continue;
			}
			err = bt_unpair(BT_ID_DEFAULT, &dump_bonds.addrs[i]);
			if (err != 0) {
				if (first_unpair_err == 0) {
					first_unpair_err = err;
					first_unpair_idx = i;
				}
			}
		}
		if (first_unpair_err != 0) {
			snprintf(config_err_msg, sizeof(config_err_msg),
				 "failed to forget bond_%d", first_unpair_idx + 1);
			printk("Config error: %s (err %d)\n",
			       config_err_msg, first_unpair_err);
			secure_zero(&cfg, sizeof(cfg));
			secure_zero(usage_score, sizeof(usage_score));
			secure_zero(source_ref, sizeof(source_ref));
			return -EIO;
		}
	}
	secure_zero(&cfg, sizeof(cfg));
	secure_zero(usage_score, sizeof(usage_score));
	secure_zero(source_ref, sizeof(source_ref));
	return 0;
}

static bool same_time_candidate(const struct config_fs_change *change)
{
	return time_candidate.valid &&
	       time_candidate.change.mtime == change->mtime &&
	       time_candidate.change.name_hash == change->name_hash &&
	       time_candidate.change.size == change->size;
}

/* Confirm a session-local host timestamp in two polls. A final scan after USB
 * disable may confirm an already staged candidate, but never promotes a file
 * first noticed only after unplug. */
static void time_sync(bool final_scan)
{
	struct config_fs_change change;
	int err;

	err = config_fs_latest_session_change(&change);
	if (err != 0) {
		if (!final_scan) {
			time_candidate.valid = false;
		}
		return;
	}
	if (change.mtime <= session_anchor_mtime) {
		time_candidate.valid = false;
		return;
	}
	if (change.mtime < TIME_SANE_MIN || change.mtime > TIME_SANE_MAX) {
		time_candidate.valid = false;
		return;
	}

	if (!same_time_candidate(&change)) {
		if (!final_scan) {
			time_candidate.change = change;
			time_candidate.first_seen_ms = k_uptime_get();
			time_candidate.valid = true;
		}
		return;
	}

	time_set_observed(change.mtime, time_candidate.first_seen_ms);
	session_anchor_mtime = change.mtime;
	strncpy(last_anchor_name, change.name, sizeof(last_anchor_name) - 1);
	last_anchor_name[sizeof(last_anchor_name) - 1] = '\0';
	time_candidate.valid = false;
	try_send();
}

static void config_poll_handler(struct k_work *work)
{
	if (!usb_vbus_present) {
		int sync_err;
		int wipe_err;

		/* Final sync after unplug: the host has finished writing, so
		 * parse the config, re-anchor to the final mtime, then physically
		 * erase and reformat the disk. On error, create a diagnostic log
		 * on the new filesystem for the next plug. */
		sync_err = config_sync(false);
		time_sync(true);
		time_usb_session_end();
		wipe_err = config_fs_wipe();
		if (wipe_err != 0) {
			printk("RAM disk wipe/format failed\n");
		} else if (sync_err != 0) {
			config_fs_log(config_err_msg);
		}
		clear_dump_state();
		resolve_pending_config();
		usb_session_active = false;
		return;
	}

	/* While plugged in: track only session-local, stable clock candidates.
	 * Config parsing is driven by a click and has its own stability check. */
	time_sync(false);
	k_work_reschedule(&config_poll_work, K_MSEC(CONFIG_POLL_MS));
}

static void config_retry_handler(struct k_work *work)
{
	int err;

	if (pending.kind != PENDING_CONFIG_READ || !usb_vbus_present) {
		return;
	}
	if (!usb_msc_is_connected()) {
		err = -EAGAIN;
	} else {
		err = config_sync(true);
	}
	if (err == -EAGAIN || err == -EIO) {
		if (++config_retry_count < CONFIG_STABLE_RETRIES) {
			k_work_reschedule(&config_retry_work,
					  K_MSEC(CONFIG_STABLE_RETRY_MS));
			return;
		}
		go_idle(IDLE_CONFIG_UNSTABLE);
		return;
	}

	resolve_pending_config();
}

/* Called (workqueue context) on VBUS change. On plug, dump config.ini before
 * the USB device is enabled (see usb_msc.c) so the host mounts a volume that
 * already contains it. */
static void usb_vbus_cb(bool present)
{
	usb_vbus_present = present;

#if IS_ENABLED(CONFIG_TOTP_BATTERY_MONITOR)
	battery_set_usb_present(present);
#endif

	if (present) {
		dbg_usb_vbus_ready++;
		/* A short low/high transition belongs to the session whose
		 * deferred removal has not committed yet. Do not overwrite the
		 * host's config with a fresh dump during that bounce. */
		if (usb_session_active) {
			k_work_reschedule(&config_poll_work,
					  K_MSEC(CONFIG_POLL_MS));
			return;
		}
		usb_session_active = true;
		time_usb_session_begin();
		config_fs_stable_reset();
		memset(&time_candidate, 0, sizeof(time_candidate));
		session_anchor_mtime = 0;
		config_dump();
		if (config_fs_capture_baseline() != 0) {
			printk("root baseline capture failed\n");
		}
		k_work_reschedule(&config_poll_work, K_MSEC(CONFIG_POLL_MS));
	} else {
		dbg_usb_vbus_removed++;
#if IS_ENABLED(CONFIG_TOTP_BATTERY_MONITOR)
		battery_resample();
#endif
		k_work_cancel_delayable(&config_retry_work);
		config_fs_stable_reset();
		k_work_cancel_delayable(&config_poll_work);
		/* Delay final sync just enough to absorb a VBUS removal bounce. */
		k_work_schedule(&config_poll_work,
				K_MSEC(USB_REMOVE_DEBOUNCE_MS));
	}
}

/* Compute the TOTP code for a slot into @out (does not touch pending). */
static bool generate_code(int slot_idx, char *out, size_t out_len)
{
	struct secret_profile *profile = profiles_current();
	struct totp_slot *slot;
	int64_t utc;

	if (!profile->have_config || !time_is_valid() || slot_idx < 0 ||
	    slot_idx >= profile->slot_count) {
		return false;
	}

	slot = &profile->slots[slot_idx];

	/* UTC = local - timezone (offset in "UTC+N" notation, signed minutes). */
	utc = (int64_t)time_get() - (int64_t)device_settings.timezone_min * 60;
	if (utc < 0) {
		return false;
	}

	if (totp_generate(slot->secret, slot->secret_len, (uint64_t)utc,
			  slot->period, slot->digits, out, out_len) != 0) {
		printk("TOTP generation failed\n");
		return false;
	}

	return true;
}

/* Preserve the requested slot until a secured peer is selected. Reconnect can
 * fall back to a different profile than the one preferred at button press. */
static void queue_request(int slot_idx)
{
	pending.kind = PENDING_SLOT;
	pending.slot_idx = slot_idx;
}

/* Resolve a raw slot request against the profile belonging to the connected
 * peer. Reports errors/hints instead of a real slot when appropriate:
 *   - config_error:        "000"  (config.ini present but invalid)
 *   - not configured:      "0000" (no config.ini / no accounts)
 *   - slot secret broken:  "000"  (this slot's secret is unresolved/error)
 * Returns false only when the requested slot is invalid (should go idle). */
static bool classify_pending(void)
{
	struct secret_profile *profile = profiles_current();
	int slot_idx;

	if (pending.kind != PENDING_SLOT) {
		return true;
	}
	slot_idx = pending.slot_idx;

	if (profile->config_error) {
		pending.kind = PENDING_CONFIG_ERROR;
		pending.slot_idx = -1;
		return true;
	}
	if (!profile->have_config || profile->slot_count == 0) {
		pending.kind = PENDING_UNCONFIGURED;
		pending.slot_idx = -1;
		return true;
	}
	if (slot_idx < 0 || slot_idx >= profile->slot_count) {
		return false;
	}
	if (profile->slots[slot_idx].secret_error) {
		pending.kind = PENDING_CONFIG_ERROR;
		pending.slot_idx = -1;
		return true;
	}

	pending.kind = PENDING_SLOT;
	pending.slot_idx = slot_idx;
	return true;
}

static void resolve_pending_config(void)
{
	int slot_idx;

	if (pending.kind != PENDING_CONFIG_READ) {
		return;
	}
	slot_idx = pending.slot_idx;
	queue_request(slot_idx);
	try_send();
}

static bool slow_typing(void)
{
#if IS_ENABLED(CONFIG_TOTP_BATTERY_MONITOR)
	return battery_is_low() && !usb_vbus_present;
#else
	return false;
#endif
}

/* Return how long to wait before generating this TOTP, or zero if the current
 * period has enough time left to type it completely. */
static uint32_t totp_boundary_wait_ms(void)
{
	struct secret_profile *profile = profiles_current();
	struct totp_slot *slot;
	int64_t utc;
	uint64_t remaining_ms;
	uint64_t typing_ms;

	if (pending.kind != PENDING_SLOT || !time_is_valid() ||
	    pending.slot_idx < 0 || pending.slot_idx >= profile->slot_count) {
		return 0;
	}

	slot = &profile->slots[pending.slot_idx];
	utc = (int64_t)time_get() - (int64_t)device_settings.timezone_min * 60;
	if (utc < 0 || slot->period == 0) {
		return 0;
	}

	typing_ms = kbd_send_duration_ms(slot->digits, slot->enter,
					device_settings.inter_key_ms, slow_typing());
	remaining_ms = (slot->period - ((uint64_t)utc % slot->period)) * 1000u;
	if (remaining_ms > typing_ms + TOTP_SEND_MARGIN_MS) {
		return 0;
	}

	/* time_get() has whole-second resolution. Waiting through the computed
	 * boundary plus a small offset guarantees the next call sees the new
	 * counter even when the real clock is near the start of that second. */
	return (uint32_t)MIN(remaining_ms + 50u, UINT32_MAX);
}

static void send_complete(int err)
{
	if (state != STATE_SENDING) {
		sending_clear();
		return;
	}
	if (err != 0) {
		sending_clear();
		printk("HID send failed (err %d); click to retry\n", err);
		go_idle(IDLE_SEND_FAILED);
		return;
	}
	if (sending_profile && sending_slot >= 0 &&
	    sending_profile->content_revision == sending_revision) {
		profiles_record_use(sending_profile, sending_slot);
	}
	sending_clear();

	set_state(STATE_GRACE);
	k_work_reschedule(&grace_work, K_MSEC(device_settings.grace_ms));
	printk("-> GRACE\n");
}

/* Generate/snapshot and start the pending request. HID report completion drives
 * the non-blocking sender; the code buffer can be cleared immediately. */
static bool type_now(void)
{
	struct secret_profile *profile = profiles_current();
	char code[16];
	bool enter = false;
	int err;
	int slot_idx = -1;

	sending_clear();

	switch (pending.kind) {
	case PENDING_CONFIG_ERROR:
		strcpy(code, "000");
		break;
	case PENDING_UNCONFIGURED:
		strcpy(code, "0000");
		break;
	case PENDING_SLOT:
		if (pending.slot_idx < 0 ||
		    pending.slot_idx >= profile->slot_count ||
		    !generate_code(pending.slot_idx, code, sizeof(code))) {
			return false;
		}
		enter = profile->slots[pending.slot_idx].enter;
		slot_idx = pending.slot_idx;
		break;
	case PENDING_NONE:
	default:
		return false;
	}

#if IS_ENABLED(CONFIG_TOTP_DEBUG)
	strcpy(last_code, code);
	printk("Typing code %s%s\n", code, enter ? " + Enter" : "");
#else
	printk("Typing code%s\n", enter ? " + Enter" : "");
#endif
	if (slot_idx >= 0) {
		sending_profile = profile;
		sending_slot = slot_idx;
		sending_revision = profile->content_revision;
	}
	pending_clear();

	err = kbd_send_code(code, enter, device_settings.inter_key_ms,
			    slow_typing(), send_complete);
	secure_zero(code, sizeof(code));
	if (err != 0) {
		sending_clear();
	}
	return err == 0;
}

/* Single evaluation point: type if the link is ready and a request is pending.
 * Handles both the normal path (CONNECTED) and a re-click during GRACE. */
static void try_send(void)
{
	uint32_t wait_ms;

	if (!pending_valid() || !link_ready()) {
		return;
	}
	if (state != STATE_CONNECTED && state != STATE_GRACE) {
		return;
	}
	if (pending.kind == PENDING_CONFIG_READ) {
		return;
	}
	if (!classify_pending()) {
		printk("No code for slot %d; going idle\n",
		       pending.slot_idx + 1);
		go_idle(IDLE_INVALID_SLOT);
		return;
	}

	if (pending.kind == PENDING_SLOT && !time_is_valid()) {
		if (usb_vbus_present) {
			/* Give the USB timestamp poll a brief chance to anchor a newly
			 * written first configuration. */
			k_work_reschedule(&totp_wait_work, K_SECONDS(1));
			return;
		}
		go_idle(IDLE_INVALID_CLOCK);
		return;
	}

	wait_ms = totp_boundary_wait_ms();
	if (wait_ms > 0) {
		k_work_cancel_delayable(&grace_work);
		if (state == STATE_GRACE) {
			set_state(STATE_CONNECTED);
		}
		k_work_reschedule(&totp_wait_work, K_MSEC(wait_ms));
		return;
	}

	k_work_cancel_delayable(&totp_wait_work);
	k_work_cancel_delayable(&grace_work);
	set_state(STATE_SENDING);
	if (!type_now()) {
		go_idle(IDLE_SEND_FAILED);
	}
}

static void totp_wait_handler(struct k_work *work)
{
	if (pending.kind == PENDING_SLOT && !time_is_valid()) {
		go_idle(IDLE_INVALID_CLOCK);
		return;
	}
	try_send();
}

/* Grace period elapsed: disconnect and sleep. */
static void grace_work_handler(struct k_work *work)
{
	if (state == STATE_GRACE) {
		go_idle(IDLE_GRACE);
	}
}

/* Link became "ready" (settle elapsed, or fast-forwarded by the LED write). */
static void settle_work_handler(struct k_work *work)
{
	link_settled = true;
	try_send();
}

/* Called (ATT RX context) when the host subscribes to notifications: start the
 * settle timer; the host is not considered ready until it fires (or the LED
 * report write fast-forwards it). */
static void on_subscribed(void)
{
	k_work_reschedule(&settle_work, K_MSEC(device_settings.settle_ms));
}

/* Called (ATT RX context) when the host writes the output (LED) report: the
 * host's HID stack is up, so skip the rest of the settle. */
static void on_output_write(void)
{
	k_work_reschedule(&settle_work, K_NO_WAIT);
}

static int app_selftest(void)
{
	struct totp_config cfg = { 0 };
	struct secret_profile *target;
	struct secret_profile *first;
	struct secret_profile *profile;
	bt_addr_le_t capacity_addr;
	bt_addr_le_t addr1;
	bt_addr_le_t addr2;
	int order[CONFIG_MAX_SLOTS];
	uint16_t scores[CONFIG_MAX_SLOTS] = { 0 };
	uint8_t refs[CONFIG_MAX_SLOTS] = { 0 };
	size_t dump_pos = 0;
	bool delete_target;
	int i;
	int j;
	int result = -1;

	profiles_init();
	profile = profiles_current();
	/* Saving device settings without an account still needs the template on
	 * the next dump. Its commented example must not create an actual slot. */
	for (i = 0; i < 2; i++) {
		dump_pos = config_dump_text();
		if (dump_overflow || !strstr(dump_buf, "# [account]\n") ||
		    config_parse(dump_buf, dump_pos, &cfg) != 0 || cfg.slot_count != 0) {
			goto out;
		}
		profile->have_config = true;
		profile->slot_count = cfg.slot_count;
	}
	secure_zero(&cfg, sizeof(cfg));
	profiles_init();
	clear_dump_state();
	dump_pos = 0;
	if (bt_addr_le_from_str("01:23:45:67:89:AB", "public", &addr1) != 0 ||
	    bt_addr_le_from_str("C1:23:45:67:89:AB", "random", &addr2) != 0 ||
	    profiles_add(&addr1) != 0) {
		goto out;
	}
	first = profiles_find(&addr1);
	if (CONFIG_TOTP_MAX_BONDS >= 2) {
		if (profiles_add(&addr2) != 0) {
			goto out;
		}
		bt_addr_le_copy(&dump_bonds.addrs[0], &addr1);
		bt_addr_le_copy(&dump_bonds.addrs[1], &addr2);
		dump_bonds.count = 2;
		dump_bonds.token = 0x7c19e2a4d6314f08ULL;
		dump_bonds.valid = true;

		cfg.bond_snapshot = dump_bonds.token;
		cfg.have_bond_snapshot = true;
		cfg.host_id = 2;
		cfg.have_host_id = true;
		cfg.keep_bonds = BIT(0) | BIT(1);
		if (validate_bond_config(&cfg) != 0 ||
		    config_target(&cfg, &target, &delete_target) != 0 ||
		    delete_target || target != profiles_find(&addr2)) {
			goto out;
		}
		cfg.edit_bonds = true;
		cfg.keep_bonds = BIT(0);
		if (config_target(&cfg, &target, &delete_target) != 0 ||
		    !delete_target) {
			goto out;
		}
		cfg.bond_snapshot++;
		if (validate_bond_config(&cfg) == 0) {
			goto out;
		}

		/* If a second bond appears after a simple USB dump, that dump still
		 * targets the original profile. A dump that began multi-bond must
		 * keep requiring its explicit host_id. */
		secure_zero(&cfg, sizeof(cfg));
		bt_addr_le_copy(&dump_profile.addr, &addr1);
		dump_profile_valid = true;
		dump_bonds.valid = false;
		if (config_target(&cfg, &target, &delete_target) != 0 ||
		    delete_target || target != first) {
			goto out;
		}
		dump_bonds.valid = true;
		if (config_target(&cfg, &target, &delete_target) == 0) {
			goto out;
		}
	} else {
		/* Reduced-capacity HIL builds must still pass their boot tests. */
		if (config_target(&cfg, &target, &delete_target) != 0 ||
		    delete_target || target != first) {
			goto out;
		}
	}

	first->slot_count = 5;
	first->have_config = true;
	strcpy(first->slots[0].name, "Alpha");
	strcpy(first->slots[1].name, "Bravo");
	strcpy(first->slots[2].name, "Charlie");
	strcpy(first->slots[3].name, "Delta");
	strcpy(first->slots[4].name, "Echo");
	first->usage_score[0] = 10;
	first->usage_score[1] = 30;
	first->usage_score[2] = 30;
	first->usage_score[3] = 20;
	first->usage_score[4] = 5;
	usage_order(first, order);
	if (order[0] != 1 || order[1] != 2 || order[2] != 3 || order[3] != 0) {
		goto out;
	}
	dump_buf[0] = '\0';
	dump_overflow = false;
	dump_keep_bond(&dump_pos, 0, first);
	if (dump_overflow ||
	    strcmp(dump_buf,
		   "keep_bond = bond_1  # 01:23:45:67:89:AB; accounts: "
		   "Bravo, Charlie, Delta, Alpha, +1 more\n") != 0) {
		goto out;
	}
	first->slots[0].secret[0] = 0x11;
	first->slots[0].secret_len = 1;
	first->slots[1].secret[0] = 0x22;
	first->slots[1].secret_len = 1;
	cfg.slot_count = 2;
	strcpy(cfg.slots[0].name, "renamed");
	cfg.slots[0].secret[0] = 0x22;
	cfg.slots[0].secret_len = 1;
	cfg.slots[1].secret[0] = 0x11;
	cfg.slots[1].secret_len = 1;
	refs[0] = 2;
	refs[1] = 1;
	preserve_usage_scores(first, &cfg, refs, scores);
	if (scores[0] != 30 || scores[1] != 10) {
		goto out;
	}

	/* A raw click follows the host that actually connects, not the profile
	 * that happened to be preferred when the button was pressed. */
	if (CONFIG_TOTP_MAX_BONDS >= 2) {
		if (profiles_set_current(&addr2) != 0) {
			goto out;
		}
	}
	queue_request(0);
	if (profiles_set_current(&addr1) != 0 || !classify_pending() ||
	    pending.kind != PENDING_SLOT || pending.slot_idx != 0) {
		goto out;
	}
	pending_clear();

	/* Exercise the largest production dump: four hosts, each with the
	 * maximum account count and maximum-length displayed names. */
	if (CONFIG_TOTP_MAX_BONDS == 4) {
		profiles_init();
		for (i = 0; i < CONFIG_TOTP_MAX_BONDS; i++) {
			secure_zero(&capacity_addr, sizeof(capacity_addr));
			capacity_addr.type = (i & 1) ? BT_ADDR_LE_RANDOM :
							 BT_ADDR_LE_PUBLIC;
			capacity_addr.a.val[0] = (uint8_t)(i + 1);
			capacity_addr.a.val[5] = (i & 1) ? 0xc0 : 0x00;
			if (profiles_add(&capacity_addr) != 0) {
				goto out;
			}
			profile = profiles_find(&capacity_addr);
			profile->have_config = true;
			profile->slot_count = CONFIG_MAX_SLOTS;
			for (j = 0; j < CONFIG_MAX_SLOTS; j++) {
				strcpy(profile->slots[j].name,
				       "1234567890123456789012345678901");
				profile->slots[j].secret[0] = (uint8_t)(i + j + 1);
				profile->slots[j].secret_len = 1;
				profile->slots[j].digits = 8;
				profile->slots[j].period = 30;
				profile->usage_score[j] = (uint16_t)(j + 1);
			}
		}
		profile = profiles_at(0);
		if (!profile || profiles_set_current(&profile->addr) != 0) {
			goto out;
		}
		config_device_defaults(&device_settings);
		dump_pos = config_dump_text();
		if (dump_overflow || strlen(dump_buf) >= CONFIG_FS_TEXT_SIZE ||
		    strstr(dump_buf, "# [account]\n")) {
			goto out;
		}
		if (dump_pos != strlen(dump_buf)) {
			goto out;
		}
	}
	result = 0;
out:
	pending_clear();
	profiles_init();
	clear_dump_state();
	secure_zero(&cfg, sizeof(cfg));
	secure_zero(&addr1, sizeof(addr1));
	secure_zero(&addr2, sizeof(addr2));
	secure_zero(&capacity_addr, sizeof(capacity_addr));
	secure_zero(order, sizeof(order));
	secure_zero(scores, sizeof(scores));
	secure_zero(refs, sizeof(refs));
	secure_zero(dump_buf, sizeof(dump_buf));
	dump_overflow = false;
	config_err_msg[0] = '\0';
	return result;
}

/* Hardcoded test config (no USB needed) + fixed time. */
#if IS_ENABLED(CONFIG_TOTP_DEBUG)
static void load_test_config(void)
{
	static const char b32[] = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
	struct secret_profile *profile = profiles_current();
	uint8_t secret[64];
	int len;

	len = base32_decode(b32, secret, sizeof(secret));
	if (len <= 0) {
		secure_zero(secret, sizeof(secret));
		return;
	}

	config_device_defaults(&device_settings);
	profiles_clear_contents(profile);
	profile->slot_count = 2;

	strcpy(profile->slots[0].name, "Google");
	memcpy(profile->slots[0].secret, secret, len);
	profile->slots[0].secret_len = len;
	profile->slots[0].digits = 6;
	profile->slots[0].period = 30;
	profile->slots[0].enter = true;

	strcpy(profile->slots[1].name, "GitHub");
	memcpy(profile->slots[1].secret, secret, len);
	profile->slots[1].secret_len = len;
	profile->slots[1].digits = 8;
	profile->slots[1].period = 60;
	profile->slots[1].enter = false;

	button_set_timings(device_settings.click_window_ms,
			   device_settings.long_press_ms);
	profile->have_config = true;
	profile->config_error = false;
	time_set(1788400000ULL);
	secure_zero(secret, sizeof(secret));
}
#endif /* CONFIG_TOTP_DEBUG */

/* Application callbacks from the button module (workqueue context). */

void button_pressed(void)
{
	/* Visual feedback: mirror the button (unless pairing is blinking). */
	if (state != STATE_PAIRING) {
		gpio_pin_set_dt(&led, 1);
	}

	/* Start connecting immediately so the link is (nearly) up by the time
	 * the click count is known. A long press will disconnect and enter
	 * pairing instead. */
	if (state == STATE_IDLE) {
		/* Fresh cycle: drop any stale pending code left by a badly-timed
		 * click from a previous cycle, so it can never be typed on this
		 * new connection (only the most recent code is ever sent). */
		pending_clear();

		advertise(false, true);
		set_state(STATE_CONNECTING);
		k_work_reschedule(&connect_timeout_work, K_SECONDS(CONNECT_TIMEOUT_S));
		printk("-> CONNECTING (press)\n");
	}
}

void button_clicked(int count)
{
	if (state == STATE_PAIRING) {
		/* Cancel pairing; no code. */
		cancel_pairing();
		return;
	}

	if (count < 1) {
		return;
	}

	/* Visual feedback: button released. */
	gpio_pin_set_dt(&led, 0);

	/* A click that completed after the device already went idle (badly
	 * timed) has no link to type on: discard it rather than leaving a
	 * stale pending code that the next connection would type. */
	if (state == STATE_IDLE) {
		return;
	}

	/* SWD hook: reload the hardcoded test config. */
#if IS_ENABLED(CONFIG_TOTP_DEBUG)
	if (dbg_reload_config) {
		dbg_reload_config = 0;
		load_test_config();
	}
#endif

	/* Re-read config if USB is connected. Apply it only after two matching
	 * snapshots so a host save cannot expose a partial file. */
	if (usb_msc_is_connected()) {
		pending.kind = PENDING_CONFIG_READ;
		pending.slot_idx = count - 1;
		config_retry_count = 0;
		config_fs_stable_reset();
		config_retry_handler(NULL);
		return;
	}

	queue_request(count - 1);
	try_send();
}

void button_long_pressed(void)
{
	if (state != STATE_PAIRING) {
		enter_pairing();
	}
}

static void led_init(void)
{
	if (!gpio_is_ready_dt(&led)) {
		printk("LED GPIO not ready\n");
		return;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
}

#if IS_ENABLED(CONFIG_TOTP_POWER_DIAGNOSTIC)
/* SWD observability on the separate bench only. A debugger changes power
 * behavior; measure the diagnostic image on a board without SWD attached. */
volatile int dbg_power_phase;

static FUNC_NORETURN void power_diagnostic(void)
{
	led_init();
	dbg_power_phase = 1;
	k_sleep(K_SECONDS(60));
	dbg_power_phase = 2;
	sys_poweroff();
}
#endif

#if IS_ENABLED(CONFIG_TOTP_SECURITY_BOOTLOADER)
static const char *boot_error;

static void boot_error_vbus(bool present)
{
	if (present) {
		config_fs_log(boot_error);
	}
}
#endif

int main(void)
{
	int err;

	printk("TOTP token starting\n");
#if IS_ENABLED(CONFIG_TOTP_SECURITY_BOOTLOADER)
	boot_error = boot_security_init();
	if (boot_error) {
		/* Diagnostic-only MSC: no button, BLE, config parser, or secrets. */
		usb_msc_set_vbus_cb(boot_error_vbus);
		usb_msc_init();
		for (;;) {
			k_sleep(K_FOREVER);
		}
	}
#endif
#if IS_ENABLED(CONFIG_TOTP_POWER_DIAGNOSTIC)
	power_diagnostic();
#endif
	config_device_defaults(&device_settings);
	pending_clear();
	profiles_init();

	led_init();
	button_init();
	kbd_init();
	button_set_timings(device_settings.click_window_ms,
			   device_settings.long_press_ms);
#if IS_ENABLED(CONFIG_TOTP_BATTERY_MONITOR)
	battery_init();
	battery_set_low_threshold(device_settings.low_battery_mv);
#endif
	kbd_set_subscribed_cb(on_subscribed);
	kbd_set_output_write_cb(on_output_write);
	bt_conn_auth_info_cb_register(&auth_info_cb);

	k_work_init_delayable(&connect_timeout_work, connect_timeout_handler);
	k_work_init_delayable(&preferred_fallback_work,
			      preferred_fallback_handler);
	k_work_init_delayable(&pairing_timeout_work, pairing_timeout_handler);
	k_work_init_delayable(&settle_work, settle_work_handler);
	k_work_init_delayable(&grace_work, grace_work_handler);
	k_work_init(&adv_work, adv_work_handler);
	k_work_init_delayable(&config_poll_work, config_poll_handler);
	k_work_init_delayable(&config_retry_work, config_retry_handler);
	k_work_init_delayable(&led_work, led_work_handler);
	k_work_init_delayable(&pairing_disconnect_work, pairing_disconnect_handler);
	k_work_init_delayable(&pairing_finish_work, pairing_finish_work_handler);
	k_work_init_delayable(&totp_wait_work, totp_wait_handler);
	usb_msc_set_vbus_cb(usb_vbus_cb);

	if (totp_init() != 0) {
		printk("Crypto init failed\n");
		selftest_failed = 1;
	} else {
		selftest_failed = totp_selftest() || config_selftest() ||
				  config_fs_selftest() || time_selftest() ||
				  profiles_selftest() || kbd_selftest() || app_selftest();
		printk("TOTP selftest %s\n", selftest_failed ? "FAILED" : "ok");
	}

	/* Hardcoded test config so no USB is needed for testing. */
#if IS_ENABLED(CONFIG_TOTP_DEBUG)
	load_test_config();
#endif

	err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	/* The device is event-driven: sleep until the button interrupt. */
	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
