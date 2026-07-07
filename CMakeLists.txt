cmake_minimum_required(VERSION 3.20)

# CMake 4.x dropped defaults for <3.5 syntax. Stay compatible with the
# googletest we fetch.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

project(ipc_framework LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Options ────────────────────────────────────────────────────────────────

option(IPC_BUILD_TESTS    "Build tests"  OFF)
option(IPC_BUILD_EXAMPLES "Build example applications"        OFF)

# Instrument the core library and test binaries with gcov coverage.
# Used by the SonarQube coverage job in CI; the resulting *.gcda are
# collected by gcovr and exported as coverage.xml. Off by default so
# release/host builds stay fast.
option(IPC_COVERAGE       "Enable gcov coverage instrumentation" OFF)

# Platform selection: posix (host) or zephyr (inside Zephyr build).
# When building inside Zephyr's CMake, IPC_PLATFORM is preset to "zephyr".
if(DEFINED ENV{ZEPHYR_BASE} OR DEFINED ZEPHYR_BASE)
    set(IPC_PLATFORM_DEFAULT "zephyr")
else()
    set(IPC_PLATFORM_DEFAULT "posix")
endif()

set(IPC_PLATFORM "${IPC_PLATFORM_DEFAULT}" CACHE STRING
    "IPC platform port: posix or zephyr")

if(NOT IPC_PLATFORM STREQUAL "posix" AND NOT IPC_PLATFORM STREQUAL "zephyr")
    message(FATAL_ERROR
        "IPC_PLATFORM must be 'posix' or 'zephyr' (got '${IPC_PLATFORM}')")
endif()

# ── Library target ─────────────────────────────────────────────────────────

add_library(ipc STATIC src/ipc.c)

target_include_directories(ipc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_options(ipc PRIVATE -Wall -Wextra)

if(IPC_COVERAGE)
    # Keep the coverage flags PRIVATE so they never leak to downstream
    # consumers via the ipc target's PUBLIC interface. The test binaries
    # opt in explicitly below.
    target_compile_options(ipc PRIVATE --coverage)
    target_link_options(ipc    PRIVATE --coverage)
endif()

if(IPC_PLATFORM STREQUAL "zephyr")
    target_sources(ipc PRIVATE src/port/zephyr/zephyr_ipc_port.c)
    # Zephyr port's ipc_config.h (CONFIG_ACTOR_* aware) shadows the public
    # include/ipc_defaults.h.  Listed FIRST so it wins in
    # #include "ipc_defaults.h" resolution when compiling the library.
    # (Downstream consumers don't see this path — they always get the
    # public defaults file via the PUBLIC include directory above.)
    target_include_directories(ipc PRIVATE
        src/port/zephyr
        src
    )
else()
    target_sources(ipc PRIVATE src/port/posix/posix_ipc_port.c)
    target_include_directories(ipc PRIVATE src)
    find_package(Threads REQUIRED)
    target_link_libraries(ipc PUBLIC Threads::Threads)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────

if(IPC_BUILD_TESTS)
    enable_testing()
    include(cmake/GTest.cmake)
    ipc_fetch_googletest()
    add_subdirectory(tests)
endif()

# ── Capacity / memory probe ───────────────────────────────────────────────

if(IPC_BUILD_PROBES)
    add_subdirectory(tests/probes)
endif()

# ── Examples ───────────────────────────────────────────────────────────────

if(IPC_BUILD_EXAMPLES)
    add_executable(led_actor_example
        examples/led_actor/main.c
        examples/led_actor/led_actor.c
        examples/led_actor/app_actor.c
        examples/led_actor/button_actor.c
    )
    target_include_directories(led_actor_example PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/led_actor
    )
    target_link_libraries(led_actor_example PRIVATE ipc)
    target_compile_options(led_actor_example PRIVATE -Wall -Wextra)
endif()
