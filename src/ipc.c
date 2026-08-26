/*
 * ipc.c — Platform-agnostic core: actor registry, registration table,
 *          subscription table, send/publish.
 *
 * No #ifdefs. All platform-specific behaviour lives in ipc_port.h
 * and is implemented per-platform in posix_ipc_port.c / zephyr_ipc_port.c.
 */
#include "ipc.h"
#include "ipc_internal.h"
#include "ipc_port.h"

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#if IPC_CONFIG_ENABLE_DIAGNOSTICS
#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h>

#if IPC_CONFIG_ENABLE_DIAGNOSTICS
#define IPC_DIAG(...) fprintf(stderr, __VA_ARGS__)
#else
#define IPC_DIAG(...) ((void) 0)
#endif

/* ── Port seam forward declarations ─────────────────────────────────────── */

int ipc_port_start(struct ipc_actor *a);
int ipc_port_send(struct ipc_actor *a, const struct ipc_msg *msg);
int ipc_port_send_after(struct ipc_actor *a, const struct ipc_msg *msg, uint32_t delay_ms);
void ipc_port_stop_actor(struct ipc_actor *a);
int ipc_port_restart_actor(struct ipc_actor *a);

/* ── Global actor registry (singly-linked list) ──────────────────────────── */

struct ipc_actor *_ipc_actor_list = NULL;

/* ── Registration table ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t msg_id;
    struct ipc_actor *actor;
} ipc_registration_t;

static ipc_registration_t reg_table[IPC_CORE_MAX_REGISTRATIONS];
static int reg_count;

/* ── Subscription table ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t msg_id;
    struct ipc_actor *actor;
} ipc_subscription_t;

#if IPC_CORE_MAX_SUBSCRIPTIONS > 0
static ipc_subscription_t sub_table[IPC_CORE_MAX_SUBSCRIPTIONS];
#endif
static int sub_count;

typedef enum {
    IPC_LIFECYCLE_STOPPED = 0,
    IPC_LIFECYCLE_RUNNING,
    IPC_LIFECYCLE_STOPPING,
} ipc_lifecycle_state_t;

static atomic_int lifecycle_state = ATOMIC_VAR_INIT(IPC_LIFECYCLE_STOPPED);
#if IPC_CONFIG_ENABLE_ASK
static uint32_t next_ask_id = 1;
#endif

typedef struct {
    struct ipc_actor *actor;
    uint32_t msg_id;
    ipc_actor_msg_handler_t handler;
} ipc_handler_binding_t;

static ipc_handler_binding_t handler_table[IPC_CORE_MAX_REGISTRATIONS + IPC_CORE_MAX_SUBSCRIPTIONS];
static int handler_count;

#if IPC_CONFIG_ENABLE_ASK
typedef struct {
    bool used;
    bool reply_sent;
    bool timeout_queued;
    uint32_t ask_id;
    uint32_t reply_id;
    uint32_t deadline_ms;
    struct ipc_actor *actor;
    ipc_ask_callback_t callback;
} ipc_pending_ask_t;

static ipc_pending_ask_t ask_table[IPC_CORE_MAX_INFLIGHT_QUERIES];
static atomic_flag ask_lock                  = ATOMIC_FLAG_INIT;

static const uint32_t ipc_ask_timeout_msg_id = UINT32_MAX;

static void lock_asks(void)
{
    while (atomic_flag_test_and_set_explicit(&ask_lock, memory_order_acquire)) {
        /* Spin until the ask table lock is available. */
    }
}

static void unlock_asks(void)
{
    atomic_flag_clear_explicit(&ask_lock, memory_order_release);
}
#endif

/* ── Test reset ─────────────────────────────────────────────────────────── */
/* See declaration in ipc_internal.h. Production code must never call this. */
void _ipc_set_next_ask_id_for_testing(uint32_t next_id)
{
#if IPC_CONFIG_ENABLE_ASK
    lock_asks();
    next_ask_id = next_id;
    unlock_asks();
#else
    (void) next_id;
#endif
}

