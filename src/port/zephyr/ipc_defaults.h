/*
 * ipc_defaults.h — Zephyr wrapper for the shared IPC defaults.
 *
 * This file intentionally has the same basename as include/ipc_defaults.h.
 * It supports direct Zephyr includes of <ipc_defaults.h>. The selected
 * Zephyr ipc_config.h maps CONFIG_ACTOR_PAYLOAD_SIZE to IPC_PAYLOAD_SIZE,
 * then includes the public defaults for the fallback value.
 */
#pragma once

#include "ipc_config.h"
