cmake_minimum_required(VERSION 3.20)
project(fix44_gateway CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT TARGET nanofix::nanofix)
    find_package(nanofix REQUIRED)
endif()

add_executable(fix44_gateway src/main.cpp)
target_link_libraries(fix44_gateway PRIVATE nanofix::nanofix)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        target_compile_options(fix44_gateway PRIVATE -mcpu=native)
    else()
        target_compile_options(fix44_gateway PRIVATE -march=native)
    endif()
endif()

nanofix_generate(
    TARGET   fix44_gateway
    SPEC_XML ${CMAKE_CURRENT_SOURCE_DIR}/fixspec/FIX44.xml)
