/* ipc_actor_define.h — Zephyr implementation of IPC_ACTOR_DEFINE.
 *
 * The public <ipc.h> includes this header via the active port include
 * directory. Zephyr actors declared with IPC_ACTOR_DEFINE get static
 * k_thread stack storage and exact-size k_msgq backing storage. Zephyr may
 * add architecture-specific stack overhead, so the usable stack size is taken
 * from K_THREAD_STACK_SIZEOF(). The Zephyr port discovers those resources
 * during ipc_start_all_actors().
 */
#pragma once

#include <zephyr/kernel.h>

struct ipc_actor;

#ifdef __cplusplus
extern "C" {
#endif
void _ipc_actor_register_static(struct ipc_actor *actor);
int ipc_port_register_static_actor_resources(struct ipc_actor *actor, void *stack,
                                             size_t stack_size, char *msgq_buf, size_t queue_depth);
#ifdef __cplusplus
}
#endif

/* Parameter names intentionally avoid struct field names such as
 * stack_size/priority/queue_depth. Preprocessor substitution is purely
 * textual, so a parameter named `stack_size` would also rewrite the
 * designated initializer `.stack_size`. */
#define IPC_ACTOR_DEFINE(actor_sym, actor_name, handler_fn, stack_sz, prio, qdepth)                \
    _Static_assert((stack_sz) > 0, #actor_sym ": stack_size must be positive");                    \
    _Static_assert((qdepth) > 0, #actor_sym ": queue_depth must be positive");                     \
    K_THREAD_STACK_DEFINE(actor_sym##_stack, (stack_sz));                                          \
    static char actor_sym##_msgq_buf[(qdepth) * sizeof(struct ipc_msg)];                           \
    static struct ipc_actor actor_sym = {                                                          \
        .name    = (actor_name),                                                                   \
        .handler = (handler_fn),                                                                   \
        .cfg =                                                                                     \
            {                                                                                      \
                .stack_size  = K_THREAD_STACK_SIZEOF(actor_sym##_stack),                           \
                .priority    = (prio),                                                             \
                .queue_depth = sizeof(actor_sym##_msgq_buf) / sizeof(struct ipc_msg),              \
            },                                                                                     \
        ._next = NULL,                                                                             \
    };                                                                                             \
    static __attribute__((constructor(101))) void actor_sym##_register_static_actor(void)          \
    {                                                                                              \
        (void) ipc_port_register_static_actor_resources(                                           \
            &(actor_sym), (void *) &(actor_sym##_stack), K_THREAD_STACK_SIZEOF(actor_sym##_stack), \
            (actor_sym##_msgq_buf), sizeof(actor_sym##_msgq_buf) / sizeof(struct ipc_msg));        \
        _ipc_actor_register_static(&(actor_sym));                                                  \
    }
