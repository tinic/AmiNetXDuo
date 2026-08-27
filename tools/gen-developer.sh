#!/usr/bin/env bash
#
# Regenerate the Developer drawer's compiler glue from its SFD.
#
#   tools/gen-developer.sh            # write developer/include/{clib,inline,proto,pragmas}
#   tools/gen-developer.sh --check    # regenerate into a temp dir and diff
#
# The generated headers are committed, because packaging the archive must not
# require sfdc.  --check is what stops them drifting from the SFD; ci.sh runs
# it whenever the resolved toolchain has an sfdc.
#
# sfdc stamps every header it writes with "Copyright (c) 2001 Amiga, Inc." and
# an unconditional "All Rights Reserved" line.  ==copyright replaces the first;
# the second is hardcoded in sfdc, and is stripped here rather than left to
# contradict the SPDX tag on a file that is ours.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# Two libraries, two SFDs, two header sets.  aminetxduo_lib.sfd is what
# bsdsocket.library adds past the end of the NDK's own SFD; tls_lib.sfd is the
# whole of tls.library, which had no machine-readable description at all and
# was therefore callable from GCC and from nothing else.
SFD="$ROOT/developer/sfd/aminetxduo_lib.sfd"
TLS_SFD="$ROOT/developer/sfd/tls_lib.sfd"

CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

SFDC="${SFDC:-}"
if [ -z "$SFDC" ]; then
    if [ -n "${AMIGA_TOOLCHAIN_ROOT:-}" ] && [ -x "$AMIGA_TOOLCHAIN_ROOT/bin/sfdc" ]; then
        SFDC="$AMIGA_TOOLCHAIN_ROOT/bin/sfdc"
    elif command -v sfdc >/dev/null 2>&1; then
        SFDC=$(command -v sfdc)
    fi
fi
if [ -z "$SFDC" ]; then
    echo "gen-developer: no sfdc found (set SFDC= or AMIGA_TOOLCHAIN_ROOT=)" >&2
    exit 3
fi

gen() {
    # gen <sfd> <mode> <dest root> <relative output path>
    local sfd="$1" mode="$2" out="$3$4"
    mkdir -p "$(dirname "$out")"
    "$SFDC" --quiet --target=m68k-amigaos --mode="$mode" "$sfd" \
        | sed '/^\*\*       All Rights Reserved$/d' > "$out"
}

emit() {
    gen "$SFD" clib    "$1" /clib/aminetxduo_protos.h
    gen "$SFD" macros  "$1" /inline/aminetxduo.h
    gen "$SFD" proto   "$1" /proto/aminetxduo.h
    gen "$SFD" pragmas "$1" /pragmas/aminetxduo_pragmas.h
    gen "$SFD" lvo     "$1" /lvo/aminetxduo_lib.i

    gen "$TLS_SFD" clib    "$1" /clib/tls_protos.h
    gen "$TLS_SFD" macros  "$1" /inline/tls.h
    gen "$TLS_SFD" proto   "$1" /proto/tls.h
    gen "$TLS_SFD" pragmas "$1" /pragmas/tls_pragmas.h
    gen "$TLS_SFD" lvo     "$1" /lvo/tls_lib.i
}

DEST="$ROOT/developer/include"

if [ "$CHECK" = 1 ]; then
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    emit "$tmp"
    if diff -ru "$DEST" "$tmp"; then
        echo "gen-developer: generated headers match $SFD"
    else
        echo "gen-developer: developer/include is stale, run tools/gen-developer.sh" >&2
        exit 1
    fi
else
    emit "$DEST"
    echo "gen-developer: wrote $DEST from $SFD"
fi
