# Basic Zephyr IPC example

A minimal Zephyr app that enables this repository as a Zephyr module and uses
`CONFIG_ACTOR` to build the IPC framework.

This directory contains a `west.yml`, so it can create a local Zephyr
workspace under `examples/` directly from this checkout:

```sh
mise exec -- west init -l examples/basic_zephyr
cd examples
mise exec -- west update
mise exec -- west build -b native_sim basic_zephyr -d basic_zephyr/build -p always
mise exec -- west build -t run -d basic_zephyr/build
```
