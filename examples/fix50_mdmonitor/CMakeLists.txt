cmake_minimum_required(VERSION 3.20)
project(fix50_mdmonitor CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT TARGET nanofix::nanofix)
    find_package(nanofix REQUIRED)
endif()

add_executable(fix50_mdmonitor src/main.cpp)
target_link_libraries(fix50_mdmonitor PRIVATE nanofix::nanofix)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        target_compile_options(fix50_mdmonitor PRIVATE -mcpu=native)
    else()
        target_compile_options(fix50_mdmonitor PRIVATE -march=native)
    endif()
endif()
