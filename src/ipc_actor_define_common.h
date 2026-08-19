/* ipc_actor_define_common.h — shared declarations for port actor macros. */
#pragma once

struct ipc_actor;
struct ipc_msg;

#ifdef __cplusplus
#define _IPC_STATIC_ASSERT(condition, message) static_assert((condition), message)
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

#define _IPC_ACTOR_HANDLE_ADAPTER(actor_sym, MsgType, handler_fn)                   \
    static_assert(sizeof(MsgType##_payload_t) <= actor_sym##_max_payload_size,      \
                  #MsgType " payload exceeds actor max payload size");              \
    static void handler_fn(struct ipc_actor *self, const MsgType##_payload_t *msg,  \
                           const struct ipc_msg *raw_msg);                          \
    static void actor_sym##_##handler_fn##_ipc_adapter(                             \
        struct ipc_actor *self, const void *payload, const struct ipc_msg *raw_msg) \
    {                                                                               \
        handler_fn(self, (const MsgType##_payload_t *) payload, raw_msg);           \
    }

#define _IPC_ACTOR_RESPONSE_HANDLE_IMPL(actor_sym, RequestType, ReplyType, handler_fn)      \
    static_assert(sizeof(ReplyType##_payload_t) <= actor_sym##_max_payload_size,            \
                  #ReplyType " reply payload exceeds actor max payload size");              \
    static void handler_fn##_ipc_typed(struct ipc_actor *self, int result,                  \
                                       const ReplyType##_payload_t *msg,                    \
                                       const struct ipc_msg *raw_msg);                      \
    static void handler_fn(struct ipc_actor *self, int result, const void *reply_payload,   \
                           size_t reply_size, const struct ipc_msg *raw_msg)                \
    {                                                                                       \
        (void) RequestType##_reply_desc;                                                    \
        assert(RequestType##_reply_desc == &(ReplyType));                                   \
        if (result == 0) {                                                                  \
            assert(reply_size == sizeof(ReplyType##_payload_t));                            \
            if (reply_size != sizeof(ReplyType##_payload_t)) {                              \
                return;                                                                     \
            }                                                                               \
        }                                                                                   \
        handler_fn##_ipc_typed(self, result, (const ReplyType##_payload_t *) reply_payload, \
                               raw_msg);                                                    \
    }                                                                                       \
    static void handler_fn##_ipc_typed(struct ipc_actor *self, int result,                  \
                                       const ReplyType##_payload_t *msg,                    \
                                       const struct ipc_msg *raw_msg)

#ifdef __cplusplus
}
#else
#define _IPC_STATIC_ASSERT(condition, message) _Static_assert((condition), message)
#endif
