/* ipc.h — Single public header for the IPC Actor Framework. */
#pragma once

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Timeout API is platform-agnostic: milliseconds + FOREVER sentinel. */
typedef uint32_t ipc_timeout_t;
#define IPC_TIMEOUT_MS(ms) ((ipc_timeout_t) (ms))
#define IPC_TIMEOUT_FOREVER ((ipc_timeout_t) UINT32_MAX)

/* ── Configuration ─────────────────────────────────────────────────────── */

/* Tunable sizing constants.  Every IPC_* macro used in the public type
 * definitions below comes from ipc_defaults.h.  Consumers may override
 * any of them per-translation-unit (define before #include <ipc.h>) or
 * globally (-D on the command line).  Zephyr users should set the
 * matching CONFIG_ACTOR_* symbol in Kconfig; the port-side overlay at
 * src/port/zephyr/ipc_config.h propagates those values into the IPC_*
 * macros before the public defaults are consulted.  See the docstring
 * at the top of ipc_defaults.h for the full override precedence list.
 */
#ifdef __ZEPHYR__
#include "ipc_config.h"
#else
#include "ipc_defaults.h"
#endif

/* ── Message kinds ──────────────────────────────────────────────────────── */

typedef enum {
    IPC_EVENT = 0,
    IPC_CMD,
    IPC_QUERY,
} ipc_msg_kind_t;

/* ── Message descriptor ─────────────────────────────────────────────────
 *
 * NOTE: NOT const — the .id field is zero-initialized in the macro and
 * computed lazily from .name (FNV-1a) on first register/subscribe/send.
 * All registration happens single-threaded during the module's
 * ipc_actor_init() call (which also spawns the actor's thread).
 * ─────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t id; /* lazily filled; 0 until first use */
    ipc_msg_kind_t kind;
    size_t size;
    const char *name;
} ipc_msg_desc_t;

/* Forward declarations */
struct ipc_actor;

/* ── Wire message ──────────────────────────────────────────────────────── */

struct ipc_msg {
    /** Message ID */
    uint32_t id;
    /** Message kind */
    ipc_msg_kind_t kind;
    /** Message payload */
    uint8_t payload[IPC_PAYLOAD_SIZE];
    /** Internal use (QUERY only) */
    void *_wait;
};

/* ── Actor config ────────────────────────────────────────────────────────── */

struct ipc_actor_cfg {
    /** Stack size for the actor's thread */
    size_t stack_size;

    /* Priority for the actor's thread */
    int priority;

    /** Depth of the actor's message queue */
    size_t queue_depth;
};

/**
 * IPC actor handler function type.
 */
typedef void (*ipc_actor_handler_t)(struct ipc_actor *self, const struct ipc_msg *msg);

typedef struct {
    /* Opaque platform-specific state for the actor's port implementation. */
    uintptr_t _opaque[IPC_PORT_STATE_WORDS];
} ipc_port_state_t;

/* ── Actor struct ─────────────────────────────────────────────────────────── */

struct ipc_actor {
    /** Actor name (for logging) */
    const char *name;
    /** Actor message handler */
    ipc_actor_handler_t handler;
    /** Stack size, priority, queue depth */
    struct ipc_actor_cfg cfg;
    /** Opaque platform state */
    ipc_port_state_t port;
    /** Linked list of all actors, for ipc_start_all_threads() */
    struct ipc_actor *_next;
};

/* ── Message definition macros ──────────────────────────────────────────── */

