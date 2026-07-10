/*
 * ipc_defaults.h — Compile-time sizing defaults for the IPC actor framework.
 *
 * This header provides the default values for the IPC_* sizing macros
 * that <ipc.h> uses to size public types (struct ipc_msg, ipc_port_state_t,
 * ipc_query_wait_t) and internal tables.  It lives on the public include
 * path because <ipc.h> #include's it, and every translation unit that
 * includes <ipc.h> therefore needs it resolvable.
 *
 * ── Override paths (in order of precedence) ─────────────────────────────
 *
 *   1. Per-TU: define the IPC_* macro before including <ipc.h>
 *
 *        #define IPC_PAYLOAD_SIZE 64
 *        #include <ipc.h>
 *
 *   2. Globally on the command line:
 *
 *        gcc -DIPC_PAYLOAD_SIZE=64 ...
 *
 *   3. On Zephyr, set CONFIG_ACTOR_* in Kconfig.  <ipc.h> includes the
 *      port-side ipc_config.h, which maps Kconfig values to IPC_* macros,
 *      then #include_next's this file to fill in any symbol the Kconfig
 *      didn't set.  In a plain POSIX build this header is the only source
 *      of values.
 *
 * ── Tuning table (defaults shown) ──────────────────────────────────────
 *
 *   Knob                          Default   Affects
 *   ─────────────────────────     ───────   ──────────────────────────────
 *   IPC_PAYLOAD_SIZE              32        wire message payload bytes
 *   IPC_MAX_REGISTRATIONS         32        CMD/QUERY ID → actor map
 *   IPC_MAX_SUBSCRIPTIONS         32        EVENT ID → actor list
 *   IPC_PORT_STATE_WORDS          64        per-actor opaque blob
 *                                           (×sizeof(uintptr_t) bytes; must
 *                                           fit struct ipc_port_state in
 *                                           the chosen port)
 *   IPC_QUERY_WAIT_WORDS          24        query-wait blob (×sizeof(uintptr_t);
 *                                           must fit struct ipc_query_wait_impl
 *                                           in the port)
 *   IPC_MAX_INFLIGHT_QUERIES      16        max concurrent in-flight queries
 *                                           (sender blocks via a slot in the
 *                                           static wait_table; the reply
 *                                           thread writes the response into
 *                                           the same slot)
 *
 * Static footprint: ~128 B for the two tables, plus
 * IPC_MAX_INFLIGHT_QUERIES × IPC_QUERY_WAIT_WORDS × sizeof(uintptr_t) for
 * the wait table, plus N actors × IPC_PORT_STATE_WORDS × sizeof(uintptr_t)
 * for embedded state.
 */
#pragma once

#ifndef IPC_PAYLOAD_SIZE
#define IPC_PAYLOAD_SIZE 32
#endif

#ifndef IPC_MAX_REGISTRATIONS
#define IPC_MAX_REGISTRATIONS 32
#endif

#ifndef IPC_MAX_SUBSCRIPTIONS
#define IPC_MAX_SUBSCRIPTIONS 32
#endif

#ifndef IPC_PORT_STATE_WORDS
#define IPC_PORT_STATE_WORDS 64
#endif

#ifndef IPC_QUERY_WAIT_WORDS
#define IPC_QUERY_WAIT_WORDS 24
#endif

#ifndef IPC_MAX_INFLIGHT_QUERIES
#define IPC_MAX_INFLIGHT_QUERIES 16
#endif
