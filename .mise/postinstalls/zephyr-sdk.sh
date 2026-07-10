#!/usr/bin/env bash
set -euo pipefail

# Activate the full mise tool environment from the project's mise.toml
# so that dependency tools (cmake, python, ...) are available in PATH
# without hardcoding versions here.
eval "$(mise env -s bash)"

ZEPHYR_SDK_INSTALL_DIR="$(mise where github:zephyrproject-rtos/sdk-ng)"
echo "Setting up Zephyr SDK toolchain(s) from ${ZEPHYR_SDK_INSTALL_DIR}..."

# 1) Cross-toolchain extraction (required).
#
# `setup.sh -t <toolchain>` downloads and extracts the per-target
# cross-toolchain tarball from the upstream release (e.g. the
# x86_64-zephyr-elf cross-gcc used to build native_sim firmware).
# `west build` only consumes these; it never looks at the host
# tools installed by `-h`.
for toolchain in $ZEPHYR_SDK_TOOLCHAINS; do
  "$ZEPHYR_SDK_INSTALL_DIR/setup.sh" -t "$toolchain"
done

# 2) Host tools + CMake package registration (optional, best-effort).
#
# `setup.sh -h` installs the Yocto userspace SDK (a Poky sysroot with
# a separate gcc/cmake/ninja for building userland apps against the
# SDK target). `setup.sh -c` registers `Zephyr-sdk` as a CMake user
# package so `find_package(Zephyr-sdk)` works without an explicit
# `ZEPHYR_SDK_INSTALL_DIR`.
#
# Neither of these is consumed by the Zephyr port, the
# `examples/zephyr_measurement/` sample, or `west build -b native_sim`
# — they're a convenience for users building Linux userland apps
# against the SDK target. So a failure here must not abort `mise
# install`: the postinstall has already done everything the project
# actually needs.
#
# We make the step best-effort for two reasons:
#
#   a) The Yocto host-tools relocator hard-depends on `/usr/bin/file`
#      (it's invoked by the relocator's shell wrapper to validate
#      ELF binaries). The `mcr.microsoft.com/devcontainers/base:ubuntu`
#      base image does not ship it. A user who only needs the Zephyr
#      cross-toolchain for `west build` shouldn't be forced to install
#      a 200-KB magic(5) tool to get `mise install` to succeed.
#
#   b) The host-tools `setup.sh` line runs the SDK's installer
#      with `&> /dev/null`, swallowing both stdout and stderr so a
#      failure surfaces only as the cryptic "Host tools installation
#      failed" banner + exit code 30. By capturing the output
#      ourselves we make the cause visible in the install log.
#
# All stdout/stderr from the host-tools step is captured in
# `${ZEPHYR_SDK_INSTALL_DIR}/.hosttools.log` for postmortem. The
# step is skipped (with a note) if the Yocto relocator's
# prerequisites aren't met.

# What host arch is this SDK bundle for? The bundle ships exactly
# one `zephyr-sdk-<arch>-hosttools-standalone-*.sh` whose filename
# embeds the arch — that's the arch the host-tools installer
# expects the host to be.
hosttools_sh="$( \
  find "$ZEPHYR_SDK_INSTALL_DIR" -maxdepth 1 -name 'zephyr-sdk-*-hosttools-standalone-*.sh' \
  -print -quit 2>/dev/null || true )"

if [[ -z "$hosttools_sh" ]]; then
  # No standalone installer in the bundle (e.g. a hand-rolled
  # toolchain-only install). Nothing to do.
  echo "Skipping host tools: no standalone installer found in bundle."
  echo
  exit 0
fi

# Host-arch sanity check: the standalone installer self-checks at
# startup and exits 1 if the host arch doesn't match the bundle's
# arch. With mise's per-platform `asset_pattern` (see mise.toml) this
# shouldn't happen, but if a user overrides the tool config and pulls
# the wrong bundle, fail loudly with a clear message instead of
# letting the cryptic "Incompatible SDK installer" bubble up as
# exit 30.
bundle_arch="$( \
  basename "$hosttools_sh" \
  | sed -nE 's/^zephyr-sdk-([^-]+)-hosttools-standalone-.*/\1/p' )"
case "$(uname -m)-${bundle_arch}" in
  x86_64-x86_64 | aarch64-aarch64 | armv7l-arm | i686-i686) ;;
  *)
    echo "Skipping host tools: bundle arch '${bundle_arch}' does not match host arch '$(uname -m)'." >&2
    echo "  (Re-check the [tools.\"github:zephyrproject-rtos/sdk-ng\".platforms] table in mise.toml.)" >&2
    echo
    exit 0
    ;;
esac

# Hard dep: the Yocto relocator's `relocate_sdk.py` invokes
# `/usr/bin file <elf>` on every binary in the sysroot. Without it
# the host-tools installer aborts partway through with exit 1,
# which the SDK's `setup.sh` reports as "Host tools installation
# failed" (exit 30).
if ! command -v file >/dev/null 2>&1; then
  echo "Skipping host tools: '/usr/bin/file' is not installed."
  echo "  The Yocto host-tools relocator requires it; install with"
  echo "  'sudo apt-get install file' (or your distro's equivalent)"
  echo "  and re-run 'mise install' to enable host tools."
  echo "  This does not affect Zephyr firmware builds — the cross-"
  echo "  toolchain above is all that 'west build' needs."
  echo
  exit 0
fi

# CMake package registration (`-c`) only needs `cmake`, which is
# already on PATH via mise.
if ! command -v cmake >/dev/null 2>&1; then
  echo "Skipping CMake package registration: 'cmake' is not on PATH."
  echo "  The Zephyr cross-toolchain above is unaffected."
  echo
  exit 0
fi

# Everything we need is present — run the host-tools step. Capture
# the full output (the SDK's own `setup.sh` would have hidden it)
# and treat any failure as a warning rather than aborting the
# install, per the rationale at the top of this file.
hosttools_log="$ZEPHYR_SDK_INSTALL_DIR/.hosttools.log"
echo "Installing host tools and registering Zephyr SDK CMake package..."
echo "  (full output: $hosttools_log)"
if "$ZEPHYR_SDK_INSTALL_DIR/setup.sh" -h -c >"$hosttools_log" 2>&1; then
  echo "Host tools installed."
else
  rc=$?
  echo "WARNING: host-tools installation failed (exit $rc); see $hosttools_log." >&2
  echo "  The Zephyr cross-toolchain above is unaffected. Re-run with" >&2
  echo "  'sudo apt-get install file' (or fix the underlying cause)" >&2
  echo "  to enable host tools." >&2
fi
echo
