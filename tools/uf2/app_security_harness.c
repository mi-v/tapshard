/* Real application functions are inserted by test_app_security.py. Transport,
 * timing and startup dependencies are fakes; security predicates are not. */
#include "security_mocks.h"
#include "profiles.h"
#include "secure_zero.h"

#define HID_SETUP_QUIET_MS 500
#define HID_READY_TIMEOUT_MS 10000
#define PAIRING_TIMEOUT_S 30
#define PAIR_DISCONNECT_MS 2000
#define PAIR_SNATCH_MS 2000
#define CONNECT_TIMEOUT_S 10
#define TOTP_SEND_MARGIN_MS 1500

/* ACTUAL_GLOBALS */
/* ACTUAL_DECLARATIONS */
/* ACTUAL_AUTH */

static struct totp_device device_settings;
static struct secret_profile dump_profile;
static bool dump_profile_valid;
static struct secret_profile *sending_profile;
static int sending_slot;
static uint32_t sending_revision;
static bool link_settled, led_live, preferred_advertising;
static bool usb_vbus_present;
static int selftest_failed = 1, bt_ready_called, adv_result;
static int dbg_idle_reason, dbg_connected, dbg_disconnect_reason;
static int dbg_sec_changed_err, dbg_sec_level, dbg_enc_key_size;
static int dbg_pairing_count, dbg_pairing_bonded, dbg_conn_interval;
static int config_retry_count;
static atomic_t settle_generation, connection_releasing, adv_restart_pending;
static struct k_work adv_work;
static struct k_work_delayable connect_timeout_work, preferred_fallback_work;
static struct k_work_delayable pairing_timeout_work, settle_work, hid_ready_timeout_work;
static struct k_work_delayable grace_work, totp_wait_work, config_retry_work;
static struct k_work_delayable pairing_disconnect_work, pairing_finish_work;
static struct k_work_delayable config_poll_work, led_work;
static const char *boot_error;

static struct bt_conn a = { .address = { .a = { { 1 } } }, .refs = 1, .security = 2, .subscribed = true };
static struct bt_conn b = { .address = { .a = { { 2 } } }, .refs = 1, .security = 2, .subscribed = true };
static struct bt_conn stranger = { .address = { .a = { { 3 } } }, .refs = 1, .security = 2, .subscribed = true };
static bt_addr_le_t bonds[4];
static int bond_count;
static int sends, disconnects, cancellations, bluetooth_starts, button_starts;
static int crypto_error, test_error, settings_error, registration_error, cleanup_error;
static struct bt_conn *sent_to;
static char sent_code[16], diagnostic[128];
static void (*vbus_handler)(bool);
static jmp_buf sleep_exit;

