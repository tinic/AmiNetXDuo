#!/usr/bin/env bash
# Reproduce a compiler-runtime archive member dropped before a libcall is
# synthesized by m68k LTRANS.  No emulator or Amiga ROM is needed.

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
HERE="$ROOT/tests/toolchain/lto-late-libcall"
CC=${CC:-m68k-amigaos-gcc}
AR=${AR:-m68k-amigaos-gcc-ar}
NM=${NM:-m68k-amigaos-gcc-nm}
OBJDUMP=${OBJDUMP:-m68k-amigaos-objdump}
OUT=$(mktemp -d "${TMPDIR:-/tmp}/anxd-lto-libcall.XXXXXX")
trap 'rm -rf "$OUT"' EXIT

"$CC" -m68000 -Os -flto -c "$HERE/runtime.c" -o "$OUT/runtime.o"
"$AR" rc "$OUT/libruntime.a" "$OUT/runtime.o"
"$CC" -m68000 -Os -flto -DKEEP_LATE_LIBCALL -c "$HERE/runtime.c" \
    -o "$OUT/runtime-used.o"
"$AR" rc "$OUT/libruntime-used.a" "$OUT/runtime-used.o"

one() { # tag, caller flags, extra link flags, runtime archive
    local tag="$1" caller_flags="$2" link_flags="$3" archive="$4"
    local body probe marker call mul_addr

    # Intentional word splitting: these are fixed flag lists above/below, not
    # user input, and GCC needs each word as a separate argument.
    # shellcheck disable=SC2086
    "$CC" -m68000 -Os $caller_flags -c "$HERE/caller.c" -o "$OUT/$tag.o"
    # shellcheck disable=SC2086
    "$CC" -m68000 -Os -flto $link_flags -nostartfiles -Wl,--gc-sections \
        -Wl,-u,_probe "$OUT/$tag.o" "$OUT/$archive" -o "$OUT/$tag"

    echo "[$tag]"
    "$NM" -an "$OUT/$tag" | grep -E '(_probe|___muldi3)$'
    body=$("$OBJDUMP" -d --disassemble=___muldi3 "$OUT/$tag")
    if grep -q '1234 5678' <<<"$body"; then
        marker=present
    else
        marker=absent
    fi
    # The broken link aliases ___muldi3 to the next linker symbol, so objdump
    # labels the call with that symbol instead.  Match its numeric destination.
    mul_addr=$("$NM" -an "$OUT/$tag" | awk '
        $3 == "___muldi3" { sub(/^0+/, "", $1); print $1; exit }')
    probe=$("$OBJDUMP" -d "$OUT/$tag")
    if [ -n "$mul_addr" ] &&
       grep -Eq "(jsr|bsr)[[:space:]]+$mul_addr([[:space:]]|$)" <<<"$probe"; then
        call=late-call
    else
        call=open-coded
    fi
    echo "stand_in_body=$marker"
    echo "multiply_lowering=$call"
    printf '%s\n' "$probe" | sed -n '1,18p'
    printf 'OUTCOME=%s,%s\n' "$marker" "$call"
}

"$CC" --version | sed -n '1p'

control=$(one control "-fno-lto" "" libruntime.a | tee /dev/stderr |
          sed -n 's/^OUTCOME=//p')
broken=$(one broken "-flto" "" libruntime.a | tee /dev/stderr |
         sed -n 's/^OUTCOME=//p')
used=$(one used "-flto" "" libruntime-used.a | tee /dev/stderr |
       sed -n 's/^OUTCOME=//p')
forced=$(one forced "-flto" "-Wl,-u,___muldi3" libruntime.a |
         tee /dev/stderr | sed -n 's/^OUTCOME=//p')

if [ "${control%%,*}" != present ] || [ "${used%%,*}" != present ] ||
   [ "${forced%%,*}" != present ]; then
    echo "RESULT=invalid_control"
    exit 2
fi

if [ "$broken" = absent,late-call ]; then
    echo "RESULT=reproduced"
else
    echo "RESULT=toolchain_fixed"
fi
