# Zephyr module build for the IPC Actor Framework.
# This file is included by Zephyr when this repository is listed in
# ZEPHYR_EXTRA_MODULES or discovered by west as a module.

if(NOT CONFIG_ACTOR)
  return()
endif()

if(DEFINED ZEPHYR_CURRENT_MODULE_DIR)
  set(IPC_MODULE_DIR ${ZEPHYR_CURRENT_MODULE_DIR})
else()
  get_filename_component(IPC_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
endif()

zephyr_library_named(ipc)

zephyr_library_sources(
  ${IPC_MODULE_DIR}/src/ipc.c
  ${IPC_MODULE_DIR}/src/port/zephyr/zephyr_ipc_port.c
)

# Make <ipc.h> and the Zephyr Kconfig overlay available to applications
# that enable CONFIG_ACTOR.
zephyr_include_directories(
  ${IPC_MODULE_DIR}/src/port/zephyr
  ${IPC_MODULE_DIR}/include
)

zephyr_library_include_directories(
  ${IPC_MODULE_DIR}/src/port/zephyr
  ${IPC_MODULE_DIR}/include
  ${IPC_MODULE_DIR}/src
)
