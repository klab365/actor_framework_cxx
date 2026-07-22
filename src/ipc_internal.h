/*
 * ipc_internal.h — Private header for IPC framework implementation.
 * NOT installed; NOT on the public include path.
 *
 * Anything that users of the framework should not see or override lives here.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ipc.h"

#ifdef __cplusplus
enum class ipc_core_limit : size_t {
    max_registrations    = 32,
    max_subscriptions    = 32,
    max_inflight_queries = 16,
};

inline constexpr size_t IPC_CORE_MAX_REGISTRATIONS =
    static_cast<size_t>(ipc_core_limit::max_registrations);
inline constexpr size_t IPC_CORE_MAX_SUBSCRIPTIONS =
    static_cast<size_t>(ipc_core_limit::max_subscriptions);
inline constexpr size_t IPC_CORE_MAX_INFLIGHT_QUERIES =
    static_cast<size_t>(ipc_core_limit::max_inflight_queries);
#else
#define IPC_CORE_MAX_REGISTRATIONS 32
#define IPC_CORE_MAX_SUBSCRIPTIONS 32
#define IPC_CORE_MAX_INFLIGHT_QUERIES 16
#endif

/* ── FNV-1a hash (used internally for lazy descriptor ID initialisation) ── */

static inline uint32_t _ipc_fnv1a(const char *s)
{
    uint32_t h = 0x811c9dc5u;
    while (*s) {
        h = (h ^ (uint8_t) *s) * 0x01000193u;
        s++;
    }
    return h;
}

/* ── Test reset ─────────────────────────────────────────────────────────── */
/*
 * Resets the registration, subscription, and actor-list tables. Intended
 * for unit/integration tests only. Defined in ipc.c. Production code must
 * never call this.
 */
void _ipc_reset_for_testing(void);

/* Registers a statically defined actor in the core actor list.
 * Called by IPC_ACTOR_DEFINE-generated startup hooks; tests may call
 * it after _ipc_reset_for_testing() because startup hooks do not rerun. */
void _ipc_actor_register_static(struct ipc_actor *actor);
void _ipc_actor_register_handler_static(struct ipc_actor *actor, ipc_msg_desc_t *desc,
                                        ipc_actor_msg_handler_t handler);
void _ipc_actor_register_start_hook_static(struct ipc_actor *actor,
                                           ipc_actor_lifecycle_hook_t hook);
void _ipc_actor_register_stop_hook_static(struct ipc_actor *actor, ipc_actor_lifecycle_hook_t hook);
void _ipc_actor_register_unknown_hook_static(struct ipc_actor *actor,
                                             ipc_actor_unknown_handler_t hook);
void _ipc_actor_register_supervision_static(struct ipc_actor *actor,
                                            ipc_supervision_strategy_t strategy);
void _ipc_actor_register_failure_hook_static(struct ipc_actor *actor,
                                             ipc_actor_failure_hook_t hook);
