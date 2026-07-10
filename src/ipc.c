/*
 * ipc.c — Platform-agnostic core: actor registry, registration table,
 *          subscription table, send/publish/query/reply.
 *
 * No #ifdefs. All platform-specific behaviour lives in ipc_port.h
 * and is implemented per-platform in posix_ipc_port.c / zephyr_ipc_port.c.
 */
#include "ipc.h"
#include "ipc_internal.h"
#include "ipc_port.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ── Port seam forward declarations ─────────────────────────────────────── */

int ipc_port_start(struct ipc_actor *a);
int ipc_port_send(struct ipc_actor *a, const struct ipc_msg *msg);
int ipc_port_send_after(struct ipc_actor *a, const struct ipc_msg *msg, uint32_t delay_ms);
void ipc_port_stop_actor(struct ipc_actor *a);

/* ── Global actor registry (singly-linked list) ──────────────────────────── */

struct ipc_actor *_ipc_actor_list = NULL;

/* ── Registration table ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t msg_id;
    struct ipc_actor *actor;
} ipc_registration_t;

static ipc_registration_t reg_table[IPC_MAX_REGISTRATIONS];
static int reg_count;

/* ── Subscription table ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t msg_id;
    struct ipc_actor *actor;
} ipc_subscription_t;

static ipc_subscription_t sub_table[IPC_MAX_SUBSCRIPTIONS];
static int sub_count;

/* ── In-flight query wait table ──────────────────────────────────────────── */

/*
 * The wait block for a query is owned by the static wait_table, not by
 * the sender's stack frame. This is what makes cross-thread query
 * round-trips safe: the reply thread (the actor's worker thread on
 * POSIX/Zephyr) writes the response into the slot, and the sender
 * reads it from the same slot after waking.
 *
 * Tokens stored in struct ipc_msg._wait are encoded as
 * (uintptr_t)(slot_index + 1) so the value 0 is the "no waiter"
 * sentinel (see IPC_QUERY_INVALID_TOKEN in ipc_port.h).
 */
typedef struct {
    ipc_query_wait_t wait;
    bool in_use;
} ipc_wait_node_t;

static ipc_wait_node_t wait_table[IPC_MAX_INFLIGHT_QUERIES];
static int wait_count;

static int _ipc_wait_table_claim(void)
{
    if (wait_count >= IPC_MAX_INFLIGHT_QUERIES) {
        return -1;
    }
    int slot                = wait_count++;
    wait_table[slot].in_use = true;
    return slot;
}

static void _ipc_wait_table_free(int slot)
{
    /* Swap-with-last keeps the free slots contiguous at [0..wait_count).
     * Matches the ipc_unsubscribe pattern. */
    wait_table[slot] = wait_table[wait_count - 1];
    wait_count--;
}

/* ── Test reset ─────────────────────────────────────────────────────────── */
/* See declaration in ipc_internal.h. Production code must never call this. */
void _ipc_reset_for_testing(void)
{
    reg_count  = 0;
    sub_count  = 0;
    wait_count = 0;
    memset(reg_table, 0, sizeof(reg_table));
    memset(sub_table, 0, sizeof(sub_table));
    memset(wait_table, 0, sizeof(wait_table));
    _ipc_actor_list = NULL;
}

/* ── Lazy ID initialisation ──────────────────────────────────────────────── */

/*
 * Ensure the descriptor's .id is populated from .name (FNV-1a).
 * Called unconditionally at every API entry point — safe because
 * ID init is idempotent (the .id field is 0 only on the first
 * call for that descriptor).
 */
static void _ipc_ensure_id(ipc_msg_desc_t *d)
{
    if (!d->id) {
        d->id = _ipc_fnv1a(d->name);
    }
}

/* ── Helper: find registration ───────────────────────────────────────────── */

static struct ipc_actor *find_registered(uint32_t msg_id)
{
    for (int i = 0; i < reg_count; i++) {
        if (reg_table[i].msg_id == msg_id) {
            return reg_table[i].actor;
        }
    }
    return NULL;
}

/* ── ipc_actor_init ──────────────────────────────────────────────────────── */

int ipc_actor_init(struct ipc_actor *actor, const char *name, ipc_actor_handler_t handler,
                   struct ipc_actor_cfg cfg)
{
    actor->name    = name;
    actor->handler = handler;
    actor->cfg     = cfg;
    actor->_next   = NULL;

    ipc_port_table_lock();
    struct ipc_actor **pp = &_ipc_actor_list;
    while (*pp) {
        pp = &(*pp)->_next;
    }
    *pp = actor;
    ipc_port_table_unlock();

    return ipc_port_actor_init(actor);
}

/* ── ipc_register ────────────────────────────────────────────────────────── */

