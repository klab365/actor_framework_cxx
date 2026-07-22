# IPC Actor Framework

A small **actor-model IPC framework** written in **C11** (with a **C++17**
test suite). Each actor owns its own thread and message inbox; messages
are typed and bound to actors with `IPC_ACTOR_HANDLE`. The core is
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
│   └── ipc_defaults.h     ← IPC_PAYLOAD_SIZE default; tunable per-TU or via -D
├── src/
│   ├── ipc.c              ← platform-agnostic core
│   ├── ipc_internal.h     ← private: FNV-1a, test reset hook
│   ├── ipc_port.h         ← generic port interface
│   └── port/
│       ├── posix/posix_ipc_port.c
│       └── zephyr/
│           ├── zephyr_ipc_port.c
│           ├── ipc_defaults.h   ← Zephyr defaults wrapper
│           ├── ipc_config.h     ← Zephyr Kconfig → IPC_* sizing macros
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

The same pattern works for events. Request/response is modeled as two commands:

```c
IPC_CMD_DEFINE(GetLedStateRequest, { uint8_t channel; });
IPC_CMD_DEFINE(GetLedStateResponse, {
    uint8_t channel;
    uint8_t on;
    uint8_t brightness;
    uint32_t on_time_ms;
});

IPC_EVENT_DEFINE(LedFault, { uint32_t error_code; uint8_t channel; });
```

### 2. Define an actor and its typed handlers

```c
IPC_ACTOR_DEFINE(led_actor, "led", 512, 5, 8);

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

### 5. Send messages (no `extern` needed)

```c
/* CMD — fire and forget */
ipc_send(LedOn, (LedOn_payload_t){.brightness = 200});

/* EVENT — broadcast to all subscribers */
ipc_publish(LedFault, (LedFault_payload_t){.error_code = 0xDEAD, .channel = 1});

/* Async request/response — two commands */
ipc_send(GetLedStateRequest, (GetLedStateRequest_payload_t){.channel = 0});

IPC_ACTOR_HANDLE(led_actor, GetLedStateRequest, on_get_state_request) {
    (void)self; (void)raw_msg;
    ipc_send(GetLedStateResponse,
             (GetLedStateResponse_payload_t){.channel = msg->channel,
                                             .on = 1,
                                             .brightness = 80,
                                             .on_time_ms = 12345});
}

/* Delayed — re-arms self (replaces any previous pending delayed msg) */
ipc_send_after(LedBlink, 500, (LedBlink_payload_t){.period_ms = 500, .brightness = 200});
```

### 6. Run the framework

POSIX (host / Linux / macOS):

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

Zephyr: actor `*_init` functions are called via `SYS_INIT(...,
APPLICATION, 85)`. After routes are registered, call
`ipc_start_all_actors()` to spawn each statically declared actor's
`k_thread` (via the Zephyr port's `ipc_port_actor_init` hook). The
sample's
[`examples/basic_zephyr/`](examples/basic_zephyr/) is structured this
way; its `main.c` only prints a banner while the actor is initialized
from `SYS_INIT`.

See `examples/led_actor/main.c` for a complete POSIX runnable.

## Configuration

The only public compile-time sizing knob is defined in
[`include/ipc_defaults.h`](include/ipc_defaults.h):

| Macro              | Default | What it sizes              |
|--------------------|---------|----------------------------|
| `IPC_PAYLOAD_SIZE` | 32      | wire message payload bytes |

Actor stack, queue storage, and port runtime state are declared per actor
with `IPC_ACTOR_DEFINE()`. Registry capacities are fixed implementation
details, not user configuration.

Override precedence (highest wins):

1. Define `IPC_PAYLOAD_SIZE` **before** `#include <ipc.h>` in a single TU.
2. Pass `-DIPC_PAYLOAD_SIZE=64` to the compiler.
3. On Zephyr, set `CONFIG_ACTOR_PAYLOAD_SIZE` in Kconfig. The port's
   overlay at `src/port/zephyr/ipc_config.h` translates it into
   `IPC_PAYLOAD_SIZE` before the public default is consulted.

