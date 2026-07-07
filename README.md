# IPC Actor Framework

A small **actor-model IPC framework** written in **C11** (with a **C++17**
test suite). Each actor owns its own thread and message inbox; messages
are typed and dispatched via `IPC_DISPATCH_TO`. The core is
platform-agnostic — POSIX (pthreads) and Zephyr (`k_msgq` / `k_thread`)
live behind a single port seam.

- One public header: `<ipc.h>`.
- No `extern struct ipc_actor` anywhere. Cross-actor sends use typed
  message descriptors looked up by ID.
- No linker scripts, no central registry file. Actors are discovered by
  name at runtime via a linked list.
- No heap allocation in the core.

## Repository layout

```
ipc/
├── include/
│   ├── ipc.h              ← ONLY public header (whole API surface)
│   └── ipc_defaults.h     ← sizing defaults; tunable per-TU or via -D
├── src/
│   ├── ipc.c              ← platform-agnostic core
│   ├── ipc_internal.h     ← private: FNV-1a, test reset hook
│   ├── ipc_port.h         ← generic port interface
│   └── port/
│       ├── posix/posix_ipc_port.c
│       └── zephyr/
│           ├── zephyr_ipc_port.c
│           ├── ipc_config.h     ← Kconfig-aware overlay (build-time only)
│           └── Kconfig
├── tests/unit/            ← gtest; links ipc.c + mock_ipc_port (no threads)
├── examples/led_actor/    ← LED + app + button actors (POSIX runnable)
├── cmake/GTest.cmake      ← FetchContent-pinned googletest v1.15.2
├── CMakeLists.txt         ← top-level build (option-gated tests/examples)
├── CMakePresets.json
├── mise.toml
└── AGENTS.md              ← contributor / agent guidance
```

## Quick start

### 1. Define a message type

```c
#include <ipc.h>

IPC_CMD_DEFINE(LedOn, { uint8_t brightness; });
```

`IPC_CMD_DEFINE` declares a `LedOn` message descriptor (with lazy
FNV-1a-hashed `.id`, set on first register/send) and a
`LedOn_payload_t` typedef for the wire payload. A `static_assert`
checks that `sizeof(LedOn_payload_t) <= IPC_PAYLOAD_SIZE` at compile
time.

The same pattern works for queries and events:

```c
IPC_QUERY_DEFINE(GetLedState,
    { uint8_t channel; },                      /* request fields */
    { uint8_t on; uint8_t brightness;
      uint32_t on_time_ms; });                 /* response fields */

IPC_EVENT_DEFINE(LedFault, { uint32_t error_code; uint8_t channel; });
```

### 2. Write a typed handler

```c
IPC_HANDLE(LedOn, on_led_on) {
    (void)self; (void)raw_msg;
    /* `msg` is `const LedOn_payload_t *` — typed, no cast */
    set_led(msg->brightness);
}
```

The macro expands to a function with signature:

```c
static void on_led_on(struct ipc_actor *self,
                      const LedOn_payload_t *msg,
                      const struct ipc_msg *raw_msg);
```

The `raw_msg` argument is what you pass to `ipc_reply()` for queries.

### 3. Wire the dispatch chain

```c
static void led_handler(struct ipc_actor *self, const struct ipc_msg *msg)
{
    IPC_DISPATCH_TO(msg, LedOn,    on_led_on)
    IPC_DISPATCH_TO(msg, LedOff,   on_led_off)
    IPC_DISPATCH_TO(msg, GetLedState, on_get_state)
    { /* unknown id — ignore */ }
}
```

`IPC_DISPATCH_TO` is a chainable `if-else` that pattern-matches on the
message ID and binds the typed payload for you.

### 4. Create the actor and register messages

```c
static struct ipc_actor led_actor;

int led_actor_init(void)
{
    ipc_actor_init(&led_actor, "led", led_handler, (struct ipc_actor_cfg){
        .stack_size  = 512,
        .priority    = 5,
        .queue_depth = 8,
    });
    IPC_REGISTER(&led_actor, LedOn);
    IPC_REGISTER(&led_actor, LedOff);
    IPC_REGISTER(&led_actor, GetLedState);
    IPC_SUBSCRIBE(&led_actor, LedFault);   /* event: pub/sub fan-out */
    return 0;
}
```

`IPC_REGISTER` maps a CMD/QUERY descriptor → actor (single target).
`IPC_SUBSCRIBE` adds the actor to a fan-out list for an EVENT
descriptor (one row per `(event_id, actor)` pair; duplicate
subscriptions are silently idempotent).

### 5. Send messages (no `extern` needed)

```c
/* CMD — fire and forget */
ipc_send(LedOn, .brightness = 200);

/* EVENT — broadcast to all subscribers */
ipc_publish(LedFault, .error_code = 0xDEAD, .channel = 1);

/* QUERY — blocks the calling thread until reply or timeout */
GetLedState_response_t state;
int rc = ipc_query(GetLedState, &state, IPC_TIMEOUT_MS(100), .channel = 0);

/* Reply from inside a query handler */
IPC_HANDLE(GetLedState, on_get_state) {
    (void)self; (void)msg;
    ipc_reply(raw_msg, GetLedState, .on = 1, .brightness = 80,
              .on_time_ms = 12345);
}

/* Delayed — re-arms self (replaces any previous pending delayed msg) */
ipc_send_after(LedBlink, 500, .period_ms = 500, .brightness = 200);
```

### 6. Run the framework

POSIX (host / Linux / macOS):

```c
int main(void) {
    led_actor_init();
    app_actor_init();
    button_actor_init();

    ipc_start_all_threads();   /* spawns one pthread per actor  */
    /* ... do work, then ... */
    ipc_stop_all();            /* joins all threads              */
    return 0;
}
```

