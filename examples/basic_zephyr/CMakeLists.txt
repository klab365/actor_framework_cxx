cmake_minimum_required(VERSION 3.20.0)

# Let this example build directly from the repository checkout:
#   west build -b native_sim examples/basic_zephyr
# If the module is already provided by a west workspace, this extra entry is
# harmless; Zephyr de-duplicates modules by name.
list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_LIST_DIR}/../..")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ipc_basic_zephyr C)

target_sources(app PRIVATE src/main.c)
