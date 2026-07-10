cmake_minimum_required(VERSION 3.20)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/nanofix-version.cmake)

project(nanofix VERSION ${NANOFIX_VERSION_BASE} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    string(REPLACE "-O2" "-O3"
        CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
endif()

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(_nanofix_default_build ON)
else()
    set(_nanofix_default_build OFF)
endif()

option(NANOFIX_BUILD
    "Build tests, integration examples, benchmarks and CLI utilities"
    ${_nanofix_default_build})
option(NANOFIX_BUILD_DOCS "Build Doxygen documentation" OFF)
option(NANOFIX_BUILD_FIXSPEC_GEN
    "Build the fixspec-gen utility (needed by nanofix_generate)"
    ${NANOFIX_BUILD})
option(NANOFIX_NATIVE_ARCH "Tune codegen for the host CPU (-march=native / -mcpu=native)" ON)
option(NANOFIX_LTO "Enable full link-time optimization" ON)
option(NANOFIX_SANITIZE
    "Enable AddressSanitizer + UndefinedBehaviorSanitizer (Debug builds)" OFF)
option(NANOFIX_SANITIZE_THREAD
    "Enable ThreadSanitizer (Debug builds). Mutually exclusive with NANOFIX_SANITIZE." OFF)
option(NANOFIX_DISABLE_SIMD
    "Compile out NEON / AVX2 paths; force scalar fallback everywhere." OFF)

if(NANOFIX_SANITIZE AND NANOFIX_SANITIZE_THREAD)
    message(FATAL_ERROR
        "Pick at most one of NANOFIX_SANITIZE / NANOFIX_SANITIZE_THREAD; they are incompatible.")
endif()

if(NOT CMAKE_BUILD_TYPE)
    if(NANOFIX_SANITIZE OR NANOFIX_SANITIZE_THREAD)
        set(CMAKE_BUILD_TYPE Debug)
    else()
        set(CMAKE_BUILD_TYPE Release)
    endif()
endif()

if(NANOFIX_SANITIZE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(WARNING "NANOFIX_SANITIZE requested but unsupported on ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

if(NANOFIX_SANITIZE_THREAD)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    else()
        message(WARNING "NANOFIX_SANITIZE_THREAD requested but unsupported on ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

if(NANOFIX_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _nanofix_ipo_ok OUTPUT _nanofix_ipo_err)
    if(_nanofix_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
            add_compile_options(-flto=full)
            add_link_options(-flto=full)
        endif()
    else()
        message(WARNING "LTO requested but not supported: ${_nanofix_ipo_err}")
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

add_library(nanofix INTERFACE)
add_library(nanofix::nanofix ALIAS nanofix)
# Generated dir first: its version.hpp must shadow the committed stub.
target_include_directories(nanofix INTERFACE
    $<BUILD_INTERFACE:${NANOFIX_VERSION_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(NANOFIX_DISABLE_SIMD)
    target_compile_definitions(nanofix INTERFACE NANOFIX_DISABLE_SIMD=1)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    # x86-64 baseline is AVX2; config.hpp #errors without __AVX2__. Build
    # interface only — the installed package applies it in nanofix-config,
    # at consumer configure time, against the consumer's compiler.
    if(MSVC)
        target_compile_options(nanofix INTERFACE $<BUILD_INTERFACE:/arch:AVX2>)
    else()
        target_compile_options(nanofix INTERFACE $<BUILD_INTERFACE:-mavx2>)
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(nanofix INTERFACE /clang:-Wall /clang:-Wextra /clang:-Wpedantic)
    add_compile_options($<$<CONFIG:RelWithDebInfo>:/clang:-fno-omit-frame-pointer>)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(nanofix INTERFACE -Wall -Wextra -Wpedantic)
    add_compile_options($<$<CONFIG:RelWithDebInfo>:-fno-omit-frame-pointer>)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(nanofix INTERFACE /W4 /WX)
endif()

include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# After the directory copy, so the generated version.hpp replaces the stub.
install(FILES ${NANOFIX_VERSION_INCLUDE_DIR}/nanofix/detail/version.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nanofix/detail)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/nanofix-generate.cmake)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/cmake/nanofix-generate.cmake
        DESTINATION ${CMAKE_INSTALL_DATADIR}/nanofix/cmake)

install(TARGETS nanofix EXPORT nanofix-targets)
install(EXPORT nanofix-targets
        FILE nanofix-targets.cmake
        NAMESPACE nanofix::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nanofix)

include(CMakePackageConfigHelpers)
set(NANOFIX_GENERATE_MODULE ${CMAKE_INSTALL_DATADIR}/nanofix/cmake/nanofix-generate.cmake)
configure_package_config_file(cmake/nanofix-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/nanofix-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nanofix
    PATH_VARS NANOFIX_GENERATE_MODULE)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/nanofix-config-version.cmake
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/nanofix-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/nanofix-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nanofix)

add_subdirectory(utils)

if(NANOFIX_BUILD)
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(benchmarks)
endif()

if(NANOFIX_BUILD_DOCS)
    add_subdirectory(docs)
endif()
