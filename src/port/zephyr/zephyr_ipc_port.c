/*
 * zephyr_ipc_port.c — Zephyr implementation of the generic ipc_port interface.
 *
 * k_msgq + optional k_work_delayable + k_thread.
 *
 * Actors declared with IPC_ACTOR_DEFINE() bring compile-time-declared
 * k_thread stack and k_msgq storage. Zephyr may add architecture-specific
 * stack overhead, so the port stores the usable K_THREAD_STACK_SIZEOF() value.
 * No actor stack/msgq pool is reserved by the port.
 */
#include "ipc.h"
#include "ipc_port.h"
#include "ipc_port_state.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>

/* ── Per-actor port state (concrete layout) ─────────────────────────────── */

static struct ipc_port_state *port_of(struct ipc_actor *a)
{
    return (struct ipc_port_state *) a->port;
}

static size_t actor_max_payload_size(const struct ipc_actor *a)
{
    return a->cfg.max_payload_size;
}

#if defined(CONFIG_ACTOR_ASK)
enum { ZEPHYR_ASK_TIMEOUT_CAPACITY = CONFIG_ACTOR_MAX_PENDING_ASK_TIMEOUTS };
typedef struct {
    struct k_work_delayable work;
    struct ipc_actor *actor;
    uint32_t ask_id;
    bool used;
} zephyr_ask_timeout_t;
static zephyr_ask_timeout_t ask_timeouts[ZEPHYR_ASK_TIMEOUT_CAPACITY];
K_MUTEX_DEFINE(ask_timeout_lock);
#endif

/* ── Queue slot helpers ─────────────────────────────────────────────────── */

static int store_msg(const struct ipc_port_state *p, char *slot, const struct ipc_msg *msg)
{
    if (msg->size > p->slot_size - IPC_MSG_SLOT_HEADER_SIZE) {
        return -EMSGSIZE;
    }

    ipc_msg_slot_header_t *header = (ipc_msg_slot_header_t *) slot;
    header->id                    = msg->id;
    header->kind                  = msg->kind;
    header->size                  = msg->size;
    header->ask_id                = msg->ask_id;
    header->reply_id              = msg->reply_id;
    header->result                = msg->result;

    char *payload                 = slot + IPC_MSG_SLOT_HEADER_SIZE;
    if (msg->size > 0) {
        if (msg->payload) {
            memcpy(payload, msg->payload, msg->size);
        } else {
            memset(payload, 0, msg->size);
        }
    }
    return 0;
}

static struct ipc_msg load_msg(const char *slot)
{
    const ipc_msg_slot_header_t *header = (const ipc_msg_slot_header_t *) slot;
    struct ipc_msg msg                  = {
        .id       = header->id,
        .kind     = header->kind,
        .size     = header->size,
        .payload  = (const uint8_t *) (slot + IPC_MSG_SLOT_HEADER_SIZE),
        .ask_id   = header->ask_id,
        .reply_id = header->reply_id,
        .result   = header->result,
    };
    return msg;
}

/* ── Actor thread ────────────────────────────────────────────────────────── */

static void ipc_thread_fn(void *p1, void *p2, void *p3)
{
    struct ipc_actor *self = (struct ipc_actor *) p1;
    (void) p2;
    (void) p3;

    struct ipc_port_state *p = port_of(self);

    while (true) {
        k_msgq_get(&p->msgq, p->recv_slot, K_FOREVER);
        struct ipc_msg msg = load_msg(p->recv_slot);
        self->handler(self, &msg);

        while (k_msgq_get(&p->msgq, p->recv_slot, K_NO_WAIT) == 0) {
            struct ipc_msg next = load_msg(p->recv_slot);
            self->handler(self, &next);
        }
    }
}

#if defined(CONFIG_ACTOR_SEND_AFTER)
/* ── Delayed work handler ────────────────────────────────────────────────── */

static void delayed_work_fn(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct ipc_port_state *p       = CONTAINER_OF(dwork, struct ipc_port_state, delayed_work);
    struct ipc_actor *a            = p->owner;

    struct ipc_msg msg             = p->delayed_msg;
    msg.payload                    = (const uint8_t *) p->delayed_payload;
    ipc_port_send(a, &msg);
}
#endif

/* ── Port API ────────────────────────────────────────────────────────────── */

int ipc_port_actor_init(struct ipc_actor *a)
{
    struct ipc_port_state *p = port_of(a);

    if (p->stack == NULL || p->msgq_buf == NULL || p->stack_size == 0 || p->queue_depth == 0 ||
        p->slot_size < IPC_MSG_SLOT_HEADER_SIZE || p->send_slot == NULL || p->recv_slot == NULL) {
        return -EINVAL;
    }

    /* Static actor macros declare resources and record their usable limits.
     * Treat cfg mismatches as programming errors rather than silently using a
     * different limit than the storage was declared for. */
    if (a->cfg.stack_size != p->stack_size || a->cfg.queue_depth != p->queue_depth) {
        return -EINVAL;
    }

    /* The actor is fully initialised and its thread is now live.
     * ipc_start_all_actors() calls into this port hook after the
     * application has registered/subscribed routes. */
    k_msgq_init(&p->msgq, p->msgq_buf, p->slot_size, p->queue_depth);
#if defined(CONFIG_ACTOR_SEND_AFTER)
    if (p->delayed_payload == NULL) {
        return -EINVAL;
    }
    p->owner = a;
    k_work_init_delayable(&p->delayed_work, delayed_work_fn);
    k_mutex_init(&p->delay_lock);
#endif

    k_thread_create(&p->thread, p->stack, p->stack_size, ipc_thread_fn, a, NULL, NULL,
                    a->cfg.priority, 0, K_NO_WAIT);
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
    if (msg->size > actor_max_payload_size(a)) {
        return -EMSGSIZE;
    }

    k_spinlock_key_t key = k_spin_lock(&p->send_lock);
    int rc               = store_msg(p, p->send_slot, msg);
    if (rc == 0) {
        rc = k_msgq_put(&p->msgq, p->send_slot, K_NO_WAIT);
    }
    k_spin_unlock(&p->send_lock, key);

    return (rc == 0) ? 0 : -ENOMEM;
}

