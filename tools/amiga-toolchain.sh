#!/usr/bin/env bash
#
# Resolve the m68k-amigaos cross toolchain.  Source this, do not run it:
#   . "$ROOT/tools/amiga-toolchain.sh"     # exports AMIGA_TOOLCHAIN_ROOT,
#                                          # AMIGA_GCC, AMIGA_NDK, AMIGA_SIZE
#
# The search order is cmake/toolchain-m68k-amigaos.cmake's, so a shell script
# and a CMake configure never disagree.  First hit wins, and 2 through 5 have
# to RUN on this host to be chosen, not merely exist:
#   1. $AMIGA_TOOLCHAIN_ROOT, explicit, always wins
#   2. the pinned tree in the fetch cache, tools/fetch-toolchain.sh --print-root
#   3. m68k-amigaos-gcc on $PATH, a container or a module load
#   4. /opt/m68k-amigaos, the amigadev/crosstools layout
#   5. $HOME/amigaos/tools/m68k-amigaos-gcc, the historical local default
#
# The cache candidate is the pin's own directory (<cache>/<sha12>), never
# <cache>/current: one that runs here and is not the pin is an error, not a
# fallback.  Set AMIGA_TOOLCHAIN_QUIET=1 to suppress the "==> toolchain:" line.
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
        echo "       export AMIGA_TOOLCHAIN_ROOT=... # or choose one on purpose" >&2
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
# it.  Printing the generic "no toolchain found" over the top buries the one
# line that names the defect.
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