void _ipc_reset_for_testing(void)
{
    reg_count     = 0;
    sub_count     = 0;
    handler_count = 0;
    memset(reg_table, 0, sizeof(reg_table));
#if IPC_CORE_MAX_SUBSCRIPTIONS > 0
    memset(sub_table, 0, sizeof(sub_table));
#endif
    memset(handler_table, 0, sizeof(handler_table));
#if IPC_CONFIG_ENABLE_ASK
    lock_asks();
    memset(ask_table, 0, sizeof(ask_table));
    next_ask_id = 1;
    unlock_asks();
#endif
    atomic_store_explicit(&lifecycle_state, IPC_LIFECYCLE_STOPPED, memory_order_release);
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
    uint32_t id = __atomic_load_n(&d->id, __ATOMIC_ACQUIRE);
    if (id == 0) {
        uint32_t expected = 0;
        uint32_t computed = _ipc_fnv1a(d->name);
        (void) __atomic_compare_exchange_n(&d->id, &expected, computed, false, __ATOMIC_RELEASE,
                                           __ATOMIC_ACQUIRE);
    }
}

static int register_cmd_unlocked(struct ipc_actor *actor, ipc_msg_desc_t *desc)
{
    assert(desc->kind == IPC_CMD);
    _ipc_ensure_id(desc);

    for (int i = 0; i < reg_count; i++) {
        if (reg_table[i].msg_id == desc->id) {
            IPC_DIAG("ipc: duplicate registration for '%s'\n", desc->name);
            assert(0 && "duplicate IPC_ACTOR_HANDLE command route");
            return -EALREADY;
        }
    }

    if (reg_count >= IPC_CORE_MAX_REGISTRATIONS) {
        assert(0 && "IPC command route table full");
        return -ENOMEM;
    }

    reg_table[reg_count].msg_id = desc->id;
    reg_table[reg_count].actor  = actor;
    reg_count++;

    return 0;
}

static int subscribe_event_unlocked(struct ipc_actor *actor, ipc_msg_desc_t *desc)
{
    assert(desc->kind == IPC_EVENT);
    _ipc_ensure_id(desc);

#if IPC_CORE_MAX_SUBSCRIPTIONS > 0
    for (int i = 0; i < sub_count; i++) {
        if (sub_table[i].msg_id == desc->id && sub_table[i].actor == actor) {
            IPC_DIAG("ipc: duplicate subscription for '%s'\n", desc->name);
            return -EALREADY;
        }
    }

    if (sub_count >= IPC_CORE_MAX_SUBSCRIPTIONS) {
        return -ENOMEM;
    }

    sub_table[sub_count].msg_id = desc->id;
    sub_table[sub_count].actor  = actor;
    sub_count++;

    return 0;
#else
    (void) actor;
    return -ENOMEM;
#endif
}

/* ── Helper: find registration ───────────────────────────────────────────── */

static size_t actor_max_payload_size(const struct ipc_actor *actor)
{
    return actor->cfg.max_payload_size;
}

static struct ipc_actor *find_registered(uint32_t msg_id)
{
    for (int i = 0; i < reg_count; i++) {
        if (reg_table[i].msg_id == msg_id) {
            return reg_table[i].actor;
        }
    }
    return NULL;
}

#if IPC_CONFIG_ENABLE_ASK
static ipc_pending_ask_t *find_pending_ask(uint32_t ask_id)
{
    for (int i = 0; i < IPC_CORE_MAX_INFLIGHT_QUERIES; i++) {
        if (ask_table[i].used && ask_table[i].ask_id == ask_id) {
            return &ask_table[i];
        }
    }
    return NULL;
}

static ipc_pending_ask_t *alloc_pending_ask(void)
{
    for (int i = 0; i < IPC_CORE_MAX_INFLIGHT_QUERIES; i++) {
        if (!ask_table[i].used) {
            return &ask_table[i];
        }
    }
    return NULL;
}

void ipc_ask_timeout_expired(struct ipc_actor *actor, uint32_t ask_id)
{
    lock_asks();
    ipc_pending_ask_t *pending = find_pending_ask(ask_id);
    if (!pending || pending->actor != actor || pending->timeout_queued) {
        unlock_asks();
        return;
    }
    pending->reply_sent     = true;
    pending->timeout_queued = true;
    unlock_asks();

    struct ipc_msg timeout_msg = {
        .id     = ipc_ask_timeout_msg_id,
        .kind   = IPC_CMD,
        .ask_id = ask_id,
        .result = -ETIMEDOUT,
    };
    if (ipc_port_send(actor, &timeout_msg) != 0) {
        lock_asks();
        pending = find_pending_ask(ask_id);
        if (pending && pending->actor == actor) {
            pending->reply_sent     = false;
            pending->timeout_queued = false;
        }
        unlock_asks();
    }
}

