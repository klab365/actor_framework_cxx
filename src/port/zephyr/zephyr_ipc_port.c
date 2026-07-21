/*
 * zephyr_ipc_port.c — Zephyr implementation of the generic ipc_port interface.
 *
 * k_msgq + k_poll_signal + k_work_delayable + k_thread.
 *
 * Actors declared with IPC_ACTOR_DEFINE() bring compile-time-declared
 * k_thread stack and k_msgq storage. Zephyr may add architecture-specific
 * stack overhead, so the port stores the usable K_THREAD_STACK_SIZEOF() value.
 * No actor stack/msgq pool is reserved by the port.
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
    k_thread_stack_t *stack;
    struct k_work_delayable delayed_work;
    struct ipc_msg delayed_msg;
    struct ipc_actor *owner;
};

_Static_assert(sizeof(struct ipc_port_state) <= sizeof(ipc_port_state_t),
               "Increase ipc_port_state_t opaque storage for Zephyr port state");

static struct ipc_port_state *port_of(struct ipc_actor *a)
{
    return (struct ipc_port_state *) (void *) &a->port;
}

/* ── Per-actor static resources ────────────────────────────────────────────
 *
 * IPC_ACTOR_DEFINE emits one stack and one msgq buffer per actor,
 * then registers them here before application init runs. When
 * ipc_start_all_actors() reaches the actor, ipc_port_actor_init()
 * uses the registered storage directly.
 */
enum { ZEPHYR_STATIC_ACTOR_CAPACITY = 32 };

struct zephyr_static_actor_resources {
    struct ipc_actor *actor;
    void *stack;
    size_t stack_size;
    char *msgq_buf;
    size_t queue_depth;
};

static struct zephyr_static_actor_resources static_actor_resources[ZEPHYR_STATIC_ACTOR_CAPACITY];
static int static_actor_resource_count;

int ipc_port_register_static_actor_resources(struct ipc_actor *actor, void *stack,
                                             size_t stack_size, char *msgq_buf, size_t queue_depth)
{
    if (actor == NULL || stack == NULL || msgq_buf == NULL || stack_size == 0 || queue_depth == 0) {
        return -EINVAL;
    }

    for (int i = 0; i < static_actor_resource_count; i++) {
        if (static_actor_resources[i].actor == actor) {
            static_actor_resources[i] = (struct zephyr_static_actor_resources) {
                .actor       = actor,
                .stack       = stack,
                .stack_size  = stack_size,
                .msgq_buf    = msgq_buf,
                .queue_depth = queue_depth,
            };
            return 0;
        }
    }

    if (static_actor_resource_count >= ZEPHYR_STATIC_ACTOR_CAPACITY) {
        return -ENOMEM;
    }

    static_actor_resources[static_actor_resource_count++] = (struct zephyr_static_actor_resources) {
        .actor       = actor,
        .stack       = stack,
        .stack_size  = stack_size,
        .msgq_buf    = msgq_buf,
        .queue_depth = queue_depth,
    };
    return 0;
}

static struct zephyr_static_actor_resources *static_resources_for(struct ipc_actor *actor)
{
    for (int i = 0; i < static_actor_resource_count; i++) {
        if (static_actor_resources[i].actor == actor) {
            return &static_actor_resources[i];
        }
    }
    return NULL;
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

    struct ipc_port_state *p                         = port_of(a);
    p->stack                                         = NULL;

    size_t stack_size                                = a->cfg.stack_size;
    size_t queue_depth                               = a->cfg.queue_depth;

    struct zephyr_static_actor_resources *static_res = static_resources_for(a);
    if (static_res == NULL) {
        return -EINVAL;
    }

    /* Static actor macros declare resources and record their usable limits.
     * Treat cfg mismatches as programming errors rather than silently using a
     * different limit than the storage was declared for. */
    if (stack_size != static_res->stack_size || queue_depth != static_res->queue_depth) {
        return -EINVAL;
    }
    p->stack       = (k_thread_stack_t *) static_res->stack;
    char *msgq_buf = static_res->msgq_buf;

    /* The actor is fully initialised and its thread is now live.
     * ipc_start_all_actors() calls into this port hook after the
     * application has registered/subscribed routes. */
    k_msgq_init(&p->msgq, msgq_buf, sizeof(struct ipc_msg), queue_depth);
    k_poll_signal_init(&p->signal);
    p->owner = a;
    k_work_init_delayable(&p->delayed_work, delayed_work_fn);

    k_thread_create(&p->thread, p->stack, stack_size, ipc_thread_fn, a, NULL, NULL, a->cfg.priority,
                    0, K_NO_WAIT);
    k_thread_name_set(&p->thread, a->name);
    return 0;
}

int ipc_port_start(struct ipc_actor *a)
{
    /* Actor thread is already spawned in ipc_port_actor_init.
     * This hook is kept for port-interface compatibility but
     * performs no work on Zephyr. */
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
     * ipc_port_actor_init (called by ipc_start_all_actors during the
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
}
