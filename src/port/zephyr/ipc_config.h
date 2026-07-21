/*
 * ipc_config.h — Zephyr port IPC configuration shim.
 *
 * Message payload size is user-configurable. Actor stack/queue storage and
 * per-actor port state are declared per actor by IPC_ACTOR_DEFINE(). Other
 * capacities are private fixed implementation constants.
 */
#pragma once

#ifdef CONFIG_ACTOR_PAYLOAD_SIZE
#ifndef IPC_PAYLOAD_SIZE
#define IPC_PAYLOAD_SIZE CONFIG_ACTOR_PAYLOAD_SIZE
#endif
#endif

#include_next "ipc_defaults.h"
