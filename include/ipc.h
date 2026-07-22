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

/* ── Configuration ─────────────────────────────────────────────────────── */

/**
 * @def IPC_PAYLOAD_SIZE
 * @brief Maximum inline payload size, in bytes, for every ipc_msg.
 *
 * Consumers may override it per translation unit by defining it before
 * including <ipc.h>, or globally with a compiler definition. Zephyr users may
 * set CONFIG_ACTOR_PAYLOAD_SIZE; the port-side ipc_config.h maps it to
 * IPC_PAYLOAD_SIZE before the public default is used.
 */

/* ── Message kinds ──────────────────────────────────────────────────────── */

/** @brief Message delivery kind. */
typedef enum {
    /** Event message, published to all subscribed actors. */
    IPC_EVENT = 0,

    /** Command message, routed to exactly one registered actor. */
    IPC_CMD,
} ipc_msg_kind_t;

/* ── Message descriptor ───────────────────────────────────────────────── */

/**
 * @brief Runtime descriptor for a statically declared message type.
 *
 * Message descriptors are created by IPC_CMD_DEFINE() and IPC_EVENT_DEFINE().
 * The descriptor is intentionally not const: `.id` starts at 0 and is filled
 * lazily from `.name` on first registration or send.
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

/** @brief Raw message stored in actor queues and passed to actor handlers. */
struct ipc_msg {
    /** Message ID matching ipc_msg_desc_t::id. */
    uint32_t id;

    /** Message delivery kind. */
    ipc_msg_kind_t kind;

    /** Inline payload storage. Only the descriptor's size bytes are valid. */
    uint8_t payload[IPC_PAYLOAD_SIZE];
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

    /** Stack, priority, and queue-depth configuration. */
    struct ipc_actor_cfg cfg;

    /** Opaque platform state owned by the active port implementation. */
    void *port;

    /** Internal linked-list pointer used by ipc_start_all_actors(). */
    struct ipc_actor *_next;
};

/**
 * @def IPC_ACTOR_DEFINE(actor_sym, actor_name, stack_sz, prio, qdepth)
 * @brief Statically declare an actor for the active platform port.
 *
 * The active port supplies this macro. It creates the actor object and any
 * required static port resources, then registers the actor for
 * ipc_start_all_actors().
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
 */
#include <ipc_actor_define.h>

/* ── Message definition macros ──────────────────────────────────────────── */

/**
 * @def IPC_CMD_DEFINE(TypeName, fields)
 * @brief Define a command message type and its payload structure.
 *
 * Creates `<TypeName>_payload_t` from @p fields and a static ipc_msg_desc_t
 * named @p TypeName. The payload must fit in IPC_PAYLOAD_SIZE.
 *
 * @param TypeName Message descriptor symbol and payload type prefix.
 * @param fields Struct body, for example `{ uint32_t value; }`.
 */
#define IPC_CMD_DEFINE(TypeName, ...)                                 \
    typedef struct __VA_ARGS__ TypeName##_payload_t;                  \
    static_assert(sizeof(TypeName##_payload_t) <= IPC_PAYLOAD_SIZE,   \
                  #TypeName " CMD payload exceeds IPC_PAYLOAD_SIZE"); \
    static ipc_msg_desc_t TypeName __attribute__((unused)) = {        \
        .id   = 0,                                                    \
        .kind = IPC_CMD,                                              \
        .size = sizeof(TypeName##_payload_t),                         \
        .name = #TypeName,                                            \
    }

/**
 * @def IPC_EVENT_DEFINE(TypeName, fields)
 * @brief Define an event message type and its payload structure.
 *
 * Creates `<TypeName>_payload_t` from @p fields and a static ipc_msg_desc_t
 * named @p TypeName. The payload must fit in IPC_PAYLOAD_SIZE.
 *
 * @param TypeName Message descriptor symbol and payload type prefix.
 * @param fields Struct body, for example `{ uint32_t value; }`.
 */
#define IPC_EVENT_DEFINE(TypeName, ...)                                 \
    typedef struct __VA_ARGS__ TypeName##_payload_t;                    \
    static_assert(sizeof(TypeName##_payload_t) <= IPC_PAYLOAD_SIZE,     \
                  #TypeName " EVENT payload exceeds IPC_PAYLOAD_SIZE"); \
    static ipc_msg_desc_t TypeName __attribute__((unused)) = {          \
        .id   = 0,                                                      \
        .kind = IPC_EVENT,                                              \
        .size = sizeof(TypeName##_payload_t),                           \
        .name = #TypeName,                                              \
    }

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
 * @def IPC_ISR_PUBLISH(MsgType, payload)
 * @brief Interrupt-context-safe event publish.
 *
 * Valid only after ipc_start_all_actors() and only for event descriptors that
 * already have static handlers registered.
 *
 * @param MsgType Event descriptor created with IPC_EVENT_DEFINE().
 * @param payload Expression of type `<MsgType>_payload_t`.
 * @return 0 on success, or a negative errno-style value on failure.
 */
#define IPC_ISR_PUBLISH(MsgType, payload) ipc_publish_isr_raw(&(MsgType), &(payload))

/**
 * @def ipc_publish_isr(MsgType, payload)
 * @brief Alias for IPC_ISR_PUBLISH().
 */
#define ipc_publish_isr(MsgType, payload) IPC_ISR_PUBLISH(MsgType, payload)

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
 * @brief Interrupt-context-safe raw event publish.
 *
 * Uses the active port's ISR-safe send seam. The descriptor must already have a
 * non-zero ID, which normally means the event has been registered by a static
 * IPC_ACTOR_HANDLE() before startup completed.
 *
 * @param desc Event descriptor. Its kind must be IPC_EVENT.
 * @param payload Pointer to payload bytes, or NULL for an empty payload.
 * @return 0 on success, -EINVAL for invalid descriptors, -EPERM before actor
 *         startup, or a negative errno-style value from the active port.
 */
int ipc_publish_isr_raw(const ipc_msg_desc_t *desc, const void *payload);

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

#ifdef __cplusplus
}
#endif