static void led_refresh(void) {}
static void led_attempt_reset(void) {}
static void led_send_finished(void) {}
static void led_button_feedback(bool active) { ARG_UNUSED(active); }
static void led_wait_window(bool settle, uint32_t ms) { ARG_UNUSED(settle); ARG_UNUSED(ms); }
static void button_set_pairing(bool active) { ARG_UNUSED(active); }
static void button_set_timings(uint32_t click, uint32_t hold) { ARG_UNUSED(click); ARG_UNUSED(hold); }
static int bt_conn_disconnect(struct bt_conn *conn, int reason)
{
	ARG_UNUSED(conn); ARG_UNUSED(reason); disconnects++; return 0;
}
static int bt_le_adv_stop(void) { return 0; }
static void advertise(bool discoverable, bool prefer) { ARG_UNUSED(discoverable); ARG_UNUSED(prefer); }
static void kbd_cancel_send(void) { cancellations++; }
static bool kbd_is_subscribed(struct bt_conn *conn) { return conn && conn->subscribed; }
static int kbd_send_code(struct bt_conn *conn, const char *code, bool enter,
			uint32_t delay, bool slow, void (*done)(int))
{
	ARG_UNUSED(enter); ARG_UNUSED(delay); ARG_UNUSED(slow); ARG_UNUSED(done);
	/* Deliberately no security check: application tests must catch bypasses. */
	sends++; sent_to = conn; strcpy(sent_code, code); return 0;
}
static uint32_t kbd_send_duration_ms(size_t digits, bool enter, uint32_t ms, bool slow)
{
	ARG_UNUSED(digits); ARG_UNUSED(enter); ARG_UNUSED(ms); ARG_UNUSED(slow); return 100;
}
static bool time_is_valid(void) { return true; }
static uint64_t time_get(void) { return 1700000000; }
static int totp_generate(const uint8_t *secret, size_t size, uint64_t time,
			uint32_t period, uint8_t digits, char *out, size_t capacity)
{
	ARG_UNUSED(size); ARG_UNUSED(time); ARG_UNUSED(period); ARG_UNUSED(digits); ARG_UNUSED(capacity);
	memset(out, '0' + secret[0], 6); out[6] = '\0'; return 0;
}
static bool usb_msc_is_connected(void) { return false; }
static void config_fs_stable_reset(void) {}
static void config_retry_handler(struct k_work *work) { ARG_UNUSED(work); }
static void usb_vbus_cb(bool present) { ARG_UNUSED(present); }
static void config_fs_log(const char *message) { snprintf(diagnostic, sizeof(diagnostic), "%s", message); }
static void usb_msc_set_vbus_cb(void (*cb)(bool)) { vbus_handler = cb; }
static int usb_msc_init(void) { return 0; }
static int settings_load(void) { return settings_error; }
static void button_init(void) { button_starts++; }
static void kbd_init(void) {}
static void led_init(void) {}
static void kbd_set_authorized_cb(bool (*cb)(struct bt_conn *)) { assert(cb == peer_is_authorized); }
static void kbd_set_subscribed_cb(void (*cb)(void)) { ARG_UNUSED(cb); }
static void kbd_set_host_setup_cb(void (*cb)(struct bt_conn *)) { ARG_UNUSED(cb); }
static void kbd_set_output_write_cb(void (*cb)(struct bt_conn *)) { ARG_UNUSED(cb); }
static int bt_conn_auth_cb_register(const struct bt_conn_auth_cb *cb)
{
	assert(cb->pairing_accept == pairing_accept); return registration_error;
}
static int bt_conn_auth_info_cb_register(const struct bt_conn_auth_info_cb *cb)
{
	assert(cb->pairing_complete == pairing_complete); return registration_error;
}
static int bt_enable(void (*cb)(int)) { bluetooth_starts++; cb(0); return 0; }
static int totp_init(void) { return crypto_error; }
#define SELFTEST(name, number) int name(void) { return test_error == number; }
SELFTEST(totp_selftest, 1)
SELFTEST(config_selftest, 2)
SELFTEST(config_fs_selftest, 3)
SELFTEST(time_selftest, 4)
SELFTEST(kbd_selftest, 6)
SELFTEST(led_selftest, 7)
SELFTEST(hid_ready_selftest, 8)
SELFTEST(app_selftest, 9)
int real_profiles_selftest(void);
int profiles_selftest(void) { return test_error == 5 ? 1 : real_profiles_selftest(); }
void config_device_defaults(struct totp_device *d)
{
	memset(d, 0, sizeof(*d)); d->settle_ms = 500; d->inter_key_ms = 60; d->grace_ms = 2000;
}
static void k_sleep(int delay) { ARG_UNUSED(delay); longjmp(sleep_exit, 1); }
#define HANDLER(name) static void name(struct k_work *work) { ARG_UNUSED(work); }
HANDLER(connect_timeout_handler)
HANDLER(preferred_fallback_handler)
HANDLER(adv_work_handler)
HANDLER(config_poll_handler)
HANDLER(led_work_handler)
HANDLER(pairing_disconnect_handler)
HANDLER(totp_wait_handler)
HANDLER(hid_ready_timeout_handler)
HANDLER(grace_work_handler)

void bt_foreach_bond(uint8_t id, void (*cb)(const struct bt_bond_info *, void *), void *data)
{
	ARG_UNUSED(id);
	for (int i = 0; i < bond_count; i++) {
		struct bt_bond_info info = { .addr = bonds[i] };
		cb(&info, data);
	}
}
static int bt_unpair(uint8_t id, const bt_addr_le_t *peer)
{
	if (cleanup_error) return cleanup_error;
	if (!peer) {
		while (bond_count) { bt_addr_le_t old = bonds[--bond_count]; bond_deleted(id, &old); }
		return 0;
	}
	for (int i = 0; i < bond_count; i++) {
		if (bt_addr_le_eq(peer, &bonds[i])) {
			bt_addr_le_t old = bonds[i]; bonds[i] = bonds[--bond_count]; bond_deleted(id, &old); return 0;
		}
	}
	return 0;
}

/* ACTUAL_FUNCTIONS */

