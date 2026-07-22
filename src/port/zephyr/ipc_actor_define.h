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

#include "ipc_port_state.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>

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
void ipc_dispatch_actor_handlers(struct ipc_actor *self, const struct ipc_msg *msg);
int ipc_port_register_static_actor_resources(struct ipc_actor *actor, void *stack,
                                             size_t stack_size, char *msgq_buf, size_t queue_depth);
#ifdef __cplusplus
}
#endif

/* Parameter names intentionally avoid struct field names such as
 * stack_size/priority/queue_depth. Preprocessor substitution is purely
 * textual, so a parameter named `stack_size` would also rewrite the
 * designated initializer `.stack_size`. */
#define IPC_ACTOR_DEFINE(actor_sym, actor_name, stack_sz, prio, qdepth)                            \
    _Static_assert((stack_sz) > 0, #actor_sym ": stack_size must be positive");                    \
    _Static_assert((qdepth) > 0, #actor_sym ": queue_depth must be positive");                     \
    K_THREAD_STACK_DEFINE(actor_sym##_stack, (stack_sz));                                          \
    static char actor_sym##_msgq_buf[(qdepth) * sizeof(struct ipc_msg)];                           \
    static struct ipc_port_state actor_sym##_port_state;                                           \
    static struct ipc_actor actor_sym = {                                                          \
        .name    = (actor_name),                                                                   \
        .handler = ipc_dispatch_actor_handlers,                                                    \
        .cfg =                                                                                     \
            {                                                                                      \
                .stack_size  = K_THREAD_STACK_SIZEOF(actor_sym##_stack),                           \
                .priority    = (prio),                                                             \
                .queue_depth = sizeof(actor_sym##_msgq_buf) / sizeof(struct ipc_msg),              \
            },                                                                                     \
        .port  = &(actor_sym##_port_state),                                                        \
        ._next = NULL,                                                                             \
    };                                                                                             \
    static int actor_sym##_register_static_actor(void)                                             \
    {                                                                                              \
        (void) ipc_port_register_static_actor_resources(                                           \
            &(actor_sym), (void *) &(actor_sym##_stack), K_THREAD_STACK_SIZEOF(actor_sym##_stack), \
            (actor_sym##_msgq_buf), sizeof(actor_sym##_msgq_buf) / sizeof(struct ipc_msg));        \
        _ipc_actor_register_static(&(actor_sym));                                                  \
        return 0;                                                                                  \
    }                                                                                              \
    SYS_INIT(actor_sym##_register_static_actor, PRE_KERNEL_2, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT)

#define IPC_ACTOR_HANDLE(actor_sym, MsgType, handler_fn)                               \
    static void handler_fn(struct ipc_actor *self, const MsgType##_payload_t *msg,     \
                           const struct ipc_msg *raw_msg);                             \
    static void actor_sym##_##handler_fn##_ipc_trampoline(                             \
        struct ipc_actor *self, const void *payload, const struct ipc_msg *raw_msg)    \
    {                                                                                  \
        handler_fn(self, (const MsgType##_payload_t *) payload, raw_msg);              \
    }                                                                                  \
    static int actor_sym##_##handler_fn##_register_handler(void)                       \
    {                                                                                  \
        _ipc_actor_register_handler_static(&(actor_sym), &(MsgType),                   \
                                           actor_sym##_##handler_fn##_ipc_trampoline); \
        return 0;                                                                      \
    }                                                                                  \
    SYS_INIT(actor_sym##_##handler_fn##_register_handler, PRE_KERNEL_2,                \
             CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);                                     \
    static void handler_fn(struct ipc_actor *self, const MsgType##_payload_t *msg,     \
                           const struct ipc_msg *raw_msg)

#define IPC_START_HOOK(actor_sym, hook_fn)                            \
    static void hook_fn(struct ipc_actor *self);                      \
    static int actor_sym##_##hook_fn##_register_hook(void)            \
    {                                                                 \
        _ipc_actor_register_start_hook_static(&(actor_sym), hook_fn); \
        return 0;                                                     \
    }                                                                 \
    SYS_INIT(actor_sym##_##hook_fn##_register_hook, PRE_KERNEL_2,     \
             CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);                    \
    static void hook_fn(struct ipc_actor *self)

#define IPC_STOP_HOOK(actor_sym, hook_fn)                            \
    static void hook_fn(struct ipc_actor *self);                     \
    static int actor_sym##_##hook_fn##_register_hook(void)           \
    {                                                                \
        _ipc_actor_register_stop_hook_static(&(actor_sym), hook_fn); \
        return 0;                                                    \
    }                                                                \
    SYS_INIT(actor_sym##_##hook_fn##_register_hook, PRE_KERNEL_2,    \
             CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);                   \
    static void hook_fn(struct ipc_actor *self)

#define IPC_UNKNOWN(actor_sym, hook_fn)                                     \
    static void hook_fn(struct ipc_actor *self, const struct ipc_msg *msg); \
    static int actor_sym##_##hook_fn##_register_hook(void)                  \
    {                                                                       \
        _ipc_actor_register_unknown_hook_static(&(actor_sym), hook_fn);     \
        return 0;                                                           \
    }                                                                       \
    SYS_INIT(actor_sym##_##hook_fn##_register_hook, PRE_KERNEL_2,           \
             CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);                          \
    static void hook_fn(struct ipc_actor *self, const struct ipc_msg *msg)
