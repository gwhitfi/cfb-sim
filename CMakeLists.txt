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

# ---------------------------------------------------------------------------
# Third-party dependencies, downloaded at configure time by FetchContent.
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)   # treat their headers as system headers: no warnings from them in our code
FetchContent_MakeAvailable(nlohmann_json)

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
    $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-fast-math>)

add_subdirectory(sim)
add_subdirectory(cli)

if(CFBSIM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