static int alloc_ask_id(uint32_t *ask_id)
{
    size_t candidates_checked = 0;
    while (candidates_checked < IPC_CORE_MAX_INFLIGHT_QUERIES) {
        uint32_t candidate = next_ask_id++;
        if (candidate == 0) {
            continue;
        }
        candidates_checked++;
        if (!find_pending_ask(candidate)) {
            *ask_id = candidate;
            return 0;
        }
    }
    return -ENOMEM;
}
#else
void ipc_ask_timeout_expired(struct ipc_actor *actor, uint32_t ask_id)
{
    (void) actor;
    (void) ask_id;
}
#endif

static bool registration_closed(const char *op)
{
    if (atomic_load_explicit(&lifecycle_state, memory_order_acquire) != IPC_LIFECYCLE_STOPPED) {
        IPC_DIAG("ipc: %s after ipc_start_all_actors() is not supported\n", op);
        assert(0 && "IPC runtime registration is forbidden");
        return true;
    }
    return false;
}

/* ── Static actor registration ───────────────────────────────────────────── */

void _ipc_actor_register_static(struct ipc_actor *actor)
{
    if (registration_closed("actor registration")) {
        return;
    }

    /* Static actor startup hooks run during process/kernel startup before
     * application concurrency begins, so keep this registration path free
     * of port locks (some ports' kernel primitives are not ready during
     * early startup). */
    struct ipc_actor **pp = &_ipc_actor_list;
    while (*pp) {
        if (*pp == actor) {
            return;
        }
        pp = &(*pp)->_next;
    }

    actor->_next = NULL;
    *pp          = actor;
}

void _ipc_actor_register_handler_static(struct ipc_actor *actor, ipc_msg_desc_t *desc,
                                        ipc_actor_msg_handler_t handler)
{
    if (registration_closed("handler registration")) {
        return;
    }

    _ipc_actor_register_static(actor);
    _ipc_ensure_id(desc);

    int rc = desc->kind == IPC_EVENT ? subscribe_event_unlocked(actor, desc)
                                     : register_cmd_unlocked(actor, desc);
    if (rc) {
        /* Static registration has no return path to its constructor caller.
         * Continuing would leave a handler with no command route. */
        IPC_DIAG("ipc: handler registration for '%s' failed: %d\n", desc->name, rc);
        abort();
    }

    if (handler_count >= (IPC_CORE_MAX_REGISTRATIONS + IPC_CORE_MAX_SUBSCRIPTIONS)) {
        IPC_DIAG("ipc: handler table full\n");
        abort();
    }

    handler_table[handler_count].actor   = actor;
    handler_table[handler_count].msg_id  = desc->id;
    handler_table[handler_count].handler = handler;
    handler_count++;
}

void _ipc_actor_register_start_hook_static(struct ipc_actor *actor, ipc_actor_lifecycle_hook_t hook)
{
    if (registration_closed("start hook registration")) {
        return;
    }
    _ipc_actor_register_static(actor);
    actor->start_hook = hook;
}

void _ipc_actor_register_stop_hook_static(struct ipc_actor *actor, ipc_actor_lifecycle_hook_t hook)
{
    if (registration_closed("stop hook registration")) {
        return;
    }
    _ipc_actor_register_static(actor);
    actor->stop_hook = hook;
}

void _ipc_actor_register_unknown_hook_static(struct ipc_actor *actor,
                                             ipc_actor_unknown_handler_t hook)
{
    if (registration_closed("unknown hook registration")) {
        return;
    }
    _ipc_actor_register_static(actor);
    actor->unknown_handler = hook;
}

void _ipc_actor_register_supervision_static(struct ipc_actor *actor,
                                            ipc_supervision_strategy_t strategy)
{
    if (registration_closed("supervision registration")) {
        return;
    }
    _ipc_actor_register_static(actor);
    actor->supervision = strategy;
}

