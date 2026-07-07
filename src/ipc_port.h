/*
 * ipc_port.h — Generic port interface for the IPC actor framework.
 *
 * No platform-specific types in this file. Implementations live in
 * posix_ipc_port.c / zephyr_ipc_port.c (or any other backend).
 */
#pragma once

#include "ipc.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* IPC_QUERY_WAIT_WORDS comes from ipc_defaults.h (see there). */

/* ── Opaque query-wait blob ────────────────────────────────────────────── */

/*
 * The query-wait blob is owned by the static wait_table in ipc.c, not by
 * a stack frame. The sender claims a slot, the reply thread writes the
 * response into it, and the sender frees it. Both sides use
 * ipc_port_query_wait_{init,destroy,block,wake} on the same slot.
 */
typedef struct {
    uintptr_t _opaque[IPC_QUERY_WAIT_WORDS];
} ipc_query_wait_t;

/* ── Wait-table token encoding (used in struct ipc_msg._wait) ─────────────── */

/*
 * Tokens in `msg._wait` are stored as (uintptr_t)(slot_index + 1) so that
 * the value 0 is reserved as the "no waiter" sentinel. A reply to a
 * message whose token is IPC_QUERY_INVALID_TOKEN (or whose decoded slot
 * is out of range / not in use) is a silent no-op.
 */
#define IPC_QUERY_INVALID_TOKEN ((uintptr_t) 0)

/* ── Layout contract for ipc_query_wait_t._opaque ────────────────────────── */

/*
 * The first IPC_QUERY_RESPONSE_SIZE bytes of an ipc_query_wait_t hold the
 * response payload, copied to/from msg.payload-sized buffers by
 * ipc_query_raw / ipc_reply_raw. Everything past that offset is opaque
 * to the core and owned by the port implementation (mutex, condvar,
 * status flag, etc.).
 *
 * Implementations of ipc_port_query_wait_* MUST place the response buffer
 * at exactly IPC_QUERY_RESPONSE_OFFSET in the concrete impl struct
 * backing ipc_query_wait_t. Both real ports and the mock port honour
 * this contract.
 */
#define IPC_QUERY_RESPONSE_OFFSET 0
#define IPC_QUERY_RESPONSE_SIZE IPC_PAYLOAD_SIZE

/* ── Per-actor port state (already opaque in ipc.h) ──────────────────────── */

/* ipc_port_state_t defined in ipc.h */

/* ── Table lock (registry / registration / subscription / wait tables) ──── */

void ipc_port_table_lock(void);
void ipc_port_table_unlock(void);

/* ── Query-wait lifecycle and wait/wake (replaces #ifdef in ipc.c) ──────── */

int ipc_port_query_wait_init(ipc_query_wait_t *w);
void ipc_port_query_wait_destroy(ipc_query_wait_t *w);

/* Block until woken, timeout expires, or success. Returns 0 on signal,
 * -ETIMEDOUT on timeout, negative errno otherwise. */
int ipc_port_query_wait_block(ipc_query_wait_t *w, ipc_timeout_t timeout);

/* Wake a blocked waiter (called from ipc_reply_raw). */
void ipc_port_query_wait_wake(ipc_query_wait_t *w);

/* ── Per-actor lifecycle (unchanged) ─────────────────────────────────────── */

int ipc_port_actor_init(struct ipc_actor *a);
int ipc_port_start(struct ipc_actor *a);
void ipc_port_stop_actor(struct ipc_actor *a);

/* ── Per-actor transport (unchanged) ─────────────────────────────────────── */

/*
 * ipc_port_send MUST copy `msg` synchronously; it must not retain a
 * pointer to the message. The publish path reuses one struct ipc_msg
 * across all subscribers and releases the table lock between sends.
 */
int ipc_port_send(struct ipc_actor *a, const struct ipc_msg *msg);
int ipc_port_send_after(struct ipc_actor *a, const struct ipc_msg *msg, uint32_t delay_ms);

/* ── Run-all (blocks on POSIX, no-op on Zephyr) ──────────────────────────── */

int ipc_port_run_all(void);
