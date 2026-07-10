/*
 * mock_ipc_port.c — Test-only port implementation. See mock_ipc_port.h.
 *
 * No threads, no condvars, no allocations beyond a per-actor state slot.
 * The static_asserts in the headers must still hold because we link this
 * against the same ipc.c that real ports link against.
 */
#define _POSIX_C_SOURCE 200809L
#include "mock_ipc_port.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

/* ── Process-wide mock state ────────────────────────────────────────────── */

#define MOCK_MAX_ACTORS 16

typedef struct {
    mock_actor_state_t slots[MOCK_MAX_ACTORS];
    int n_slots;

    pthread_mutex_t lock; /* protects mock global state */

    /* Programmable behaviour */
    bool block_query_timeout;
    bool block_should_wait;
    bool send_should_fail;
    bool invoke_handlers;
    int run_all_rc;
    /* Per-actor "next start should fail" flag. Set by
     * mock_port_set_next_start_should_fail(a). Cleared by the next call
     * to ipc_port_start on that actor. */
    struct ipc_actor *next_start_should_fail;
    /* One-shot return codes for ipc_port_send / ipc_port_send_after.
     * When non-zero, the corresponding port call returns this value
     * exactly once and then the field is cleared back to 0. Lets a
     * test inject any errno (e.g. -EINVAL) without disturbing the
     * uniform-failure knob above. */
    int next_send_rc;
    int next_send_after_rc;
} mock_state_t;

static mock_state_t g_mock;

void mock_port_init(void)
{
    memset(&g_mock, 0, sizeof(g_mock));
    pthread_mutex_init(&g_mock.lock, NULL);
}

void mock_port_reset(void)
{
    pthread_mutex_lock(&g_mock.lock);
    int n = g_mock.n_slots;
    memset(g_mock.slots, 0, sizeof(g_mock.slots));
    g_mock.n_slots                = n;
    g_mock.block_query_timeout    = false;
    g_mock.block_should_wait      = false;
    g_mock.send_should_fail       = false;
    g_mock.invoke_handlers        = false;
    g_mock.run_all_rc             = 0;
    g_mock.next_start_should_fail = NULL;
    g_mock.next_send_rc           = 0;
    g_mock.next_send_after_rc     = 0;
    pthread_mutex_unlock(&g_mock.lock);
}

mock_actor_state_t *mock_port_actor_state(struct ipc_actor *a)
{
    pthread_mutex_lock(&g_mock.lock);
    for (int i = 0; i < g_mock.n_slots; i++) {
        if (g_mock.slots[i].actor == a) {
            pthread_mutex_unlock(&g_mock.lock);
            return &g_mock.slots[i];
        }
    }
    if (g_mock.n_slots >= MOCK_MAX_ACTORS) {
        pthread_mutex_unlock(&g_mock.lock);
        assert(0 && "mock: too many actors registered");
    }
    mock_actor_state_t *s = &g_mock.slots[g_mock.n_slots++];
    s->actor              = a;
    pthread_mutex_unlock(&g_mock.lock);
    return s;
}

void mock_port_set_block_timeout(bool enabled)
{
    g_mock.block_query_timeout = enabled;
}

void mock_port_set_block_should_wait(bool enabled)
{
    g_mock.block_should_wait = enabled;
}

void mock_port_set_send_should_fail(bool enabled)
{
    g_mock.send_should_fail = enabled;
}

void mock_port_set_next_send_rc(int rc)
{
    g_mock.next_send_rc = rc;
}

void mock_port_set_next_send_after_rc(int rc)
{
    g_mock.next_send_after_rc = rc;
}

void mock_port_set_next_start_should_fail(struct ipc_actor *a)
{
    g_mock.next_start_should_fail = a;
}

void mock_port_set_invoke_handlers(bool enabled)
{
    g_mock.invoke_handlers = enabled;
}

void mock_port_set_run_all_rc(int rc)
{
    g_mock.run_all_rc = rc;
}

const struct ipc_msg *mock_port_last_send_msg(struct ipc_actor *a)
{
    mock_actor_state_t *s = mock_port_actor_state(a);
    return s->has_last_send_msg ? &s->last_send_msg : NULL;
}

bool mock_port_has_last_send_msg(struct ipc_actor *a)
{
    mock_actor_state_t *s = mock_port_actor_state(a);
    return s->has_last_send_msg;
}

bool mock_port_has_pending_send_after(struct ipc_actor *a)
{
    mock_actor_state_t *s = mock_port_actor_state(a);
    return s->has_pending_send_after;
}

const struct ipc_msg *mock_port_pending_send_after_msg(struct ipc_actor *a)
{
    mock_actor_state_t *s = mock_port_actor_state(a);
    return s->has_pending_send_after ? &s->pending_send_after_msg : NULL;
}

uint32_t mock_port_pending_send_after_delay_ms(struct ipc_actor *a)
{
    mock_actor_state_t *s = mock_port_actor_state(a);
    return s->pending_send_after_delay_ms;
}

/* ── ipc_port_* seam implementations ────────────────────────────────────── */

/* Table lock is a no-op for the mock — tests are single-threaded by
 * default and the registration/subscription paths in ipc.c already guard
 * their tables via this lock. We still implement it as a real mutex so
 * any concurrent test that races two registrations is well-defined. */

void ipc_port_table_lock(void)
{
    pthread_mutex_lock(&g_mock.lock);
}

void ipc_port_table_unlock(void)
{
    pthread_mutex_unlock(&g_mock.lock);
}

/* ── Per-actor lifecycle ────────────────────────────────────────────────── */