void _ipc_actor_register_failure_hook_static(struct ipc_actor *actor, ipc_actor_failure_hook_t hook)
{
    if (registration_closed("failure hook registration")) {
        return;
    }
    _ipc_actor_register_static(actor);
    actor->failure_hook = hook;
}

static struct ipc_msg make_msg(const ipc_msg_desc_t *desc, const void *payload)
{
    struct ipc_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.id      = desc->id;
    msg.kind    = desc->kind;
    msg.size    = desc->size;
    msg.payload = (const uint8_t *) payload;
    return msg;
}

static int prepare_registered_cmd(ipc_msg_desc_t *desc, const void *payload, const char *op,
                                  struct ipc_actor **target, struct ipc_msg *msg)
{
    _ipc_ensure_id(desc);
    *target = find_registered(desc->id);
    if (!*target) {
        IPC_DIAG("ipc: %s '%s' — not registered\n", op, desc->name);
        return -ENOENT;
    }
    if (desc->size > actor_max_payload_size(*target)) {
        return -EMSGSIZE;
    }

    *msg = make_msg(desc, payload);
    return 0;
}

/* ── ipc_send_raw ────────────────────────────────────────────────────────── */

int ipc_send_raw(ipc_msg_desc_t *desc, const void *payload)
{
    struct ipc_actor *target;
    struct ipc_msg msg;
    int rc = prepare_registered_cmd(desc, payload, "send", &target, &msg);
    return rc ? rc : ipc_port_send(target, &msg);
}

/* ── ipc_send_after_raw ──────────────────────────────────────────────────── */

int ipc_send_after_raw(ipc_msg_desc_t *desc, uint32_t delay_ms, const void *payload)
{
    struct ipc_actor *target;
    struct ipc_msg msg;
    int rc = prepare_registered_cmd(desc, payload, "send_after", &target, &msg);
    return rc ? rc : ipc_port_send_after(target, &msg, delay_ms);
}

#if IPC_CONFIG_ENABLE_ASK
int ipc_ask_with_id_raw(struct ipc_actor *self, ipc_msg_desc_t *request_desc,
                        const void *request_payload, ipc_msg_desc_t *reply_desc,
                        ipc_ask_callback_t callback, uint32_t *ask_id_out, uint32_t timeout_ms)
{
    if (ask_id_out) {
        *ask_id_out = 0;
    }
    if (!self || !request_desc || !reply_desc || !callback || request_desc->kind != IPC_CMD ||
        reply_desc->kind != IPC_CMD) {
        return -EINVAL;
    }

    struct ipc_actor *target;
    struct ipc_msg msg;
    int rc = prepare_registered_cmd(request_desc, request_payload, "ask", &target, &msg);
    if (rc) {
        return rc;
    }

    _ipc_ensure_id(reply_desc);
    if (reply_desc->size > actor_max_payload_size(self)) {
        return -EMSGSIZE;
    }

    uint32_t now_ms = ipc_port_now_ms();

    lock_asks();
    ipc_pending_ask_t *pending = alloc_pending_ask();
    if (!pending) {
        unlock_asks();
        return -ENOMEM;
    }

    uint32_t ask_id = 0;
    rc              = alloc_ask_id(&ask_id);
    if (rc) {
        unlock_asks();
        return rc;
    }

    *pending = (ipc_pending_ask_t) {
        .used        = true,
        .ask_id      = ask_id,
        .reply_id    = reply_desc->id,
        .deadline_ms = timeout_ms > 0 ? now_ms + timeout_ms : 0,
        .actor       = self,
        .callback    = callback,
    };
    unlock_asks();

    if (timeout_ms > 0) {
        rc = ipc_port_schedule_ask_timeout(self, ask_id, timeout_ms);
        if (rc) {
            lock_asks();
            pending = find_pending_ask(ask_id);
            if (pending) {
                memset(pending, 0, sizeof(*pending));
            }
            unlock_asks();
            return rc;
        }
    }

    msg.ask_id   = ask_id;
    msg.reply_id = reply_desc->id;
    rc           = ipc_port_send(target, &msg);
    if (rc) {
        lock_asks();
        ipc_pending_ask_t *failed_pending = find_pending_ask(ask_id);
        if (failed_pending) {
            memset(failed_pending, 0, sizeof(*failed_pending));
        }
        unlock_asks();
        return rc;
    }

    if (ask_id_out) {
        *ask_id_out = ask_id;
    }
    return 0;
}

