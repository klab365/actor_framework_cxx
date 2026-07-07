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
