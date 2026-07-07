# AGENTS.md

Guidance for AI coding agents (and humans) working on this repository.

## Project summary

A small **actor-model IPC framework** (working name `ipc`, library target
`ipc`) written in **C11** with a **C++17** test suite. Each actor owns its
own thread and message inbox; messages are typed and dispatched via
`IPC_DISPATCH_TO`. The core is platform-agnostic; platform behaviour
(pthread vs Zephyr kernel) lives in a port seam under `src/port/`.

Two supported platforms:

- **POSIX** (host / Linux / macOS) — pthread + mutex + cond, uses `Threads::Threads`.
- **Zephyr** (embedded) — `k_msgq` + `k_poll` + `k_work_delayable` + `k_thread`.

There are no linker scripts, no `extern` actor declarations, and no central
registry file. Actors are discovered by name; the registry is a runtime
linked list.

## Repository layout

```
include/
  ipc.h                 ← ONLY public header (whole API surface)
  ipc_defaults.h        ← sizing defaults; tunable per-TU or via -D
src/
  ipc.c                 ← platform-agnostic core (registry, send, query, …)
  ipc_internal.h        ← private: FNV-1a, test reset hook
  ipc_port.h            ← generic port interface
  port/
    posix/posix_ipc_port.c
    zephyr/
      zephyr_ipc_port.c
      ipc_config.h      ← shadows include/ipc_defaults.h on Zephyr
      Kconfig
tests/
  unit/                 ← gtest, links ipc.c + mock_ipc_port (no threads)
  mocks/mock_ipc_port.{c,h}
examples/led_actor/     ← LED + app + button actors (POSIX runnable)
cmake/GTest.cmake       ← FetchContent-pinned googletest v1.15.2
CMakeLists.txt          ← top-level build (option-gated tests/examples)
mise.toml               ← dev toolchain + task runners
.devcontainer/          ← Ubuntu base + mise feature
```

## Conventions

- **Public surface is exactly one header**: `include/ipc.h`. Do not add
  new public headers. The `include/ipc_defaults.h` file is on the
  public include path because `<ipc.h>` `#include`s it; it carries
  sizing constants that consumers may override, not a separate API.
  The port-side `ipc_config.h` overlay (Zephyr) and the internal
  `ipc_internal.h` are not on the public include path.
- **C11, C99-compatible**, `-Wall -Wextra`, no GNU extensions. C++17 is
  only used for the test binary.
- **No `#ifdef` in `ipc.c` or `ipc.h`**. All platform branching is
  delegated to the port seam (`ipc_port.h` + `src/port/<platform>/*`).
- **Tables are sized at compile time** (`IPC_MAX_REGISTRATIONS`,
  `IPC_MAX_SUBSCRIPTIONS`). No heap allocations in the core.
- **Lazy ID init**: message descriptor `.id` is FNV-1a-hashed from
  `.name` on first register/subscribe/send. All registration happens
  single-threaded before `ipc_run_all()`. Never call `_ipc_fnv1a` from
  user code.
- **One delayed message per actor**. `ipc_send_after` replaces the
  previous pending delayed msg (see `todo.md` for follow-ups).
- **Payload cap** is `IPC_PAYLOAD_SIZE` (default 32 B, overridable).
  Every `IPC_*_DEFINE` macro `static_assert`s the payload size at
  compile time — respect that.
- **Naming**: public API prefix `ipc_`; private helpers `_ipc_`; macros
  `IPC_*`; types `struct ipc_*` / `ipc_*_t`.

## Build & test