int ipc_ask_raw(struct ipc_actor *self, ipc_msg_desc_t *request_desc, const void *request_payload,
                ipc_msg_desc_t *reply_desc, ipc_ask_callback_t callback, uint32_t timeout_ms)
{
    return ipc_ask_with_id_raw(self, request_desc, request_payload, reply_desc, callback, NULL,
                               timeout_ms);
}

int ipc_ask_cancel(const struct ipc_actor *self, uint32_t ask_id)
{
    if (!self || ask_id == 0) {
        return -EINVAL;
    }
    if (atomic_load_explicit(&lifecycle_state, memory_order_acquire) != IPC_LIFECYCLE_RUNNING) {
        return -EPERM;
    }

    lock_asks();
    ipc_pending_ask_t *pending = find_pending_ask(ask_id);
    if (!pending || pending->actor != self) {
        unlock_asks();
        return -ENOENT;
    }
    memset(pending, 0, sizeof(*pending));
    unlock_asks();
    return 0;
}

int ipc_reply_raw(const struct ipc_msg *request_msg, ipc_msg_desc_t *reply_desc,
                  const void *reply_payload)
{
    if (!request_msg || !reply_desc || request_msg->ask_id == 0 || reply_desc->kind != IPC_CMD) {
        return -EINVAL;
    }

    _ipc_ensure_id(reply_desc);
    lock_asks();
    ipc_pending_ask_t *pending = find_pending_ask(request_msg->ask_id);
    if (!pending) {
        unlock_asks();
        return -ENOENT;
    }
    if (pending->reply_id != reply_desc->id || request_msg->reply_id != reply_desc->id) {
        unlock_asks();
        return -EINVAL;
    }
    if (pending->reply_sent) {
        unlock_asks();
        return -EALREADY;
    }
    struct ipc_actor *target = pending->actor;
    if (reply_desc->size > actor_max_payload_size(target)) {
        memset(pending, 0, sizeof(*pending));
        unlock_asks();
        return -EMSGSIZE;
    }
    pending->reply_sent = true;
    unlock_asks();

    struct ipc_msg msg = make_msg(reply_desc, reply_payload);
    msg.ask_id         = request_msg->ask_id;
    int rc             = ipc_port_send(target, &msg);
    if (rc) {
        lock_asks();
        ipc_pending_ask_t *failed_pending = find_pending_ask(request_msg->ask_id);
        if (failed_pending) {
            memset(failed_pending, 0, sizeof(*failed_pending));
        }
        unlock_asks();
    }
    return rc;
}

int ipc_reply_error_raw(const struct ipc_msg *request_msg, int result)
{
    if (!request_msg || request_msg->ask_id == 0 || result == 0) {
        return -EINVAL;
    }

    lock_asks();
    ipc_pending_ask_t *pending = find_pending_ask(request_msg->ask_id);
    if (!pending) {
        unlock_asks();
        return -ENOENT;
    }
    if (pending->reply_id != request_msg->reply_id) {
        unlock_asks();
        return -EINVAL;
    }
    if (pending->reply_sent) {
        unlock_asks();
        return -EALREADY;
    }

    struct ipc_actor *target = pending->actor;
    uint32_t reply_id        = pending->reply_id;
    pending->reply_sent      = true;
    unlock_asks();

    struct ipc_msg msg = {
        .id     = reply_id,
        .kind   = IPC_CMD,
        .ask_id = request_msg->ask_id,
        .result = result,
    };
    int rc = ipc_port_send(target, &msg);
    if (rc) {
        lock_asks();
        ipc_pending_ask_t *failed_pending = find_pending_ask(request_msg->ask_id);
        if (failed_pending) {
            memset(failed_pending, 0, sizeof(*failed_pending));
        }
        unlock_asks();
    }
    return rc;
}
#else
int ipc_ask_with_id_raw(struct ipc_actor *self, ipc_msg_desc_t *request_desc,
                        const void *request_payload, ipc_msg_desc_t *reply_desc,
                        ipc_ask_callback_t callback, uint32_t *ask_id_out, uint32_t timeout_ms)
{
    (void) self;
    (void) request_desc;
    (void) request_payload;
    (void) reply_desc;
    (void) callback;
    (void) timeout_ms;
    if (ask_id_out) {
        *ask_id_out = 0;
    }
    return -ENOSYS;
}

