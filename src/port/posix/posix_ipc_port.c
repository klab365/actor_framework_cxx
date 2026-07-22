/*
 * posix_ipc_port.c — POSIX implementation of the generic ipc_port interface.
 *
 * pthreads + mutex + condvar. Ring buffer for mailbox, one helper thread
 * per actor for delayed sends.
 */
#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include "ipc_port.h"
#include "ipc_port_state.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Per-actor port state (concrete layout) ─────────────────────────────── */

static struct ipc_port_state *port_of(struct ipc_actor *a)
{
    return (struct ipc_port_state *) a->port;
}

static size_t actor_max_payload_size(const struct ipc_actor *a)
{
    return a->cfg.max_payload_size > 0 ? a->cfg.max_payload_size : IPC_PAYLOAD_SIZE;
}

/* ── Queue slot helpers ─────────────────────────────────────────────────── */

static uint8_t *slot_at(struct ipc_port_state *p, size_t index)
{
    return p->ring + (index * p->slot_size);
}

static int store_msg(struct ipc_port_state *p, uint8_t *slot, const struct ipc_msg *msg)
{
    if (msg->size > p->slot_size - IPC_MSG_SLOT_HEADER_SIZE) {
        return -EMSGSIZE;
    }

    ipc_msg_slot_header_t *header = (ipc_msg_slot_header_t *) slot;
    header->id                    = msg->id;
    header->kind                  = msg->kind;
    header->size                  = msg->size;

    uint8_t *payload              = slot + IPC_MSG_SLOT_HEADER_SIZE;
    if (msg->size > 0) {
        if (msg->payload) {
            memcpy(payload, msg->payload, msg->size);
        } else {
            memset(payload, 0, msg->size);
        }
    }
    return 0;
}

static struct ipc_msg load_msg(uint8_t *slot)
{
    ipc_msg_slot_header_t *header = (ipc_msg_slot_header_t *) slot;
    struct ipc_msg msg            = {
        .id      = header->id,
        .kind    = header->kind,
        .size    = header->size,
        .payload = slot + IPC_MSG_SLOT_HEADER_SIZE,
    };
    return msg;
}

/* ── Actor thread ────────────────────────────────────────────────────────── */

extern struct ipc_actor *_ipc_actor_list;

