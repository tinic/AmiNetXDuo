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
SFD="$ROOT/developer/sfd/aminetxduo_lib.sfd"

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
    # gen <mode> <relative output path>
    local mode="$1" out="$2$3"
    mkdir -p "$(dirname "$out")"
    "$SFDC" --quiet --target=m68k-amigaos --mode="$mode" "$SFD" \
        | sed '/^\*\*       All Rights Reserved$/d' > "$out"
}

emit() {
    gen clib    "$1" /clib/aminetxduo_protos.h
    gen macros  "$1" /inline/aminetxduo.h
    gen proto   "$1" /proto/aminetxduo.h
    gen pragmas "$1" /pragmas/aminetxduo_pragmas.h
    gen lvo     "$1" /lvo/aminetxduo_lib.i
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
