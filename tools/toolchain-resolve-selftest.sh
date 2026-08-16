#!/usr/bin/env bash
#
# Prove that a cache holding the wrong toolchain cannot be used quietly.
#
#   tools/toolchain-resolve-selftest.sh
#
# tools/amiga-toolchain.sh and cmake/toolchain-m68k-amigaos.cmake used to take
# <cache>/current on the "it runs" test alone.  That symlink is written by
# tools/fetch-toolchain.sh when it installs, so a cache that already held the
# pinned tree kept whatever was current last, and nothing compared the two.
#
# The self-hosted emulator runner held eabb6789378f (the pin, GCC 16.2.0b) and
# c63033fd4473 (GCC 15.2) side by side with `current` on the older one.  Every
# cross build there ran under GCC 15.2, and the only symptom was
# tools/gen-developer.sh --check calling the committed headers stale.  The arm
# was red through v0.22.0, v0.22.1 and v0.23.0 on that.
#
# So the rule now is: the cache candidate is the pin, and a `current` that runs
# here and is not the pin is an error.  Both resolvers have to agree about it,
# which is what this checks.  It uses a fake cache, so it needs no toolchain,
# no network and no runner.
#
# Output is key=value plus an exit code.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
bad=0

say()  { printf '%s=%s\n' "$1" "$2"; }
fail() { printf 'FAIL %s\n' "$*" >&2; bad=$((bad + 1)); }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/anxd-tcsel.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# A root the resolvers accept: an executable that answers -dumpversion, plus
# the NDK headers they check for beyond the selection itself.
make_root() {
    local dir="$1" ver="$2"
    mkdir -p "$dir/bin" "$dir/m68k-amigaos/ndk-include/exec" \
             "$dir/m68k-amigaos/ndk-include/inline"
    printf '#!/bin/sh\n[ "$1" = -dumpversion ] && echo %s\nexit 0\n' \
        "$ver" > "$dir/bin/m68k-amigaos-gcc"
    chmod +x "$dir/bin/m68k-amigaos-gcc"
    : > "$dir/m68k-amigaos/ndk-include/exec/types.h"
    : > "$dir/m68k-amigaos/ndk-include/inline/dos.h"
}

CACHE="$TMP/cache"
mkdir -p "$CACHE"

# The real pin for this platform, named the way fetch-toolchain.sh names it,
# so the test cannot drift from the thing it is testing.
PIN=$(AMINETXDUO_TOOLCHAIN_CACHE="$CACHE" "$ROOT/tools/fetch-toolchain.sh" \
      --print-root 2>/dev/null || true)
if [ -z "$PIN" ]; then
    say toolchain_resolve_selftest SKIPPED
    say reason "no published toolchain asset for $(uname -s)/$(uname -m)"
    exit 0
fi

STALE="$CACHE/c0ffee000000"
make_root "$STALE" 15.2.0
ln -sfn "$STALE" "$CACHE/current"

resolve() {
    # Prints the resolved root on stdout, the diagnostic on fd 3, and returns
    # what a caller of `. tools/amiga-toolchain.sh` sees.
    AMINETXDUO_TOOLCHAIN_CACHE="$CACHE" AMIGA_TOOLCHAIN_QUIET=1 \
    AMIGA_TOOLCHAIN_ROOT="" AMIGA_GCC="" AMIGA_NDK="" AMIGA_SIZE="" \
    PATH="/usr/bin:/bin" HOME="$TMP/nohome" \
        bash -c ". '$ROOT/tools/amiga-toolchain.sh' || exit \$?
                 printf '%s\n' \"\$AMIGA_TOOLCHAIN_ROOT\"" 2>"$TMP/err.txt"
}

# ---- 1. the pin is not installed, and `current` is not a substitute --------

out=$(resolve); rc=$?
if [ "$rc" != 3 ]; then
    fail "a stale current with no pin installed resolved rc=$rc, root='$out'"
