/* ipc_actor_define_common.h — shared declarations for port actor macros. */
#pragma once

struct ipc_actor;
struct ipc_msg;

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