Zephyr: actor `*_init` functions are called via `SYS_INIT(...,
APPLICATION, 85)`. The framework's own `SYS_INIT(..., APPLICATION,
90)` starts the actor threads after registration completes. No
`ipc_run_all()` is needed — Zephyr's idle thread is your main loop.

See `examples/led_actor/main.c` for a complete POSIX runnable.

## Configuration

The framework's compile-time sizing knobs are defined in
[`include/ipc_defaults.h`](include/ipc_defaults.h) and are overridable
per translation unit. The public knobs:

| Macro                      | Default | What it sizes                                          |
|----------------------------|---------|--------------------------------------------------------|
| `IPC_PAYLOAD_SIZE`         | 32      | wire message payload bytes                             |
| `IPC_MAX_REGISTRATIONS`    | 32      | CMD/QUERY ID → actor map                               |
| `IPC_MAX_SUBSCRIPTIONS`    | 32      | EVENT ID → actor list (one row per subscriber)         |
| `IPC_PORT_STATE_WORDS`     | 64      | per-actor opaque blob (×`sizeof(uintptr_t)`)           |
| `IPC_QUERY_WAIT_WORDS`     | 24      | query-wait blob (×`sizeof(uintptr_t)`)                 |
| `IPC_MAX_INFLIGHT_QUERIES` | 16      | max concurrent in-flight queries                       |

Override precedence (highest wins):

1. Define the `IPC_*` macro **before** `#include <ipc.h>` in a single TU.
2. Pass `-DIPC_PAYLOAD_SIZE=64` to the compiler.
3. On Zephyr, set the matching `CONFIG_ACTOR_*` symbol in Kconfig. The
   port's overlay at `src/port/zephyr/ipc_config.h` translates Kconfig
   values into `IPC_*` macros before the public defaults are consulted.

Every `IPC_*_DEFINE` macro `static_assert`s that its payload fits
`IPC_PAYLOAD_SIZE` at compile time, so an oversized payload fails the
build, not at runtime.

## Building

This repo uses [`mise`](https://mise.jst.jr) for toolchain pinning.
With `mise trust` accepted once, the available tasks are:

```bash
mise run configure           # cmake --preset debug
mise run build               # cmake --build --preset debug
mise run test-unit           # ctest --preset debug
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

| Option                 | Default | Effect                                              |
|------------------------|---------|-----------------------------------------------------|
| `IPC_BUILD_TESTS`      | OFF     | unit tests (gtest, FetchContent-pinned v1.15.2)     |
| `IPC_BUILD_EXAMPLES`   | OFF     | `led_actor_example` binary                          |
| `IPC_PLATFORM`         | posix   | `posix` (host) or `zephyr` (set by Zephyr's build)  |

### Zephyr

Inside a Zephyr app, drop this repo in as the `ipc` module (or use
`CMakeLists.txt`-level integration) and add to `prj.conf`:

```kconfig
CONFIG_ACTOR=y
```

Tune sizing via `CONFIG_ACTOR_PAYLOAD_SIZE`, `CONFIG_ACTOR_MAX_REGISTRATIONS`,
etc. (see `src/port/zephyr/Kconfig`). The Zephyr port's
`ipc_config.h` overlay maps Kconfig values to the `IPC_*` macros that
`<ipc.h>` consumes.

## Distribution / consumption paths

This repo is designed to be consumed in three ways:

1. **Zephyr module** — installed as `modules/ipc/`. The module's
   `include/` is automatically added to the app's include path by
   Zephyr's build system. The Zephyr overlay header
   (`src/port/zephyr/ipc_config.h`) is only on the library's
   *private* include path, so consumers always see the public
   `include/ipc_defaults.h`.
2. **CMake `add_subdirectory` / `FetchContent`** — `target_link_libraries(my_app PRIVATE ipc)`
   gives you the public include dir transitively. The default platform
   is POSIX; the Zephyr port is only compiled when `IPC_PLATFORM=zephyr`.
3. **`find_package(ipc)`** (future) — install with `cmake --install`.

The public surface is intentionally one header (`<ipc.h>`) plus the
sizing defaults (`<ipc_defaults.h>`, included by `<ipc.h>`). Anything
under `src/` is implementation detail and not exported to consumers.

## Design notes

- **No linker scripts** — the registry is a simple linked list built at
  runtime by `ipc_actor_init` and walked by name lookup.
- **No `extern struct ipc_actor`** — `ipc_send` / `ipc_publish` /
  `ipc_query` route via the message ID; consumers never need a
  reference to the target actor.
- **Lazy ID init** — message descriptor `.id` is FNV-1a-hashed from
  `.name` on first register/subscribe/send. All registration happens
  single-threaded before `ipc_start_all_threads` (POSIX) or
  `ipc_run_all` (Zephyr).
- **One delayed message per actor** — `ipc_send_after` replaces the
  previous pending delayed msg. Explicit cancellation is a known TODO
  and must be done through the port seam if added.
- **Port seam** — `struct ipc_port_state` (in `ipc_port_state_t`) is
  the only platform-specific type visible in the public API. New ports
  must implement the full `src/ipc_port.h` interface and stay within
  `IPC_PORT_STATE_WORDS × sizeof(uintptr_t)` bytes.
- **ISR-safe sends on Zephyr** — `k_msgq_put` + `k_poll_signal_raise`
  are IRQ-safe; CMDs and EVENTS can be sent from interrupt context.
  Queries cannot (they need a blocking wait).

See `AGENTS.md` for contributor-facing guidance, including the
"adding a new message kind" and "adding a new port" checklists.
