/*
 * ipc_config.h — Zephyr port IPC configuration shim.
 *
 * Actor queue payload capacity is declared per actor by IPC_ACTOR_DEFINE().
 * The shared IPC defaults still provide IPC_PAYLOAD_SIZE as a compatibility
 * fallback for low-level/manual actor setup.
 */
#pragma once

#include_next "ipc_defaults.h"