int ipc_ask_raw(struct ipc_actor *self, ipc_msg_desc_t *request_desc, const void *request_payload,
                ipc_msg_desc_t *reply_desc, ipc_ask_callback_t callback, uint32_t timeout_ms)
{
    (void) self;
    (void) request_desc;
    (void) request_payload;
    (void) reply_desc;
    (void) callback;
    (void) timeout_ms;
    return -ENOSYS;
}

int ipc_ask_cancel(const struct ipc_actor *self, uint32_t ask_id)
{
    (void) self;
    (void) ask_id;
    return -ENOSYS;
}

int ipc_reply_raw(const struct ipc_msg *request_msg, ipc_msg_desc_t *reply_desc,
                  const void *reply_payload)
{
    (void) request_msg;
    (void) reply_desc;
    (void) reply_payload;
    return -ENOSYS;
}

int ipc_reply_error_raw(const struct ipc_msg *request_msg, int result)
{
    (void) request_msg;
    (void) result;
    return -ENOSYS;
}
#endif

static int publish_prepared_msg(const struct ipc_msg *msg, uint32_t msg_id,
                                int (*send_fn)(struct ipc_actor *, const struct ipc_msg *))
{
#if IPC_CORE_MAX_SUBSCRIPTIONS > 0
    int first_rc = 0;
    for (int i = 0; i < sub_count; i++) {
        if (sub_table[i].msg_id == msg_id) {
            struct ipc_actor *actor = sub_table[i].actor;
            int rc = msg->size > actor_max_payload_size(actor) ? -EMSGSIZE : send_fn(actor, msg);
            if (rc && !first_rc) {
                first_rc = rc;
            }
        }
    }
    return first_rc;
#else
    (void) msg;
    (void) msg_id;
    (void) send_fn;
    return 0;
#endif
}

/* ── ipc_publish_raw ─────────────────────────────────────────────────────── */

int ipc_publish_raw(ipc_msg_desc_t *desc, const void *payload)
{
    /* Only EVENT descriptors may be published. Without this guard the code
     * below would silently override a command's kind and fan it out. */
    if (!desc || desc->kind != IPC_EVENT) {
        return -EINVAL;
    }
    _ipc_ensure_id(desc);

    struct ipc_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.id      = desc->id;
    msg.kind    = IPC_EVENT;
    msg.size    = desc->size;
    msg.payload = (const uint8_t *) payload;

    return publish_prepared_msg(&msg, desc->id, ipc_port_send);
}

/* ── ipc_send_to_raw ────────────────────────────────────────────────────── */

int ipc_send_to_raw(struct ipc_actor *actor, const ipc_msg_desc_t *desc, const void *payload)
{
    /* Direct delivery intentionally preserves either command or event kind;
     * callers use it for local mailbox posting, not route/subscription lookup. */
    if (!actor || !desc) {
        return -EINVAL;
    }
    if (atomic_load_explicit(&lifecycle_state, memory_order_acquire) != IPC_LIFECYCLE_RUNNING) {
        return -EPERM;
    }
    if (desc->id == 0) {
        return -EINVAL;
    }
    if (desc->size > actor_max_payload_size(actor)) {
        return -EMSGSIZE;
    }

    struct ipc_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.id      = desc->id;
    msg.kind    = desc->kind;
    msg.size    = desc->size;
    msg.payload = (const uint8_t *) payload;

    return ipc_port_send_isr(actor, &msg);
}

/* ── static handler dispatch ────────────────────────────────────────────── */