Per-actor port runtime state is emitted by the active port's
`IPC_ACTOR_DEFINE()` implementation, so there is no separate public sizing
knob for port-state storage.

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

Tune inline message payload size via `CONFIG_ACTOR_PAYLOAD_SIZE`
(see `src/port/zephyr/Kconfig`). Actor stack and queue storage are
specified per actor with `IPC_ACTOR_DEFINE()`.

The repo ships a runnable Zephyr sample app at
[`examples/basic_zephyr/`](examples/basic_zephyr/) that links this
framework as a Zephyr module. It defines ping and pong actors, registers
`BasicPing`/`BasicPong` commands, sends the initial command from
`SYS_INIT`, and then re-arms itself with `ipc_send_after()` a few times.
Before exiting, `main()` also sends `BasicStatusRequest`; the pong actor
answers with `BasicStatusResponse` containing the current ping/pong counters. The sample has
its own `west.yml`, so it can create a local Zephyr workspace under
`examples/` directly from this checkout:

```bash
mise exec -- west init -l examples/basic_zephyr
cd examples
mise exec -- west update
mise exec -- west build -b native_sim basic_zephyr -d basic_zephyr/build -p always
mise exec -- west build -t run -d basic_zephyr/build
```

## Distribution / consumption paths

This repo is designed to be consumed in three ways:

1. **Zephyr module** — the `zephyr/module.yml` declares
   `build.cmake: zephyr` and `build.kconfig: zephyr/Kconfig`. Mount
   the repo at `modules/ipc/` in a Zephyr workspace or list it in
   `ZEPHYR_EXTRA_MODULES` and west picks it up automatically. The
   app include path gets both `src/port/zephyr/` and `include/`, so
   `<ipc.h>` can include the Kconfig-aware `ipc_config.h` and all
   translation units agree on the same `IPC_*` sizing values.
2. **CMake `add_subdirectory` / `FetchContent`** — `target_link_libraries(my_app PRIVATE ipc)`
   gives you the public include dir transitively. The default platform
   is POSIX; the Zephyr port is only compiled when `IPC_PLATFORM=zephyr`.
3. **`find_package(ipc)`** (future) — install with `cmake --install`.

The public surface is intentionally one header (`<ipc.h>`) plus the
sizing defaults (`<ipc_defaults.h>`, included by `<ipc.h>`). Anything
under `src/` is implementation detail and not exported to consumers.

## Design notes

- **No linker scripts** — the registry is a simple linked list built by
  port-specific static actor startup hooks (POSIX constructors, Zephyr
  `SYS_INIT`) and walked by name lookup.
- **No `extern struct ipc_actor`** — `ipc_send` and `ipc_publish` route
  via the message ID; consumers never need a reference to the target actor.
- **Lazy ID init** — message descriptor `.id` is FNV-1a-hashed from
  `.name` when static actor handler tables are registered, or on first send
  for unregistered descriptors. Route registration happens during startup
  before `ipc_start_all_actors()` starts actor threads.
- **One delayed message per actor** — `ipc_send_after` replaces the
  previous pending delayed msg. The current implementation uses a
  per-actor delay primitive (POSIX: one helper `pthread` per actor;
  Zephyr: one `k_work_delayable` per actor). See
  [`docs/timer_wheel.md`](docs/timer_wheel.md) for the full layout
  and the contract that surrounds it. Explicit cancellation is not
  supported.
- **Port seam** — `struct ipc_actor::port` is an opaque pointer to
  platform-specific state emitted by the active port's `IPC_ACTOR_DEFINE()`
  implementation. New ports must implement the full `src/ipc_port.h`
  interface and provide their own per-actor state storage.
- **Interrupt-context publish** — use `IPC_ISR_PUBLISH(EventType, payload)`
  only after `ipc_start_all_actors()` succeeds. The event descriptor must
  already be bound with `IPC_ACTOR_HANDLE`, and the active port's
  `ipc_port_send_isr()` must be safe for its interrupt context.

See `AGENTS.md` for contributor-facing guidance, including the
"adding a new message kind" and "adding a new port" checklists.
