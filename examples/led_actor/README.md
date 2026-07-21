# POSIX LED actor example

This example demonstrates commands, events, and async request/response on the POSIX port.

State flow:

1. `app_actor.c` sends `GetLedStateRequest` with `ipc_send()`.
2. `led_actor.c` handles the request and sends `GetLedStateResponse`.
3. The app actor handles `GetLedStateResponse` and prints the LED state.

Run it from the repository root with:

```sh
cmake --preset debug-examples
cmake --build build/debug-examples
./build/debug-examples/led_actor_example
```
