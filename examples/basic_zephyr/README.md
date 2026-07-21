# Basic Zephyr IPC example

A minimal Zephyr app that enables this repository as a Zephyr module and uses
`CONFIG_ACTOR` to build the IPC framework. The sample starts two actors that
exchange `BasicPing` and `BasicPong` commands to demonstrate inter-actor
communication and message handling. Before exiting, `main()` sends a
`BasicStatusRequest` command to the pong actor; the actor answers with
`BasicStatusResponse` and reports the observed ping/pong counters.

This directory contains a `west.yml`, so it can create a local Zephyr
workspace under `examples/` directly from this checkout:

```sh
mise exec -- west init -l examples/basic_zephyr
cd examples
mise exec -- west update
mise exec -- west build -b native_sim basic_zephyr -d basic_zephyr/build -p always
mise exec -- west build -t run -d basic_zephyr/build
```
