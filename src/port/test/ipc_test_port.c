/* ipc_test_port.c — deterministic, single-threaded port for actor tests. */
#include "ipc_port.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define IPC_TEST_MAX_ACTORS 32

typedef struct {
    const struct ipc_actor *actor;
    uint8_t *queue;
    uint8_t *receive_slot;
    size_t slot_size;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    bool started;
} ipc_test_actor_state_t;

static ipc_test_actor_state_t actor_states[IPC_TEST_MAX_ACTORS];
static size_t actor_state_count;

extern struct ipc_actor *_ipc_actor_list;

static ipc_test_actor_state_t *state_for(const struct ipc_actor *actor, bool create)
{
    for (size_t i = 0; i < actor_state_count; i++) {
        if (actor_states[i].actor == actor) {
            return &actor_states[i];
        }
    }

    if (!create || actor_state_count == IPC_TEST_MAX_ACTORS) {
        return NULL;
    }

    ipc_test_actor_state_t *state = &actor_states[actor_state_count++];
    state->actor                  = actor;
    return state;
}

static void release_state(ipc_test_actor_state_t *state)
{
    free(state->queue);
    free(state->receive_slot);
    state->queue        = NULL;
    state->receive_slot = NULL;
    state->slot_size    = 0;
    state->capacity     = 0;
    state->head         = 0;
    state->tail         = 0;
    state->count        = 0;
    state->started      = false;
}

static uint8_t *slot_at(const ipc_test_actor_state_t *state, size_t index)
{
    return state->queue + (index * state->slot_size);
}

static int enqueue(const struct ipc_actor *actor, const struct ipc_msg *msg)
{
    ipc_test_actor_state_t *state = state_for(actor, false);
    if (!state || !state->started) {
        return -EPERM;
    }
    if (msg->size > actor->cfg.max_payload_size) {
        return -EMSGSIZE;
    }
    if (state->count == state->capacity) {
        return -ENOMEM;
    }

    uint8_t *slot                 = slot_at(state, state->tail);
    ipc_msg_slot_header_t *header = (ipc_msg_slot_header_t *) slot;
    *header                       = (ipc_msg_slot_header_t) {
        .id       = msg->id,
        .kind     = msg->kind,
        .size     = msg->size,
        .ask_id   = msg->ask_id,
        .reply_id = msg->reply_id,
        .result   = msg->result,
    };

    uint8_t *payload = slot + IPC_MSG_SLOT_HEADER_SIZE;
    if (msg->size > 0) {
        if (msg->payload) {
            memcpy(payload, msg->payload, msg->size);
        } else {
            memset(payload, 0, msg->size);
        }
    }

    state->tail = (state->tail + 1) % state->capacity;
    state->count++;
    return 0;
}

int ipc_port_actor_init(struct ipc_actor *actor)
{
    if (!actor || actor->cfg.queue_depth == 0) {
        return -EINVAL;
    }

    ipc_test_actor_state_t *state = state_for(actor, true);
    if (!state) {
        return -ENOMEM;
    }

    release_state(state);
    actor->port         = state;
    state->slot_size    = IPC_MSG_SLOT_SIZE(actor->cfg.max_payload_size);
    state->capacity     = actor->cfg.queue_depth;
    state->queue        = calloc(state->capacity, state->slot_size);
    state->receive_slot = calloc(1, state->slot_size);
    if (!state->queue || !state->receive_slot) {
        release_state(state);
        return -ENOMEM;
    }

    state->started = true;
    return 0;
}

int ipc_port_start(const struct ipc_actor *actor)
{
    (void) actor;
    return 0;
}

void ipc_port_stop_actor(const struct ipc_actor *actor)
{
    ipc_test_actor_state_t *state = state_for(actor, false);
    if (state) {
        state->started = false;
        state->count   = 0;
    }
}

int ipc_port_restart_actor(const struct ipc_actor *actor)
{
    ipc_test_actor_state_t *state = state_for(actor, false);
    if (!state || !state->started) {
        return -EPERM;
    }
    state->head  = 0;
    state->tail  = 0;
    state->count = 0;
    return 0;
}

int ipc_port_send(const struct ipc_actor *actor, const struct ipc_msg *msg)
{
    return !actor || !msg ? -EINVAL : enqueue(actor, msg);
}

int ipc_port_send_isr(const struct ipc_actor *actor, const struct ipc_msg *msg)
{
    return ipc_port_send(actor, msg);
}

int ipc_port_send_after(const struct ipc_actor *actor, const struct ipc_msg *msg, uint32_t delay_ms)
{
    (void) actor;
    (void) msg;
    (void) delay_ms;
    return -ENOSYS;
}

int ipc_port_schedule_ask_timeout(const struct ipc_actor *actor, uint32_t ask_id,
                                  uint32_t timeout_ms)
{
    (void) actor;
    (void) ask_id;
    (void) timeout_ms;
    return -ENOSYS;
}

int ipc_port_run_all(void)
{
    for (size_t i = 0; i < actor_state_count; i++) {
        release_state(&actor_states[i]);
    }
    return 0;
}

uint32_t ipc_port_now_ms(void)
{
    return 0;
}

int ipc_test_start(void)
{
    return ipc_start_all_actors();
}

size_t ipc_test_run_until_idle(void)
{
    size_t dispatched = 0;
    bool made_progress;

    do {
        made_progress = false;
        for (struct ipc_actor *actor = _ipc_actor_list; actor; actor = actor->_next) {
            ipc_test_actor_state_t *state = state_for(actor, false);
            if (!state || !state->started || state->count == 0) {
                continue;
            }

            memcpy(state->receive_slot, slot_at(state, state->head), state->slot_size);
            state->head = (state->head + 1) % state->capacity;
            state->count--;

            const ipc_msg_slot_header_t *header =
                (const ipc_msg_slot_header_t *) state->receive_slot;
            struct ipc_msg msg = {
                .id       = header->id,
                .kind     = header->kind,
                .size     = header->size,
                .payload  = state->receive_slot + IPC_MSG_SLOT_HEADER_SIZE,
                .ask_id   = header->ask_id,
                .reply_id = header->reply_id,
                .result   = header->result,
            };
            actor->handler(actor, &msg);
            dispatched++;
            made_progress = true;
        }
    } while (made_progress);

    return dispatched;
}

void ipc_test_stop(void)
{
    ipc_stop_all();
    (void) ipc_run_all();
}