void ipc_dispatch_actor_handlers(struct ipc_actor *self, const struct ipc_msg *msg)
{
    if (!self || !msg) {
        return;
    }

#if IPC_CONFIG_ENABLE_ASK
    if (msg->ask_id != 0) {
        ipc_ask_callback_t callback = NULL;
        lock_asks();
        ipc_pending_ask_t *pending = find_pending_ask(msg->ask_id);
        if (msg->id == ipc_ask_timeout_msg_id && pending && pending->actor == self &&
            pending->timeout_queued) {
            callback = pending->callback;
            memset(pending, 0, sizeof(*pending));
        } else if (pending && pending->actor == self && pending->reply_id == msg->id) {
            callback = pending->callback;
            memset(pending, 0, sizeof(*pending));
        }
        unlock_asks();
        if (callback) {
            callback(self, msg->result, msg->payload, msg->size, msg);
            return;
        }
    }
#endif

    for (int i = 0; i < handler_count; i++) {
        const ipc_handler_binding_t *binding = &handler_table[i];
        if (binding->actor == self && binding->msg_id == msg->id) {
            binding->handler(self, msg->payload, msg);
            return;
        }
    }

    if (self->unknown_handler) {
        self->unknown_handler(self, msg);
    }
}

/* ── ipc_start_all_actors / ipc_run_all / ipc_stop_all ──────────────────── */

static void stop_actors_through(const struct ipc_actor *last)
{
    for (struct ipc_actor *a = _ipc_actor_list; a; a = a->_next) {
        if (a->stop_hook) {
            a->stop_hook(a);
        }
        ipc_port_stop_actor(a);
        if (a == last) {
            break;
        }
    }
}

int ipc_start_all_actors(void)
{
    int expected = IPC_LIFECYCLE_STOPPED;
    if (!atomic_compare_exchange_strong_explicit(&lifecycle_state, &expected, IPC_LIFECYCLE_RUNNING,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        return expected == IPC_LIFECYCLE_STOPPING ? -EBUSY : -EALREADY;
    }

    struct ipc_actor *last_started = NULL;
    for (struct ipc_actor *a = _ipc_actor_list; a; a = a->_next) {
        int rc = ipc_port_actor_init(a);
        if (rc) {
            if (last_started) {
                stop_actors_through(last_started);
            }
            atomic_store_explicit(&lifecycle_state, IPC_LIFECYCLE_STOPPING, memory_order_release);
            return rc;
        }
        last_started = a;
        if (a->start_hook) {
            a->start_hook(a);
        }
        rc = ipc_port_start(a);
        if (rc) {
            stop_actors_through(last_started);
            atomic_store_explicit(&lifecycle_state, IPC_LIFECYCLE_STOPPING, memory_order_release);
            return rc;
        }
    }
    return 0;
}

int ipc_run_all(void)
{
    /* On POSIX the port walks the actor list and pthread_joins each
     * thread, blocking here until they've all exited. On Zephyr the
     * port is a no-op (the kernel keeps scheduling) and we return
     * without joining. See ipc_port_run_all in each backend. */
    int rc = ipc_port_run_all();
    if (rc == 0 &&
        atomic_load_explicit(&lifecycle_state, memory_order_acquire) == IPC_LIFECYCLE_STOPPING) {
        atomic_store_explicit(&lifecycle_state, IPC_LIFECYCLE_STOPPED, memory_order_release);
    }
    return rc;
}

void ipc_stop_all(void)
{
    int expected = IPC_LIFECYCLE_RUNNING;
    if (!atomic_compare_exchange_strong_explicit(&lifecycle_state, &expected,
                                                 IPC_LIFECYCLE_STOPPING, memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return;
    }

    struct ipc_actor *a = _ipc_actor_list;
    while (a) {
        if (a->stop_hook) {
            a->stop_hook(a);
        }
        ipc_port_stop_actor(a);
        a = a->_next;
    }
}

int ipc_actor_fail(struct ipc_actor *self, int reason)
{
    if (!self) {
        return -EINVAL;
    }

    if (self->failure_hook) {
        self->failure_hook(self, reason);
    }

    switch (self->supervision) {
    case IPC_SUPERVISE_NONE:
        return 0;

    case IPC_SUPERVISE_STOP:
        if (self->stop_hook) {
            self->stop_hook(self);
        }
        ipc_port_stop_actor(self);
        return 0;

    case IPC_SUPERVISE_RESTART: {
        if (self->stop_hook) {
            self->stop_hook(self);
        }
        int rc = ipc_port_restart_actor(self);
        if (rc) {
            return rc;
        }
        if (self->start_hook) {
            self->start_hook(self);
        }
        return 0;
    }
    }

    return -EINVAL;
}
