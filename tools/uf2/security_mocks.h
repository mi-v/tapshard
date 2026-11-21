#ifndef SECURITY_MOCKS_H
#define SECURITY_MOCKS_H

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>

#define CONFIG_TOTP_DEBUG 0
#define CONFIG_TOTP_TEST_POLL 1
#define CONFIG_TOTP_BATTERY_MONITOR 0
#define CONFIG_TOTP_SECURITY_BOOTLOADER 0
#define CONFIG_TOTP_POWER_DIAGNOSTIC 0
#define CONFIG_SETTINGS 1
#define IS_ENABLED(x) (x)
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARG_UNUSED(x) (void)(x)
#define K_NO_WAIT 0
#define K_FOREVER (-1)
#define K_MSEC(x) (x)
#define K_SECONDS(x) ((x) * 1000)
#define BT_ID_DEFAULT 0
#define BT_HCI_ERR_REMOTE_USER_TERM_CONN 0x13
#define BT_ADDR_LE_PUBLIC 0
#define BT_ADDR_LE_RANDOM 1

typedef struct { uint8_t val[6]; } bt_addr_t;
typedef struct { uint8_t type; bt_addr_t a; } bt_addr_le_t;
static const bt_addr_le_t empty_address;
#define BT_ADDR_LE_ANY (&empty_address)
static inline bool bt_addr_le_eq(const bt_addr_le_t *a, const bt_addr_le_t *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}
static inline void bt_addr_le_copy(bt_addr_le_t *a, const bt_addr_le_t *b)
{
	memcpy(a, b, sizeof(*a));
}
enum bt_security_err {
	BT_SECURITY_ERR_SUCCESS, BT_SECURITY_ERR_AUTH_FAIL,
	BT_SECURITY_ERR_PAIR_NOT_ALLOWED,
};
typedef int bt_security_t;
#define BT_SECURITY_L2 2
struct bt_conn { bt_addr_le_t address; int security; int refs; bool subscribed; };
struct bt_conn_info { struct { int interval_us; } le; };
struct bt_conn_pairing_feat { int unused; };
struct bt_bond_info { bt_addr_le_t addr; };
struct bt_conn_auth_cb { enum bt_security_err (*pairing_accept)(struct bt_conn *, const struct bt_conn_pairing_feat *); };
struct bt_conn_auth_info_cb { void (*pairing_complete)(struct bt_conn *, bool); void (*bond_deleted)(uint8_t, const bt_addr_le_t *); };
void bt_foreach_bond(uint8_t id, void (*cb)(const struct bt_bond_info *, void *), void *data);
static inline struct bt_conn *bt_conn_ref(struct bt_conn *conn)
{
	assert(conn && conn->refs > 0);
	conn->refs++;
	return conn;
}
static inline void bt_conn_unref(struct bt_conn *conn)
{
	assert(conn && conn->refs > 0);
	conn->refs--;
}
static inline const bt_addr_le_t *bt_conn_get_dst(struct bt_conn *conn) { return &conn->address; }
static inline int bt_conn_get_security(struct bt_conn *conn) { return conn->security; }
static inline int bt_conn_enc_key_size(struct bt_conn *conn) { return conn->security >= 2 ? 16 : 0; }
static inline int bt_conn_get_info(struct bt_conn *conn, struct bt_conn_info *info)
{
	ARG_UNUSED(conn);
	info->le.interval_us = 10000;
	return 0;
}

typedef int atomic_t;
typedef int atomic_val_t;
static inline int atomic_get(const atomic_t *p) { return *p; }
static inline void atomic_set(atomic_t *p, int v) { *p = v; }
static inline void atomic_clear(atomic_t *p) { *p = 0; }
static inline void atomic_inc(atomic_t *p) { ++*p; }
struct k_spinlock { int unused; };
typedef int k_spinlock_key_t;
static inline int k_spin_lock(struct k_spinlock *lock) { ARG_UNUSED(lock); return 0; }
static inline void k_spin_unlock(struct k_spinlock *lock, int key) { ARG_UNUSED(lock); ARG_UNUSED(key); }
struct k_work { void (*handler)(struct k_work *); bool queued; };
struct k_work_delayable { struct k_work work; int delay; };
static inline void k_work_init(struct k_work *work, void (*cb)(struct k_work *)) { work->handler = cb; work->queued = false; }
static inline void k_work_init_delayable(struct k_work_delayable *work, void (*cb)(struct k_work *)) { k_work_init(&work->work, cb); work->delay = 0; }
static inline int k_work_submit(struct k_work *work) { work->queued = true; return 1; }
static inline int k_work_schedule(struct k_work_delayable *work, int ms) { work->delay = ms; return 1; }
static inline int k_work_reschedule(struct k_work_delayable *work, int ms) { work->delay = ms; return 1; }
static inline int k_work_cancel_delayable(struct k_work_delayable *work) { work->delay = 0; return 0; }
static inline int k_work_delayable_remaining_get(struct k_work_delayable *work) { return work->delay; }
struct mock_msgq { uint8_t data[4096]; size_t size; int capacity, head, count; };
#define K_MSGQ_DEFINE(name, width, number, align) static struct mock_msgq name = { .size = width, .capacity = number }
static inline int k_msgq_put(struct mock_msgq *q, const void *data, int wait)
{
	ARG_UNUSED(wait);
	if (q->count == q->capacity) return -ENOMSG;
	memcpy(q->data + ((q->head + q->count) % q->capacity) * q->size, data, q->size);
	q->count++;
	return 0;
}
static inline int k_msgq_get(struct mock_msgq *q, void *data, int wait)
{
	ARG_UNUSED(wait);
	if (!q->count) return -ENOMSG;
	memcpy(data, q->data + q->head * q->size, q->size);
	q->head = (q->head + 1) % q->capacity;
	q->count--;
	return 0;
}
#define printk(...) ((void)0)
#endif