The repo uses [`mise`](https://mise.jst.jr) for toolchain pinning. With
mise activated (`mise trust` once), the available tasks are:

```bash
mise run configure    # cmake -S . -B build -DIPC_BUILD_TESTS=ON -DIPC_PLATFORM=posix
mise run build        # cmake --build build
mise run test-unit    # ctest --test-dir build --output-on-failure --verbose
mise run example-build  # builds the led_actor example
mise run clean        # rm -rf build build-*
```

Equivalents without mise:

```bash
cmake -S . -B build -DIPC_BUILD_TESTS=ON -DIPC_PLATFORM=posix
cmake --build build
ctest --test-dir build --output-on-failure --verbose
```

Build options (see top-level `CMakeLists.txt`):

- `IPC_BUILD_TESTS`    — unit + integration tests (default OFF)
- `IPC_BUILD_PROBES`   — capacity/memory probe binary (default OFF)
- `IPC_BUILD_EXAMPLES` — `led_actor_example` (default OFF)
- `IPC_PLATFORM`       — `posix` (default on host) or `zephyr` (set by
  Zephyr's CMake when `ZEPHYR_BASE` is defined)

Zephyr payload/table sizes come from Kconfig (`CONFIG_ACTOR_*`); the
host defaults are in `include/ipc_defaults.h` and can be overridden with
`-DIPC_PAYLOAD_SIZE=64` etc.

## Testing notes

- The unit test binary (`ipc_unit_test`) links `src/ipc.c` as an
  `OBJECT` library together with `tests/mocks/mock_ipc_port.c`. It
  does **not** spin up real threads — the mock port lets each test
  inspect what the core asked the port to do and/or invoke handlers
  deterministically. `mock_port_init()` and `_ipc_reset_for_testing()`
  reset all tables between cases.
- Unit tests deliberately call the `_raw` API directly, not the
  `ipc_send(MsgType, …)` macros. The macros wrap a compound literal
  in an address-of-temporary, which is a `-Waddress-of-temporary`
  diagnostic under C++. The raw API is the seam that matters for unit
  testing; the macros are exercised by the example app and any
  integration test.
- When adding tests, place them in `tests/unit/`, append the source
  file to `tests/CMakeLists.txt`, and follow the existing
  `<name>_test.cpp` naming. The pattern is: reset → mock init →
  `ipc_actor_init` → `IPC_REGISTER`/`IPC_SUBSCRIBE` → assert on
  `mock_port_*` state.

## Writing a new actor (TL;DR)

```c
#include <ipc.h>

IPC_CMD_DEFINE(MyCmd, { uint32_t value; });

IPC_HANDLE(MyCmd, my_cmd_handler) {
    (void)self; (void)raw_msg;
    /* handle msg->value */
}

static struct ipc_actor my_actor;

int my_module_init(void) {
    ipc_actor_init(&my_actor, "my_actor", my_handler, (struct ipc_actor_cfg){
        .stack_size = 512, .priority = 5, .queue_depth = 8,
    });
    IPC_REGISTER(&my_actor, MyCmd);
    return 0;
}

int main(void) {
    my_module_init();
    /* ...other module inits... */
    ipc_run_all();   /* POSIX: starts threads, blocks until ipc_stop_all() */
    return 0;
}
```

Cross-actor sends go through name lookup-free `ipc_send(MsgType, ...)`
(no `extern struct ipc_actor` ever). For events use
`ipc_publish(MsgType, ...)` and have consumers call
`IPC_SUBSCRIBE(actor, MsgType)`.

## When changing the core

- `ipc.h` is ABI. Adding a field is fine; renaming or reordering is
  not. Keep the macro definitions in the same order (the example app
  relies on a stable include order with `<ipc.h>` then actor headers).
- New message kinds need a new enum value in `ipc_msg_kind_t` and a
  matching handler-macro / dispatch-macro pair. Don't reuse enum
  values.
- The query path (`ipc_query_raw` / `ipc_reply_raw`) assumes a
  stack-allocated `ipc_query_wait_t` sized by `IPC_QUERY_WAIT_WORDS`.
  If you change its layout, update the `static_assert`s in both ports.
- Don't introduce heap allocation in the core. If a feature needs it,
  add a port hook.
- All cross-table mutations go through `ipc_port_table_lock` /
  `_unlock` — including the actor list walk in `ipc_actor_init`.

## When changing a port

- POSIX and Zephyr ports must implement the full `ipc_port.h`
  interface. Adding a new port function means updating all three:
  POSIX, Zephyr, and `tests/mocks/mock_ipc_port.{c,h}` (else unit
  tests fail to link).
- Per-actor state must fit in `IPC_PORT_STATE_WORDS × sizeof(uintptr_t)`
  bytes. `static_assert(sizeof(struct ipc_port_state) <= IPC_PORT_STATE_WORDS * sizeof(uintptr_t))`
  must hold.

## Known TODOs

- `todo.md` tracks open issues (currently: error-handling contract for
  CMD and QUERY paths). Update it when closing one of these.
- Cancelling a pending `ipc_send_after` message is not yet supported;
  a new send replaces the previous one. If you implement explicit
  cancellation, do it through the port seam so both backends stay in
  sync.