static void fixture(void)
{
	assert(!peer_events.count && !peer_cleanup_conn);
	if (cur_conn) { bt_conn_unref(cur_conn); cur_conn = NULL; }
	assert(a.refs == 1 && b.refs == 1 && stranger.refs == 1);
	profiles_init(); bond_count = 0; state = STATE_IDLE; peer_authorized = false;
	peer_rejected = false; pairing_window_open = false; peer_pairing_admitted = false;
	peer_pairing_handled = false; atomic_set(&startup_ready, 1); selftest_failed = 0;
	boot_error = NULL; vbus_handler = NULL; diagnostic[0] = '\0'; dump_profile_valid = false;
	crypto_error = test_error = settings_error = registration_error = cleanup_error = 0;
	dbg_force_startup_failure = 0; bluetooth_starts = button_starts = 0;
	sends = disconnects = cancellations = 0; sent_to = NULL;
	link_settled = true; pending.kind = PENDING_NONE;
	a.security = b.security = stranger.security = 2;
	a.subscribed = b.subscribed = stranger.subscribed = true;
	config_device_defaults(&device_settings); k_work_init(&peer_work, peer_work_handler);
}

static void account(struct bt_conn *conn, int secret)
{
	struct secret_profile *profile;
	bonds[bond_count++] = conn->address;
	assert(profiles_add(&conn->address) == 0);
	profile = profiles_find(&conn->address); profile->have_config = true; profile->slot_count = 1;
	profile->slots[0].secret[0] = secret; profile->slots[0].secret_len = 1;
	profile->slots[0].digits = 6; profile->slots[0].period = 30;
}

static void connect_peer(struct bt_conn *conn)
{
	state = STATE_CONNECTING; connected(conn, 0);
}

static void secured(struct bt_conn *conn)
{
	security_changed(conn, conn->security, BT_SECURITY_ERR_SUCCESS); peer_work_handler(NULL);
}

static void startup(void)
{
	if (!setjmp(sleep_exit)) app_main();
}

