cmake_minimum_required(VERSION 3.20)

project(upstream_benchmarks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Path to an upstream jamesdbrock/hffix include tree; must contain hffix.hpp.
# scripts/run.sh sets this to the cloned upstream-src.
set(NANOFIX_UPSTREAM_INCLUDE_DIR "" CACHE PATH
    "Path to upstream jamesdbrock/hffix include directory (must contain hffix.hpp).")
if(NOT NANOFIX_UPSTREAM_INCLUDE_DIR)
    message(FATAL_ERROR
        "upstream_benchmarks: set "
        "-DNANOFIX_UPSTREAM_INCLUDE_DIR=/path/to/upstream/include")
endif()
if(NOT EXISTS "${NANOFIX_UPSTREAM_INCLUDE_DIR}/hffix.hpp")
    message(FATAL_ERROR
        "NANOFIX_UPSTREAM_INCLUDE_DIR='${NANOFIX_UPSTREAM_INCLUDE_DIR}' "
        "does not contain hffix.hpp.")
endif()

# Shared dataset path with the main repo's nanofix_benchmarks. This project lives
# at compare2upstream/upstream-benchmarks/, so the repo's benchmarks/data is two
# levels up.
set(NANOFIX_BENCH_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../benchmarks/data"
    CACHE PATH "Directory holding .fix datasets")

option(NANOFIX_NATIVE_ARCH "Tune codegen for the host CPU" ON)
option(NANOFIX_LTO "Enable full link-time optimization" ON)

if(NANOFIX_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_err)
    if(_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
            add_compile_options(-flto=full)
            add_link_options(-flto=full)
        endif()
    else()
        message(WARNING "LTO requested but not supported: ${_ipo_err}")
    endif()
endif()

if(NANOFIX_NATIVE_ARCH)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            add_compile_options(-mcpu=native)
        else()
            add_compile_options(-march=native)
        endif()
    endif()
endif()

find_package(benchmark REQUIRED)

add_executable(upstream_benchmarks upstream_benchmarks.cpp)
target_link_libraries(upstream_benchmarks PRIVATE benchmark::benchmark)
target_include_directories(upstream_benchmarks PRIVATE
    "${NANOFIX_UPSTREAM_INCLUDE_DIR}")
target_compile_definitions(upstream_benchmarks PRIVATE
    NANOFIX_BENCH_DATA_DIR="${NANOFIX_BENCH_DATA_DIR}")
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(upstream_benchmarks PRIVATE /Zc:__cplusplus)
endif()
