/*
 * ipc_config.h — Zephyr port overlay for IPC configuration.
 *
 * src/port/zephyr/ipc_defaults.h shadows include/ipc_defaults.h when
 * building for Zephyr, then includes this file to map Kconfig symbols.
 * The Zephyr CMake glue adds src/port/zephyr to the include path ahead
 * of include/, so both the library and application code see the same
 * sizing values.
 *
 * Reads CONFIG_ACTOR_* symbols defined by the Kconfig file in this
 * directory.  Any symbol not set in Kconfig falls through to the
 * default in include/ipc_defaults.h (which we include_next at the bottom).
 */
#pragma once

/* Payload size in bytes. Must be >= largest message struct used. */
#ifdef CONFIG_ACTOR_PAYLOAD_SIZE
#ifndef IPC_PAYLOAD_SIZE
#define IPC_PAYLOAD_SIZE CONFIG_ACTOR_PAYLOAD_SIZE
#endif
#endif

/* Maximum CMD/QUERY registrations (one entry per msg_id). */
#ifdef CONFIG_ACTOR_MAX_REGISTRATIONS
#ifndef IPC_MAX_REGISTRATIONS
#define IPC_MAX_REGISTRATIONS CONFIG_ACTOR_MAX_REGISTRATIONS
#endif
#endif

/* Maximum EVENT subscriptions (one entry per (msg_id, actor) pair). */
#ifdef CONFIG_ACTOR_MAX_SUBSCRIPTIONS
#ifndef IPC_MAX_SUBSCRIPTIONS
#define IPC_MAX_SUBSCRIPTIONS CONFIG_ACTOR_MAX_SUBSCRIPTIONS
#endif
#endif

/* Per-actor opaque port-state blob, in words (sizeof(uintptr_t) each).
 * Default 64 = 512 B on 64-bit, 256 B on 32-bit.  Tighten for small MCUs;
 * the concrete struct ipc_port_state in zephyr_ipc_port.c must still fit. */
#ifdef CONFIG_ACTOR_PORT_STATE_WORDS
#ifndef IPC_PORT_STATE_WORDS
#define IPC_PORT_STATE_WORDS CONFIG_ACTOR_PORT_STATE_WORDS
#endif
#endif

/* Stack-allocated query-wait blob, in words.  Default 24 = 192 B / 96 B.
 * Must fit struct ipc_query_wait_impl in zephyr_ipc_port.c. */
#ifdef CONFIG_ACTOR_QUERY_WAIT_WORDS
#ifndef IPC_QUERY_WAIT_WORDS
#define IPC_QUERY_WAIT_WORDS CONFIG_ACTOR_QUERY_WAIT_WORDS
#endif
#endif

/* Fall back to public defaults for anything Kconfig didn't define. */
#include_next "ipc_defaults.h"
