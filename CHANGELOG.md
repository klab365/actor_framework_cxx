# Changelog

All notable changes to this project will be documented in this file.

## 1.1.0 (2026-08-06)

### Added

- Added `ipc_send_to()` for O(1) direct actor mailbox delivery, intended for interrupt-originated work after actor startup.
- Kept actors strictly file-local: `IPC_ACTOR_DEFINE()` always creates a `static` actor with internal linkage, so there is no externally-exported actor handle or `extern struct ipc_actor` across translation units.
- Added shared message descriptor declarations with `IPC_CMD_DECLARE()` / `IPC_EVENT_DECLARE()` and extern definitions with `IPC_CMD_DEFINE()` / `IPC_EVENT_DEFINE()`.
- Added local-only message definition macros `IPC_CMD_DEFINE_LOCAL()` and `IPC_EVENT_DEFINE_LOCAL()`.
- Added asynchronous ask/reply messaging with typed reply descriptors, correlation IDs, response handlers, reply helpers, and cancellation support.
- Added ask/reply coverage to POSIX and Zephyr message storage paths, examples, README documentation, and unit tests.

### Changed

- Documented the recommended ISR pattern: post directly to a driver/local actor with `ipc_send_to()`, then publish fan-out events from actor context.
- Updated examples and tests to use the explicit local/shared message definition macros.

### Removed

- Removed ISR publish APIs because publish fan-out scans subscribers and is not O(1) in interrupt context.

## 1.0.0 (2026-07-27)

### Added

- Added static actor registration with typed message descriptors and dispatch helpers.
- Added Zephyr platform integration and a Zephyr example application.
- Added actor lifecycle hooks, supervision strategies, and restart handling.
- Added CI, unit tests, and expanded test coverage for IPC send paths, hooks, registry behavior, and restart errors.
- Added an agent workflow for maintaining changelog entries.

### Changed

- Refactored actor definitions and message handling to support per-actor maximum payload sizes using `IPC_MESSAGE_MAX`.
- Refactored payload processing through a shared adapter path for POSIX and Zephyr actor definitions.
- Improved per-actor state management, static actor startup hooks, and Zephyr integration behavior.
- Updated examples to use the current actor/message API.
- Clarified public API documentation in `include/ipc.h`.

### Fixed

- Fixed IPC handler registration static assertions.
- Improved actor restart error handling and related tests.
