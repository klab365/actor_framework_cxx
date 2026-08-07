/**
 * @file ipc.h
 * @brief Public API for the IPC Actor Framework.
 */
#pragma once

#include <assert.h>
#include <errno.h>
#include <ipc_config.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Message kinds ──────────────────────────────────────────────────────── */

/** @brief Message delivery kind. */
typedef enum {
    /** Event message, published to all subscribed actors. */
    IPC_EVENT = 0,

    /** Command message, routed to exactly one registered actor. */
    IPC_CMD,
} ipc_msg_kind_t;

/* ── Message sizing ─────────────────────────────────────────────────────── */

#ifdef __cplusplus
#define IPC_ALIGNOF(type) alignof(type)
#else
#define IPC_ALIGNOF(type) _Alignof(type)
#endif

#define IPC_ALIGN_UP(value, alignment) (((value) + (alignment) - 1U) & ~((alignment) - 1U))

/** @brief Header stored at the front of each port-owned queue slot. */
typedef struct {
    uint32_t id;
    ipc_msg_kind_t kind;
    size_t size;
    uint32_t ask_id;
    uint32_t reply_id;
} ipc_msg_slot_header_t;

#define IPC_MSG_SLOT_HEADER_SIZE \
    IPC_ALIGN_UP(sizeof(ipc_msg_slot_header_t), IPC_ALIGNOF(max_align_t))
#define IPC_MSG_SLOT_SIZE(max_payload_size) (IPC_MSG_SLOT_HEADER_SIZE + (max_payload_size))

#define IPC_MESSAGE_SIZE(MsgType) sizeof(MsgType##_payload_t)
#define IPC_MESSAGE_MAX1(A) IPC_MESSAGE_SIZE(A)
#define IPC_MESSAGE_MAX2(A, B) \
    (IPC_MESSAGE_SIZE(A) > IPC_MESSAGE_SIZE(B) ? IPC_MESSAGE_SIZE(A) : IPC_MESSAGE_SIZE(B))
#define IPC_MESSAGE_MAX3(A, B, C) \
    (IPC_MESSAGE_MAX2(A, B) > IPC_MESSAGE_SIZE(C) ? IPC_MESSAGE_MAX2(A, B) : IPC_MESSAGE_SIZE(C))
#define IPC_MESSAGE_MAX4(A, B, C, D)                                             \
    (IPC_MESSAGE_MAX3(A, B, C) > IPC_MESSAGE_SIZE(D) ? IPC_MESSAGE_MAX3(A, B, C) \
                                                     : IPC_MESSAGE_SIZE(D))
#define IPC_MESSAGE_MAX5(A, B, C, D, E)                                                \
    (IPC_MESSAGE_MAX4(A, B, C, D) > IPC_MESSAGE_SIZE(E) ? IPC_MESSAGE_MAX4(A, B, C, D) \
                                                        : IPC_MESSAGE_SIZE(E))
#define IPC_MESSAGE_MAX6(A, B, C, D, E, F)                                                   \
    (IPC_MESSAGE_MAX5(A, B, C, D, E) > IPC_MESSAGE_SIZE(F) ? IPC_MESSAGE_MAX5(A, B, C, D, E) \
                                                           : IPC_MESSAGE_SIZE(F))
#define IPC_MESSAGE_MAX7(A, B, C, D, E, F, G)                                                      \
    (IPC_MESSAGE_MAX6(A, B, C, D, E, F) > IPC_MESSAGE_SIZE(G) ? IPC_MESSAGE_MAX6(A, B, C, D, E, F) \
                                                              : IPC_MESSAGE_SIZE(G))
#define IPC_MESSAGE_MAX8(A, B, C, D, E, F, G, H)                 \
    (IPC_MESSAGE_MAX7(A, B, C, D, E, F, G) > IPC_MESSAGE_SIZE(H) \
         ? IPC_MESSAGE_MAX7(A, B, C, D, E, F, G)                 \
         : IPC_MESSAGE_SIZE(H))
