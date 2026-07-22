/*
 * mock_ipc_port.h — Test-only port implementation.
 *
 * Replaces posix_ipc_port / zephyr_ipc_port at link time. Implements the
 * same ipc_port_* seam but does not start threads, does not block on
 * condvars, and records every call into inspectable state so tests can
 * assert against what ipc.c asked the port to do.
 *
 * Behaviour is programmable via mock_ipc_*_set_* helpers:
 *   - send can be made to fail (-ENOMEM) by toggling fail_next_send
 *   - send_after can record the requested delay without actually waiting
 *   - handlers can be invoked synchronously from send via mock_invoke_handlers
 *   - per-actor mock state is kept in a separate test-only slot table
 */
#pragma once

#include "ipc.h"
#include "ipc_port.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Per-actor state held by the mock port. */
typedef struct {
    struct ipc_actor *actor;
    int start_count;
    int stop_count;
    int restart_count;
    int send_count;
    int send_after_count;
    uint32_t last_send_after_delay_ms;
    struct ipc_msg last_send_msg;
    bool has_last_send_msg;
    /* Per-actor "send_after" is a single-slot replacement. The mock
     * records the currently-scheduled delayed message (if any) here so
     * tests can assert that a second send_after replaces the first. */
    bool has_pending_send_after;
    struct ipc_msg pending_send_after_msg;
    uint32_t pending_send_after_delay_ms;
} mock_actor_state_t;

/* Initialise/teardown the mock port. Call once per test. */
void mock_port_init(void);
void mock_port_reset(void); /* clear call logs but keep registrations */

/* Per-actor accessors. */
mock_actor_state_t *mock_port_actor_state(struct ipc_actor *a);

/* Programmable send behaviour. */
void mock_port_set_send_should_fail(bool enabled);

/*
 * Inject a specific return code on the *next* ipc_port_send call,
 * then auto-clear. Negative values are returned verbatim (most
 * callers pass -EINVAL, -EAGAIN, -EIO, -ENOSPC, etc. to exercise
 * the propagation contract for non-ENOMEM port errors). The
 * send_should_fail path is preserved for tests that want every
 * send to fail uniformly.
 */
void mock_port_set_next_send_rc(int rc);

/*
 * Same, but for ipc_port_send_after. Used to exercise the
 * "scheduling failed" branch of the send-after contract.
 */
void mock_port_set_next_send_after_rc(int rc);

/* If non-null, ipc_port_start fails the next time the named actor is
 * started. Cleared by mock_port_reset(). The first matching call fails
 * and returns -EINVAL; subsequent calls succeed (so lifecycle tests can
 * assert that start_all aborts on the first failure). */
void mock_port_set_next_start_should_fail(struct ipc_actor *a);

/* If true, ipc_port_send will synchronously invoke the target actor's
 * handler from the calling thread before returning. */
void mock_port_set_invoke_handlers(bool enabled);

/* Returned by ipc_port_run_all. Programmable so tests can simulate
 * "ipc_run_all() blocked because the actor thread exited with an
 * error" without actually running a thread. */
void mock_port_set_run_all_rc(int rc);

/* Snapshot of the most recent message passed to ipc_port_send for an actor. */
const struct ipc_msg *mock_port_last_send_msg(struct ipc_actor *a);
bool mock_port_has_last_send_msg(struct ipc_actor *a);

/* Snapshot of the currently-scheduled (not yet delivered) delayed message. */
bool mock_port_has_pending_send_after(struct ipc_actor *a);
const struct ipc_msg *mock_port_pending_send_after_msg(struct ipc_actor *a);
uint32_t mock_port_pending_send_after_delay_ms(struct ipc_actor *a);
