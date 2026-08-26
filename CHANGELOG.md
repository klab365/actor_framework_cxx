# Changelog

All notable changes to this project will be documented in this file.

## 1.3.0

### Added

- Added Zephyr Kconfig options to size core command-route, event-subscription, in-flight ask, and pending ask-timeout tables for smaller applications.
- Added Zephyr Kconfig options to disable delayed sends, ask/reply support, and framework diagnostics when unused.
- Documented Zephyr footprint tuning guidance for queue depth, payload sizing, actor stacks, and optional timed features.

### Changed

- Reduced Zephyr port overhead by blocking actor threads directly on `k_msgq` mailboxes instead of using `k_poll_signal` wakeups.
- Reduced Zephyr actor startup/resource overhead by storing static actor resources directly in each actor's port state instead of a fixed global registry table.
- Reduced optional ask/reply footprint by compiling out core pending-ask storage when Zephyr ask support is disabled.

## 1.2.1

### Fixed

- Fixed unaligned-access faults in Zephyr actor message queues on architectures that require naturally aligned memory access.

## 1.2.0

### Added

- Added configurable timeouts for asynchronous asks; timed-out callbacks receive `-ETIMEDOUT`.
- Added `ipc_reply_error()` so ask responders can return an errno-style error without a reply payload.
- Added POSIX lifecycle integration coverage and expanded ask, send, and lifecycle test coverage.

### Changed

- Strengthened actor lifecycle handling to reject duplicate starts, prevent restart before stopped threads are joined, and clean up partially started actors.
- Documented ask timeouts, error replies, lifecycle constraints, and framework error codes.

### Fixed

- Return `-EINVAL` instead of asserting when callers try to publish an invalid or command message descriptor.

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
