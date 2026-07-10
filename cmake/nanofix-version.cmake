get_filename_component(_nanofix_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(DEFINED NANOFIX_VERSION_OVERRIDE AND NOT "${NANOFIX_VERSION_OVERRIDE}" STREQUAL "")
    set(NANOFIX_VERSION_FULL "${NANOFIX_VERSION_OVERRIDE}")
else()
    find_package(Git QUIET)
    set(NANOFIX_VERSION_FULL "")
    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${_nanofix_root}" describe --tags --match "v[0-9]*"
            RESULT_VARIABLE _nanofix_rc
            OUTPUT_VARIABLE _nanofix_desc
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_nanofix_rc EQUAL 0)
            string(REGEX REPLACE "^v" "" NANOFIX_VERSION_FULL "${_nanofix_desc}")
            string(REGEX REPLACE "-([0-9]+)-g([0-9a-f]+)$" "+\\1.g\\2"
                NANOFIX_VERSION_FULL "${NANOFIX_VERSION_FULL}")
        else()
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" -C "${_nanofix_root}" rev-parse --short HEAD
                RESULT_VARIABLE _nanofix_rc
                OUTPUT_VARIABLE _nanofix_sha
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(_nanofix_rc EQUAL 0)
                set(NANOFIX_VERSION_FULL "0.0.0+g${_nanofix_sha}")
                message(WARNING
                    "nanofix: no reachable v* tag; version ${NANOFIX_VERSION_FULL}. "
                    "Tag a release (git tag -a v<X.Y.Z>) for real versions.")
            endif()
        endif()
    endif()
    if(NANOFIX_VERSION_FULL STREQUAL "")
        set(NANOFIX_VERSION_FULL "0.0.0")
        message(WARNING
            "nanofix: not a git checkout and NANOFIX_VERSION_OVERRIDE unset; "
            "version 0.0.0.")
    endif()
endif()

if(NOT NANOFIX_VERSION_FULL MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "nanofix: cannot parse version '${NANOFIX_VERSION_FULL}'")
endif()
set(NANOFIX_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(NANOFIX_VERSION_MINOR "${CMAKE_MATCH_2}")
set(NANOFIX_VERSION_PATCH "${CMAKE_MATCH_3}")
set(NANOFIX_VERSION_BASE
    "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")

# The generated header lives in the build tree, never in the source tree; the
# committed include/nanofix/detail/version.hpp is a 0.0.0+unknown stub for
# builds that bypass CMake. Callers put NANOFIX_VERSION_INCLUDE_DIR ahead of
# the source include dir so the generated header wins.
set(NANOFIX_VERSION_INCLUDE_DIR "${CMAKE_BINARY_DIR}/nanofix-generated/include")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/version.hpp.in"
    "${NANOFIX_VERSION_INCLUDE_DIR}/nanofix/detail/version.hpp"
    @ONLY)

unset(_nanofix_root)
unset(_nanofix_rc)
unset(_nanofix_desc)
unset(_nanofix_sha)
