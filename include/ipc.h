/* ipc.h — Single public header for the IPC Actor Framework. */
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

/* IPC_PAYLOAD_SIZE controls inline message payload bytes. Consumers may
 * override it per translation unit (define before #include <ipc.h>)
 * or globally (-D on the command line). Zephyr users may set
 * CONFIG_ACTOR_PAYLOAD_SIZE; the port-side ipc_config.h maps it to
 * IPC_PAYLOAD_SIZE before the public default is consulted.
 */

/* ── Message kinds ──────────────────────────────────────────────────────── */

typedef enum {
    IPC_EVENT = 0,
    IPC_CMD,
} ipc_msg_kind_t;

/* ── Message descriptor ─────────────────────────────────────────────────
 *
 * NOTE: NOT const — the .id field is zero-initialized in the macro and
 * computed lazily from .name (FNV-1a) on static route setup or send.
 * Actors are declared statically with IPC_ACTOR_DEFINE(); registration
 * happens during startup before ipc_start_all_actors().
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

typedef void (*ipc_actor_msg_handler_t)(struct ipc_actor *self, const void *payload,
                                        const struct ipc_msg *raw_msg);

struct ipc_actor_handler_entry {
    ipc_msg_desc_t *desc;
    ipc_actor_msg_handler_t handler;
};

/* ── Actor struct ─────────────────────────────────────────────────────────── */

struct ipc_actor {
    /** Actor name (for logging) */
    const char *name;
    /** Actor message handler */
    ipc_actor_handler_t handler;
    /** Stack size, priority, queue depth */
    struct ipc_actor_cfg cfg;
    /** Optional static per-message handler table. */
    const struct ipc_actor_handler_entry *handlers;
    size_t handler_count;
    /** Opaque platform state owned by the active port implementation. */
    void *port;
    /** Linked list of all actors, for ipc_start_all_actors() */
    struct ipc_actor *_next;
};

#include <ipc_actor_define.h>

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

#define IPC_ON(MsgType, handler_fn) \
    {.desc = &(MsgType), .handler = (ipc_actor_msg_handler_t) (handler_fn)}

#define IPC_ACTOR_RAW_HANDLER(handler_fn) \
    .handler = (handler_fn), .handlers = NULL, .handler_count = 0

#define IPC_ACTOR_HANDLERS(handlers_arr)                                \
    .handler = ipc_dispatch_actor_handlers, .handlers = (handlers_arr), \
    .handler_count = sizeof(handlers_arr) / sizeof((handlers_arr)[0])

void ipc_dispatch_actor_handlers(struct ipc_actor *self, const struct ipc_msg *msg);

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

/*
 * IPC_UNKNOWN({ ... });
 * IPC_DISPATCH_IGNORE_UNKNOWN();
 *
 * Terminate an IPC_DISPATCH_TO chain. IPC_UNKNOWN runs caller-provided
 * statements for unmatched message IDs; IPC_DISPATCH_IGNORE_UNKNOWN drops
 * unmatched messages intentionally.
 */
#define IPC_UNKNOWN(...) __VA_ARGS__
#define IPC_DISPATCH_IGNORE_UNKNOWN() \
    {                                 \
    }

/* ── Send / publish macros ──────────────────────────────────────────────── */

/*
 * All send-style macros take the payload as a single, fully-typed
 * expression. The user writes a compound literal of the descriptor's
 * payload type, e.g.:
 *
 *     ipc_send(LedOn, (LedOn_payload_t){.brightness = 50});
 *     ipc_publish(LedFault, (LedFault_payload_t){.error_code = 0xDEAD, .channel = 1});
 * The macro takes the address of the user-supplied expression, so named
 * variables are equally fine:
 *
 *     LedOn_payload_t p = {.brightness = 50};
 *     ipc_send(LedOn, p);
 *
 * Internally the macro calls the corresponding *_raw function. The
 * `MsgType` symbol is required because the macro needs the descriptor
 * (for the wire id/kind/size).
 */
#define ipc_send(MsgType, payload) ipc_send_raw(&(MsgType), &(payload))

#define ipc_send_after(MsgType, delay_ms, payload) \
    ipc_send_after_raw(&(MsgType), (uint32_t) (delay_ms), &(payload))

#define ipc_publish(MsgType, payload) ipc_publish_raw(&(MsgType), &(payload))

/*
 * Interrupt-context-safe event publish. Valid only after ipc_start_all_actors()
 * and only for EVENT descriptors already present in a static handler table.
 */
#define IPC_ISR_PUBLISH(MsgType, payload) ipc_publish_isr_raw(&(MsgType), &(payload))
#define ipc_publish_isr(MsgType, payload) IPC_ISR_PUBLISH(MsgType, payload)

/* ── Raw API ─────────────────────────────────────────────────────────────── */

/** Send raw messages */
int ipc_send_raw(ipc_msg_desc_t *desc, const void *payload);

/** Send a message after a delay */
int ipc_send_after_raw(ipc_msg_desc_t *desc, uint32_t delay_ms, const void *payload);

/** Publish a message to all subscribed actors */
int ipc_publish_raw(ipc_msg_desc_t *desc, const void *payload);

/** Interrupt-context-safe publish variant; uses the port ISR send seam. */
int ipc_publish_isr_raw(const ipc_msg_desc_t *desc, const void *payload);

/* ── Actor lifecycle ────────────────────────────────────────────────────── */

/** Initialize/start all statically defined actors */
int ipc_start_all_actors(void);

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

/* ── Static actor declaration ──────────────────────────────────────────────
 *
 * IPC_ACTOR_DEFINE is selected by the active port's include directory.
 * It fully declares an actor statically. Runtime startup is a single
 * call to ipc_start_all_actors():
 *
 *   IPC_ACTOR_DEFINE(my_actor, "my_actor", 1024, 5, 4,
 *                    IPC_ACTOR_RAW_HANDLER(my_handler));
 *   int rc = ipc_start_all_actors();
 */
