/*
 * zephyr_ipc_port.c — Zephyr implementation of the generic ipc_port interface.
 *
 * k_msgq + k_poll_signal + k_work_delayable + k_thread.
 *
 * The port owns each actor's per-thread resources (k_msgq buffer and
 * k_thread stack) via a static pool sized at build time.  Users do
 * NOT supply these resources — they only set the *size* of each
 * actor's resources in struct ipc_actor_cfg, and ipc_port_actor_init
 * carves a slot from the pool.  The pool is bounded by Kconfig:
 *   CONFIG_ACTOR_MAX_ACTORS        number of slots
 *   CONFIG_ACTOR_MAX_STACK_SIZE    per-slot stack bytes
 *   CONFIG_ACTOR_MAX_QUEUE_DEPTH   per-slot msgq capacity
 *
 * This keeps <ipc.h> free of platform-specific fields and means the
 * Zephyr port doesn't need a public helper header.
 */
#include "ipc.h"
#include "ipc_port.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>

/* ── Table mutex ─────────────────────────────────────────────────────────── */

static struct k_mutex table_mutex;
static bool table_inited;

static void table_init_once(void)
{
    if (!table_inited) {
        k_mutex_init(&table_mutex);
        table_inited = true;
    }
}

void ipc_port_table_lock(void)
{
    table_init_once();
    k_mutex_lock(&table_mutex, K_FOREVER);
}
void ipc_port_table_unlock(void)
{
    k_mutex_unlock(&table_mutex);
}

/* ── Per-actor port state (concrete layout) ─────────────────────────────── */

struct ipc_port_state {
    struct k_msgq msgq;
    struct k_poll_signal signal;
    struct k_thread thread;
    k_thread_stack_t *stack; /* points into zephyr_stk_pool */
    struct k_work_delayable delayed_work;
    struct ipc_msg delayed_msg;
    struct ipc_actor *owner;
    int pool_slot; /* index into the static pool, -1 if unallocated */
};

_Static_assert(sizeof(struct ipc_port_state) <= sizeof(ipc_port_state_t),
               "Increase IPC_PORT_STATE_WORDS for Zephyr port state");

static struct ipc_port_state *port_of(struct ipc_actor *a)
{
    return (struct ipc_port_state *) (void *) &a->port;
}

/* ── Static resource pool ────────────────────────────────────────────────
 *
 * One pool backs all actors. Each ipc_port_actor_init() carves a
 * (msgq buffer, k_thread stack) pair from the pool. The pool slot
 * count and per-slot sizes are fixed at build time via Kconfig.
 *
 * The pool is sized for the *worst case*: every slot gets the same
 * max stack and max msgq. An actor that asks for more than the max
 * fails to init with -ENOMEM.
 */
#define POOL_N_ACTORS CONFIG_ACTOR_MAX_ACTORS
#define POOL_SLOT_STACK_SIZE CONFIG_ACTOR_MAX_STACK_SIZE
#define POOL_SLOT_MSGQ_DEPTH CONFIG_ACTOR_MAX_QUEUE_DEPTH
#define POOL_SLOT_MSGQ_BYTES ((size_t) POOL_SLOT_MSGQ_DEPTH * sizeof(struct ipc_msg))

/* One big aligned block for all stacks. Sliced at compile-time
 * multiples of POOL_SLOT_STACK_SIZE — Z_THREAD_STACK_ALIGNMENT is
 * always <= 8 bytes and POOL_SLOT_STACK_SIZE is a multiple of 8 by
 * default, so each slice is properly aligned for k_thread_create. */
K_THREAD_STACK_DEFINE(zephyr_stk_pool, POOL_N_ACTORS *POOL_SLOT_STACK_SIZE);
static uint8_t zephyr_msgq_pool[POOL_N_ACTORS][POOL_SLOT_MSGQ_BYTES];
static bool zephyr_pool_used[POOL_N_ACTORS];

static k_thread_stack_t *pool_stack(int slot)
{
    return (k_thread_stack_t *) ((uint8_t *) &zephyr_stk_pool[0] +
                                 (size_t) slot * POOL_SLOT_STACK_SIZE);
}

static int pool_claim(struct ipc_port_state *p, size_t stack_size, size_t queue_depth)
{
    if (stack_size > POOL_SLOT_STACK_SIZE || queue_depth > POOL_SLOT_MSGQ_DEPTH) {
        return -ENOMEM;
    }
    for (int i = 0; i < POOL_N_ACTORS; i++) {
        if (!zephyr_pool_used[i]) {
            zephyr_pool_used[i] = true;
            p->pool_slot        = i;
            return i;
        }
    }
    return -ENOMEM;
}

static void pool_release(struct ipc_port_state *p)
{
    if (p->pool_slot >= 0) {
        zephyr_pool_used[p->pool_slot] = false;
        p->pool_slot                   = -1;
    }
}

/* ── Query-wait impl (response bytes at IPC_QUERY_RESPONSE_OFFSET) ──────── */

struct ipc_query_wait_impl {
    uint8_t response[IPC_QUERY_RESPONSE_SIZE];
    int status;
    bool expired;
    struct k_poll_signal signal;
};

_Static_assert(sizeof(struct ipc_query_wait_impl) <= sizeof(ipc_query_wait_t),
               "Increase IPC_QUERY_WAIT_WORDS for Zephyr query wait");

static struct ipc_query_wait_impl *qw_of(ipc_query_wait_t *w)
{
    return (struct ipc_query_wait_impl *) (void *) w;
}

int ipc_port_query_wait_init(ipc_query_wait_t *w)
{
    struct ipc_query_wait_impl *impl = qw_of(w);
    memset(impl, 0, sizeof(*impl));
    k_poll_signal_init(&impl->signal);
    return 0;
}

