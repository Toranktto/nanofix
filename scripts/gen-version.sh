#!/bin/sh
# Version from git: v1.2.3 -> 1.2.3, v1.2.3-5-gabc1234 -> 1.2.3+5.gabc1234,
# no tag -> 0.0.0+g<sha>, no git -> $NANOFIX_VERSION_OVERRIDE or 0.0.0.
# Keep in sync with cmake/nanofix-version.cmake and conanfile.py.
#
#   gen-version.sh          write include/nanofix/detail/version.hpp
#   gen-version.sh --print  print the version
#   gen-version.sh --check  fail if the header on disk does not match git
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
HEADER=$ROOT/include/nanofix/detail/version.hpp

if [ -n "${NANOFIX_VERSION_OVERRIDE:-}" ]; then
    FULL=$NANOFIX_VERSION_OVERRIDE
elif git -C "$ROOT" rev-parse --git-dir > /dev/null 2>&1; then
    if DESC=$(git -C "$ROOT" describe --tags --match 'v[0-9]*' 2> /dev/null); then
        FULL=$(printf '%s' "$DESC" \
            | sed -E 's/^v//; s/-([0-9]+)-g([0-9a-f]+)$/+\1.g\2/')
    else
        FULL="0.0.0+g$(git -C "$ROOT" rev-parse --short HEAD)"
        echo "warning: no reachable v* tag; version $FULL" >&2
    fi
else
    FULL=0.0.0
    echo "warning: not a git checkout; version $FULL" >&2
fi

if ! printf '%s' "$FULL" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+'; then
    echo "error: cannot parse version '$FULL'" >&2
    exit 1
fi
BASE=$(printf '%s' "$FULL" | grep -Eo '^[0-9]+\.[0-9]+\.[0-9]+')
MAJOR=${BASE%%.*}
REST=${BASE#*.}
MINOR=${REST%%.*}
PATCH=${REST#*.}

generate() {
    cat << EOF
// Generated from git describe (v* tags) — do not edit; see scripts/gen-version.sh.
#pragma once

#define NANOFIX_VERSION_MAJOR $MAJOR
#define NANOFIX_VERSION_MINOR $MINOR
#define NANOFIX_VERSION_PATCH $PATCH
#define NANOFIX_VERSION "$FULL"
EOF
}

case ${1:-} in
--print)
    printf '%s\n' "$FULL"
    ;;
--check)
    if [ ! -f "$HEADER" ]; then
        echo "error: $HEADER missing; run a CMake configure or scripts/gen-version.sh" >&2
        exit 1
    fi
    TMP=$(mktemp)
    trap 'rm -f "$TMP"' EXIT
    generate > "$TMP"
    if ! diff -u "$HEADER" "$TMP"; then
        echo "error: include/nanofix/detail/version.hpp does not match git describe" >&2
        exit 1
    fi
    echo "version.hpp in sync with git ($FULL)"
    ;;
*)
    generate > "$HEADER"
    echo "wrote include/nanofix/detail/version.hpp ($FULL)"
    ;;
esac