int ipc_register(struct ipc_actor *actor, ipc_msg_desc_t *desc)
{
    assert(desc->kind == IPC_CMD || desc->kind == IPC_QUERY);
    _ipc_ensure_id(desc);

    ipc_port_table_lock();

    for (int i = 0; i < reg_count; i++) {
        if (reg_table[i].msg_id == desc->id) {
            ipc_port_table_unlock();
            fprintf(stderr, "ipc: duplicate registration for '%s'\n", desc->name);
            assert(0 && "duplicate IPC_REGISTER");
            return -EALREADY;
        }
    }

    if (reg_count >= IPC_MAX_REGISTRATIONS) {
        ipc_port_table_unlock();
        assert(0 && "IPC registration table full");
        return -ENOMEM;
    }

    reg_table[reg_count].msg_id = desc->id;
    reg_table[reg_count].actor  = actor;
    reg_count++;

    ipc_port_table_unlock();
    return 0;
}

/* ── ipc_subscribe / ipc_unsubscribe ─────────────────────────────────────── */

int ipc_subscribe(struct ipc_actor *actor, ipc_msg_desc_t *desc)
{
    /* Mirrors ipc_register's kind assertion: only EVENT descriptors may
     * be subscribed. Subscribing a CMD/QUERY descriptor is a programming
     * error — those are routed by ipc_register, not by the publish path. */
    assert(desc->kind == IPC_EVENT);
    _ipc_ensure_id(desc);
    ipc_port_table_lock();

    /* Dedup: ipc_subscribe is idempotent for the same (actor, MsgType)
     * pair. A second call with the same pair returns 0 without adding a
     * second row, mirroring the find_registered() / reg_table append
     * style in ipc_register. Without this, a duplicate subscribe would
     * cause a single publish to deliver to the same actor twice. */
    for (int i = 0; i < sub_count; i++) {
        if (sub_table[i].msg_id == desc->id && sub_table[i].actor == actor) {
            ipc_port_table_unlock();
            return 0;
        }
    }

    if (sub_count >= IPC_MAX_SUBSCRIPTIONS) {
        ipc_port_table_unlock();
        return -ENOMEM;
    }

    sub_table[sub_count].msg_id = desc->id;
    sub_table[sub_count].actor  = actor;
    sub_count++;

    ipc_port_table_unlock();
    return 0;
}

int ipc_unsubscribe(struct ipc_actor *actor, ipc_msg_desc_t *desc)
{
    _ipc_ensure_id(desc);
    ipc_port_table_lock();

    for (int i = 0; i < sub_count; i++) {
        if (sub_table[i].msg_id == desc->id && sub_table[i].actor == actor) {
            sub_table[i] = sub_table[sub_count - 1];
            sub_count--;
            ipc_port_table_unlock();
            return 0;
        }
    }

    ipc_port_table_unlock();
    return -ENOENT;
}

/* ── ipc_send_raw ────────────────────────────────────────────────────────── */

int ipc_send_raw(ipc_msg_desc_t *desc, const void *payload)
{
    _ipc_ensure_id(desc);
    ipc_port_table_lock();
    struct ipc_actor *target = find_registered(desc->id);
    ipc_port_table_unlock();

    if (!target) {
        fprintf(stderr, "ipc: send '%s' — not registered\n", desc->name);
        return -ENOENT;
    }

    struct ipc_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.id   = desc->id;
    msg.kind = desc->kind;
    if (payload && desc->size > 0) {
        memcpy(msg.payload, payload, desc->size);
    }

    return ipc_port_send(target, &msg);
}

/* ── ipc_send_after_raw ──────────────────────────────────────────────────── */

int ipc_send_after_raw(ipc_msg_desc_t *desc, uint32_t delay_ms, const void *payload)
{
    _ipc_ensure_id(desc);
    ipc_port_table_lock();
    struct ipc_actor *target = find_registered(desc->id);
    ipc_port_table_unlock();

    if (!target) {
        fprintf(stderr, "ipc: send_after '%s' — not registered\n", desc->name);
        return -ENOENT;
    }

    struct ipc_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.id   = desc->id;
    msg.kind = desc->kind;
    if (payload && desc->size > 0) {
        memcpy(msg.payload, payload, desc->size);
    }

    return ipc_port_send_after(target, &msg, delay_ms);
}

/* ── ipc_publish_raw ─────────────────────────────────────────────────────── */

int ipc_publish_raw(ipc_msg_desc_t *desc, const void *payload)
{
    /* Mirrors ipc_register's kind assertion: only EVENT descriptors may
     * be published. Publishing a CMD or QUERY descriptor is a programming
     * error — those go through ipc_send_raw / ipc_query_raw. Without this
     * assert the code silently overrides msg.kind = IPC_EVENT below,
     * which masks the bug (and would cause a cmd/query message to be
     * fan-out delivered to event subscribers). */
    assert(desc->kind == IPC_EVENT);
    _ipc_ensure_id(desc);

    struct ipc_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.id   = desc->id;
    msg.kind = IPC_EVENT;
    if (payload && desc->size > 0) {
        memcpy(msg.payload, payload, desc->size);
    }

    int last_rc = 0;

    ipc_port_table_lock();
    for (int i = 0; i < sub_count; i++) {
        if (sub_table[i].msg_id == desc->id) {
            struct ipc_actor *a = sub_table[i].actor;
            ipc_port_table_unlock();
            int rc = ipc_port_send(a, &msg);
            /* Return the FIRST non-zero rc so a later subscriber's
             * failure cannot mask an earlier one. */
            if (rc && !last_rc) {
                last_rc = rc;
            }
            ipc_port_table_lock();
        }
    }
    ipc_port_table_unlock();

    return last_rc;
}

