# POSIX LED actor example

This example demonstrates commands, events, and async request/response on the POSIX port.

Ask/reply flow:

1. `app_actor.c` sends a normal state query with `ipc_ask_with_id()` and logs
   the correlation ID that could be passed to `ipc_ask_cancel()` if the result
   were no longer needed.
2. `led_actor.c` handles the request and sends `GetLedStateResponse` with
   `ipc_reply()`.
3. The app actor's `IPC_ACTOR_RESPONSE_HANDLE()` prints the typed reply.
4. The app also asks for an unavailable channel. The LED actor answers with
   `ipc_reply_error(raw_msg, -ENODEV)`, demonstrating that the callback receives
   a non-zero `result` and must not read `msg` on that path.
5. If the LED actor did not reply before the 1000 ms timeout, the same callback
   would receive `result == -ETIMEDOUT`.

Run it from the repository root with:

```sh
cmake --preset debug-examples
cmake --build build/debug-examples
./build/debug-examples/led_actor_example
```
