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

#include "../../ipc_actor_define_common.h"
#include "ipc_port_state.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>

struct ipc_actor;

#ifdef __cplusplus
extern "C" {
#endif
int ipc_port_register_static_actor_resources(struct ipc_actor *actor, void *stack,
                                             size_t stack_size, char *msgq_buf, size_t queue_depth,
                                             size_t slot_size, char *delayed_payload,
                                             char *send_slot, char *recv_slot);
#ifdef __cplusplus
}
#endif

/* Parameter names intentionally avoid struct field names such as
 * stack_size/priority/queue_depth. Preprocessor substitution is purely
 * textual, so a parameter named `stack_size` would also rewrite the
 * designated initializer `.stack_size`. */
#define _IPC_ACTOR_DEFINE(actor_sym, actor_name, stack_sz, prio, qdepth, max_payload)              \
    _IPC_STATIC_ASSERT((stack_sz) > 0, #actor_sym ": stack_size must be positive");                \
    _IPC_STATIC_ASSERT((qdepth) > 0, #actor_sym ": queue_depth must be positive");                 \
    _IPC_STATIC_ASSERT((max_payload) >= 0, #actor_sym ": max_payload must be non-negative");       \
    enum { actor_sym##_max_payload_size = (max_payload) };                                         \
    enum { actor_sym##_msg_slot_size = IPC_MSG_SLOT_SIZE(max_payload) };                           \
    K_THREAD_STACK_DEFINE(actor_sym##_stack, (stack_sz));                                          \
    static char actor_sym##_msgq_buf[(qdepth) * actor_sym##_msg_slot_size];                        \
    static char actor_sym##_delayed_payload[(max_payload) > 0 ? (max_payload) : 1];                \
    static char actor_sym##_send_slot[actor_sym##_msg_slot_size];                                  \
    static char actor_sym##_recv_slot[actor_sym##_msg_slot_size];                                  \
    static struct ipc_port_state actor_sym##_port_state;                                           \
    static struct ipc_actor actor_sym = {                                                          \
        .name            = (actor_name),                                                           \
        .handler         = ipc_dispatch_actor_handlers,                                            \
        .start_hook      = NULL,                                                                   \
        .stop_hook       = NULL,                                                                   \
        .unknown_handler = NULL,                                                                   \
        .supervision     = IPC_SUPERVISE_NONE,                                                     \
        .failure_hook    = NULL,                                                                   \
        .cfg =                                                                                     \
            {                                                                                      \
                .stack_size       = K_THREAD_STACK_SIZEOF(actor_sym##_stack),                      \
                .priority         = (prio),                                                        \
                .queue_depth      = sizeof(actor_sym##_msgq_buf) / actor_sym##_msg_slot_size,      \
                .max_payload_size = (max_payload),                                                 \
            },                                                                                     \
        .port  = &(actor_sym##_port_state),                                                        \
        ._next = NULL,                                                                             \
    };                                                                                             \
    static int actor_sym##_register_static_actor(void)                                             \
    {                                                                                              \
        (void) ipc_port_register_static_actor_resources(                                           \
            &(actor_sym), (void *) &(actor_sym##_stack), K_THREAD_STACK_SIZEOF(actor_sym##_stack), \
            (actor_sym##_msgq_buf), sizeof(actor_sym##_msgq_buf) / actor_sym##_msg_slot_size,      \
            actor_sym##_msg_slot_size, actor_sym##_delayed_payload, actor_sym##_send_slot,         \
            actor_sym##_recv_slot);                                                                \
        _ipc_actor_register_static(&(actor_sym));                                                  \
        return 0;                                                                                  \
    }                                                                                              \
    SYS_INIT(actor_sym##_register_static_actor, PRE_KERNEL_2, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT)

/* The public IPC_ACTOR_DEFINE macro is defined in <ipc.h> and delegates to the
 * private _IPC_ACTOR_DEFINE() above, so the public signature and
 * documentation live in the public header. */

#define _IPC_ACTOR_HANDLE_IMPL(actor_sym, MsgType, handler_fn)                      \
    _IPC_ACTOR_HANDLE_ADAPTER(actor_sym, MsgType, handler_fn)                       \
    static int actor_sym##_##handler_fn##_register_handler(void)                    \
    {                                                                               \
        _ipc_actor_register_handler_static(&(actor_sym), &(MsgType),                \
                                           actor_sym##_##handler_fn##_ipc_adapter); \
        return 0;                                                                   \
    }                                                                               \
    SYS_INIT(actor_sym##_##handler_fn##_register_handler, PRE_KERNEL_2,             \
             CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);                                  \
    static void handler_fn(struct ipc_actor *self, const MsgType##_payload_t *msg,  \
                           const struct ipc_msg *raw_msg)

#define _IPC_START_HOOK_IMPL(actor_sym, hook_fn)                      \
    static void hook_fn(struct ipc_actor *self);                      \
    static int actor_sym##_##hook_fn##_register_hook(void)            \
    {                                                                 \
        _ipc_actor_register_start_hook_static(&(actor_sym), hook_fn); \
        return 0;                                                     \
    }                                                                 \
    SYS_INIT(actor_sym##_##hook_fn##_register_hook, PRE_KERNEL_2,     \
             CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);                    \
    static void hook_fn(struct ipc_actor *self)

#define _IPC_STOP_HOOK_IMPL(actor_sym, hook_fn)                      \
    static void hook_fn(struct ipc_actor *self);                     \
    static int actor_sym##_##hook_fn##_register_hook(void)           \
    {                                                                \
        _ipc_actor_register_stop_hook_static(&(actor_sym), hook_fn); \
        return 0;                                                    \
    }                                                                \
    SYS_INIT(actor_sym##_##hook_fn##_register_hook, PRE_KERNEL_2,    \
             CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);                   \
    static void hook_fn(struct ipc_actor *self)

#define _IPC_UNKNOWN_IMPL(actor_sym, hook_fn)                               \
    static void hook_fn(struct ipc_actor *self, const struct ipc_msg *msg); \
    static int actor_sym##_##hook_fn##_register_hook(void)                  \
    {                                                                       \
        _ipc_actor_register_unknown_hook_static(&(actor_sym), hook_fn);     \
        return 0;                                                           \
    }                                                                       \
    SYS_INIT(actor_sym##_##hook_fn##_register_hook, PRE_KERNEL_2,           \
             CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);                          \
    static void hook_fn(struct ipc_actor *self, const struct ipc_msg *msg)

#define _IPC_SUPERVISE_IMPL(actor_sym, strategy)                          \
    static int actor_sym##_register_supervision(void)                     \
    {                                                                     \
        _ipc_actor_register_supervision_static(&(actor_sym), (strategy)); \
        return 0;                                                         \
    }                                                                     \
    SYS_INIT(actor_sym##_register_supervision, PRE_KERNEL_2, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT)

#define _IPC_FAIL_HOOK_IMPL(actor_sym, hook_fn)                         \
    static void hook_fn(struct ipc_actor *self, int reason);            \
    static int actor_sym##_##hook_fn##_register_hook(void)              \
    {                                                                   \
        _ipc_actor_register_failure_hook_static(&(actor_sym), hook_fn); \
        return 0;                                                       \
    }                                                                   \
    SYS_INIT(actor_sym##_##hook_fn##_register_hook, PRE_KERNEL_2,       \
             CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);                      \
    static void hook_fn(struct ipc_actor *self, int reason)
