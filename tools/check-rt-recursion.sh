#!/usr/bin/env bash
#
# No compiler runtime helper may compile into a call to itself.
#
#   tools/check-rt-recursion.sh <build-dir>
#
# src/common/ami_udivdi3.c must write every shift as a pair of 32-bit ones: a
# shift written wide lowers to a call to the helper being compiled, which then
# recurses until the stack is gone.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-}"

[ -n "$BUILD" ] || { echo "usage: check-rt-recursion.sh <build-dir>" >&2; exit 2; }

. "$ROOT/tools/amiga-toolchain.sh" >/dev/null 2>&1 || true
OBJDUMP="${AMIGA_TOOLCHAIN_ROOT:-}/bin/m68k-amigaos-objdump"
[ -x "$OBJDUMP" ] || OBJDUMP="$(command -v m68k-amigaos-objdump || true)"

if [ -z "$OBJDUMP" ] || [ ! -x "$OBJDUMP" ]; then
    echo "rt_recursion=skipped reason=no_objdump"
    exit 2
fi

OBJS=$(find "$BUILD" -name 'ami_udivdi3.c.obj' 2>/dev/null || true)
if [ -z "$OBJS" ]; then
    echo "rt_recursion=skipped reason=no_object build=$BUILD"
    exit 2
fi

rc=0
checked=0
for obj in $OBJS; do
    checked=$((checked + 1))

    # objdump prints `00000594 00000594 ___lshrdi3:` for a label and
    # `5d0: 4eba ffc2  jsr 594 594 ___lshrdi3(pc)` for a call.  A call naming
    # the function it appears inside is the defect.
    bad=$("$OBJDUMP" -d "$obj" | awk '
        /^[0-9a-f]+ [0-9a-f]+ [^ ]+:$/ { fn = $3; sub(/:$/, "", fn); next }
        fn != "" && (/\<jsr\>/ || /\<bsr/) && index($0, fn "(pc)") {
            printf "  %s calls itself: %s\n", fn, $0
        }')

    if [ -n "$bad" ]; then
        echo "rt_recursion=FAILED object=$obj"
        printf '%s\n' "$bad"
        rc=1
    fi
done

[ "$rc" = 0 ] && echo "rt_recursion=clean objects=$checked"
exit "$rc"