int ipc_port_actor_init(struct ipc_actor *a)
{
    /* The actor is fully brought up here (per the new model where
     * ipc_actor_init spawns the thread). The mock port's notion of
     * "started" is just "actor_init was called and returned 0" —
     * increment start_count so unit tests still have a hook to
     * verify each actor was initialised exactly once. */
    mock_actor_state_t *s = mock_port_actor_state(a);
    s->start_count++;
    if (g_mock.next_start_should_fail == a) {
        g_mock.next_start_should_fail = NULL;
        return -EINVAL;
    }
    return 0;
}

int ipc_port_start(struct ipc_actor *a)
{
    /* The actor thread is spawned in ipc_port_actor_init on real
     * platforms. The mock's start_count is already incremented
     * there; this hook is a no-op kept for port-interface
     * compatibility. */
    (void) a;
    return 0;
}

void ipc_port_stop_actor(struct ipc_actor *a)
{
    mock_actor_state_t *s = mock_port_actor_state(a);
    s->stop_count++;
}

int ipc_port_send(struct ipc_actor *a, const struct ipc_msg *msg)
{
    mock_actor_state_t *s = mock_port_actor_state(a);
    s->send_count++;
    s->last_send_msg     = *msg;
    s->has_last_send_msg = true;

    int rc               = 0;
    if (g_mock.next_send_rc) {
        /* One-shot: consume and clear. */
        rc                  = g_mock.next_send_rc;
        g_mock.next_send_rc = 0;
    } else if (g_mock.send_should_fail) {
        rc = -ENOMEM;
    } else if (g_mock.invoke_handlers && a->handler) {
        a->handler(a, msg);
    }
    return rc;
}

int ipc_port_send_after(struct ipc_actor *a, const struct ipc_msg *msg, uint32_t delay_ms)
{
    mock_actor_state_t *s = mock_port_actor_state(a);
    s->send_after_count++;
    s->last_send_after_delay_ms    = delay_ms;
    s->last_send_msg               = *msg;
    s->has_last_send_msg           = true;
    /* Single-slot replacement: the most recent send_after overwrites
     * the previous pending one. This mirrors the contract documented in
     * AGENTS.md ("One delayed message per actor. ipc_send_after replaces
     * the previous pending delayed msg"). */
    s->pending_send_after_msg      = *msg;
    s->pending_send_after_delay_ms = delay_ms;
    s->has_pending_send_after      = true;
    if (g_mock.next_send_after_rc) {
        int rc                    = g_mock.next_send_after_rc;
        g_mock.next_send_after_rc = 0;
        return rc;
    }
    return 0;
}

int ipc_port_run_all(void)
{
    return g_mock.run_all_rc;
}

/* ── Query-wait ─────────────────────────────────────────────────────────── */

/* Layout contract: see IPC_QUERY_RESPONSE_OFFSET / IPC_QUERY_RESPONSE_SIZE
 * in ipc_port.h. The response area must be the first
 * IPC_QUERY_RESPONSE_SIZE bytes of _opaque; bookkeeping for status/done
 * lives past the response area so the two regions never collide. */
#define MOCK_QW_BODY_OFFSET IPC_QUERY_RESPONSE_SIZE

struct mock_query_wait {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int status;
    bool done;
};

static struct mock_query_wait *qw_of(ipc_query_wait_t *w)
{
    return (struct mock_query_wait *) ((uint8_t *) w->_opaque + MOCK_QW_BODY_OFFSET);
}

int ipc_port_query_wait_init(ipc_query_wait_t *w)
{
    struct mock_query_wait *m = qw_of(w);
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->cond, NULL);
    m->status = 0;
    m->done   = false;
    /* Zero the response area so reads see deterministic state. */
    memset(w->_opaque, 0, IPC_QUERY_RESPONSE_SIZE);
    return 0;
}

void ipc_port_query_wait_destroy(ipc_query_wait_t *w)
{
    struct mock_query_wait *m = qw_of(w);
    pthread_cond_destroy(&m->cond);
    pthread_mutex_destroy(&m->lock);
}

int ipc_port_query_wait_block(ipc_query_wait_t *w, ipc_timeout_t timeout)
{
    struct mock_query_wait *m = qw_of(w);
    if (g_mock.block_query_timeout) {
        return -ETIMEDOUT;
    }
    if (!g_mock.block_should_wait) {
        /* Legacy behaviour: synchronous check, no real wait. */
        if (!m->done) {
            return -ETIMEDOUT;
        }
        return m->status;
    }
    /* Real wait on the condvar. Translate the ipc_timeout_t (ms) into
     * an absolute deadline so the timeout is honoured under load. */
    pthread_mutex_lock(&m->lock);
    if (!m->done) {
        if (timeout == IPC_TIMEOUT_FOREVER) {
            pthread_cond_wait(&m->cond, &m->lock);
        } else {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            uint64_t ns = (uint64_t) timeout * 1000000ull;
            deadline.tv_sec += (time_t) (ns / 1000000000ull);
            deadline.tv_nsec += (long) (ns % 1000000000ull);
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec += 1;
                deadline.tv_nsec -= 1000000000L;
            }
            int wrc = 0;
            while (!m->done && wrc == 0) {
                wrc = pthread_cond_timedwait(&m->cond, &m->lock, &deadline);
            }
            if (wrc == ETIMEDOUT && !m->done) {
                pthread_mutex_unlock(&m->lock);
                return -ETIMEDOUT;
            }
        }
    }
    int rc = m->status;
    pthread_mutex_unlock(&m->lock);
    return rc;
}

void ipc_port_query_wait_wake(ipc_query_wait_t *w)
{
    struct mock_query_wait *m = qw_of(w);
    pthread_mutex_lock(&m->lock);
    m->status = 0;
    m->done   = true;
    pthread_cond_signal(&m->cond);
    pthread_mutex_unlock(&m->lock);
}
