cmake_minimum_required(VERSION 3.20)

project(nanofix-fuzz LANGUAGES CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/nanofix-version.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "nanofix-fuzz requires LLVM Clang with the libFuzzer runtime. "
        "(got '${CMAKE_CXX_COMPILER_ID}').")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# libFuzzer wants `-O1 -g` plus the sanitizer instrumentation; pair it with
# AddressSanitizer + UndefinedBehaviorSanitizer so every fuzz iteration also
# checks for OOB, UAF, signed overflow, alignment, and bad casts.
set(_fuzz_flags
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
    -fno-sanitize-recover=undefined
    -g
    -O1)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    # x86-64 baseline is AVX2 (nanofix config.hpp #errors without __AVX2__).
    list(APPEND _fuzz_flags -mavx2)
endif()

add_executable(fuzz_reader fuzz_reader.cpp)
target_include_directories(fuzz_reader PRIVATE
    "${NANOFIX_VERSION_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")

# Fail fast on a tripped NANOFIX_ASSERT so the sanitizer runtime reports it as a
# crash (a nanofix invariant violated on fuzzer input is a bug to surface).
target_compile_definitions(fuzz_reader PRIVATE NANOFIX_ASSERT_FAILFAST)
target_compile_options(fuzz_reader PRIVATE ${_fuzz_flags})
target_link_options(fuzz_reader PRIVATE ${_fuzz_flags})

set(_dataset_dir "${CMAKE_CURRENT_BINARY_DIR}/dataset")
set(_dataset_stamp "${CMAKE_CURRENT_BINARY_DIR}/dataset.stamp")
set(_seed_src "${CMAKE_CURRENT_SOURCE_DIR}/../tests/data")

file(GLOB _seed_files CONFIGURE_DEPENDS "${_seed_src}/*")

add_custom_command(
    OUTPUT "${_dataset_stamp}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_dataset_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${_seed_src}" "${_dataset_dir}"
    COMMAND ${CMAKE_COMMAND} -E touch "${_dataset_stamp}"
    DEPENDS ${_seed_files}
    VERBATIM)

add_custom_target(fuzz_dataset DEPENDS "${_dataset_stamp}")

add_custom_target(fuzz
    COMMAND ${CMAKE_COMMAND} -E echo "Running fuzz_reader (Ctrl+C to stop)"
    COMMAND $<TARGET_FILE:fuzz_reader>
            -print_final_stats=1
            -timeout=10
            -dict=${CMAKE_CURRENT_SOURCE_DIR}/fix.dict
            -max_total_time=$<IF:$<BOOL:${NANOFIX_FUZZ_MAX_TIME}>,${NANOFIX_FUZZ_MAX_TIME},60>
            ${_dataset_dir}
    DEPENDS fuzz_reader fuzz_dataset
    USES_TERMINAL
    VERBATIM)
