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

static ipc_registration_t reg_table[IPC_CORE_MAX_REGISTRATIONS];
static int reg_count;

/* ── Subscription table ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t msg_id;
    struct ipc_actor *actor;
} ipc_subscription_t;

static ipc_subscription_t sub_table[IPC_CORE_MAX_SUBSCRIPTIONS];
static int sub_count;
static bool actors_started;

typedef struct {
    struct ipc_actor *actor;
    uint32_t msg_id;
    ipc_actor_msg_handler_t handler;
} ipc_handler_binding_t;

static ipc_handler_binding_t handler_table[IPC_CORE_MAX_REGISTRATIONS + IPC_CORE_MAX_SUBSCRIPTIONS];
static int handler_count;

/* ── Test reset ─────────────────────────────────────────────────────────── */
/* See declaration in ipc_internal.h. Production code must never call this. */
void _ipc_reset_for_testing(void)
{
    reg_count     = 0;
    sub_count     = 0;
    handler_count = 0;
    memset(reg_table, 0, sizeof(reg_table));
    memset(sub_table, 0, sizeof(sub_table));
    memset(handler_table, 0, sizeof(handler_table));
    actors_started  = false;
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

static int register_cmd_unlocked(struct ipc_actor *actor, ipc_msg_desc_t *desc)
{
    assert(desc->kind == IPC_CMD);
    _ipc_ensure_id(desc);

    for (int i = 0; i < reg_count; i++) {
        if (reg_table[i].msg_id == desc->id) {
            fprintf(stderr, "ipc: duplicate registration for '%s'\n", desc->name);
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

    for (int i = 0; i < sub_count; i++) {
        if (sub_table[i].msg_id == desc->id && sub_table[i].actor == actor) {
            return 0;
        }
    }

    if (sub_count >= IPC_CORE_MAX_SUBSCRIPTIONS) {
        return -ENOMEM;
    }

    sub_table[sub_count].msg_id = desc->id;
    sub_table[sub_count].actor  = actor;
    sub_count++;

    return 0;
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

/* ── Static actor registration ───────────────────────────────────────────── */

void _ipc_actor_register_static(struct ipc_actor *actor)
{
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
    _ipc_actor_register_static(actor);
    _ipc_ensure_id(desc);

    if (handler_count >= (IPC_CORE_MAX_REGISTRATIONS + IPC_CORE_MAX_SUBSCRIPTIONS)) {
        assert(0 && "IPC handler table full");
        return;
    }

    handler_table[handler_count].actor   = actor;
    handler_table[handler_count].msg_id  = desc->id;
    handler_table[handler_count].handler = handler;
    handler_count++;

    int rc = desc->kind == IPC_EVENT ? subscribe_event_unlocked(actor, desc)
                                     : register_cmd_unlocked(actor, desc);
    (void) rc;
}

void _ipc_actor_register_start_hook_static(struct ipc_actor *actor, ipc_actor_lifecycle_hook_t hook)
{
    _ipc_actor_register_static(actor);
    actor->start_hook = hook;
}

void _ipc_actor_register_stop_hook_static(struct ipc_actor *actor, ipc_actor_lifecycle_hook_t hook)
{
    _ipc_actor_register_static(actor);
    actor->stop_hook = hook;
}

void _ipc_actor_register_unknown_hook_static(struct ipc_actor *actor,
                                             ipc_actor_unknown_handler_t hook)
{
    _ipc_actor_register_static(actor);
    actor->unknown_handler = hook;
}

/* ── ipc_send_raw ────────────────────────────────────────────────────────── */

int ipc_send_raw(ipc_msg_desc_t *desc, const void *payload)
{
    _ipc_ensure_id(desc);
    struct ipc_actor *target = find_registered(desc->id);

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
    struct ipc_actor *target = find_registered(desc->id);

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

static int publish_prepared_msg(const struct ipc_msg *msg, uint32_t msg_id,
                                int (*send_fn)(struct ipc_actor *, const struct ipc_msg *))
{
    int first_rc = 0;
    for (int i = 0; i < sub_count; i++) {
        if (sub_table[i].msg_id == msg_id) {
            int rc = send_fn(sub_table[i].actor, msg);
            if (rc && !first_rc) {
                first_rc = rc;
            }
        }
    }
    return first_rc;
}

/* ── ipc_publish_raw ─────────────────────────────────────────────────────── */

int ipc_publish_raw(ipc_msg_desc_t *desc, const void *payload)
{
    /* Only EVENT descriptors may be published. Publishing a CMD descriptor is a programming
     * error — those go through ipc_send_raw. Without this assert the code
     * silently overrides msg.kind = IPC_EVENT below, which masks the bug
     * (and would cause a cmd message to be fan-out delivered to event
     * subscribers). */
    assert(desc->kind == IPC_EVENT);
    _ipc_ensure_id(desc);

    struct ipc_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.id   = desc->id;
    msg.kind = IPC_EVENT;
    if (payload && desc->size > 0) {
        memcpy(msg.payload, payload, desc->size);
    }

    return publish_prepared_msg(&msg, desc->id, ipc_port_send);
}

/* ── ipc_publish_isr_raw ────────────────────────────────────────────────── */

int ipc_publish_isr_raw(const ipc_msg_desc_t *desc, const void *payload)
{
    if (!desc || desc->kind != IPC_EVENT) {
        return -EINVAL;
    }
    if (!actors_started) {
        return -EPERM;
    }
    if (desc->id == 0) {
        return -EINVAL;
    }

    struct ipc_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.id   = desc->id;
    msg.kind = IPC_EVENT;
    if (payload && desc->size > 0) {
        memcpy(msg.payload, payload, desc->size);
    }

    return publish_prepared_msg(&msg, desc->id, ipc_port_send_isr);
}

/* ── static handler dispatch ────────────────────────────────────────────── */

void ipc_dispatch_actor_handlers(struct ipc_actor *self, const struct ipc_msg *msg)
{
    for (int i = 0; i < handler_count; i++) {
        const ipc_handler_binding_t *binding = &handler_table[i];
        if (binding->actor == self && binding->msg_id == msg->id) {
            binding->handler(self, msg->payload, msg);
            return;
        }
    }

    if (self && self->unknown_handler) {
        self->unknown_handler(self, msg);
    }
}

/* ── ipc_start_all_actors / ipc_run_all / ipc_stop_all ──────────────────── */

int ipc_start_all_actors(void)
{
    struct ipc_actor *a = _ipc_actor_list;
    while (a) {
        int rc = ipc_port_actor_init(a);
        if (rc) {
            return rc;
        }
        if (a->start_hook) {
            a->start_hook(a);
        }
        rc = ipc_port_start(a);
        if (rc) {
            return rc;
        }
        a = a->_next;
    }
    actors_started = true;
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
        if (a->stop_hook) {
            a->stop_hook(a);
        }
        ipc_port_stop_actor(a);
        a = a->_next;
    }
}