/*
 * IPC_CMD_DEFINE(TypeName, { fields... })
 *
 * The .id field is initialised to 0 and filled in lazily on first
 * register/send via _ipc_ensure_id().  All other fields are const-correct
 * at compile time.
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

#define IPC_QUERY_DEFINE(TypeName, ReqFields, RespFields)                \
    typedef struct ReqFields TypeName##_payload_t;                       \
    typedef struct RespFields TypeName##_response_t;                     \
    static_assert(sizeof(TypeName##_payload_t) <= IPC_PAYLOAD_SIZE,      \
                  #TypeName " QUERY request exceeds IPC_PAYLOAD_SIZE");  \
    static_assert(sizeof(TypeName##_response_t) <= IPC_PAYLOAD_SIZE,     \
                  #TypeName " QUERY response exceeds IPC_PAYLOAD_SIZE"); \
    static ipc_msg_desc_t TypeName __attribute__((unused)) = {           \
        .id   = 0,                                                       \
        .kind = IPC_QUERY,                                               \
        .size = sizeof(TypeName##_payload_t),                            \
        .name = #TypeName,                                               \
    }

/**
 * IPC_EVENT_DEFINE(TypeName, { fields... })
 *
 * Defines a new IPC event type with the given name and payload fields.
 * The payload fields are specified as a struct definition in the second argument.
 * The .id field is initialized to 0 and filled in lazily on first register/send
 * via _ipc_ensure_id(). All other fields are const-correct at compile time.
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

/* ── Handler macros ─────────────────────────────────────────────────────── */

/*
 * IPC_HANDLE(MsgType, handler_fn)
 *
 * Defines an explicit typed handler function with this signature:
 *   static void handler_fn(struct ipc_actor *self,
 *                          const MsgType##_payload_t *msg,
 *                          const struct ipc_msg *raw_msg)
 */
#define IPC_HANDLE(MsgType, handler_fn)                                            \
    static void handler_fn(struct ipc_actor *self, const MsgType##_payload_t *msg, \
                           const struct ipc_msg *raw_msg)

/*
 * IPC_DISPATCH_TO(raw_msg, MsgType, handler_fn)
 *
 * In actor dispatch chains, casts payload to MsgType##_payload_t and
 * calls the provided handler function if IDs match.
 */