int main(void)
{
	fixture(); account(&a, 1); profiles_set_current(&a.address); connect_peer(&stranger);
	queue_request(0); try_send(); assert(!sends); secured(&stranger);
	assert(!sends && !peer_is_authorized(&stranger) && disconnects && state == STATE_IDLE);
	assert(profiles_current() == profiles_find(&a.address));
	fixture(); account(&a, 1); profiles_set_current(&a.address);
	bonds[bond_count++] = b.address; connect_peer(&b); secured(&b);
	assert(!peer_is_authorized(&b) && !profiles_find(&b.address) && disconnects);
	assert(profiles_current() == profiles_find(&a.address));
	fixture(); account(&a, 1); account(&b, 2); profiles_set_current(&a.address); connect_peer(&b);
	queue_request(0); try_send(); assert(!sends); secured(&b); link_settled = true; try_send();
	assert(sends == 1 && sent_to == &b && !strcmp(sent_code, "222222"));
	fixture(); account(&a, 1); connect_peer(&a); on_output_write(&a); try_send();
	assert(!peer_is_authorized(&a));
	assert(pairing_accept(&a, NULL) == BT_SECURITY_ERR_PAIR_NOT_ALLOWED);
	assert(bond_count == 1 && profiles_find(&a.address));
	secured(&a); assert(peer_is_authorized(&a));
	security_changed(&a, 1, BT_SECURITY_ERR_AUTH_FAIL);
	assert(!peer_is_authorized(&a)); peer_work_handler(NULL); assert(state == STATE_IDLE);
	fixture(); account(&a, 1); connect_peer(&a); secured(&a);
	assert(peer_is_authorized(&a)); bt_unpair(0, &a.address);
	assert(!peer_is_authorized(&a)); peer_work_handler(NULL); assert(disconnects);
	fixture(); account(&a, 1); connect_peer(&a);
	security_changed(&stranger, 2, BT_SECURITY_ERR_SUCCESS); peer_work_handler(NULL);
	assert(!peer_is_authorized(&a) && !disconnects);
	fixture(); account(&a, 1); connect_peer(&a);
	security_changed(&a, 2, BT_SECURITY_ERR_SUCCESS); disconnected(&a, 0);
	peer_work_handler(NULL); assert(!peer_authorized && a.refs == 1);
	fixture(); pairing_window_set(true); state = STATE_PAIRING; connected(&b, 0);
	assert(pairing_accept(&b, NULL) == BT_SECURITY_ERR_SUCCESS);
	bonds[bond_count++] = b.address; pairing_complete(&b, true); peer_work_handler(NULL);
	assert(state == STATE_CONNECTED && peer_is_authorized(&b) && bond_count == 1);
	fixture(); account(&a, 1); pairing_window_set(true); state = STATE_PAIRING; connected(&a, 0);
	assert(pairing_accept(&a, NULL) == BT_SECURITY_ERR_SUCCESS);
	pairing_complete(&a, true); peer_work_handler(NULL);
	assert(peer_is_authorized(&a) && profiles_current()->slots[0].secret[0] == 1);
	fixture(); account(&a, 1); profiles_set_current(&a.address);
	for (int i = 0; i < 3; i++) {
		bt_addr_le_t extra = { .a = { { (uint8_t)(10 + i) } } };
		assert(profiles_add(&extra) == 0);
	}
	pairing_window_set(true); state = STATE_PAIRING; connected(&b, 0);
	assert(pairing_accept(&b, NULL) == BT_SECURITY_ERR_SUCCESS);
	bonds[bond_count++] = b.address; pairing_complete(&b, true); peer_work_handler(NULL);
	assert(!peer_authorized && state == STATE_PAIRING && bond_count == 1);
	assert(profiles_current() == profiles_find(&a.address));
	fixture(); account(&a, 1); pairing_window_set(true); state = STATE_PAIRING; connected(&a, 0);
	secured(&a); assert(!peer_authorized && pairing_finish_work.delay == PAIR_SNATCH_MS);
	assert(pairing_accept(&a, NULL) == BT_SECURITY_ERR_SUCCESS);
	pairing_finish_work_handler(NULL); assert(!disconnects);
	pairing_complete(&a, true); peer_work_handler(NULL); assert(peer_is_authorized(&a));
	fixture(); pairing_window_set(true); state = STATE_PAIRING; connected(&b, 0);
	assert(pairing_accept(&b, NULL) == BT_SECURITY_ERR_SUCCESS);
	bonds[bond_count++] = b.address; pairing_complete(&b, true); cancel_pairing();
	peer_work_handler(NULL); assert(!bond_count && !peer_is_authorized(&b));
	fixture(); pairing_window_set(true); state = STATE_PAIRING; connected(&b, 0);
	assert(pairing_accept(&b, NULL) == BT_SECURITY_ERR_SUCCESS);
	cancel_pairing(); bonds[bond_count++] = b.address; pairing_complete(&b, true);
	disconnected(&b, 0); peer_work_handler(NULL); assert(!bond_count && b.refs == 1);
	fixture(); account(&a, 1); connect_peer(&a); enter_pairing();
	assert(pairing_accept(&a, NULL) == BT_SECURITY_ERR_PAIR_NOT_ALLOWED);
	fixture(); pairing_window_set(true); state = STATE_PAIRING; connected(&b, 0);
	assert(pairing_accept(&b, NULL) == BT_SECURITY_ERR_SUCCESS);
	for (int i = 0; i < 8; i++) security_changed(&b, 2, BT_SECURITY_ERR_SUCCESS);
	bonds[bond_count++] = b.address; pairing_complete(&b, true); peer_work_handler(NULL);
	assert(!bond_count && !peer_authorized && b.refs == 2);
	fixture(); account(&a, 1); pairing_window_set(true); state = STATE_PAIRING; connected(&a, 0);
	assert(pairing_accept(&a, NULL) == BT_SECURITY_ERR_SUCCESS);
	cancel_pairing(); cleanup_error = -EIO; pairing_complete(&a, true); peer_work_handler(NULL);
	assert(!atomic_get(&startup_ready) && !profiles_find(&a.address) && boot_error);
	assert(!peer_is_authorized(&a) && state == STATE_IDLE);
	for (int failed = 1; failed <= 9; failed++) {
		fixture(); test_error = failed; startup();
		assert(selftest_failed && !bluetooth_starts && !button_starts && !atomic_get(&startup_ready));
		assert(vbus_handler == boot_error_vbus); vbus_handler(true); assert(diagnostic[0]);
		button_pressed(); button_clicked(1); button_long_pressed(); assert(!sends && state == STATE_IDLE);
	}
	fixture(); crypto_error = -1; startup(); assert(selftest_failed && !bluetooth_starts && !button_starts);
	fixture(); registration_error = -1; startup(); assert(!bluetooth_starts && !button_starts && dbg_startup_stage == -3);
	fixture(); settings_error = -1; startup(); assert(bluetooth_starts == 1 && !button_starts && !atomic_get(&startup_ready));
	fixture(); cleanup_error = -1; startup(); assert(!button_starts && !atomic_get(&startup_ready));
	fixture(); startup(); assert(!selftest_failed && bluetooth_starts == 1 && button_starts == 1 && atomic_get(&startup_ready));
	fixture();
	puts("application security scenarios passed");
	return 0;
}
