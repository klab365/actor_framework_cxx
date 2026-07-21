/* ipc_actor_define.h — POSIX implementation of IPC_ACTOR_DEFINE.
 *
 * The public <ipc.h> includes this header via the active port include
 * directory. POSIX pthreads own their stacks internally, so the macro
 * declares the actor object/config and registers the actor into the
 * static actor list.
 */
#pragma once

struct ipc_actor;

#ifdef __cplusplus
extern "C" {
#endif
void _ipc_actor_register_static(struct ipc_actor *actor);
#ifdef __cplusplus
}
#endif

/* Parameter names intentionally avoid struct field names such as
 * stack_size/priority/queue_depth. Preprocessor substitution is purely
 * textual, so a parameter named `stack_size` would also rewrite the
 * designated initializer `.stack_size`. */
#define IPC_ACTOR_DEFINE(actor_sym, actor_name, handler_fn, stack_sz, prio, qdepth)       \
    _Static_assert((stack_sz) > 0, #actor_sym ": stack_size must be positive");           \
    _Static_assert((qdepth) > 0, #actor_sym ": queue_depth must be positive");            \
    static struct ipc_actor actor_sym = {                                                 \
        .name    = (actor_name),                                                          \
        .handler = (handler_fn),                                                          \
        .cfg =                                                                            \
            {                                                                             \
                .stack_size  = (stack_sz),                                                \
                .priority    = (prio),                                                    \
                .queue_depth = (qdepth),                                                  \
            },                                                                            \
        ._next = NULL,                                                                    \
    };                                                                                    \
    static __attribute__((constructor(101))) void actor_sym##_register_static_actor(void) \
    {                                                                                     \
        _ipc_actor_register_static(&(actor_sym));                                         \
    }