else
    grep -q "is not the toolchain this tree pins" "$TMP/err.txt" ||
        fail "the stale-cache diagnostic does not say what is wrong"
    grep -q "$PIN" "$TMP/err.txt" ||
        fail "the stale-cache diagnostic does not name the pin it wanted"
    grep -q "15.2.0" "$TMP/err.txt" ||
        fail "the stale-cache diagnostic does not name what it found"
fi
say stale_cache_without_pin "$( [ "$rc" = 3 ] && echo refused || echo USED )"

# ---- 2. the pin is installed and `current` still points elsewhere ----------
#
# The runner's actual state.  The pin wins on its own name, so the symlink
# being wrong costs nothing and the arm is green.

make_root "$PIN" 16.2.0b
out=$(resolve); rc=$?
if [ "$rc" != 0 ] || [ "$out" != "$PIN" ]; then
    fail "with the pin installed the resolver chose rc=$rc root='$out', want $PIN"
fi
say pin_beats_stale_current "$( [ "$out" = "$PIN" ] && echo pass || echo FAIL )"

# ---- 3. tools/fetch-toolchain.sh repairs its own symlink -------------------
#
# It owns `current`, it took the already-here exit without looking at it, and
# that is how the two trees came to disagree in the first place.

# AMINETXDUO_TOOLCHAIN_URL is a tripwire, not a fixture: case 2 put a root at
# the pin, so this must take the already-here exit and never reach the network.
# Without it, a fake root that failed to appear downloads 200 MB in the host
# CI stage instead of failing the case.
AMINETXDUO_TOOLCHAIN_CACHE="$CACHE" \
AMINETXDUO_TOOLCHAIN_URL="file:///nonexistent-this-must-not-be-fetched" \
    "$ROOT/tools/fetch-toolchain.sh" >/dev/null 2>&1
have=$(cd "$CACHE/current" 2>/dev/null && pwd -P || echo none)
want=$(cd "$PIN" && pwd -P)
[ "$have" = "$want" ] || fail "fetch-toolchain.sh left current at $have, want $want"
say fetch_repoints_current "$( [ "$have" = "$want" ] && echo pass || echo FAIL )"

# ---- 4. CMake says the same thing ------------------------------------------
#
# The two resolvers are meant to be one rule in two languages.  A check that
# covers only the shell half covers half the builds.

CMAKE=$(command -v cmake 2>/dev/null || true)
if [ -n "$CMAKE" ]; then
    # Back to case 1: no pin installed, `current` on the older tree.  Case 3
    # left the symlink pointing at the pin, so it is put back explicitly --
    # a dangling `current` is a different state and tests nothing.
    rm -rf "$PIN"
    ln -sfn "$STALE" "$CACHE/current"
    mkdir -p "$TMP/proj"
    printf 'cmake_minimum_required(VERSION 3.16)\nproject(tcsel C)\n' \
        > "$TMP/proj/CMakeLists.txt"
    # No PATH scrub here: the configure needs cmake's own tree, and the point
    # of the case is what the toolchain file does with the cache, not what it
    # finds on $PATH -- entry 3 is only reached if entry 2 lets it be.
    env -u AMIGA_TOOLCHAIN_ROOT \
        AMINETXDUO_TOOLCHAIN_CACHE="$CACHE" HOME="$TMP/nohome" \
        "$CMAKE" -S "$TMP/proj" -B "$TMP/proj/build" \
              -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-m68k-amigaos.cmake" \
              > "$TMP/cmake.txt" 2>&1
    crc=$?
    if [ "$crc" = 0 ]; then
        fail "cmake configured against a stale cache instead of refusing"
    elif ! grep -q "is not the toolchain this tree pins" "$TMP/cmake.txt"; then
        fail "cmake refused for some other reason: $(tail -3 "$TMP/cmake.txt" | tr '\n' ' ')"
    fi
    say cmake_refuses_stale_cache \
        "$( grep -q "is not the toolchain this tree pins" "$TMP/cmake.txt" \
            && echo pass || echo FAIL )"
else
    say cmake_refuses_stale_cache SKIPPED
fi

say toolchain_resolve_errors "$bad"
if [ "$bad" -eq 0 ]; then say toolchain_resolve_selftest PASS; exit 0; fi
say toolchain_resolve_selftest FAIL
exit 1
