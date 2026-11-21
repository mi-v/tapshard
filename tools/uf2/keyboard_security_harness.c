#include "security_mocks.h"
#include "keyboard.h"
#include "secure_zero.h"
#include <sys/types.h>

#define BT_GATT_CCC_NOTIFY 1
#define BT_ATT_ERR_AUTHORIZATION 8
#define BT_GATT_ERR(x) (-(x))
struct bt_gatt_attr { int unused; };
static struct { struct bt_gatt_attr attrs[7]; } hog_svc;
struct bt_gatt_notify_params {
	const struct bt_gatt_attr *attr;
	const void *data;
	uint16_t len;
	void (*func)(struct bt_conn *, void *);
	void *user_data;
};

static struct bt_conn a = { .refs = 1, .security = 2, .subscribed = true };
static struct bt_conn b = { .refs = 1, .security = 2, .subscribed = true };
static bool allowed;
static int reports, read_calls, done_calls, done_error;
static struct {
	struct bt_conn *conn;
	void (*func)(struct bt_conn *, void *);
	void *user_data;
} flights[4];
static int flights_count;
static bool authorized(struct bt_conn *conn)
{
	return allowed && conn == &a && conn->security >= 2;
}
static bool bt_gatt_is_subscribed(struct bt_conn *conn,
				  const struct bt_gatt_attr *attr, uint16_t ccc)
{
	ARG_UNUSED(attr); ARG_UNUSED(ccc); return conn->subscribed;
}
static int bt_gatt_notify_cb(struct bt_conn *conn, const struct bt_gatt_notify_params *params)
{
	assert(conn == &a && params->len == 8 && flights_count < 4);
	flights[flights_count].conn = bt_conn_ref(conn);
	flights[flights_count].func = params->func;
	flights[flights_count++].user_data = params->user_data;
	reports++; return 0;
}
static ssize_t bt_gatt_attr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				void *buffer, uint16_t len, uint16_t offset,
				const void *value, uint16_t size)
{
	ARG_UNUSED(conn); ARG_UNUSED(attr); ARG_UNUSED(offset);
	read_calls++; memcpy(buffer, value, MIN(len, size)); return MIN(len, size);
}
static void done(int err) { done_calls++; done_error = err; }

/* ACTUAL_GLOBALS */
/* ACTUAL_FUNCTIONS */

static void complete_first(void)
{
	struct bt_conn *conn = flights[0].conn;
	void (*callback)(struct bt_conn *, void *) = flights[0].func;
	void *data = flights[0].user_data;

	assert(flights_count);
	memmove(flights, flights + 1, sizeof(flights[0]) * --flights_count);
	callback(conn, data); bt_conn_unref(conn);
}
static void finish(void)
{
	for (int i = 0; i < 100 && sender.active; i++) {
		if (flights_count) complete_first();
		else sender_work_handler(NULL);
	}
	assert(!sender.active && !flights_count && a.refs == 1);
}
static void fixture(void)
{
	kbd_cancel_send();
	while (flights_count) complete_first();
	assert(a.refs == 1 && b.refs == 1);
	kbd_init(); kbd_set_authorized_cb(authorized); allowed = true;
	a.security = 2; a.subscribed = true;
	reports = read_calls = done_calls = done_error = 0;
	dbg_force_notify_err = dbg_force_notify_after = 0;
}

int main(void)
{
	uint8_t buffer[8];
	fixture(); kbd_set_authorized_cb(NULL);
	assert(kbd_send_code(&a, "123456", false, 60, false, done) == -EACCES);
	assert(read_input_report(&a, NULL, buffer, sizeof(buffer), 0) == -BT_ATT_ERR_AUTHORIZATION);
	assert(!reports && !read_calls && a.refs == 1);
	fixture(); assert(kbd_send_code(&b, "123456", false, 60, false, done) == -EACCES);
	assert(kbd_send_code(NULL, "123456", false, 60, false, done) == -EACCES);
	assert(!reports && b.refs == 1);
	assert(read_input_report(&b, NULL, buffer, sizeof(buffer), 0) == -BT_ATT_ERR_AUTHORIZATION);
	fixture(); assert(kbd_selftest() == 0);
	assert(kbd_send_code(&a, "123456", false, 60, false, done) == 0 && a.refs == 2);
	finish(); assert(reports == 13 && done_calls == 1 && done_error == 0);
	fixture(); kbd_send_code(&a, "123456", false, 60, false, done);
	sender_work_handler(NULL); complete_first(); sender_work_handler(NULL);
	assert(reports == 2); allowed = false; complete_first(); sender_work_handler(NULL);
	assert(reports == 2 && done_calls == 1 && done_error == -EACCES && a.refs == 1);
	for (size_t i = 0; i < sizeof(report_value); i++) assert(report_value[i] == 0);
	fixture(); kbd_send_code(&a, "123456", false, 60, false, done);
	sender_work_handler(NULL); complete_first(); a.subscribed = false; sender_work_handler(NULL);
	assert(reports == 1 && done_error == -ENOTCONN && a.refs == 1);
	fixture(); kbd_send_code(&a, "123456", false, 60, false, done);
	sender_work_handler(NULL); assert(a.refs == 3); sender_work_handler(NULL);
	assert(done_error == -ETIMEDOUT && a.refs == 2); complete_first(); assert(a.refs == 1);
	fixture(); kbd_send_code(&a, "123456", false, 60, false, done);
	sender_work_handler(NULL); kbd_cancel_send();
	assert(a.refs == 2 && !done_calls);
	kbd_send_code(&a, "654321", false, 60, false, done); sender_work_handler(NULL);
	complete_first(); assert(sender.in_flight && sender.phase == SEND_WARMUP && sender.index == 0);
	complete_first(); finish(); assert(done_calls == 1 && done_error == 0);
	fixture(); dbg_force_notify_err = -ENOMEM;
	kbd_send_code(&a, "123456", false, 60, false, done); sender_work_handler(NULL);
	assert(!reports && sender.active && sender.retries == 1); finish();
	assert(reports == 13 && done_error == 0 && a.refs == 1);
	fixture(); dbg_force_notify_err = -EIO;
	kbd_send_code(&a, "123456", false, 60, false, done); sender_work_handler(NULL);
	assert(!reports && !sender.active && done_error == -EIO && a.refs == 1);
	fixture();
	puts("keyboard security scenarios passed"); return 0;
}