#define IPC_MESSAGE_MAX_SELECT(_1, _2, _3, _4, _5, _6, _7, _8, NAME, ...) NAME
#define IPC_MESSAGE_MAX(...)                                                                       \
    IPC_MESSAGE_MAX_SELECT(__VA_ARGS__, IPC_MESSAGE_MAX8, IPC_MESSAGE_MAX7, IPC_MESSAGE_MAX6,      \
                           IPC_MESSAGE_MAX5, IPC_MESSAGE_MAX4, IPC_MESSAGE_MAX3, IPC_MESSAGE_MAX2, \
                           IPC_MESSAGE_MAX1)(__VA_ARGS__)

/* ── Message descriptor ───────────────────────────────────────────────── */

/**
 * @brief Runtime descriptor for a statically declared message type.
 *
 * Message descriptors are created locally by IPC_CMD_DEFINE_LOCAL() /
 * IPC_EVENT_DEFINE_LOCAL(), or declared in headers with IPC_CMD_DECLARE() /
 * IPC_EVENT_DECLARE() and defined once with IPC_CMD_DEFINE(TypeName) /
 * IPC_EVENT_DEFINE(TypeName). The descriptor is intentionally not const: `.id`
 * starts at 0 and is filled lazily from `.name` on first registration or send.
 */
typedef struct {
    /** Lazily computed message ID; 0 until first use. */
    uint32_t id;

    /** Delivery kind: command or event. */
    ipc_msg_kind_t kind;

    /** Payload size in bytes. */
    size_t size;

    /** Human-readable message type name, also used to derive `.id`. */
    const char *name;
} ipc_msg_desc_t;

/* Forward declarations */
struct ipc_actor;

/* ── Wire message ──────────────────────────────────────────────────────── */

/** @brief Raw message passed to actor handlers and copied by port queues. */
struct ipc_msg {
    /** Message ID matching ipc_msg_desc_t::id. */
    uint32_t id;

    /** Message delivery kind. */
    ipc_msg_kind_t kind;

    /** Payload size in bytes. */
    size_t size;

    /** Payload bytes valid for the duration of the handler/port call. */
    const uint8_t *payload;

    /** Internal ask correlation ID, or 0 for normal sends. */
    uint32_t ask_id;

    /** Expected reply message ID for ask requests, or 0 for normal sends. */
    uint32_t reply_id;
};

/* ── Actor config ────────────────────────────────────────────────────────── */

/** @brief Static actor runtime configuration. */
struct ipc_actor_cfg {
    /** Stack size for the actor's thread. */
    size_t stack_size;

    /** Priority for the actor's thread. */
    int priority;

    /** Depth of the actor's message queue. */
    size_t queue_depth;

    /** Maximum payload size accepted by this actor's queue. */
    size_t max_payload_size;
};

/**
 * @brief Actor-level raw message entry point.
 *
 * Called by the active port when an actor dequeues a message. Actors declared
 * with IPC_ACTOR_DEFINE() normally use ipc_dispatch_actor_handlers() here,
 * which dispatches to handlers registered with IPC_ACTOR_HANDLE().
 *
 * @param self Actor receiving the message.
 * @param msg Raw message valid for the duration of the call.
 */
typedef void (*ipc_actor_handler_t)(struct ipc_actor *self, const struct ipc_msg *msg);

/** @brief Actor lifecycle hook called during framework-managed startup/stop. */
typedef void (*ipc_actor_lifecycle_hook_t)(struct ipc_actor *self);

/** @brief Actor hook called when no typed handler matches a received message. */
typedef void (*ipc_actor_unknown_handler_t)(struct ipc_actor *self, const struct ipc_msg *msg);

/** @brief Actor supervision action applied when ipc_actor_fail() is called. */
typedef enum {
    /** No automatic action. The failure hook, if any, is still called. */
    IPC_SUPERVISE_NONE = 0,

    /** Request the actor to stop. */
    IPC_SUPERVISE_STOP,

    /** Reset actor port state and run stop/start hooks. */
    IPC_SUPERVISE_RESTART,
} ipc_supervision_strategy_t;

