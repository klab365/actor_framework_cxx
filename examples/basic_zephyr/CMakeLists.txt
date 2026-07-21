cmake_minimum_required(VERSION 3.20.0)

# Let this example build directly from the repository checkout:
#   west build -b native_sim examples/basic_zephyr
# If the module is already provided by a west workspace, this extra entry is
# harmless; Zephyr de-duplicates modules by name.
list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_LIST_DIR}/../..")

# Ask Zephyr/binutils to print linker memory-region usage after the build.
# Keep this in CMake so the example always reports flash/ROM usage without
# requiring users to remember an extra `west build -t rom_report` command.
set(IPC_FLASH_USAGE_CONF "${CMAKE_CURRENT_BINARY_DIR}/ipc_flash_usage.conf")
file(WRITE "${IPC_FLASH_USAGE_CONF}" "CONFIG_OUTPUT_PRINT_MEMORY_USAGE=y\n")
list(APPEND EXTRA_CONF_FILE "${IPC_FLASH_USAGE_CONF}")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ipc_basic_zephyr C)

target_sources(app PRIVATE src/main.c)

# Print a compact ELF size summary as part of the normal build. In the output,
# flash/ROM usage is approximately `text + data`; RAM usage is `data + bss`.
find_program(IPC_SIZE_TOOL NAMES ${CROSS_COMPILE}size size)
if(IPC_SIZE_TOOL AND TARGET zephyr_pre0)
  add_custom_target(
    ipc_basic_size_summary ALL
    COMMAND ${CMAKE_COMMAND} -E echo "IPC basic Zephyr size summary (flash ~= text + data):"
    COMMAND ${IPC_SIZE_TOOL} $<TARGET_FILE:zephyr_pre0>
    DEPENDS zephyr_pre0
    VERBATIM)
endif()
