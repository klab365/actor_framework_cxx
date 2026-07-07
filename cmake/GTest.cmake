# cmake/GTest.cmake — Google Test bootstrap via FetchContent.
#
# Pin to a specific release so CI doesn't drift. Cached in build/_deps/
# after the first configure, so subsequent builds are offline.
include_guard(GLOBAL)

function(ipc_fetch_googletest)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endfunction()