void ipc_port_query_wait_destroy(ipc_query_wait_t *w)
{
    (void) w; /* nothing to clean up on Zephyr */
}

int ipc_port_query_wait_block(ipc_query_wait_t *w, ipc_timeout_t timeout)
{
    struct ipc_query_wait_impl *impl = qw_of(w);
    struct k_poll_event events[1]    = {
        K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &impl->signal),
    };
    int rc = k_poll(events, 1, timeout == IPC_TIMEOUT_FOREVER ? K_FOREVER : K_MSEC(timeout));
    if (rc == -EAGAIN) {
        impl->expired = true;
        return -ETIMEDOUT;
    }
    if (rc != 0) {
        return rc;
    }
    return impl->status;
}

void ipc_port_query_wait_wake(ipc_query_wait_t *w)
{
    struct ipc_query_wait_impl *impl = qw_of(w);
    impl->status                     = 0;
    k_poll_signal_raise(&impl->signal, 0);
}

/* ── Actor thread ────────────────────────────────────────────────────────── */

static void ipc_thread_fn(void *p1, void *p2, void *p3)
{
    struct ipc_actor *self = (struct ipc_actor *) p1;
    (void) p2;
    (void) p3;

    struct k_poll_event events[1] = {
        K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY,
                                 &port_of(self)->signal),
    };

    while (true) {
        k_poll(events, 1, K_FOREVER);

        if (events[0].state == K_POLL_STATE_SIGNALED) {
            k_poll_signal_reset(&port_of(self)->signal);
            events[0].state = K_POLL_STATE_NOT_READY;

            struct ipc_msg msg;
            while (k_msgq_get(&port_of(self)->msgq, &msg, K_NO_WAIT) == 0) {
                self->handler(self, &msg);
            }
        }
    }
}

/* ── Delayed work handler ────────────────────────────────────────────────── */

static void delayed_work_fn(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct ipc_port_state *p       = CONTAINER_OF(dwork, struct ipc_port_state, delayed_work);
    struct ipc_actor *a            = p->owner;

    ipc_port_send(a, &p->delayed_msg);
}

/* ── Port API ────────────────────────────────────────────────────────────── */

int ipc_port_actor_init(struct ipc_actor *a)
{
    table_init_once();

    struct ipc_port_state *p = port_of(a);
    /* Defensive: the port state lives in the actor's opaque blob and
     * is unitialised at first use. Make sure pool_slot starts at -1
     * so a later stop without a successful init doesn't release an
     * unowned slot. */
    p->pool_slot             = -1;
    p->stack                 = NULL;

    size_t stack_size        = a->cfg.stack_size > 0 ? a->cfg.stack_size : 1024;
    size_t queue_depth       = a->cfg.queue_depth > 0 ? a->cfg.queue_depth : 8;

    int slot                 = pool_claim(p, stack_size, queue_depth);
    if (slot < 0) {
        return slot;
    }
    p->stack = pool_stack(slot);

    /* The actor is fully initialised and its thread is now live.
     * Spawning inside ipc_actor_init is what lets the Zephyr sample
     * wire each module with its own SYS_INIT and leave main() empty:
     * by the time SYS_INIT returns, the actor is scheduled and
     * polling its msgq. */
    k_msgq_init(&p->msgq, (char *) zephyr_msgq_pool[slot], sizeof(struct ipc_msg), queue_depth);
    k_poll_signal_init(&p->signal);
    p->owner = a;
    k_work_init_delayable(&p->delayed_work, delayed_work_fn);

    k_thread_create(&p->thread, p->stack, a->cfg.stack_size, ipc_thread_fn, a, NULL, NULL,
                    a->cfg.priority, 0, K_NO_WAIT);
    k_thread_name_set(&p->thread, a->name);
    return 0;
}

int ipc_port_start(struct ipc_actor *a)
{
    /* Actor thread is already spawned in ipc_port_actor_init.
     * This hook is kept for port-interface compatibility but
     * performs no work on Zephyr — the actor is live the moment
     * ipc_actor_init returns. */
    (void) a;
    return 0;
}

int ipc_port_send(struct ipc_actor *a, const struct ipc_msg *msg)
{
    struct ipc_port_state *p = port_of(a);
    int rc                   = k_msgq_put(&p->msgq, msg, K_NO_WAIT);
    if (rc == 0) {
        k_poll_signal_raise(&p->signal, 0);
    }
    return (rc == 0) ? 0 : -ENOMEM;
}

int ipc_port_send_after(struct ipc_actor *a, const struct ipc_msg *msg, uint32_t delay_ms)
{
    struct ipc_port_state *p = port_of(a);
    p->delayed_msg           = *msg;
    k_work_reschedule(&p->delayed_work, K_MSEC(delay_ms));
    return 0;
}

int ipc_port_run_all(void)
{
    /*
     * On Zephyr the actor's k_thread is spawned inside
     * ipc_port_actor_init (called by ipc_actor_init during the
     * module's own SYS_INIT). The kernel keeps scheduling those
     * threads until the app calls exit() (or the last thread
     * returns), so there is nothing to join here. ipc_run_all()
     * is a no-op on Zephyr; calling it is still safe and gives a
     * single portable "wait for shutdown" point for code that
     * wants to be cross-platform.
     */
    return 0;
}

void ipc_port_stop_actor(struct ipc_actor *a)
{
    struct ipc_port_state *p = port_of(a);
    k_work_cancel_delayable(&p->delayed_work);
    k_thread_abort(&p->thread);
    pool_release(p);
}
