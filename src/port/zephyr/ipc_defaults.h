/*
 * ipc_defaults.h — Zephyr wrapper for the shared IPC defaults.
 *
 * This file intentionally has the same basename as include/ipc_defaults.h.
 * It supports direct Zephyr includes of <ipc_defaults.h> by routing through
 * the selected port's ipc_config.h, which includes the public defaults.
 */
#pragma once

#include "ipc_config.h"
