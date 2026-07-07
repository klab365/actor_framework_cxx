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
#include "ipc_defaults.h"

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
 * All registration happens single-threaded before ipc_run_all().
 * ─────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t       id; /* lazily filled; 0 until first use */
    ipc_msg_kind_t kind;
    size_t         size;
    const char    *name;
} ipc_msg_desc_t;

/* Forward declarations */
struct ipc_actor;

/* ── Wire message ──────────────────────────────────────────────────────── */

struct ipc_msg {
    uint32_t       id;
    ipc_msg_kind_t kind;
    uint8_t        payload[IPC_PAYLOAD_SIZE];
    void          *_wait; /* internal (QUERY only) */
};

/* ── Actor config ────────────────────────────────────────────────────────── */

struct ipc_actor_cfg {
    size_t stack_size;
    int    priority;
    size_t queue_depth;
};

typedef void (*ipc_actor_handler_t)(struct ipc_actor *self, const struct ipc_msg *msg);

typedef struct {
    uintptr_t _opaque[IPC_PORT_STATE_WORDS];
} ipc_port_state_t;

/* ── Actor struct ─────────────────────────────────────────────────────────── */

struct ipc_actor {
    const char          *name;
    ipc_actor_handler_t  handler;
    struct ipc_actor_cfg cfg;
    ipc_port_state_t     port; /* opaque platform state */
    struct ipc_actor    *_next;
};

/* ── Runtime FNV-1a hash lives in ipc_internal.h — descriptor IDs are
 *    computed lazily by ipc_register/ipc_subscribe on first use; users
 *    never need to call this directly. ─────────────────────────────────────── */

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
    typedef struct ReqFields  TypeName##_payload_t;                      \
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
        const struct ipc_msg      *__ipc_raw = (raw_msg);                    \
        const MsgType##_payload_t *__ipc_typed =                             \
            (const MsgType##_payload_t *) (const void *) __ipc_raw->payload; \
        handler_fn(self, __ipc_typed, __ipc_raw);                            \
    } else

/* ── Registration / subscription macros ─────────────────────────────────── */

#define IPC_REGISTER(actor, MsgType) ipc_register((actor), &(MsgType))
#define IPC_SUBSCRIBE(actor, MsgType) ipc_subscribe((actor), &(MsgType))
#define IPC_UNSUBSCRIBE(actor, MsgType) ipc_unsubscribe((actor), &(MsgType))

/* ── Send / publish / query macros ──────────────────────────────────────── */

#define ipc_send(MsgType, ...) ipc_send_raw(&(MsgType), &(MsgType##_payload_t) {__VA_ARGS__})

#define ipc_send_after(MsgType, delay_ms, ...) \
    ipc_send_after_raw(&(MsgType), (uint32_t) (delay_ms), &(MsgType##_payload_t) {__VA_ARGS__})

#define ipc_publish(MsgType, ...) ipc_publish_raw(&(MsgType), &(MsgType##_payload_t) {__VA_ARGS__})

#define ipc_query(MsgType, response_ptr, timeout, ...)                              \
    ipc_query_raw(&(MsgType), &(MsgType##_payload_t) {__VA_ARGS__}, (response_ptr), \
                  sizeof(MsgType##_response_t), (timeout))

/* ── Reply macro ────────────────────────────────────────────────────────── */

#define ipc_reply(raw_msg, MsgType, ...) \
    ipc_reply_raw((raw_msg), &(MsgType##_response_t) {__VA_ARGS__}, sizeof(MsgType##_response_t))

/* ── Raw API ─────────────────────────────────────────────────────────────── */

int  ipc_send_raw(ipc_msg_desc_t *desc, const void *payload);
int  ipc_send_after_raw(ipc_msg_desc_t *desc, uint32_t delay_ms, const void *payload);
int  ipc_publish_raw(ipc_msg_desc_t *desc, const void *payload);
int  ipc_query_raw(ipc_msg_desc_t *desc, const void *payload, void *response, size_t resp_size,
                   ipc_timeout_t timeout);
void ipc_reply_raw(const struct ipc_msg *msg, const void *response, size_t len);

/* ── Registration API ───────────────────────────────────────────────────── */

int  ipc_register(struct ipc_actor *actor, ipc_msg_desc_t *desc);

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
int  ipc_subscribe(struct ipc_actor *actor, ipc_msg_desc_t *desc);

/*
 * Remove a subscription created by ipc_subscribe. The (actor, MsgType)
 * pair must match an existing subscription; otherwise -ENOENT is
 * returned. Idempotency is the inverse of ipc_subscribe: a second
 * call after the first returns -ENOENT (the row is gone).
 */
int  ipc_unsubscribe(struct ipc_actor *actor, ipc_msg_desc_t *desc);

/* ── Actor lifecycle ────────────────────────────────────────────────────── */

int  ipc_actor_init(struct ipc_actor *actor, const char *name, ipc_actor_handler_t handler,
                    struct ipc_actor_cfg cfg);
int  ipc_start_all_threads(void);
int  ipc_run_all(void);
void ipc_stop_all(void);

#ifdef __cplusplus
}
#endif
