#!/usr/bin/env bash
#
# Resolve the m68k-amigaos cross toolchain.  Source this, do not run it:
#
#   . "$ROOT/tools/amiga-toolchain.sh"     # exports AMIGA_TOOLCHAIN_ROOT,
#                                          # AMIGA_GCC, AMIGA_NDK, AMIGA_SIZE
#
# Every script in the tree used to hardcode $HOME/amigaos/tools/m68k-amigaos-gcc
# in its own copy of the same two lines, so a clean checkout on any machine but
# one built nothing.  The search order here is the same one
# cmake/toolchain-m68k-amigaos.cmake uses, so a shell script and a CMake
# configure never disagree about which compiler they are using.
#
# Order, first hit wins:
#   1. $AMIGA_TOOLCHAIN_ROOT, explicit, always wins
#   2. the PINNED tree in the fetch cache, tools/fetch-toolchain.sh --print-root
#   3. m68k-amigaos-gcc on $PATH, a container or a module load
#   4. /opt/m68k-amigaos, the amigadev/crosstools layout
#   5. $HOME/amigaos/tools/m68k-amigaos-gcc, the historical local default
#
# 2 through 5 have to RUN on this host to be chosen, not merely exist: the
# fetch cache is a linux/amd64 tree and can be populated on a machine that
# cannot execute it.
#
# THE CACHE CANDIDATE IS THE PIN, NOT <cache>/current.
#
#   It used to be <cache>/current, on the "it runs" test alone.  That symlink
#   is written by tools/fetch-toolchain.sh when it INSTALLS, so a cache that
#   already holds the pinned tree -- put there by some other route, or fetched
#   before a pin bump moved the directory -- leaves it addressing whatever was
#   current last.  Nothing compared it to anything.  On the self-hosted
#   emulator runner it pointed at c63033fd4473 (GCC 15.2) while eabb6789378f
#   (GCC 16.2.0b) sat in the same directory unused, so every build and every
#   generator ran under the wrong compiler and the only symptom was
#   tools/gen-developer.sh --check reporting the committed headers stale.  That
#   arm was red from 2026-08-14 through three releases on a wrong answer, which
#   is the failure a hash pin exists to prevent.
#
#   The pin names its own directory (<cache>/<sha12>), so ask for that and a
#   stale symlink cannot be mistaken for it.  A <cache>/current that runs here
#   and is NOT the pin is then an error, not a fallback: the cache is this
#   project's, something wrote a toolchain we did not ask for into it, and
#   ranking around it quietly is how the wrong answer got used the first time.
#
# Set AMIGA_TOOLCHAIN_QUIET=1 to suppress the "==> toolchain:" line.
#
# SPDX-License-Identifier: MIT

_ami_tc_ok() {
    [ -n "$1" ] && [ -x "$1/bin/m68k-amigaos-gcc" ]
}

# For a SEARCHED candidate, being executable is not enough, it has to be
# executable HERE.  tools/fetch-toolchain.sh caches a linux/amd64 tree, and it
# will happily do so on a Mac (the headers and the pin are still useful), which
# left the cache outranking a perfectly good native install and every cross
# build failing on a compiler that cannot start.  The +x bit does not mean what
# it looks like it means across architectures; asking the thing for its version
# does.
_ami_tc_runnable() {
    _ami_tc_ok "$1" && "$1/bin/m68k-amigaos-gcc" -dumpversion >/dev/null 2>&1
}

_ami_tc_here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

