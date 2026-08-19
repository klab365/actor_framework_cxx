# IPC Actor Framework

A small **actor-model IPC framework** written in **C11** (with a **C++17** test suite).

Each actor owns its own thread and its own message inbox. Actors register typed
handlers by message type; the framework routes commands and events by message
ID, so callers never need a direct reference to the actor they're talking to.
The core is platform-agnostic: POSIX (pthreads) and Zephyr (`k_msgq` /
`k_thread`) both sit behind a single port interface.

**Design principles:**
- 📄 One public header: `<ipc.h>` — that's the entire API surface.
- 🔒 No `extern struct ipc_actor` — actors are always file-local. Routed sends
  use typed message descriptors looked up by ID; `ipc_send_to()` provides an
  O(1) direct path from the same file (for example an ISR).
- 🔍 No linker scripts, no central registry file — actors are discovered by
  name at runtime via a linked list.
- 🚫 No heap allocation in the core.

---

## Table of contents

- [Repository layout](#repository-layout)
- [Quick start](#quick-start)
  1. [Define a message type](#1-define-a-message-type)
  2. [Define an actor and its typed handlers](#2-define-an-actor-and-its-typed-handlers)
  3. [Optional actor hooks and supervision](#3-optional-actor-hooks-and-supervision)
  4. [Send messages (no `extern` actor handles)](#4-send-messages-no-extern-actor-handles)
  5. [Run the framework](#5-run-the-framework)
- [Error codes](#error-codes)
- [Configuration](#configuration)
- [Building](#building)
  - [Zephyr](#zephyr)
- [Distribution / consumption paths](#distribution--consumption-paths)
- [Design notes](#design-notes)
- [For contributors and AI coding agents](#for-contributors-and-ai-coding-agents)

---

## Repository layout

```
ipc/
├── include/
│   └── ipc.h              ← ONLY public header (whole API surface)
├── src/
│   ├── ipc.c              ← platform-agnostic core
│   ├── ipc_internal.h     ← private: FNV-1a, test reset hook
│   ├── ipc_port.h         ← generic port interface
│   └── port/
│       ├── posix/posix_ipc_port.c
│       └── zephyr/
│           ├── zephyr_ipc_port.c
│           ├── ipc_config.h     ← Zephyr port config shim
│           └── Kconfig
├── tests/
│   ├── unit/              ← gtest; links ipc.c + mock_ipc_port (no threads)
│   └── (future test work)
├── docs/                  ← design notes (timer wheel, future topics)
├── examples/
│   ├── led_actor/         ← LED + app + button actors (POSIX runnable)
│   └── basic_zephyr/      ← minimal native_sim-friendly Zephyr app
├── zephyr/                ← Zephyr module manifest, Kconfig, CMake glue
├── cmake/GTest.cmake      ← FetchContent-pinned googletest v1.15.2
├── CMakeLists.txt         ← top-level build (option-gated tests/examples)
├── CMakePresets.json
├── mise.toml
└── AGENTS.md              ← contributor / agent guidance
```

---

## Quick start

### 1. Define a message type

```c
#include <ipc.h>

IPC_CMD_DEFINE_LOCAL(LedOn, { uint8_t brightness; });
```

`IPC_CMD_DEFINE_LOCAL(LedOn, fields)` declares an `LedOn_payload_t` typedef for
wire payloads, plus a file-local `LedOn` message descriptor (its `.id` is
lazily FNV-1a-hashed during handler registration, or on first normal send).
Use this two-argument form for messages that are only used within one
translation unit:

```c
/* button_actor.c */
IPC_CMD_DEFINE_LOCAL(ButtonIrq, { uint8_t pin; });

IPC_ACTOR_DEFINE(button_actor, "button", 1024, 5, 8, IPC_MESSAGE_MAX(ButtonIrq));

IPC_ACTOR_HANDLE(button_actor, ButtonIrq, on_button_irq)
{
    /* ButtonIrq.id has been initialized by handler registration. */
}

void gpio_isr_callback(void)
{
    ButtonIrq_payload_t payload = {.pin = 3};
    ipc_send_to(&button_actor, ButtonIrq, payload); /* O(1) direct post */
}
```

For messages shared across multiple files, put `DECLARE` in a header and the
one-argument `DEFINE` in exactly one source file:

```c
/* messages.h */
IPC_CMD_DECLARE(LedOn, { uint8_t brightness; });
IPC_EVENT_DECLARE(LedFault, { uint32_t error_code; uint8_t channel; });

/* messages.c */
IPC_CMD_DEFINE(LedOn);
IPC_EVENT_DEFINE(LedFault);
```

Every other file includes `messages.h` and uses the same `extern` descriptor
object. The same pattern applies to events and command replies.

Async request/response is modeled as an askable command plus an associated
command reply:

```c
IPC_CMD_DEFINE_LOCAL(GetLedStateRequest, { uint8_t channel; });
IPC_CMD_REPLY_DEFINE_LOCAL(GetLedStateRequest, GetLedStateResponse, {
    uint8_t channel;
    uint8_t on;
    uint8_t brightness;
    uint32_t on_time_ms;
});

IPC_EVENT_DEFINE_LOCAL(LedFault, { uint32_t error_code; uint8_t channel; });
```

For shared ask/reply APIs, declare both the request and reply in the header:

```c
/* led.h */
IPC_CMD_DECLARE(GetLedStateRequest, { uint8_t channel; });
IPC_CMD_REPLY_DECLARE(GetLedStateRequest, GetLedStateResponse, {
    uint8_t channel;
    uint8_t on;
    uint8_t brightness;
    uint32_t on_time_ms;
});

/* led.c */
IPC_CMD_DEFINE(GetLedStateRequest);
IPC_CMD_REPLY_DEFINE(GetLedStateRequest, GetLedStateResponse);
```

The reply macros associate the reply descriptor with the request type for
`ipc_ask()` / `ipc_reply()`.

### 2. Define an actor and its typed handlers

```c
IPC_ACTOR_DEFINE(led_actor, "led", 512, 5, 8, IPC_MESSAGE_MAX(LedOn));

IPC_ACTOR_HANDLE(led_actor, LedOn, on_led_on)
{
    (void) self;
    (void) raw_msg;
    /* `msg` is `const LedOn_payload_t *` — typed, no cast */
    set_led(msg->brightness);
}
```

`IPC_ACTOR_HANDLE(actor, MsgType, handler_fn)` expands to a typed handler
function plus static routing metadata. CMD handlers become single-target
routes; EVENT handlers become fan-out subscriptions automatically.

### 3. Optional actor hooks and supervision

Actors may define lifecycle, unknown-message, and failure hooks:

```c
IPC_START_HOOK(led_actor, led_on_start) { reset_led_state(); }
IPC_STOP_HOOK(led_actor, led_on_stop) { cleanup_led_state(); }
IPC_UNKNOWN(led_actor, led_on_unknown) { log_unknown(msg->id); }
```

Supervision is actor-local and defaults to `IPC_SUPERVISE_NONE`:

```c
IPC_SUPERVISE(led_actor, IPC_SUPERVISE_RESTART);

IPC_FAIL_HOOK(led_actor, led_on_failure) {
    printf("led failed: %d\n", reason);
}

IPC_ACTOR_HANDLE(led_actor, LedOn, on_led_on) {
    if (driver_failed()) {
        ipc_actor_fail(self, -EIO);
        return;
    }
}
```

| Supervision mode        | Behavior on failure                                                                 |
|--------------------------|--------------------------------------------------------------------------------------|
| `IPC_SUPERVISE_NONE`     | Default — failure is reported via the fail hook only; the actor keeps running.       |
| `IPC_SUPERVISE_STOP`     | Requests the actor to stop.                                                          |
| `IPC_SUPERVISE_RESTART`  | Soft restart for message-driven actors: pending delayed work and queued messages are dropped, then stop/start hooks run so state can be reset. |

### 4. Send messages (no `extern` actor handles)

| Pattern                      | Targets                          | Use it for                                        |
|-------------------------------|-----------------------------------|----------------------------------------------------|
| `ipc_send` / `ipc_send_to`     | Exactly one actor (the registered command handler) | "Tell this actor to do something."       |
| `ipc_publish`                  | Every subscribed actor (fan-out)  | "Announce something happened."                     |
| `ipc_ask` / `ipc_ask_with`     | One actor, with a correlated async reply | "Ask an actor for a typed response."         |
| `ipc_send_after`               | One actor, after a delay          | "Do this later" (one pending slot per actor).       |

#### Command: tell one actor to do something

Commands are routed to exactly one actor: whichever actor registered a
handler for that command type.

```c
ipc_send(LedOn, (LedOn_payload_t){.brightness = 200});
```

To post O(1) straight into an actor's mailbox, keep the actor and the code
that targets it in the same file (for example an interrupt handler):

```c
/* button.c — actor and its ISR live in the same translation unit */
IPC_ACTOR_DEFINE(button_actor, "button", 1024, 5, 8,
                 IPC_MESSAGE_MAX(ButtonIrq));

void gpio_isr_callback(void)
{
    ButtonIrq_payload_t payload = {.pin = 3};
    ipc_send_to(&button_actor, ButtonIrq, payload);
}
```

Prefer routed `ipc_send()` for normal cross-actor traffic; use `ipc_send_to()`
for the explicit O(1) path from the same file, typically an ISR. Actors are
always file-local, so there is no `extern` actor handle across translation
units.

#### Event: announce something to all subscribers

Events are published to every actor that registered a handler for that event
type. Publishing still succeeds if there are no subscribers.

```c
ipc_publish(LedFault, (LedFault_payload_t){.error_code = 0xDEAD, .channel = 1});
```

#### Ask/reply: ask one actor for a typed async response

Ask/reply sends a command request to one actor and correlates the reply back
to the asking actor. The response callback runs in the asking actor's context.
The final argument is the reply timeout in milliseconds (`0` means no timeout);
if the reply does not arrive in time, the callback is invoked with
`result == -ETIMEDOUT`.

```c
IPC_ACTOR_HANDLE(led_actor, GetLedStateRequest, on_get_state_request) {
    (void)self;
    ipc_reply(raw_msg, GetLedStateResponse,
              (GetLedStateResponse_payload_t){.channel = msg->channel,
                                              .on = 1,
                                              .brightness = 80,
                                              .on_time_ms = 12345});
}

IPC_ACTOR_RESPONSE_HANDLE(app_actor, GetLedStateRequest, GetLedStateResponse, on_led_state) {
    (void)self; (void)raw_msg;
    if (result == 0) {
        printf("LED ch=%u on=%u brightness=%u\n", msg->channel, msg->on, msg->brightness);
    }
}

GetLedStateRequest_payload_t req = {.channel = 0};
ipc_ask_with(&app_actor, GetLedStateRequest, req, on_led_state, 1000);
```

If you need cancellation, keep the correlation ID returned by the `_id` variant:

```c
uint32_t ask_id;
int rc = ipc_ask_with_id(&app_actor, GetLedStateRequest, req, on_led_state, &ask_id, 1000);
if (rc == 0) {
    /* Cancellation is valid after ipc_start_all_actors(). */
    ipc_ask_cancel(&app_actor, ask_id);
}
```

##### Ask an empty request

Use `ipc_ask()` when the request payload is empty. The reply type is still
associated with the request using `IPC_CMD_REPLY_DEFINE_LOCAL()` (or the shared
`DECLARE` / `DEFINE` pair).

```c
IPC_CMD_DEFINE_LOCAL(RefreshStatus, {});
IPC_CMD_REPLY_DEFINE_LOCAL(RefreshStatus, RefreshStatusReply, {
    uint32_t generation;
});

IPC_ACTOR_RESPONSE_HANDLE(app_actor, RefreshStatus, RefreshStatusReply, on_refresh_status)
{
    (void)self;
    (void)raw_msg;
    if (result != 0) {
        printf("refresh failed: %d\n", result);
        return;
    }
    printf("status generation: %u\n", msg->generation);
}

/* No request payload argument is needed. */
int rc = ipc_ask(&app_actor, RefreshStatus, on_refresh_status, 250);
```

##### Return an application error instead of a payload

A responder can complete an ask without constructing the typed response. The
callback receives the non-zero result and must not dereference `msg` on this
path.

```c
IPC_ACTOR_HANDLE(led_actor, GetLedStateRequest, on_get_state_request)
{
    if (!led_driver_is_ready()) {
        (void)ipc_reply_error(raw_msg, -EIO);
        return;
    }

    GetLedStateResponse_payload_t response = {.channel = msg->channel, .on = 1};
    (void)ipc_reply(raw_msg, GetLedStateResponse, response);
}

IPC_ACTOR_RESPONSE_HANDLE(app_actor, GetLedStateRequest, GetLedStateResponse, on_led_state)
{
    (void)self;
    (void)raw_msg;
    if (result != 0) {
        printf("LED state unavailable: %d\n", result);
        return;
    }
    use_led_state(msg);
}
```

##### Track multiple outstanding asks

Each `_id` ask has its own correlation ID. Keep the IDs in actor-owned state
when requests can overlap; cancel only the specific request that is no longer
needed.

```c
static uint32_t pending_channel_0;
static uint32_t pending_channel_1;

GetLedStateRequest_payload_t ch0 = {.channel = 0};
GetLedStateRequest_payload_t ch1 = {.channel = 1};

if (ipc_ask_with_id(&app_actor, GetLedStateRequest, ch0, on_led_state,
                    &pending_channel_0, 1000) == 0 &&
    ipc_ask_with_id(&app_actor, GetLedStateRequest, ch1, on_led_state,
                    &pending_channel_1, 1000) == 0) {
    /* Each reply invokes on_led_state independently. */
}

/* For example, when channel 0 is no longer visible: */
if (pending_channel_0 != 0) {
    (void)ipc_ask_cancel(&app_actor, pending_channel_0);
    pending_channel_0 = 0;
}
```

#### Delayed command: send a command later

Delayed sends target the same single registered command receiver. Each actor
has one pending delayed slot; a newer `ipc_send_after()` call replaces the
previous one. Explicit cancellation of a delayed send is not supported.

```c
ipc_send_after(LedOn, 500, (LedOn_payload_t){.brightness = 200});
```

### 5. Run the framework

**POSIX (host / Linux / macOS):**

```c
int main(void) {
    led_actor_init();
    app_actor_init();
    button_actor_init();
    ipc_start_all_actors();
    /* ... do work, then ... */
    ipc_stop_all();            /* signal all threads to exit */
    ipc_run_all();             /* block in pthread_join until done */
    return 0;
}
```

**Zephyr:** actor `*_init` functions are called via `SYS_INIT(...,
APPLICATION, 85)`. After routes are registered, call `ipc_start_all_actors()`
to spawn each statically declared actor's `k_thread` (via the Zephyr port's
`ipc_port_actor_init` hook). The sample at
[`examples/basic_zephyr/`](examples/basic_zephyr/) is structured this way —
its `main.c` only prints a banner, since the actor is initialized from
`SYS_INIT`.

See [`examples/led_actor/main.c`](examples/led_actor/main.c) for a complete
POSIX runnable.

---

## Error codes

Framework APIs return `0` on success and a negative errno-style value on
failure. Include `<errno.h>` (already included by `<ipc.h>`) and compare
against the standard error constants; do not compare numeric values.

| Code | Meaning |
|---|---|
| `-EINVAL` | Invalid API arguments, an invalid message kind, or an invalid reply/error result. |
| `-ENOENT` | No route exists for a command, or an ask/reply correlation ID is no longer pending. |
| `-EMSGSIZE` | The message or expected reply payload exceeds the target actor's configured capacity. |
| `-ENOMEM` | A fixed framework table, mailbox, or port timeout resource is full. |
| `-EALREADY` | A reply was already sent for the ask, or actor startup was requested while already running. |
| `-EBUSY` | Actor startup was requested while the previous generation is stopping and has not yet been joined. |
| `-EPERM` | The operation requires running actors (for example `ipc_send_to()` or `ipc_ask_cancel()`) but startup has not completed or shutdown has started. |
| `-ETIMEDOUT` | An ask did not receive a reply before its requested timeout. Delivered through the ask callback's `result`, rather than returned by `ipc_ask*()`. |

Port operations can also return other negative system errno values, such as
`-EAGAIN` or `-EIO`. Always handle a non-zero return from send, ask, reply, and
lifecycle APIs. For `IPC_ACTOR_RESPONSE_HANDLE()`, `result == 0` is the only
case where the typed `msg` payload may be read.

## Configuration

Actor stack, queue depth, payload capacity, and port runtime state are declared
per actor with `IPC_ACTOR_DEFINE()`. Registry capacities are fixed
implementation details, not user configuration.

Use `IPC_MESSAGE_MAX(...)` to size each actor queue for exactly the messages it
receives, for example:

```c
IPC_ACTOR_DEFINE(app_actor, "app", 2048, 0, 8,
                 IPC_MESSAGE_MAX(AppStart, AppStop, LedFault));
```

The generated `IPC_ACTOR_HANDLE()` adapters `static_assert` that each handled
message payload fits that actor's `max_payload` value. Oversized payloads fail
the build instead of failing at runtime.

---

## Building

This repo uses [`mise`](https://mise.jdx.dev) for toolchain pinning. Once
you've accepted `mise trust`, the available tasks are:

```bash
mise run configure           # cmake --preset debug
mise run build               # cmake --build --preset debug
mise run tests               # unit + POSIX integration tests
mise run example-build       # debug + examples preset
mise run run-example         # builds and runs the led_actor example
mise run clean               # rm -rf build build-*
```

Equivalent raw CMake invocations:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Build options (see top-level `CMakeLists.txt`):

| Option                | Default | Effect                                             |
|------------------------|---------|-----------------------------------------------------|
| `IPC_BUILD_TESTS`      | OFF     | unit tests (gtest, FetchContent-pinned v1.15.2)     |
| `IPC_BUILD_EXAMPLES`   | OFF     | `led_actor_example` binary                          |
| `IPC_PLATFORM`         | posix   | `posix` (host) or `zephyr` (set by Zephyr's build)  |

### Zephyr

Skip this section if you only use POSIX/host builds.

Inside a Zephyr app, drop this repo in as the `ipc` module (or use
`CMakeLists.txt`-level integration) and add to `prj.conf`:

```kconfig
CONFIG_ACTOR=y
```

Actor stack, queue storage, and payload capacity are specified per actor with
`IPC_ACTOR_DEFINE()`.

The repo ships a runnable Zephyr sample app at
[`examples/basic_zephyr/`](examples/basic_zephyr/) that links this framework
as a Zephyr module. It defines ping and pong actors, registers
`BasicPing`/`BasicPong` commands, sends the initial command from `SYS_INIT`,
and then re-arms itself with `ipc_send_after()` a few times. Before exiting,
`main()` also sends `BasicStatusRequest`; the pong actor answers with
`BasicStatusResponse` containing the current ping/pong counters. The sample
has its own `west.yml`, so it can create a local Zephyr workspace under
`examples/` directly from this checkout:

```bash
mise exec -- west init -l examples/basic_zephyr
cd examples
mise exec -- west update
mise exec -- west build -b native_sim basic_zephyr -d basic_zephyr/build -p always
mise exec -- west build -t run -d basic_zephyr/build
```

---

## Distribution / consumption paths

This repo is designed to be consumed in three ways:

1. **Zephyr module** — `zephyr/module.yml` declares `build.cmake: zephyr` and
   `build.kconfig: zephyr/Kconfig`. Mount the repo at `modules/ipc/` in a
   Zephyr workspace, or list it in `ZEPHYR_EXTRA_MODULES`, and west picks it
   up automatically. The app include path gets both `src/port/zephyr/` and
   `include/`, so `<ipc.h>` can include the Kconfig-aware `ipc_config.h` and
   all translation units agree on the same `IPC_*` sizing values.
2. **CMake `add_subdirectory` / `FetchContent`** —
   `target_link_libraries(my_app PRIVATE ipc)` gives you the public include
   dir transitively. The default platform is POSIX; the Zephyr port is only
   compiled when `IPC_PLATFORM=zephyr`.
3. **`find_package(ipc)`** *(future)* — install with `cmake --install`.

The public surface is intentionally one header (`<ipc.h>`). Anything under
`src/` is an implementation detail and is not exported to consumers.

---

## Design notes

- **No linker scripts** — the registry is a simple linked list built by
  port-specific static actor startup hooks (POSIX constructors, Zephyr
  `SYS_INIT`) and walked by name lookup.
- **Actor handles are explicit** — `IPC_ACTOR_DEFINE()` always keeps actors
  file-local. Use `ipc_send_to()` from the same file (for example an ISR)
  for the O(1) direct path.
- **Message descriptors** — use `IPC_CMD_DECLARE()` / `IPC_EVENT_DECLARE()`
  in headers and one-argument `IPC_CMD_DEFINE()` / `IPC_EVENT_DEFINE()` in
  one source file when a message is shared across translation units.
  `IPC_CMD_DEFINE_LOCAL(Type, fields)` / `IPC_EVENT_DEFINE_LOCAL(Type, fields)`
  creates a file-local descriptor for local-only messages.
- **Lazy ID init** — a message descriptor's `.id` is FNV-1a-hashed from
  `.name` when static actor handler tables are registered, or on first
  normal routed send for unregistered descriptors. Route registration
  happens during startup, before `ipc_start_all_actors()` starts actor
  threads. `ipc_send_to()` requires the ID to already be initialized —
  typically by `IPC_ACTOR_HANDLE()`.
- **One delayed message per actor** — `ipc_send_after` replaces the previous
  pending delayed message. The current implementation uses a per-actor delay
  primitive (POSIX: one helper `pthread` per actor; Zephyr: one
  `k_work_delayable` per actor). See [`docs/timer_wheel.md`](docs/timer_wheel.md)
  for the full layout and the contract around it. Explicit cancellation is
  not supported.
- **Port seam** — `struct ipc_actor::port` is an opaque pointer to
  platform-specific state emitted by the active port's actor definition
  macro `IPC_ACTOR_DEFINE()`. New ports must implement the full `src/ipc_port.h`
  interface and provide their own per-actor state storage.
- **Interrupt-context work** — don't publish from ISR context: `ipc_publish`
  is a fan-out operation that scans subscriptions and may enqueue to many
  actors. Use `ipc_send_to(&actor, MsgType, payload)` to post O(1) to a
  driver/local actor after `ipc_start_all_actors()` succeeds, then call
  `ipc_publish()` from that actor's thread context if fan-out is needed.

---

## For contributors and AI coding agents

See [`AGENTS.md`](AGENTS.md) for contributor-facing guidance, including the
"adding a new message kind" and "adding a new port" checklists. If you're an
AI agent working in this repo, start there — it documents the conventions
this README only summarizes.