/** @brief Actor hook called when ipc_actor_fail() is called. */
typedef void (*ipc_actor_failure_hook_t)(struct ipc_actor *self, int reason);

/**
 * @brief Message-specific handler trampoline type.
 *
 * Common erased-payload signature used internally by IPC_ACTOR_HANDLE(). User
 * handlers receive typed payload pointers; generated trampolines adapt this
 * signature to the typed handler.
 *
 * Normal application code should not need to name this type directly.
 *
 * @param self Actor receiving the message.
 * @param payload Raw payload pointer, cast by the trampoline to the message's
 *        `<MsgType>_payload_t`.
 * @param raw_msg Complete raw message.
 */
typedef void (*ipc_actor_msg_handler_t)(struct ipc_actor *self, const void *payload,
                                        const struct ipc_msg *raw_msg);

/** @brief Callback invoked in the asking actor context when an ask reply arrives. */
typedef void (*ipc_ask_callback_t)(struct ipc_actor *self, int result, const void *reply_payload,
                                   size_t reply_size, const struct ipc_msg *raw_msg);

/* ── Actor struct ─────────────────────────────────────────────────────────── */

/**
 * @brief Actor instance registered with the framework.
 *
 * Actors are normally declared with IPC_ACTOR_DEFINE(); application code should
 * not construct or mutate this structure directly.
 */
struct ipc_actor {
    /** Actor name, used for diagnostics and logging. */
    const char *name;

    /** Raw actor message entry point called by the port. */
    ipc_actor_handler_t handler;

    /** Optional hook called after port resources are initialized and before thread start. */
    ipc_actor_lifecycle_hook_t start_hook;

    /** Optional hook called before a framework stop request is passed to the port. */
    ipc_actor_lifecycle_hook_t stop_hook;

    /** Optional hook called by the default dispatcher for unhandled messages. */
    ipc_actor_unknown_handler_t unknown_handler;

    /** Supervision strategy used by ipc_actor_fail(). Defaults to IPC_SUPERVISE_NONE. */
    ipc_supervision_strategy_t supervision;

    /** Optional hook called before the supervision strategy is applied. */
    ipc_actor_failure_hook_t failure_hook;

    /** Stack, priority, and queue-depth configuration. */
    struct ipc_actor_cfg cfg;

    /** Opaque platform state owned by the active port implementation. */
    void *port;

    /** Internal linked-list pointer used by ipc_start_all_actors(). */
    struct ipc_actor *_next;
};

/**
 * @def IPC_ACTOR_DEFINE(actor_sym, actor_name, stack_sz, prio, qdepth, max_payload)
 * @brief Define a file-local actor for the active platform port.
 *
 * The active port supplies this macro. It creates a static actor object and any
 * required static port resources, then registers the actor for
 * ipc_start_all_actors().
 *
 * @def IPC_ACTOR_DEFINE_PUBLIC(actor_sym, actor_name, stack_sz, prio, qdepth, max_payload)
 * @brief Define an externally-linkable actor for the active platform port.
 *
 * Use this variant when other translation units intentionally need a direct
 * actor handle, for example with ipc_send_to(). Those files may declare
 * `extern struct ipc_actor actor_sym`.
 */

/**
 * @def IPC_ACTOR_HANDLE(actor_sym, MsgType, handler_fn)
 * @brief Register a typed handler for a message on an actor.
 *
 * Defines @p handler_fn with a typed `<MsgType>_payload_t` payload pointer and
 * registers it for @p actor_sym. Commands are routed to one actor; events are
 * delivered to each actor that registers a handler for the event type.
 *
 * @def IPC_START_HOOK(actor_sym, hook_fn)
 * @brief Define a hook called during ipc_start_all_actors() for @p actor_sym.
 *
 * @def IPC_STOP_HOOK(actor_sym, hook_fn)
 * @brief Define a hook called during ipc_stop_all() for @p actor_sym.
 *
 * @def IPC_UNKNOWN(actor_sym, hook_fn)
 * @brief Define a hook called when @p actor_sym receives an unhandled message.
 *
 * @def IPC_SUPERVISE(actor_sym, strategy)
 * @brief Set @p actor_sym's failure strategy used by ipc_actor_fail().
 *
 * @def IPC_FAIL_HOOK(actor_sym, hook_fn)
 * @brief Define a hook called when @p actor_sym reports failure.
 *
 * @def IPC_ACTOR_RESPONSE_HANDLE(actor_sym, RequestType, ReplyType, handler_fn)
 * @brief Define a typed callback for an ipc_ask() reply.
 */
