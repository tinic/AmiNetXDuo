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
#   1. $AMIGA_TOOLCHAIN_ROOT              -- explicit, always wins
#   2. the fetch cache                    -- what tools/fetch-toolchain.sh made
#   3. m68k-amigaos-gcc on $PATH          -- a container or a module load
#   4. /opt/m68k-amigaos                  -- the amigadev/crosstools layout
#   5. $HOME/amigaos/tools/m68k-amigaos-gcc -- the historical local default
#
# Set AMIGA_TOOLCHAIN_QUIET=1 to suppress the "==> toolchain:" line.
#
# SPDX-License-Identifier: MIT

_ami_tc_ok() {
    [ -n "$1" ] && [ -x "$1/bin/m68k-amigaos-gcc" ]
}

_ami_tc_resolve() {

    local cache candidate onpath

    if _ami_tc_ok "${AMIGA_TOOLCHAIN_ROOT:-}"; then
        printf '%s\n' "$AMIGA_TOOLCHAIN_ROOT"
        return 0
    fi

    cache="${AMINETXDUO_TOOLCHAIN_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aminetxduo/toolchain}"
    if _ami_tc_ok "$cache/current"; then
        printf '%s\n' "$cache/current"
        return 0
    fi

    onpath=$(command -v m68k-amigaos-gcc 2>/dev/null || true)
    if [ -n "$onpath" ]; then
        # <root>/bin/m68k-amigaos-gcc -> <root>
        candidate=$(cd "$(dirname "$onpath")/.." && pwd)
        if _ami_tc_ok "$candidate"; then
            printf '%s\n' "$candidate"
            return 0
        fi
    fi

    for candidate in "/opt/m68k-amigaos" "$HOME/amigaos/tools/m68k-amigaos-gcc"; do
        if _ami_tc_ok "$candidate"; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

if ! AMIGA_TOOLCHAIN_ROOT=$(_ami_tc_resolve); then
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

export AMIGA_TOOLCHAIN_ROOT
export AMIGA_GCC="${AMIGA_GCC:-$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-gcc}"
export AMIGA_NDK="${AMIGA_NDK:-$AMIGA_TOOLCHAIN_ROOT/m68k-amigaos/ndk-include}"
export AMIGA_SIZE="${AMIGA_SIZE:-$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size}"

if [ ! -d "$AMIGA_NDK" ]; then
    echo "!! $AMIGA_NDK does not exist -- the NDK headers ship WITH the" >&2
    echo "   toolchain; a root without them cannot build this project." >&2
    return 2 2>/dev/null || exit 2
fi

if [ "${AMIGA_TOOLCHAIN_QUIET:-0}" != "1" ]; then
    echo "==> toolchain: $AMIGA_TOOLCHAIN_ROOT ($("$AMIGA_GCC" -dumpversion 2>/dev/null || echo '?'))"
fi
