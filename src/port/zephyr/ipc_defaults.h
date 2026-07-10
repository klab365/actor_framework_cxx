/*
 * ipc_defaults.h — Zephyr Kconfig-aware override wrapper.
 *
 * This file intentionally has the same basename as include/ipc_defaults.h.
 * It supports direct Zephyr includes of <ipc_defaults.h>.  Normal <ipc.h>
 * includes use ipc_config.h directly when __ZEPHYR__ is defined.
 * ipc_config.h maps CONFIG_ACTOR_* symbols to IPC_* macros and then includes
 * the public defaults with #include_next for any value not set by Kconfig.
 */
#pragma once

#include "ipc_config.h"
