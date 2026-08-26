# Basic Zephyr IPC example

A minimal Zephyr app that enables this repository as a Zephyr module and uses
`CONFIG_ACTOR` to build the IPC framework. The sample starts two actors that
exchange `BasicPing` and `BasicPong` commands to demonstrate inter-actor
communication and message handling. Before exiting, `main()` sends a
`BasicStatusRequest` ask to the pong actor with `ipc_ask_with_id()`; the actor
answers with `BasicStatusResponse` and reports the observed ping/pong counters.
The example also sends an invalid status ask to demonstrate `ipc_reply_error()`
and handling a non-zero `result` in `IPC_ACTOR_RESPONSE_HANDLE()`.

## Footprint tuning

Each Zephyr actor reserves RAM roughly equal to:

```text
stack_size
+ queue_depth * IPC_MSG_SLOT_SIZE(max_payload)
+ one send slot and one receive slot
+ delayed-send payload/work state, if CONFIG_ACTOR_SEND_AFTER=y
+ struct ipc_port_state and Zephyr kernel object overhead
```

For smaller applications:

- keep `queue_depth` as low as practical;
- use `IPC_MESSAGE_MAX(...)` rather than a large generic payload limit;
- send small handles/references instead of large payloads when possible;
- tune actor stack sizes with Zephyr stack analysis;
- reduce `CONFIG_ACTOR_MAX_REGISTRATIONS` and `CONFIG_ACTOR_MAX_SUBSCRIPTIONS` to match your command/event handler counts;
- set `CONFIG_ACTOR_SEND_AFTER=n` if `ipc_send_after()` is not used;
- set `CONFIG_ACTOR_ASK=n`, or reduce `CONFIG_ACTOR_MAX_INFLIGHT_ASKS` and `CONFIG_ACTOR_MAX_PENDING_ASK_TIMEOUTS`, if ask/reply is not used heavily.

This directory contains a `west.yml`, so it can create a local Zephyr
workspace under `examples/` directly from this checkout:

```sh
mise exec -- west init -l examples/basic_zephyr
cd examples
mise exec -- west update
mise exec -- west build -b native_sim basic_zephyr -d basic_zephyr/build -p always
mise exec -- west build -t run -d basic_zephyr/build
```