int ipc_port_send_isr(struct ipc_actor *a, const struct ipc_msg *msg)
{
    return ipc_port_send(a, msg);
}

#if defined(CONFIG_ACTOR_ASK)
static void ask_timeout_work_fn(struct k_work *work)
{
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    zephyr_ask_timeout_t *timeout      = CONTAINER_OF(delayable, zephyr_ask_timeout_t, work);
    k_mutex_lock(&ask_timeout_lock, K_FOREVER);
    timeout->used           = false;
    struct ipc_actor *actor = timeout->actor;
    uint32_t ask_id         = timeout->ask_id;
    k_mutex_unlock(&ask_timeout_lock);
    ipc_ask_timeout_expired(actor, ask_id);
}
#endif

int ipc_port_schedule_ask_timeout(struct ipc_actor *a, uint32_t ask_id, uint32_t timeout_ms)
{
#if defined(CONFIG_ACTOR_ASK)
    k_mutex_lock(&ask_timeout_lock, K_FOREVER);
    for (int i = 0; i < ZEPHYR_ASK_TIMEOUT_CAPACITY; i++) {
        if (!ask_timeouts[i].used) {
            zephyr_ask_timeout_t *timeout = &ask_timeouts[i];
            timeout->actor                = a;
            timeout->ask_id               = ask_id;
            timeout->used                 = true;
            k_work_init_delayable(&timeout->work, ask_timeout_work_fn);
            k_work_schedule(&timeout->work, K_MSEC(timeout_ms));
            k_mutex_unlock(&ask_timeout_lock);
            return 0;
        }
    }
    k_mutex_unlock(&ask_timeout_lock);
    return -ENOMEM;
#else
    (void) a;
    (void) ask_id;
    (void) timeout_ms;
    return -ENOSYS;
#endif
}

int ipc_port_send_after(struct ipc_actor *a, const struct ipc_msg *msg, uint32_t delay_ms)
{
#if defined(CONFIG_ACTOR_SEND_AFTER)
    struct ipc_port_state *p = port_of(a);
    if (msg->size > actor_max_payload_size(a)) {
        return -EMSGSIZE;
    }

    /* Serialize send_after callers and ensure any in-flight delayed work has
     * fully finished (including its ipc_port_send tail) before we overwrite
     * the shared delayed_msg/delayed_payload buffers. This mirrors the POSIX
     * port's delay_lock + join contract. */
    k_mutex_lock(&p->delay_lock, K_FOREVER);
    k_work_cancel_delayable(&p->delayed_work);
    k_work_flush(&p->delayed_work.work, &p->delayed_work_sync);

    p->delayed_msg         = *msg;
    p->delayed_msg.payload = (const uint8_t *) p->delayed_payload;
    if (msg->size > 0) {
        if (msg->payload) {
            memcpy(p->delayed_payload, msg->payload, msg->size);
        } else {
            memset(p->delayed_payload, 0, msg->size);
        }
    }
    k_work_reschedule(&p->delayed_work, K_MSEC(delay_ms));
    k_mutex_unlock(&p->delay_lock);
    return 0;
#else
    (void) a;
    (void) msg;
    (void) delay_ms;
    return -ENOSYS;
#endif
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

uint32_t ipc_port_now_ms(void)
{
    return k_uptime_get_32();
}

#if defined(CONFIG_ACTOR_SEND_AFTER)
static void cancel_delayed_send(struct ipc_port_state *p)
{
    k_mutex_lock(&p->delay_lock, K_FOREVER);
    k_work_cancel_delayable(&p->delayed_work);
    k_work_flush(&p->delayed_work.work, &p->delayed_work_sync);
    k_mutex_unlock(&p->delay_lock);
}
#endif

void ipc_port_stop_actor(struct ipc_actor *a)
{
    struct ipc_port_state *p = port_of(a);
#if defined(CONFIG_ACTOR_SEND_AFTER)
    cancel_delayed_send(p);
#endif
    k_thread_abort(&p->thread);
}

int ipc_port_restart_actor(struct ipc_actor *a)
{
    struct ipc_port_state *p = port_of(a);

    /* Soft restart for message-driven actors: cancel delayed work and
     * drop queued messages. The actor thread keeps running, so this is
     * safe even when an actor reports failure from inside its own handler. */
#if defined(CONFIG_ACTOR_SEND_AFTER)
    cancel_delayed_send(p);
#endif
    k_msgq_purge(&p->msgq);
    return 0;
}
