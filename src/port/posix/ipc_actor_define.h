/* ipc_actor_define.h — POSIX implementation of IPC_ACTOR_DEFINE.
 *
 * The public <ipc.h> includes this header via the active port include
 * directory. POSIX pthreads own their stacks internally, so the macro
 * declares the actor object/config and registers the actor into the
 * static actor list.
 */
#pragma once

#include "../../ipc_actor_define_common.h"
#include "ipc_port_state.h"

struct ipc_actor;

/* Parameter names intentionally avoid struct field names such as
 * stack_size/priority/queue_depth. Preprocessor substitution is purely
 * textual, so a parameter named `stack_size` would also rewrite the
 * designated initializer `.stack_size`. */
#define _IPC_ACTOR_DEFINE(actor_sym, actor_name, stack_sz, prio, qdepth, max_payload)        \
    _IPC_STATIC_ASSERT((stack_sz) > 0, #actor_sym ": stack_size must be positive");          \
    _IPC_STATIC_ASSERT((qdepth) > 0, #actor_sym ": queue_depth must be positive");           \
    _IPC_STATIC_ASSERT((max_payload) >= 0, #actor_sym ": max_payload must be non-negative"); \
    enum { actor_sym##_max_payload_size = (max_payload) };                                   \
    static struct ipc_port_state actor_sym##_port_state;                                     \
    static struct ipc_actor actor_sym = {                                                    \
        .name            = (actor_name),                                                     \
        .handler         = ipc_dispatch_actor_handlers,                                      \
        .start_hook      = NULL,                                                             \
        .stop_hook       = NULL,                                                             \
        .unknown_handler = NULL,                                                             \
        .supervision     = IPC_SUPERVISE_NONE,                                               \
        .failure_hook    = NULL,                                                             \
        .cfg =                                                                               \
            {                                                                                \
                .stack_size       = (stack_sz),                                              \
                .priority         = (prio),                                                  \
                .queue_depth      = (qdepth),                                                \
                .max_payload_size = (max_payload),                                           \
            },                                                                               \
        .port  = &(actor_sym##_port_state),                                                  \
        ._next = NULL,                                                                       \
    };                                                                                       \
    static __attribute__((constructor(101))) void actor_sym##_register_static_actor(void)    \
    {                                                                                        \
        _ipc_actor_register_static(&(actor_sym));                                            \
    }

/* The public IPC_ACTOR_DEFINE macro is defined in <ipc.h> and delegates to the
 * private _IPC_ACTOR_DEFINE() above, so the public signature and
 * documentation live in the public header. */

#define _IPC_ACTOR_HANDLE_IMPL(actor_sym, MsgType, handler_fn)                                 \
    _IPC_ACTOR_HANDLE_ADAPTER(actor_sym, MsgType, handler_fn)                                  \
    static __attribute__((constructor(102))) void actor_sym##_##handler_fn##_register_handler( \
        void)                                                                                  \
    {                                                                                          \
        _ipc_actor_register_handler_static(&(actor_sym), &(MsgType),                           \
                                           actor_sym##_##handler_fn##_ipc_adapter);            \
    }                                                                                          \
    static void handler_fn(struct ipc_actor *self, const MsgType##_payload_t *msg,             \
                           const struct ipc_msg *raw_msg)

#define _IPC_START_HOOK_IMPL(actor_sym, hook_fn)                                              \
    static void hook_fn(struct ipc_actor *self);                                              \
    static __attribute__((constructor(102))) void actor_sym##_##hook_fn##_register_hook(void) \
    {                                                                                         \
        _ipc_actor_register_start_hook_static(&(actor_sym), hook_fn);                         \
    }                                                                                         \
    static void hook_fn(struct ipc_actor *self)

#define _IPC_STOP_HOOK_IMPL(actor_sym, hook_fn)                                               \
    static void hook_fn(struct ipc_actor *self);                                              \
    static __attribute__((constructor(102))) void actor_sym##_##hook_fn##_register_hook(void) \
    {                                                                                         \
        _ipc_actor_register_stop_hook_static(&(actor_sym), hook_fn);                          \
    }                                                                                         \
    static void hook_fn(struct ipc_actor *self)

#define _IPC_UNKNOWN_IMPL(actor_sym, hook_fn)                                                 \
    static void hook_fn(struct ipc_actor *self, const struct ipc_msg *msg);                   \
    static __attribute__((constructor(102))) void actor_sym##_##hook_fn##_register_hook(void) \
    {                                                                                         \
        _ipc_actor_register_unknown_hook_static(&(actor_sym), hook_fn);                       \
    }                                                                                         \
    static void hook_fn(struct ipc_actor *self, const struct ipc_msg *msg)

#define _IPC_SUPERVISE_IMPL(actor_sym, strategy)                                         \
    static __attribute__((constructor(102))) void actor_sym##_register_supervision(void) \
    {                                                                                    \
        _ipc_actor_register_supervision_static(&(actor_sym), (strategy));                \
    }

#define _IPC_FAIL_HOOK_IMPL(actor_sym, hook_fn)                                               \
    static void hook_fn(struct ipc_actor *self, int reason);                                  \
    static __attribute__((constructor(102))) void actor_sym##_##hook_fn##_register_hook(void) \
    {                                                                                         \
        _ipc_actor_register_failure_hook_static(&(actor_sym), hook_fn);                       \
    }                                                                                         \
    static void hook_fn(struct ipc_actor *self, int reason)
