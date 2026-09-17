#!/usr/bin/env bash
#
# bsdsocket.library's memcpy is net68k's, not libc's.
#
#   tools/check-memcpy-hook.sh [builddir]
#
# AMINETXDUO_NET68K_MEMCPY (src/net68k/CMakeLists.txt) resolves memcpy() to
# the movem.l copy, and for as long as the library linked with -flto it did
# not: GCC generates the memcpy calls after the archives were scanned, so
# libc's came in instead, and the 68000 libc's memcpy copies a byte at a time
# whenever a pointer is not longword aligned.  Every build from the first -flto
# one to 0.28.3 shipped that way and no gate could see it, because the link
# succeeds either way.  Profiled on a real A3000 it was 18.3% of the CPU
# during an http:// fetch (src/bsdsocket/CMakeLists.txt, at the -u).
#
# The linker's map, not the binary: the map names the archive member each
# symbol was taken from, which is exactly the question.  A map that is not
# there is not a pass.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD="${1:-${AMINETXDUO_BUILD:-$ROOT/build/cm}}"
case "$BUILD" in
    /*) ;;
    *)  BUILD="$ROOT/$BUILD" ;;
esac

MAP="$BUILD/src/bsdsocket/bsdsocket.library.map"
CACHE="$BUILD/CMakeCache.txt"

if [ ! -f "$CACHE" ]; then
    echo "memcpy_hook=skipped reason=no_cmake_cache build=$BUILD"
    exit 0
fi
if ! grep -q '^AMINETXDUO_NET68K_MEMCPY:BOOL=ON' "$CACHE"; then
    echo "memcpy_hook=skipped reason=option_off build=$BUILD"
    exit 0
fi
if [ ! -f "$MAP" ]; then
    echo "memcpy_hook=FAIL reason=no_map map=$MAP"
    echo "!! the option is on and there is no bsdsocket.library.map to read;" >&2
    echo "!! nothing was checked, which is not a pass." >&2
    exit 1
fi

# The member the symbol came from.  A `.text` line that places
# `libc.a(lib_a-memcpy.o)` is libc's copy in the image; the hook's object
# placed with a size, or named as the plugin's symbol, is ours.
LIBC=$(grep -c 'lib_a-memcpy\.o' "$MAP" || true)
HOOK=$(grep -c 'n68k_memcpy_hook' "$MAP" || true)

if [ "$LIBC" != "0" ] || [ "$HOOK" = "0" ]; then
    echo "memcpy_hook=FAIL libc_memcpy_lines=$LIBC hook_lines=$HOOK map=$MAP"
    echo "!! bsdsocket.library links libc's memcpy, not net68k's.  The link" >&2
    echo "!! option -Wl,-u,_memcpy in src/bsdsocket/CMakeLists.txt is what" >&2
    echo "!! pulls the hook in ahead of LTO; check it is still there." >&2
    exit 1
fi

echo "memcpy_hook=PASS hook_lines=$HOOK map=$MAP"
exit 0
