cmake_minimum_required(VERSION 3.20)
project(fix44_gateway CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Standalone build path: find nanofix via Conan. In-tree path: parent
# CMake already exposes nanofix::nanofix and nanofix_generate().
if(NOT TARGET nanofix::nanofix)
    find_package(nanofix REQUIRED)
endif()

add_executable(fix44_gateway src/main.cpp)
target_link_libraries(fix44_gateway PRIVATE nanofix::nanofix)

nanofix_generate(
    TARGET   fix44_gateway
    SPEC_XML ${CMAKE_CURRENT_SOURCE_DIR}/fixspec/FIX44.xml)