#include <ipc_actor_define.h>

/* ── Message definition macros ──────────────────────────────────────────── */

/**
 * @def IPC_CMD_DEFINE_LOCAL(TypeName, fields)
 * @brief Define a command message type, payload type, and file-local descriptor.
 *
 * Creates `<TypeName>_payload_t` from @p fields and a static ipc_msg_desc_t
 * named @p TypeName. This form is suitable for messages used in one translation
 * unit.
 *
 * @param TypeName Message descriptor symbol and payload type prefix.
 * @param fields Struct body, for example `{ uint32_t value; }`.
 */
#define IPC_CMD_DEFINE_LOCAL(TypeName, ...)                    \
    typedef struct __VA_ARGS__ TypeName##_payload_t;           \
    static ipc_msg_desc_t TypeName __attribute__((unused)) = { \
        .id   = 0,                                             \
        .kind = IPC_CMD,                                       \
        .size = sizeof(TypeName##_payload_t),                  \
        .name = #TypeName,                                     \
    }

/**
 * @def IPC_CMD_DEFINE(TypeName)
 * @brief Define the extern command descriptor declared by IPC_CMD_DECLARE().
 */
#define IPC_CMD_DEFINE(TypeName)              \
    ipc_msg_desc_t TypeName = {               \
        .id   = 0,                            \
        .kind = IPC_CMD,                      \
        .size = sizeof(TypeName##_payload_t), \
        .name = #TypeName,                    \
    }

/**
 * @def IPC_CMD_DECLARE(TypeName, fields)
 * @brief Declare a command payload type and extern descriptor in a header.
 *
 * Pair with IPC_CMD_DEFINE(TypeName) in exactly one source file.
 */
#define IPC_CMD_DECLARE(TypeName, ...)               \
    typedef struct __VA_ARGS__ TypeName##_payload_t; \
    extern ipc_msg_desc_t TypeName

/**
 * @def IPC_EVENT_DEFINE_LOCAL(TypeName, fields)
 * @brief Define an event message type, payload type, and file-local descriptor.
 *
 * Creates `<TypeName>_payload_t` from @p fields and a static ipc_msg_desc_t
 * named @p TypeName. This form is suitable for messages used in one translation
 * unit.
 *
 * @param TypeName Message descriptor symbol and payload type prefix.
 * @param fields Struct body, for example `{ uint32_t value; }`.
 */
#define IPC_EVENT_DEFINE_LOCAL(TypeName, ...)                  \
    typedef struct __VA_ARGS__ TypeName##_payload_t;           \
    static ipc_msg_desc_t TypeName __attribute__((unused)) = { \
        .id   = 0,                                             \
        .kind = IPC_EVENT,                                     \
        .size = sizeof(TypeName##_payload_t),                  \
        .name = #TypeName,                                     \
    }

/**
 * @def IPC_EVENT_DEFINE(TypeName)
 * @brief Define the extern event descriptor declared by IPC_EVENT_DECLARE().
 */
#define IPC_EVENT_DEFINE(TypeName)            \
    ipc_msg_desc_t TypeName = {               \
        .id   = 0,                            \
        .kind = IPC_EVENT,                    \
        .size = sizeof(TypeName##_payload_t), \
        .name = #TypeName,                    \
    }

/**
 * @def IPC_EVENT_DECLARE(TypeName, fields)
 * @brief Declare an event payload type and extern descriptor in a header.
 *
 * Pair with IPC_EVENT_DEFINE(TypeName) in exactly one source file.
 */
#define IPC_EVENT_DECLARE(TypeName, ...)             \
    typedef struct __VA_ARGS__ TypeName##_payload_t; \
    extern ipc_msg_desc_t TypeName

/**
 * @def IPC_CMD_REPLY_DEFINE_LOCAL(RequestType, ReplyType, fields)
 * @brief Define a file-local reply payload and descriptor for an askable command.
 */
#define IPC_CMD_REPLY_DEFINE_LOCAL(RequestType, ReplyType, ...) \
    IPC_CMD_DEFINE_LOCAL(ReplyType, __VA_ARGS__);               \
    static ipc_msg_desc_t *RequestType##_reply_desc __attribute__((unused)) = &(ReplyType)

/**
 * @def IPC_CMD_REPLY_DECLARE(RequestType, ReplyType, fields)
 * @brief Declare a shared reply payload and descriptor for an askable command.
 *
 * Pair with IPC_CMD_REPLY_DEFINE(RequestType, ReplyType) in exactly one source
 * file.
 */
#define IPC_CMD_REPLY_DECLARE(RequestType, ReplyType, ...) \
    typedef struct __VA_ARGS__ ReplyType##_payload_t;      \
    extern ipc_msg_desc_t ReplyType;                       \
    extern ipc_msg_desc_t *RequestType##_reply_desc

/**
 * @def IPC_CMD_REPLY_DEFINE(RequestType, ReplyType)
 * @brief Define the extern reply descriptor declared by IPC_CMD_REPLY_DECLARE().
 */
#define IPC_CMD_REPLY_DEFINE(RequestType, ReplyType) \
    IPC_CMD_DEFINE(ReplyType);                       \
    ipc_msg_desc_t *RequestType##_reply_desc = &(ReplyType)

/* ── Handler dispatch ───────────────────────────────────────────────────── */

/**
 * @brief Dispatch a raw message to the typed handler registered for @p self.
 *
 * This is the default ipc_actor_handler_t used by IPC_ACTOR_DEFINE().
 *
 * @param self Actor receiving the message.
 * @param msg Raw message to dispatch.
 */
void ipc_dispatch_actor_handlers(struct ipc_actor *self, const struct ipc_msg *msg);

/* ── Send / publish macros ──────────────────────────────────────────────── */

/**
 * @def ipc_send(MsgType, payload)
 * @brief Send a command payload to the actor registered for @p MsgType.
 *
 * @param MsgType Command descriptor created with IPC_CMD_DEFINE().
 * @param payload Expression of type `<MsgType>_payload_t`.
 * @return 0 on success, or a negative errno-style value on failure.
 */
#define ipc_send(MsgType, payload) ipc_send_raw(&(MsgType), &(payload))

/**
 * @def ipc_send_after(MsgType, delay_ms, payload)
 * @brief Send a command payload after a delay.
 *
 * @param MsgType Command descriptor created with IPC_CMD_DEFINE().
 * @param delay_ms Delay in milliseconds before enqueueing the message.
 * @param payload Expression of type `<MsgType>_payload_t`.
 * @return 0 on success, or a negative errno-style value on failure.
 */
#define ipc_send_after(MsgType, delay_ms, payload) \
    ipc_send_after_raw(&(MsgType), (uint32_t) (delay_ms), &(payload))

/**
 * @def ipc_publish(MsgType, payload)
 * @brief Publish an event payload to all subscribed actors.
 *
 * @param MsgType Event descriptor created with IPC_EVENT_DEFINE().
 * @param payload Expression of type `<MsgType>_payload_t`.
 * @return 0 on success, or the first negative errno-style send failure.
 */
#define ipc_publish(MsgType, payload) ipc_publish_raw(&(MsgType), &(payload))

/**
 * @def ipc_send_to(actor, MsgType, payload)
 * @brief Directly enqueue a typed message to @p actor, bypassing route lookup.
 *
 * This is the O(1) direct-mailbox path and is safe for interrupt context when
 * the active port's direct send seam is ISR-safe. Valid only after
 * ipc_start_all_actors(). The descriptor ID must already be initialized, normally
 * by IPC_ACTOR_HANDLE() during static startup.
 *
 * @param actor Pointer to the target actor.
 * @param MsgType Message descriptor created with IPC_CMD_DEFINE() or IPC_EVENT_DEFINE().
 * @param payload Expression of type `<MsgType>_payload_t`.
 * @return 0 on success, or a negative errno-style value on failure.
 */
#define ipc_send_to(actor, MsgType, payload) ipc_send_to_raw((actor), &(MsgType), &(payload))

/**
 * @note ipc_publish() fans out by scanning subscriptions and enqueueing to each
 * subscriber, so it is not intended for interrupt context. For ISR-originated
 * work, use ipc_send_to() to post O(1) to a driver/local actor, then call
 * ipc_publish() from that actor's thread context if fan-out is needed.
 */
/**
 * @def ipc_ask(self, ReqType, callback)
 * @brief Asynchronously send an empty ask request and invoke @p callback on reply.
 */
#define ipc_ask(self, ReqType, callback) \
    ipc_ask_raw((self), &(ReqType), NULL, ReqType##_reply_desc, (ipc_ask_callback_t) (callback))

/**
 * @def ipc_ask_with(self, ReqType, payload, callback)
 * @brief Asynchronously send an ask request payload and invoke @p callback on reply.
 */
#define ipc_ask_with(self, ReqType, payload, callback)                \
    ipc_ask_raw((self), &(ReqType), &(payload), ReqType##_reply_desc, \
                (ipc_ask_callback_t) (callback))

/**
 * @def ipc_ask_id(self, ReqType, callback, ask_id_out)
 * @brief Asynchronously send an empty ask request and return its correlation ID.
 */
#define ipc_ask_id(self, ReqType, callback, ask_id_out)                 \
    ipc_ask_with_id_raw((self), &(ReqType), NULL, ReqType##_reply_desc, \
                        (ipc_ask_callback_t) (callback), (ask_id_out))

/**
 * @def ipc_ask_with_id(self, ReqType, payload, callback, ask_id_out)
 * @brief Asynchronously send an ask request payload and return its correlation ID.
 */
#define ipc_ask_with_id(self, ReqType, payload, callback, ask_id_out)         \
    ipc_ask_with_id_raw((self), &(ReqType), &(payload), ReqType##_reply_desc, \
                        (ipc_ask_callback_t) (callback), (ask_id_out))

/**
 * @def ipc_reply(request_msg, ReplyType, payload)
 * @brief Reply to an ask request from inside the request handler.
 */
#define ipc_reply(request_msg, ReplyType, payload) \
    ipc_reply_raw((request_msg), &(ReplyType), &(payload))

/* ── Raw API ─────────────────────────────────────────────────────────────── */

/**
 * @brief Send a raw payload using a message descriptor.
 *
 * Most application code should prefer ipc_send().
 *
 * @param desc Command descriptor. Its kind must be IPC_CMD.
 * @param payload Pointer to payload bytes, or NULL for an empty payload.
 * @return 0 on success, -ENOENT if no actor is registered for @p desc, or a
 *         negative errno-style value from the active port.
 */
int ipc_send_raw(ipc_msg_desc_t *desc, const void *payload);

/**
 * @brief Send a raw payload after a delay.
 *
 * Most application code should prefer ipc_send_after().
 *
 * @param desc Command descriptor. Its kind must be IPC_CMD.
 * @param delay_ms Delay in milliseconds before enqueueing the message.
 * @param payload Pointer to payload bytes, or NULL for an empty payload.
 * @return 0 on success, -ENOENT if no actor is registered for @p desc, or a
 *         negative errno-style value from the active port.
 */
int ipc_send_after_raw(ipc_msg_desc_t *desc, uint32_t delay_ms, const void *payload);

/**
 * @brief Publish a raw event payload to all subscribed actors.
 *
 * Most application code should prefer ipc_publish().
 *
 * @param desc Event descriptor. Its kind must be IPC_EVENT.
 * @param payload Pointer to payload bytes, or NULL for an empty payload.
 * @return 0 on success, or the first negative errno-style send failure.
 */
int ipc_publish_raw(ipc_msg_desc_t *desc, const void *payload);

/**
 * @brief Directly enqueue a raw message to an actor, bypassing route lookup.
 *
 * This is the O(1) direct-mailbox path and may be used from interrupt context
 * when the active port's direct send seam is ISR-safe. The descriptor must
 * already have a non-zero ID; this function does not lazily initialize
 * descriptors.
 *
 * Most application code should prefer ipc_send() / ipc_publish() unless it
 * intentionally needs a direct actor handle.
 *
 * @param actor Target actor.
 * @param desc Message descriptor. Its kind and ID are preserved in the queued message.
 * @param payload Pointer to payload bytes, or NULL for an empty payload.
 * @return 0 on success, -EINVAL for invalid arguments or uninitialized descriptor
 *         IDs, -EPERM before actor startup, -EMSGSIZE if the payload does not fit
 *         @p actor, or a negative errno-style value from the port.
 */
int ipc_send_to_raw(struct ipc_actor *actor, const ipc_msg_desc_t *desc, const void *payload);

/**
 * @brief Asynchronously send a request and register a callback for its reply.
 */
int ipc_ask_raw(struct ipc_actor *self, ipc_msg_desc_t *request_desc, const void *request_payload,
                ipc_msg_desc_t *reply_desc, ipc_ask_callback_t callback);

/**
 * @brief Asynchronously send a request and return its correlation ID.
 */
int ipc_ask_with_id_raw(struct ipc_actor *self, ipc_msg_desc_t *request_desc,
                        const void *request_payload, ipc_msg_desc_t *reply_desc,
                        ipc_ask_callback_t callback, uint32_t *ask_id_out);

/**
 * @brief Cancel a pending ask by correlation ID.
 */
int ipc_ask_cancel(const struct ipc_actor *self, uint32_t ask_id);

/**
 * @brief Send a reply to an ask request.
 */
int ipc_reply_raw(const struct ipc_msg *request_msg, ipc_msg_desc_t *reply_desc,
                  const void *reply_payload);

/* ── Actor lifecycle ────────────────────────────────────────────────────── */

/**
 * @brief Initialize and start all statically declared actors.
 *
 * Starts every actor registered by IPC_ACTOR_DEFINE(). Call this after all
 * static actor/message declarations are available and before normal sending.
 *
 * @return 0 on success, or a negative errno-style value from the active port.
 */
int ipc_start_all_actors(void);

/**
 * @brief Block until actor threads have finished running.
 *
 * On POSIX this joins actor threads. Calling this without first arranging for
 * actors to exit, typically via ipc_stop_all(), can block forever. On Zephyr
 * this is a no-op because the kernel owns scheduling.
 *
 * @return 0 on success, or a negative errno-style value from the active port.
 */
int ipc_run_all(void);

/**
 * @brief Request all actor threads to stop.
 *
 * On POSIX this signals actor threads to exit but does not join them; call
 * ipc_run_all() afterwards to wait for cleanup. Port behavior may differ on
 * non-POSIX targets.
 */
void ipc_stop_all(void);

/**
 * @brief Report that an actor failed and apply its supervision strategy.
 *
 * Calls the actor's failure hook, if registered. Then applies the actor's
 * IPC_SUPERVISE() strategy: none, stop, or restart.
 *
 * @param self Failed actor.
 * @param reason Negative errno-style failure reason, or any application code.
 * @return 0 on success, -EINVAL for NULL @p self, or a negative port error.
 */
int ipc_actor_fail(struct ipc_actor *self, int reason);

#ifdef __cplusplus
}
#endif
