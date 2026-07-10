// Committed stub for builds that bypass CMake (plain -I include). A CMake
// configure generates the real header from git into the build tree
// (cmake/nanofix-version.cmake); it shadows this file via include order and
// `cmake --install` ships it in place of this stub.
#pragma once

#define NANOFIX_VERSION_MAJOR 0
#define NANOFIX_VERSION_MINOR 0
#define NANOFIX_VERSION_PATCH 0
#define NANOFIX_VERSION "0.0.0+unknown"
