/*
 * posix_ipc_port.c — POSIX implementation of the generic ipc_port interface.
 *
 * pthreads + mutex + condvar. Ring buffer for mailbox, one helper thread
 * per actor for delayed sends.
 */
#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include "ipc_port.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Table mutex (one per process) ───────────────────────────────────────── */

static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t table_once   = PTHREAD_ONCE_INIT;

static void table_mutex_init_once(void)
{
    /* PTHREAD_MUTEX_INITIALIZER already initialised it; this exists
     * only to match the "init once" pattern used by the Zephyr port. */
}

void ipc_port_table_lock(void)
{
    pthread_mutex_lock(&table_mutex);
}
void ipc_port_table_unlock(void)
{
    pthread_mutex_unlock(&table_mutex);
}

/* ── Per-actor port state (concrete layout) ─────────────────────────────── */

struct ipc_port_state {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    /* Ring buffer (calloc'd once in ipc_port_start) */
    struct ipc_msg *ring;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;

    bool running;

    /* Delayed send: single pending delayed message per actor */
    pthread_t delay_thread;
    bool delay_active;
    pthread_mutex_t delay_lock;
    pthread_cond_t delay_cond;
    struct ipc_msg delay_msg;
    uint32_t delay_ms;
    bool delay_cancel;
};

_Static_assert(sizeof(struct ipc_port_state) <= sizeof(ipc_port_state_t),
               "Increase IPC_PORT_STATE_WORDS for POSIX port state");

static struct ipc_port_state *port_of(struct ipc_actor *a)
{
    return (struct ipc_port_state *) (void *) &a->port;
}

/* ── Query-wait impl (response bytes at IPC_QUERY_RESPONSE_OFFSET) ─────── */

struct ipc_query_wait_impl {
    uint8_t response[IPC_QUERY_RESPONSE_SIZE];
    int status;
    bool expired;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool done;
};

_Static_assert(sizeof(struct ipc_query_wait_impl) <= sizeof(ipc_query_wait_t),
               "Increase IPC_QUERY_WAIT_WORDS for POSIX query wait");

static struct ipc_query_wait_impl *qw_of(ipc_query_wait_t *w)
{
    return (struct ipc_query_wait_impl *) (void *) w;
}

int ipc_port_query_wait_init(ipc_query_wait_t *w)
{
    struct ipc_query_wait_impl *impl = qw_of(w);
    memset(impl, 0, sizeof(*impl));
    if (pthread_mutex_init(&impl->lock, NULL) != 0) {
        return -ENOMEM;
    }
    if (pthread_cond_init(&impl->cond, NULL) != 0) {
        pthread_mutex_destroy(&impl->lock);
        return -ENOMEM;
    }
    return 0;
}

void ipc_port_query_wait_destroy(ipc_query_wait_t *w)
{
    struct ipc_query_wait_impl *impl = qw_of(w);
    pthread_mutex_destroy(&impl->lock);
    pthread_cond_destroy(&impl->cond);
}

int ipc_port_query_wait_block(ipc_query_wait_t *w, ipc_timeout_t timeout)
{
    struct ipc_query_wait_impl *impl = qw_of(w);
    pthread_mutex_lock(&impl->lock);

    int rc = 0;
    if (timeout == IPC_TIMEOUT_FOREVER) {
        while (!impl->done) {
            pthread_cond_wait(&impl->cond, &impl->lock);
        }
        rc = impl->status;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int64_t ms = (int64_t) timeout;
        ts.tv_sec += (time_t) (ms / 1000);
        ts.tv_nsec += (long) (ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        int crc = 0;
        while (!impl->done && crc == 0) {
            crc = pthread_cond_timedwait(&impl->cond, &impl->lock, &ts);
        }
        if (!impl->done) {
            impl->expired = true;
            rc            = -ETIMEDOUT;
        } else {
            rc = impl->status;
        }
    }

    pthread_mutex_unlock(&impl->lock);
    return rc;
}

void ipc_port_query_wait_wake(ipc_query_wait_t *w)
{
    struct ipc_query_wait_impl *impl = qw_of(w);
    pthread_mutex_lock(&impl->lock);
    impl->status = 0;
    impl->done   = true;
    pthread_cond_broadcast(&impl->cond);
    pthread_mutex_unlock(&impl->lock);
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
            struct ipc_msg msg = p->ring[p->head];
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
    /* Lazy: pthread_once ensures table_mutex is set up before first lock. */
    pthread_once(&table_once, table_mutex_init_once);
    (void) a;
    return 0;
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
    pthread_mutex_unlock(&p->delay_lock);

    if (!cancelled) {
        ipc_port_send(self, &msg);
    }
    return NULL;
}

/* ── Port API ────────────────────────────────────────────────────────────── */

int ipc_port_start(struct ipc_actor *a)
{
    struct ipc_port_state *p = port_of(a);
    size_t cap               = a->cfg.queue_depth > 0 ? a->cfg.queue_depth : 8;

    p->ring                  = (struct ipc_msg *) calloc(cap, sizeof(struct ipc_msg));
    if (!p->ring) {
        return -ENOMEM;
    }

    p->capacity = cap;
    p->head = p->tail = p->count = 0;
    p->running                   = true;
    p->delay_active              = false;
    p->delay_cancel              = false;

    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cond, NULL);
    pthread_mutex_init(&p->delay_lock, NULL);
    pthread_cond_init(&p->delay_cond, NULL);

    return pthread_create(&p->thread, NULL, ipc_thread_fn, a);
}

int ipc_port_send(struct ipc_actor *a, const struct ipc_msg *msg)
{
    struct ipc_port_state *p = port_of(a);
    int rc                   = 0;

    pthread_mutex_lock(&p->lock);
    if (p->count >= p->capacity) {
        rc = -ENOMEM;
    } else {
        p->ring[p->tail] = *msg;
        p->tail          = (p->tail + 1) % p->capacity;
        p->count++;
        pthread_cond_signal(&p->cond);
    }
    pthread_mutex_unlock(&p->lock);
    return rc;
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
    p->delay_msg = *msg;
    p->delay_ms  = delay_ms;

    /* If pthread_create fails, leave delay_active false so the next call
     * doesn't try to join a thread that was never created. */
    rc           = pthread_create(&p->delay_thread, NULL, delay_thread_fn, a);
    if (rc == 0) {
        p->delay_active = true;
    }
    pthread_mutex_unlock(&p->delay_lock);

    return rc == 0 ? 0 : -ENOMEM;
}

int ipc_port_run_all(void)
{
    struct ipc_actor *a = _ipc_actor_list;
    while (a) {
        pthread_join(port_of(a)->thread, NULL);
        a = a->_next;
    }
    return 0;
}

void ipc_port_stop_actor(struct ipc_actor *a)
{
    struct ipc_port_state *p = port_of(a);

    /* Cancel delay thread if active */
    pthread_mutex_lock(&p->delay_lock);
    if (p->delay_active) {
        p->delay_cancel = true;
        pthread_cond_signal(&p->delay_cond);
        pthread_mutex_unlock(&p->delay_lock);
        pthread_join(p->delay_thread, NULL);
        pthread_mutex_lock(&p->delay_lock);
    }
    pthread_mutex_unlock(&p->delay_lock);

    pthread_mutex_lock(&p->lock);
    p->running = false;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);

    pthread_join(p->thread, NULL);

    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->cond);
    pthread_mutex_destroy(&p->delay_lock);
    pthread_cond_destroy(&p->delay_cond);

    free(p->ring);
    p->ring = NULL;
}