#define IPC_DISPATCH_TO(raw_msg, MsgType, handler_fn)                        \
    if ((raw_msg)->id == (MsgType).id) {                                     \
        const struct ipc_msg *__ipc_raw = (raw_msg);                         \
        const MsgType##_payload_t *__ipc_typed =                             \
            (const MsgType##_payload_t *) (const void *) __ipc_raw->payload; \
        handler_fn(self, __ipc_typed, __ipc_raw);                            \
    } else

/* ── Registration / subscription macros ─────────────────────────────────── */

#define IPC_REGISTER(actor, MsgType) ipc_register((actor), &(MsgType))
#define IPC_SUBSCRIBE(actor, MsgType) ipc_subscribe((actor), &(MsgType))
#define IPC_UNSUBSCRIBE(actor, MsgType) ipc_unsubscribe((actor), &(MsgType))

/* ── Send / publish / query macros ──────────────────────────────────────── */

/*
 * All send-style macros take the payload as a single, fully-typed
 * expression. The user writes a compound literal of the descriptor's
 * payload type, e.g.:
 *
 *     ipc_send(LedOn, (LedOn_payload_t){.brightness = 50});
 *     ipc_publish(LedFault, (LedFault_payload_t){.error_code = 0xDEAD, .channel = 1});
 *     ipc_query(GetLedState, &state, IPC_TIMEOUT_MS(100),
 *               (GetLedState_payload_t){.channel = 0});
 *     ipc_reply(raw_msg, GetLedState,
 *               (GetLedState_response_t){.on = 1, .brightness = 80, .on_time_ms = 12345});
 *
 * The macro takes the address of the user-supplied expression, so named
 * variables are equally fine:
 *
 *     LedOn_payload_t p = {.brightness = 50};
 *     ipc_send(LedOn, p);
 *
 * Internally the macro calls the corresponding *_raw function. The
 * `MsgType` symbol is required because the macro needs both the
 * descriptor (for the wire id/kind/size) and, for query/reply, the
 * response type's size.
 */
#define ipc_send(MsgType, payload) ipc_send_raw(&(MsgType), &(payload))

#define ipc_send_after(MsgType, delay_ms, payload) \
    ipc_send_after_raw(&(MsgType), (uint32_t) (delay_ms), &(payload))

#define ipc_publish(MsgType, payload) ipc_publish_raw(&(MsgType), &(payload))

#define ipc_query(MsgType, response_ptr, timeout, payload) \
    ipc_query_raw(&(MsgType), &(payload), (response_ptr), sizeof(MsgType##_response_t), (timeout))

/* ── Reply macro ────────────────────────────────────────────────────────── */

#define ipc_reply(raw_msg, MsgType, response) \
    ipc_reply_raw((raw_msg), &(response), sizeof(MsgType##_response_t))

/* ── Raw API ─────────────────────────────────────────────────────────────── */

/** Send raw messages */
int ipc_send_raw(ipc_msg_desc_t *desc, const void *payload);

/** Send a message after a delay */
int ipc_send_after_raw(ipc_msg_desc_t *desc, uint32_t delay_ms, const void *payload);

/** Publish a message to all subscribed actors */
int ipc_publish_raw(ipc_msg_desc_t *desc, const void *payload);

/** Query an actor and wait for a response */
int ipc_query_raw(ipc_msg_desc_t *desc, const void *payload, void *response, size_t resp_size,
                  ipc_timeout_t timeout);

/** Reply to a query */
void ipc_reply_raw(const struct ipc_msg *msg, const void *response, size_t len);

/* ── Registration API ───────────────────────────────────────────────────── */

/** Register an actor to handle a specific message descriptor. */
int ipc_register(struct ipc_actor *actor, ipc_msg_desc_t *desc);

/*
 * Subscribe an actor to an EVENT descriptor. Events are delivered via
 * ipc_publish(): every actor subscribed to the event receives a copy
 * of the published message in its own inbox.
 *
 * - `desc->kind` must be IPC_EVENT; subscribing a CMD/QUERY descriptor
 *   is a programming error and asserts in debug builds. Use
 *   ipc_register for CMDs and QUERIES.
 * - `ipc_subscribe` is idempotent for the same (actor, MsgType) pair:
 *   a second call with the same pair is a silent no-op and returns 0
 *   without adding a duplicate row. (Without this, a single
 *   ipc_publish would deliver twice to the same actor.)
 * - Returns 0 on success, -ENOMEM if the subscription table is full.
 *   Duplicate subscriptions do not fail.
 */
int ipc_subscribe(struct ipc_actor *actor, ipc_msg_desc_t *desc);

/*
 * Remove a subscription created by ipc_subscribe. The (actor, MsgType)
 * pair must match an existing subscription; otherwise -ENOENT is
 * returned. Idempotency is the inverse of ipc_subscribe: a second
 * call after the first returns -ENOENT (the row is gone).
 */
int ipc_unsubscribe(struct ipc_actor *actor, ipc_msg_desc_t *desc);

/* ── Actor lifecycle ────────────────────────────────────────────────────── */

/** Initialize an actor */
int ipc_actor_init(struct ipc_actor *actor, const char *name, ipc_actor_handler_t handler,
                   struct ipc_actor_cfg cfg);

/** Start all actor threads */
int ipc_start_all_threads(void);

/*
 * Block until every actor has exited. On POSIX this is the join phase
 * — ipc_stop_all() signals each actor's thread to exit and returns
 * immediately; ipc_run_all() then joins them so the program doesn't
 * fall out of main() and kill still-running pthreads. On Zephyr the
 * kernel keeps scheduling until the app calls exit() (or the last
 * thread returns), so ipc_run_all() is a no-op there — it just
 * returns 0 without joining.
 *
 * Calling ipc_run_all() without first calling ipc_stop_all() will
 * block forever on POSIX; that's intentional — it gives the caller
 * a single, well-defined "I am done, let the framework clean up"
 * point. If you want a "stop the world and clean up" sequence, call
 * ipc_stop_all() then ipc_run_all(). If you want a manual "I've
 * already signalled shutdown, just wait for threads to drain" flow,
 * you can call ipc_run_all() directly — but you must have first
 * arranged for each actor's thread to exit (typically by sending
 * a SHUTDOWN command that the actor's handler turns into a return).
 */
int ipc_run_all(void);

/** Stop all actor threads */
void ipc_stop_all(void);

#ifdef __cplusplus
}
#endif