_ami_tc_resolve() {

    local cache candidate onpath pin have

    # An explicit AMIGA_TOOLCHAIN_ROOT is an instruction, not a suggestion: it
    # is taken as given and never silently swapped for something else.  If it
    # is the wrong architecture the build says so, which is the right kind of
    # failure for a setting someone typed on purpose.
    if _ami_tc_ok "${AMIGA_TOOLCHAIN_ROOT:-}"; then
        printf '%s\n' "$AMIGA_TOOLCHAIN_ROOT"
        return 0
    fi

    cache="${AMINETXDUO_TOOLCHAIN_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aminetxduo/toolchain}"

    # Empty on a host with no published asset, where the pin does not name a
    # directory at all and the checks below have nothing to compare against.
    pin=$("$_ami_tc_here/fetch-toolchain.sh" --print-root 2>/dev/null || true)

    if [ -n "$pin" ] && _ami_tc_runnable "$pin"; then
        printf '%s\n' "$pin"
        return 0
    fi

    if [ -n "$pin" ] && _ami_tc_runnable "$cache/current"; then
        have=$(cd "$cache/current" && pwd -P)
        echo "" >&2
        echo "!! $cache/current is not the toolchain this tree pins." >&2
        echo "   want $pin" >&2
        echo "   got  $have  (GCC $("$cache/current/bin/m68k-amigaos-gcc" \
              -dumpversion 2>/dev/null || echo '?'))" >&2
        echo "" >&2
        echo "   Refusing to build with it.  A cache that answers for a" >&2
        echo "   toolchain it is not produces wrong output, not an error:" >&2
        echo "   the headers, the codegen and the library ABI all move." >&2
        echo "" >&2
        echo "       tools/fetch-toolchain.sh        # install the pin" >&2
        echo "       export AMIGA_TOOLCHAIN_ROOT=... # or say so on purpose" >&2
        return 3
    fi

    onpath=$(command -v m68k-amigaos-gcc 2>/dev/null || true)
    if [ -n "$onpath" ]; then
        # <root>/bin/m68k-amigaos-gcc -> <root>
        candidate=$(cd "$(dirname "$onpath")/.." && pwd)
        if _ami_tc_runnable "$candidate"; then
            printf '%s\n' "$candidate"
            return 0
        fi
    fi

    for candidate in "/opt/m68k-amigaos" "$HOME/amigaos/tools/m68k-amigaos-gcc"; do
        if _ami_tc_runnable "$candidate"; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

AMIGA_TOOLCHAIN_ROOT=$(_ami_tc_resolve)
_ami_tc_rc=$?

# 3 is the stale cache, which has already said what it is and what to do about
# it.  Printing the generic "no toolchain found" over the top would bury the
# one line that names the defect.
if [ "$_ami_tc_rc" = 3 ]; then
    unset _ami_tc_rc
    return 3 2>/dev/null || exit 3
fi

if [ "$_ami_tc_rc" != 0 ]; then
    unset _ami_tc_rc
    cat >&2 <<'EOF'
No m68k-amigaos cross toolchain found.

Fix it with one of:
  tools/fetch-toolchain.sh                 # download the pinned toolchain
  export AMIGA_TOOLCHAIN_ROOT=<path>       # point at one you already have

The root is the directory containing bin/m68k-amigaos-gcc and
m68k-amigaos/ndk-include.
EOF
    return 2 2>/dev/null || exit 2
fi
unset _ami_tc_rc

export AMIGA_TOOLCHAIN_ROOT
export AMIGA_GCC="${AMIGA_GCC:-$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-gcc}"
export AMIGA_NDK="${AMIGA_NDK:-$AMIGA_TOOLCHAIN_ROOT/m68k-amigaos/ndk-include}"
export AMIGA_SIZE="${AMIGA_SIZE:-$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size}"

if [ ! -d "$AMIGA_NDK" ]; then
    echo "!! $AMIGA_NDK does not exist, the NDK headers ship WITH the" >&2
    echo "   toolchain; a root without them cannot build this project." >&2
    return 2 2>/dev/null || exit 2
fi

if [ "${AMIGA_TOOLCHAIN_QUIET:-0}" != "1" ]; then
    echo "==> toolchain: $AMIGA_TOOLCHAIN_ROOT ($("$AMIGA_GCC" -dumpversion 2>/dev/null || echo '?'))"
fi
