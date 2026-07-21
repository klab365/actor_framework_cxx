# AGENTS.md

Guidance for AI/coding agents working in this repository.

## Project overview

This is a small actor-model IPC framework written in C11 with C++17 tests.

- Public API lives in `include/ipc.h` only.
- Core implementation is platform-agnostic in `src/ipc.c`.
- Platform ports live under `src/port/posix/` and `src/port/zephyr/` behind `src/ipc_port.h`.
- Unit tests use GoogleTest and a mock port in `tests/mocks/`.
- Examples are under `examples/`.

Preserve these design constraints unless explicitly asked to change them:

- No heap allocation in the core.
- Do not expose `struct ipc_actor` instances across modules with `extern`.
- Cross-actor sends should use typed message descriptors / IDs and runtime actor-name lookup.
- Keep platform-specific behavior inside the port layer, not in the public header or core logic.
- Keep `<ipc.h>` as the single public API surface.

## Build and test commands

Prefer `mise` tasks when available:

```sh
mise run configure
mise run build
mise run test-unit
mise run check-format
```

Useful alternatives:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Other tasks:

```sh
mise run coverage
mise run example-build
mise run run-example
mise run build-zephyr-example
mise run run-zephyr-example
mise run format
```

Before handing off code changes, run at least the relevant build/tests. For broad C/C++ edits, also run `mise run check-format`.

## Style

- C core/ports/examples: C11.
- Tests: C++17 with GoogleTest.
- Indentation: 4 spaces, no tabs.
- Naming: `snake_case`, with `ipc_` / `_ipc_` prefixes for project symbols.
- Use the repository `.clang-format`; do not hand-format contrary to it.
- Keep comments focused on intent, invariants, and non-obvious behavior.
- Avoid unnecessary churn in public headers and examples.

## Public API and macro rules

- Message types are declared with `IPC_CMD_DEFINE` / `IPC_EVENT_DEFINE`.
- Handlers should use `IPC_HANDLE` when possible.
- Dispatch chains intentionally use `IPC_DISPATCH_TO(...)` followed by a final unknown-message macro. Do not add semicolons that break the chain shape.
- Maintain compile-time payload size checks against `IPC_PAYLOAD_SIZE`.
- Be very conservative about changing macro expansion semantics; add or update tests for macro behavior.

## Testing expectations

Add or update tests for behavior changes, especially around:

- actor lifecycle and registry behavior,
- send/error paths,
- dispatch and message ID handling,
- platform-port contract behavior,
- payload size/copy semantics.

Unit tests should generally link `src/ipc.c` with `tests/mocks/mock_ipc_port.c` rather than using real threads.

## Zephyr notes

- Zephyr integration is module-oriented; related files are in `zephyr/`, `src/port/zephyr/`, and `examples/basic_zephyr/`.
- Do not make host/POSIX assumptions in code shared with Zephyr.
- Zephyr builds use `west` through the mise tasks above.

## Agent workflow

- Inspect existing code before editing.
- Make minimal, targeted changes.
- Use precise file edits rather than broad rewrites unless creating a new file.
- Keep generated/build artifacts out of commits (`build/`, `build-*`, Zephyr build outputs).
- If a requested change conflicts with repository design constraints, call that out and ask for confirmation.
