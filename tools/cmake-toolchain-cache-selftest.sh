#!/usr/bin/env bash
# Exercise both decisions in cmake-toolchain-cache.sh without a real compiler.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$ROOT/tools/cmake-toolchain-cache.sh"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/anxd-cmake-cache.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

old=/toolchains/old/bin/m68k-amigaos-gcc
wanted=/toolchains/pinned/bin/m68k-amigaos-gcc

# Modern CMake keeps the actual compiler in CMakeCCompiler.cmake.  A mismatch
# must discard generated configure state while preserving neighboring logs.
stale="$WORK/stale"
mkdir -p "$stale/CMakeFiles/4.0.0"
printf '%s\n' '# generated cache' > "$stale/CMakeCache.txt"
printf 'set(CMAKE_C_COMPILER "%s")\n' "$old" \
    > "$stale/CMakeFiles/4.0.0/CMakeCCompiler.cmake"
printf '%s\n' keep > "$stale-configure.log"
refresh_cmake_compiler_cache "$stale" "$wanted" selftest
[ ! -e "$stale/CMakeCache.txt" ]
[ ! -e "$stale/CMakeFiles" ]
[ -f "$stale-configure.log" ]

# A cache already using the selected compiler is valid and must be untouched.
current="$WORK/current"
mkdir -p "$current/CMakeFiles"
printf 'CMAKE_C_COMPILER:FILEPATH=%s\n' "$wanted" \
    > "$current/CMakeCache.txt"
printf '%s\n' keep > "$current/CMakeFiles/marker"
refresh_cmake_compiler_cache "$current" "$wanted" selftest
[ -f "$current/CMakeCache.txt" ]
[ -f "$current/CMakeFiles/marker" ]

echo 'cmake_cache_selftest=PASS cases=2'
