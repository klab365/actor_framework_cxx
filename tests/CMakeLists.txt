# tests/CMakeLists.txt
#
# Three test binaries:
#   - ipc_unit_test: links ipc.c against the mock port (no threads).
#   - ipc_integration_test: links against the real POSIX port with threads.
#
# Tests live in tests/unit and tests/integration. Shared sources
# (mock port) live in tests/mocks.

# ── IPC core as an object library so we can relink it with the mock port ──

add_library(ipc_core OBJECT ${CMAKE_SOURCE_DIR}/src/ipc.c)
target_include_directories(ipc_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_options(ipc_core PRIVATE -Wall -Wextra)

# ── Mock port (used only by the unit test binary) ────────────────────────

add_library(mock_port STATIC mocks/mock_ipc_port.c)
target_include_directories(mock_port PUBLIC
    mocks
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(mock_port PUBLIC Threads::Threads)
target_compile_options(mock_port PRIVATE -Wall -Wextra)

# ── Unit tests ────────────────────────────────────────────────────────────

add_executable(ipc_unit_test
    unit/test_ipc_registry.cpp
    unit/test_ipc_send.cpp
    unit/test_ipc_query.cpp
    unit/test_ipc_lifecycle.cpp
)
target_link_libraries(ipc_unit_test PRIVATE
    ipc_core
    mock_port
    gtest_main
)
target_include_directories(ipc_unit_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_options(ipc_unit_test PRIVATE -Wall -Wextra)

add_test(NAME ipc_unit COMMAND ipc_unit_test)
