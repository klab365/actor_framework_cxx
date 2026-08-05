# Changelog

All notable changes to this project will be documented in this file.

## 1.1.0 (2026-08-05)

### Added

- Added asynchronous ask/reply messaging with typed reply descriptors, correlation IDs, response handlers, reply helpers, and cancellation support.
- Added ask/reply coverage to POSIX and Zephyr message storage paths, examples, README documentation, and unit tests.

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
