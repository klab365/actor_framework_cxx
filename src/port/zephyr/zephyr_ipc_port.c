/*
 * zephyr_ipc_port.c — Zephyr implementation of the generic ipc_port interface.
 *
 * k_msgq + k_poll_signal + k_work_delayable + k_thread.
 * The actor's msgq buffer and thread stack must be supplied by the caller
 */
#include "ipc.h"
#include "ipc_port.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>

extern struct ipc_actor *_ipc_actor_list;

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
    uint8_t *msgq_buf; /* assigned by caller via init helper */
    struct k_poll_signal signal;
    struct k_thread thread;
    k_thread_stack_t *stack;
    struct k_work_delayable delayed_work;
    struct ipc_msg delayed_msg;
    struct ipc_actor *owner;
};

_Static_assert(sizeof(struct ipc_port_state) <= sizeof(ipc_port_state_t),
               "Increase IPC_PORT_STATE_WORDS for Zephyr port state");

static struct ipc_port_state *port_of(struct ipc_actor *a)
{
    return (struct ipc_port_state *) (void *) &a->port;
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
    /* On Zephyr, msgq_buf and stack must be wired before ipc_port_start.
     * See TODO in README about ipc_actor_init_zephyr helper. */
    table_init_once();
    (void) a;
    return 0;
}

int ipc_port_start(struct ipc_actor *a)
{
    struct ipc_port_state *p = port_of(a);
    size_t cap               = a->cfg.queue_depth > 0 ? a->cfg.queue_depth : 8;

    if (!p->msgq_buf || !p->stack) {
        return -EINVAL;
    }

    k_msgq_init(&p->msgq, (char *) p->msgq_buf, sizeof(struct ipc_msg), cap);
    k_poll_signal_init(&p->signal);
    p->owner = a;
    k_work_init_delayable(&p->delayed_work, delayed_work_fn);

    k_thread_create(&p->thread, p->stack, a->cfg.stack_size, ipc_thread_fn, a, NULL, NULL,
                    a->cfg.priority, 0, K_NO_WAIT);
    k_thread_name_set(&p->thread, a->name);
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
    /* Zephyr threads are already running (started via SYS_INIT). */
    return 0;
}

void ipc_port_stop_actor(struct ipc_actor *a)
{
    struct ipc_port_state *p = port_of(a);
    k_work_cancel_delayable(&p->delayed_work);
    k_thread_abort(&p->thread);
}
