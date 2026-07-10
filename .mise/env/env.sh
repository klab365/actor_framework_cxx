#!/usr/bin/env bash
# Extracts and exports paths from the installed Zephyr SDK.
# Sourced by mise via _.source in mise.toml (requires tools to be active).

ZEPHYR_SDK_INSTALL_DIR="$(mise where github:zephyrproject-rtos/sdk-ng)"
export ZEPHYR_SDK_INSTALL_DIR

for env_setup in "$ZEPHYR_SDK_INSTALL_DIR"/environment-setup-*-pokysdk-linux; do
  if [[ -f "$env_setup" ]]; then
    # shellcheck source=/dev/null
    source "$env_setup"
  fi
done

# Add each configured toolchain's bin directory to PATH.
for toolchain in $ZEPHYR_SDK_TOOLCHAINS; do
  toolchain_bin="$ZEPHYR_SDK_INSTALL_DIR/$toolchain/bin"
  if [[ -d "$toolchain_bin" ]]; then
    export PATH="$toolchain_bin:$PATH"
  fi
done