/* ── ipc_query_raw ───────────────────────────────────────────────────────── */

int ipc_query_raw(ipc_msg_desc_t *desc, const void *payload, void *response, size_t resp_size,
                  ipc_timeout_t timeout)
{
    _ipc_ensure_id(desc);
    ipc_port_table_lock();
    struct ipc_actor *target = find_registered(desc->id);
    ipc_port_table_unlock();

    if (!target) {
        fprintf(stderr, "ipc: query '%s' — not registered\n", desc->name);
        return -ENOENT;
    }

    /* Claim a wait-table slot before building the wire message. The slot
     * holds a stable, process-wide ipc_query_wait_t that the reply thread
     * can write to (it lives in static storage, not on our stack). */
    ipc_port_table_lock();
    int slot = _ipc_wait_table_claim();
    ipc_port_table_unlock();
    if (slot < 0) {
        return -ENOMEM;
    }

    int rc = ipc_port_query_wait_init(&wait_table[slot].wait);
    if (rc) {
        ipc_port_table_lock();
        _ipc_wait_table_free(slot);
        ipc_port_table_unlock();
        return rc;
    }

    struct ipc_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.id    = desc->id;
    msg.kind  = IPC_QUERY;
    /* Token = (uintptr_t)(slot + 1); reply subtracts 1. 0 is reserved as
     * the "no waiter" sentinel (IPC_QUERY_INVALID_TOKEN). */
    msg._wait = (void *) (uintptr_t) (slot + 1);
    if (payload && desc->size > 0) {
        memcpy(msg.payload, payload, desc->size);
    }

    rc = ipc_port_send(target, &msg);
    if (rc != 0) {
        goto cleanup_locked;
    }

    rc = ipc_port_query_wait_block(&wait_table[slot].wait, timeout);
    if (rc == 0 && response) {
        size_t copy = resp_size < IPC_QUERY_RESPONSE_SIZE ? resp_size : IPC_QUERY_RESPONSE_SIZE;
        memcpy(response,
               (const uint8_t *) wait_table[slot].wait._opaque + IPC_QUERY_RESPONSE_OFFSET, copy);
    }

cleanup_locked:
    /* destroy and free both happen under the table lock so that a reply
     * thread racing with a timeout-driven cleanup cannot find the impl
     * destroyed while it is in the middle of ipc_port_query_wait_wake. */
    ipc_port_table_lock();
    ipc_port_query_wait_destroy(&wait_table[slot].wait);
    _ipc_wait_table_free(slot);
    ipc_port_table_unlock();
    return rc;
}

/* ── ipc_reply_raw ───────────────────────────────────────────────────────── */

void ipc_reply_raw(const struct ipc_msg *msg, const void *response, size_t len)
{
    uintptr_t token = (uintptr_t) msg->_wait;
    if (token == IPC_QUERY_INVALID_TOKEN) {
        return;
    }

    int slot = (int) (token - 1);
    if (slot < 0 || slot >= wait_count) {
        return;
    }

    ipc_port_table_lock();
    if (!wait_table[slot].in_use) {
        /* Slot was freed by the sender (timeout or earlier reply).
         * The wait is no longer alive; treat as silent no-op. */
        ipc_port_table_unlock();
        return;
    }

    size_t copy_len = len < IPC_QUERY_RESPONSE_SIZE ? len : IPC_QUERY_RESPONSE_SIZE;
    memcpy((uint8_t *) wait_table[slot].wait._opaque + IPC_QUERY_RESPONSE_OFFSET, response,
           copy_len);

    /* The sender frees the slot after waking. The wake is performed
     * under the table lock so the sender cannot destroy the impl
     * mid-wake (see the concurrency note in the wait_table block). */
    ipc_port_query_wait_wake(&wait_table[slot].wait);
    ipc_port_table_unlock();
}

/* ── ipc_start_all_threads / ipc_run_all / ipc_stop_all ─────────────────── */

int ipc_start_all_threads(void)
{
    struct ipc_actor *a = _ipc_actor_list;
    while (a) {
        int rc = ipc_port_start(a);
        if (rc) {
            return rc;
        }
        a = a->_next;
    }
    return 0;
}

int ipc_run_all(void)
{
    /* On POSIX the port walks the actor list and pthread_joins each
     * thread, blocking here until they've all exited. On Zephyr the
     * port is a no-op (the kernel keeps scheduling) and we return
     * without joining. See ipc_port_run_all in each backend. */
    return ipc_port_run_all();
}

void ipc_stop_all(void)
{
    struct ipc_actor *a = _ipc_actor_list;
    while (a) {
        ipc_port_stop_actor(a);
        a = a->_next;
    }
}
