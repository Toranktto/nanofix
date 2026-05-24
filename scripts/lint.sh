#!/bin/sh
# Run clang-tidy over the library and the CLI tools, using the build's
# compile_commands.json. The library header is linted through the .cpp
# TUs that include it; HeaderFilterRegex in .clang-tidy bounds diagnostics
# to include/nanofix.hpp (generated headers and third-party code excluded).
# Pass a build dir as $1 or via BUILD_DIR.
#
#   scripts/lint.sh [build-dir]
#
# Scope is every translation unit we own: the CLI tools, the tests, and the
# benchmarks.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${1:-${BUILD_DIR:-build}}
CLANG_TIDY=${CLANG_TIDY:-clang-tidy}

if ! command -v "$CLANG_TIDY" >/dev/null 2>&1; then
    echo "error: $CLANG_TIDY not found (set CLANG_TIDY)" >&2
    exit 1
fi
if [ ! -d "$ROOT/$BUILD_DIR" ]; then
    echo "error: build dir '$BUILD_DIR' does not exist; configure it first" \
         "(cmake --preset conan-release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)" >&2
    exit 1
fi
if [ ! -f "$ROOT/$BUILD_DIR/compile_commands.json" ]; then
    echo "error: $BUILD_DIR/compile_commands.json missing; configure with" \
         "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
    exit 1
fi
cd "$ROOT"
TUS=$(find utils tests benchmarks -type f -name '*.cpp' | sort)

status=0
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
for tu in $TUS; do
    echo ">> clang-tidy $tu"
    # -extra-arg=-w: clang-tidy must not replay the build's compiler warnings.
    # clang-diagnostic-* is not in our enabled checks (.clang-tidy starts with
    # -*), and the build/test job already compiles with -Wall -Wextra
    # -Wpedantic, so the only effect here would be tens of thousands of
    # "N warnings generated" noise lines from system / generated headers. The
    # tidy checks themselves are unaffected by -w.
    if ! "$CLANG_TIDY" -p "$BUILD_DIR" --quiet --extra-arg=-w "$tu" >"$tmp" 2>&1; then
        status=1
    fi
    # Drop clang's bulk-count summary lines; real diagnostics pass through.
    grep -vE '^[0-9]+ (warning|error)s? generated\.$|^Suppressed [0-9]+ warnings? ' "$tmp" || true
done
exit $status
