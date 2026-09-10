#!/usr/bin/env bash
#
# Shared handling for CMake build directories that follow the pinned Amiga
# compiler.  CMake cannot change compilers in place, while the project's
# `current` toolchain advances whenever the pin does.  CI build directories
# are disposable caches, so remove only CMake's generated configure state
# when it names another compiler; the caller then configures normally.
#
# SPDX-License-Identifier: MIT

refresh_cmake_compiler_cache() { # builddir wanted-compiler [label]
    local builddir="$1" wanted="$2" label="${3:-cmake}"
    local cached="" compiler_file

    [ -f "$builddir/CMakeCache.txt" ] || return 0

    cached=$(sed -n 's|^CMAKE_C_COMPILER:FILEPATH=||p' \
                    "$builddir/CMakeCache.txt" | head -1)

    # Modern CMake records the compiler itself in the generated platform file
    # and may put only gcc-ar/gcc-ranlib in CMakeCache.txt.
    if [ -z "$cached" ]; then
        for compiler_file in "$builddir"/CMakeFiles/*/CMakeCCompiler.cmake; do
            [ -f "$compiler_file" ] || continue
            cached=$(sed -n 's|^set(CMAKE_C_COMPILER "\(.*\)")$|\1|p' \
                         "$compiler_file" | head -1)
            [ -z "$cached" ] || break
        done
    fi

    if [ -n "$cached" ] && [ "$cached" != "$wanted" ]; then
        echo "$label: refreshing stale CMake compiler cache" >&2
        echo "  cached:   $cached" >&2
        echo "  selected: $wanted" >&2
        cmake -E remove -f "$builddir/CMakeCache.txt"
        cmake -E remove_directory "$builddir/CMakeFiles"
    fi
}