static void *ipc_thread_fn(void *arg)
{
    struct ipc_actor *self   = (struct ipc_actor *) arg;
    struct ipc_port_state *p = port_of(self);

    pthread_mutex_lock(&p->lock);
    while (p->running) {
        while (p->count == 0 && p->running) {
            pthread_cond_wait(&p->cond, &p->lock);
        }
        while (p->count > 0) {
            memcpy(p->recv_slot, slot_at(p, p->head), p->slot_size);
            struct ipc_msg msg = load_msg(p->recv_slot);
            p->head            = (p->head + 1) % p->capacity;
            p->count--;
            pthread_mutex_unlock(&p->lock);

            self->handler(self, &msg);

            pthread_mutex_lock(&p->lock);
        }
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

int ipc_port_actor_init(struct ipc_actor *a)
{
    struct ipc_port_state *p = port_of(a);
    size_t cap               = a->cfg.queue_depth > 0 ? a->cfg.queue_depth : 8;

    size_t max_payload       = actor_max_payload_size(a);
    p->slot_size             = IPC_MSG_SLOT_SIZE(max_payload);
    p->ring                  = (uint8_t *) calloc(cap, p->slot_size);
    p->recv_slot             = (uint8_t *) calloc(1, p->slot_size);
    p->delay_payload         = (uint8_t *) calloc(1, max_payload > 0 ? max_payload : 1);
    if (!p->ring || !p->recv_slot || !p->delay_payload) {
        free(p->ring);
        free(p->recv_slot);
        free(p->delay_payload);
        p->ring          = NULL;
        p->recv_slot     = NULL;
        p->delay_payload = NULL;
        return -ENOMEM;
    }

    p->capacity     = cap;
    p->head         = 0;
    p->tail         = 0;
    p->count        = 0;
    p->running      = true;
    p->joined       = false;
    p->delay_active = false;
    p->delay_cancel = false;

    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cond, NULL);
    pthread_mutex_init(&p->delay_lock, NULL);
    pthread_cond_init(&p->delay_cond, NULL);

    /* The actor's thread is spawned here during ipc_start_all_actors().
     * By the time startup returns, the actor is scheduled and polling
     * its queue. */
    return pthread_create(&p->thread, NULL, ipc_thread_fn, a);
}

/* ── Delayed send thread ─────────────────────────────────────────────────── */

static void *delay_thread_fn(void *arg)
{
    struct ipc_actor *self   = (struct ipc_actor *) arg;
    struct ipc_port_state *p = port_of(self);

    pthread_mutex_lock(&p->delay_lock);

    uint32_t ms = p->delay_ms;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t) (ms / 1000);
    ts.tv_nsec += (long) (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    int rc = 0;
    while (!p->delay_cancel && rc == 0) {
        rc = pthread_cond_timedwait(&p->delay_cond, &p->delay_lock, &ts);
    }

    /* NOTE: do NOT clear p->delay_active here. `delay_active` is the
     * "join in progress" flag: it stays true until the NEXT send_after
     * (or stop_actor) calls pthread_join on p->delay_thread. Clearing it
     * here would let a concurrent send_after skip the join and race with
     * our unlocked ipc_port_send tail. The next send_after will see
     * delay_active == true and join us; if we've already finished, the
     * join is a no-op. */
    bool cancelled     = p->delay_cancel;
    p->delay_cancel    = false;
    struct ipc_msg msg = p->delay_msg;
    msg.payload        = p->delay_payload;
    pthread_mutex_unlock(&p->delay_lock);

    if (!cancelled) {
        ipc_port_send(self, &msg);
    }
    return NULL;
}

/* ── Port API ────────────────────────────────────────────────────────────── */

int ipc_port_start(struct ipc_actor *a)
{
    /* Actor thread is already spawned in ipc_port_actor_init.
     * This hook is kept for port-interface compatibility but
     * performs no work on POSIX. */
    (void) a;
    return 0;
}

int ipc_port_send(struct ipc_actor *a, const struct ipc_msg *msg)
{
    struct ipc_port_state *p = port_of(a);
    int rc                   = 0;

    pthread_mutex_lock(&p->lock);
    if (p->count >= p->capacity) {
        rc = -ENOMEM;
    } else {
        rc = store_msg(p, slot_at(p, p->tail), msg);
        if (rc == 0) {
            p->tail = (p->tail + 1) % p->capacity;
        }
        if (rc == 0) {
            p->count++;
            pthread_cond_signal(&p->cond);
        }
    }
    pthread_mutex_unlock(&p->lock);
    return rc;
}

int ipc_port_send_isr(struct ipc_actor *a, const struct ipc_msg *msg)
{
    return ipc_port_send(a, msg);
}

int ipc_port_send_after(struct ipc_actor *a, const struct ipc_msg *msg, uint32_t delay_ms)
{
    struct ipc_port_state *p = port_of(a);
    int rc                   = 0;

    pthread_mutex_lock(&p->delay_lock);
    /* delay_active is a "join in progress" flag: it stays true from
     * pthread_create success until we (or stop_actor) join the thread.
     * This is what guarantees the previous delay thread has fully
     * finished — including its unlocked ipc_port_send tail — before we
     * install a new delay_msg and start a new thread. Joining an
     * already-finished thread is a no-op, so a true here is always safe
     * to act on. */
    if (p->delay_active) {
        p->delay_cancel = true;
        pthread_cond_signal(&p->delay_cond);
        pthread_mutex_unlock(&p->delay_lock);
        pthread_join(p->delay_thread, NULL);
        pthread_mutex_lock(&p->delay_lock);
        p->delay_cancel = false;
        p->delay_active = false;
    }
    if (msg->size > actor_max_payload_size(a)) {
        rc = -EMSGSIZE;
    } else {
        p->delay_msg         = *msg;
        p->delay_msg.payload = p->delay_payload;
        if (msg->size > 0) {
            if (msg->payload) {
                memcpy(p->delay_payload, msg->payload, msg->size);
            } else {
                memset(p->delay_payload, 0, msg->size);
            }
        }
        p->delay_ms = delay_ms;
    }

    /* If pthread_create fails, leave delay_active false so the next call
     * doesn't try to join a thread that was never created. */
    if (rc == 0) {
        rc = pthread_create(&p->delay_thread, NULL, delay_thread_fn, a);
        if (rc == 0) {
            p->delay_active = true;
        }
    }
    pthread_mutex_unlock(&p->delay_lock);

    if (rc == EMSGSIZE || rc == -EMSGSIZE) {
        return -EMSGSIZE;
    }
    return rc == 0 ? 0 : -ENOMEM;
}

static void cleanup_actor(struct ipc_actor *a)
{
    struct ipc_port_state *p = port_of(a);

    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->cond);
    pthread_mutex_destroy(&p->delay_lock);
    pthread_cond_destroy(&p->delay_cond);

    free(p->ring);
    free(p->recv_slot);
    free(p->delay_payload);
    p->ring          = NULL;
    p->recv_slot     = NULL;
    p->delay_payload = NULL;
}

int ipc_port_run_all(void)
{
    /*
     * Block in pthread_join until every actor thread has exited. The
     * caller is expected to have already called ipc_stop_all() to
     * signal the threads to exit (ipc_port_stop_actor sets
     * p->running = false and broadcasts). Note: on Zephyr this is a
     * no-op — see zephyr_ipc_port.c.
     */
    struct ipc_actor *a = _ipc_actor_list;
    while (a) {
        struct ipc_port_state *p = port_of(a);
        if (!p->joined) {
            pthread_join(p->thread, NULL);
            p->joined = true;
            cleanup_actor(a);
        }
        a = a->_next;
    }
    return 0;
}

static void cancel_delayed_send(struct ipc_port_state *p)
{
    pthread_mutex_lock(&p->delay_lock);
    if (p->delay_active) {
        p->delay_cancel = true;
        pthread_cond_signal(&p->delay_cond);
        pthread_mutex_unlock(&p->delay_lock);
        pthread_join(p->delay_thread, NULL);
        pthread_mutex_lock(&p->delay_lock);
        p->delay_cancel = false;
        p->delay_active = false;
    }
    pthread_mutex_unlock(&p->delay_lock);
}

void ipc_port_stop_actor(struct ipc_actor *a)
{
    struct ipc_port_state *p = port_of(a);

    /* Cancel delay thread if active. The delayed-send helper owns a
     * separate pthread; join it here so no helper can enqueue more work
     * after the actor has been asked to stop. */
    cancel_delayed_send(p);

    pthread_mutex_lock(&p->lock);
    p->running = false;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
}

int ipc_port_restart_actor(struct ipc_actor *a)
{
    struct ipc_port_state *p = port_of(a);

    /* Soft restart for message-driven actors: cancel delayed work and
     * drop queued messages. The actor thread keeps running, so this is
     * safe even when an actor reports failure from inside its own handler. */
    cancel_delayed_send(p);

    pthread_mutex_lock(&p->lock);
    p->head    = 0;
    p->tail    = 0;
    p->count   = 0;
    p->running = true;
    pthread_mutex_unlock(&p->lock);

    return 0;
}
