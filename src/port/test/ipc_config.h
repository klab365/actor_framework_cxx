/* ipc_config.h — deterministic host test-port configuration shim. */
#pragma once

#define IPC_CONFIG_TEST_PORT 1

#ifndef IPC_CORE_MAX_REGISTRATIONS
#define IPC_CORE_MAX_REGISTRATIONS 32
#endif

#ifndef IPC_CORE_MAX_SUBSCRIPTIONS
#define IPC_CORE_MAX_SUBSCRIPTIONS 32
#endif

#ifndef IPC_CORE_MAX_INFLIGHT_QUERIES
#define IPC_CORE_MAX_INFLIGHT_QUERIES 16
#endif

#ifndef IPC_CONFIG_ENABLE_ASK
#define IPC_CONFIG_ENABLE_ASK 1
#endif

#ifndef IPC_CONFIG_ENABLE_DIAGNOSTICS
#define IPC_CONFIG_ENABLE_DIAGNOSTICS 1
#endif
