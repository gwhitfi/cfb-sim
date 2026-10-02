# Root build file. CMake reads this first, then each add_subdirectory() file.
cmake_minimum_required(VERSION 3.25)

project(cfbsim
    VERSION 0.1.0
    DESCRIPTION "College football management sim: game sim core"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)           # -std=c++20, not -std=gnu++20
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # compile_commands.json for editors/clangd

option(CFBSIM_BUILD_TESTS "Build the GoogleTest suites" ON)
option(CFBSIM_BUILD_SERVER "Build the WebSocket server for the viewer" ON)

# ---------------------------------------------------------------------------
# Third-party dependencies, downloaded at configure time by FetchContent.
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)   # treat their headers as system headers: no warnings from them in our code
FetchContent_MakeAvailable(nlohmann_json)

if(CFBSIM_BUILD_SERVER)
    # The WebSocket library for server/. Local plain ws:// only: no TLS, no
    # compression, so it needs no other libraries.
    FetchContent_Declare(ixwebsocket
        URL https://github.com/machinezone/IXWebSocket/archive/refs/tags/v11.4.6.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)
    set(USE_TLS OFF CACHE BOOL "" FORCE)
    set(USE_ZLIB OFF CACHE BOOL "" FORCE)
    set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(ixwebsocket)
    # Its own sources warn with our flags (unused parameters with zlib off); not our code.
    target_compile_options(ixwebsocket PRIVATE -w)
endif()

if(CFBSIM_BUILD_TESTS)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)   # mocking library; not needed yet
    FetchContent_MakeAvailable(googletest)
endif()

# ---------------------------------------------------------------------------
# Shared compiler settings for our own targets (not third-party code).
# An INTERFACE library builds nothing; linking to it just passes these flags on.
# ---------------------------------------------------------------------------
add_library(cfbsim_options INTERFACE)
target_compile_options(cfbsim_options INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion>
    # Never -ffast-math: it lets the compiler reorder float math, which breaks
    # replay determinism (see docs/design/movement-and-interaction.md#determinism).
    $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-fast-math>
    # No fused multiply-add: a*b+c computed in one step rounds differently from
    # two steps, and compilers only fuse when the CPU supports it, so results
    # could change with the machine or -march flags.
    $<$<CXX_COMPILER_ID:GNU,Clang>:-ffp-contract=off>)

add_subdirectory(sim)
add_subdirectory(cli)
if(CFBSIM_BUILD_SERVER)
    add_subdirectory(server)
endif()

if(CFBSIM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
