/* ipc_actor_define.h — POSIX implementation of IPC_ACTOR_DEFINE.
 *
 * The public <ipc.h> includes this header via the active port include
 * directory. POSIX pthreads own their stacks internally, so the macro
 * declares the actor object/config and registers the actor into the
 * static actor list.
 */
#pragma once

#include "ipc_port_state.h"

struct ipc_actor;

#ifdef __cplusplus
extern "C" {
#endif
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
void ipc_dispatch_actor_handlers(struct ipc_actor *self, const struct ipc_msg *msg);
#ifdef __cplusplus
}
#endif

/* Parameter names intentionally avoid struct field names such as
 * stack_size/priority/queue_depth. Preprocessor substitution is purely
 * textual, so a parameter named `stack_size` would also rewrite the
 * designated initializer `.stack_size`. */
#define IPC_ACTOR_DEFINE(actor_sym, actor_name, stack_sz, prio, qdepth)                   \
    _Static_assert((stack_sz) > 0, #actor_sym ": stack_size must be positive");           \
    _Static_assert((qdepth) > 0, #actor_sym ": queue_depth must be positive");            \
    static struct ipc_port_state actor_sym##_port_state;                                  \
    static struct ipc_actor actor_sym = {                                                 \
        .name    = (actor_name),                                                          \
        .handler = ipc_dispatch_actor_handlers,                                           \
        .cfg =                                                                            \
            {                                                                             \
                .stack_size  = (stack_sz),                                                \
                .priority    = (prio),                                                    \
                .queue_depth = (qdepth),                                                  \
            },                                                                            \
        .port  = &(actor_sym##_port_state),                                               \
        ._next = NULL,                                                                    \
    };                                                                                    \
    static __attribute__((constructor(101))) void actor_sym##_register_static_actor(void) \
    {                                                                                     \
        _ipc_actor_register_static(&(actor_sym));                                         \
    }

#define IPC_ACTOR_HANDLE(actor_sym, MsgType, handler_fn)                                       \
    static void handler_fn(struct ipc_actor *self, const MsgType##_payload_t *msg,             \
                           const struct ipc_msg *raw_msg);                                     \
    static void actor_sym##_##handler_fn##_ipc_trampoline(                                     \
        struct ipc_actor *self, const void *payload, const struct ipc_msg *raw_msg)            \
    {                                                                                          \
        handler_fn(self, (const MsgType##_payload_t *) payload, raw_msg);                      \
    }                                                                                          \
    static __attribute__((constructor(102))) void actor_sym##_##handler_fn##_register_handler( \
        void)                                                                                  \
    {                                                                                          \
        _ipc_actor_register_handler_static(&(actor_sym), &(MsgType),                           \
                                           actor_sym##_##handler_fn##_ipc_trampoline);         \
    }                                                                                          \
    static void handler_fn(struct ipc_actor *self, const MsgType##_payload_t *msg,             \
                           const struct ipc_msg *raw_msg)

#define IPC_START_HOOK(actor_sym, hook_fn)                                                    \
    static void hook_fn(struct ipc_actor *self);                                              \
    static __attribute__((constructor(102))) void actor_sym##_##hook_fn##_register_hook(void) \
    {                                                                                         \
        _ipc_actor_register_start_hook_static(&(actor_sym), hook_fn);                         \
    }                                                                                         \
    static void hook_fn(struct ipc_actor *self)

#define IPC_STOP_HOOK(actor_sym, hook_fn)                                                     \
    static void hook_fn(struct ipc_actor *self);                                              \
    static __attribute__((constructor(102))) void actor_sym##_##hook_fn##_register_hook(void) \
    {                                                                                         \
        _ipc_actor_register_stop_hook_static(&(actor_sym), hook_fn);                          \
    }                                                                                         \
    static void hook_fn(struct ipc_actor *self)

#define IPC_UNKNOWN(actor_sym, hook_fn)                                                       \
    static void hook_fn(struct ipc_actor *self, const struct ipc_msg *msg);                   \
    static __attribute__((constructor(102))) void actor_sym##_##hook_fn##_register_hook(void) \
    {                                                                                         \
        _ipc_actor_register_unknown_hook_static(&(actor_sym), hook_fn);                       \
    }                                                                                         \
    static void hook_fn(struct ipc_actor *self, const struct ipc_msg *msg)

#define IPC_SUPERVISE(actor_sym, strategy)                                               \
    static __attribute__((constructor(102))) void actor_sym##_register_supervision(void) \
    {                                                                                    \
        _ipc_actor_register_supervision_static(&(actor_sym), (strategy));                \
    }

#define IPC_FAIL_HOOK(actor_sym, hook_fn)                                                     \
    static void hook_fn(struct ipc_actor *self, int reason);                                  \
    static __attribute__((constructor(102))) void actor_sym##_##hook_fn##_register_hook(void) \
    {                                                                                         \
        _ipc_actor_register_failure_hook_static(&(actor_sym), hook_fn);                       \
    }                                                                                         \
    static void hook_fn(struct ipc_actor *self, int reason)
