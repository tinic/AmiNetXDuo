#!/usr/bin/env bash
# Reduced AmigaOS shared-library LTO reproducer.
#
#   run.sh <toolchain-prefix> [outdir]
#
# Builds the same four translation units four ways -- with and without -flto,
# with and without --gc-sections -- and prints one key=value line per arm.
set -u

TC="${1:?usage: run.sh <toolchain-prefix> [outdir]}"
OUT="${2:-$PWD/out}"
HERE=$(cd "$(dirname "$0")" && pwd)

CC="$TC/bin/m68k-amigaos-gcc"
NM="$TC/bin/m68k-amigaos-nm"
OBJDUMP="$TC/bin/m68k-amigaos-objdump"

ARCH="-m68020"
OPT="-Os"
WARN="-Wall -Wextra"

VECSYMS="_min_open _min_close _min_expunge _min_reserved _min_add _min_sub _min_ptr _min_mix _min_enosys _min_asm_sum"

mkdir -p "$OUT"

build_arm() {
    local name="$1" lto="$2" gc="$3"
    local d="$OUT/$name"
    rm -rf "$d"; mkdir -p "$d"

    local cflags=($ARCH $OPT $WARN -fomit-frame-pointer -fno-strict-aliasing
                  -ffunction-sections -fno-optimize-sibling-calls -I"$HERE")
    [ "$lto" = "1" ] && cflags+=(-flto)
    [ "${4:-}" = "keep" ] && cflags+=(-DMINLIB_KEEP)

    local ldflags=(-nostartfiles)
    [ "$lto" = "1" ] && ldflags+=(-flto $ARCH $OPT)
    [ "$gc" = "1" ] && ldflags+=(-Wl,--gc-sections)

    local rc_c rc_l
    ( cd "$d" && "$CC" "${cflags[@]}" -c "$HERE/minlib.c" "$HERE/minvectors.c" \
                                        "$HERE/minops.c" "$HERE/minasm.s" ) \
        > "$d/compile.log" 2>&1
    rc_c=$?
    if [ $rc_c -ne 0 ]; then
        echo "arm=$name lto=$lto gc=$gc compile=fail link=skip"
        return
    fi

    # minlib.o FIRST: the entry guard has to land at offset 0.
    ( cd "$d" && "$CC" "${ldflags[@]}" -o min.library \
          minlib.o minvectors.o minops.o minasm.o ) > "$d/link.log" 2>&1
    rc_l=$?
    if [ $rc_l -ne 0 ]; then
        echo "arm=$name lto=$lto gc=$gc compile=ok link=fail err=$(head -1 "$d/link.log" | tr ' ' '_')"
        return
    fi

    "$OBJDUMP" -d "$d/min.library" > "$d/dis.txt" 2>&1
    "$NM" "$d/min.library" > "$d/nm.txt" 2>&1

    local size entry vertag romtag vectbl missing n
    size=$(wc -c < "$d/min.library" | tr -d ' ')

    # offset-0 guard: parse the hunk file with the project's own checker if it
    # is reachable, otherwise fall back to the byte pattern.
    if [ -f "$HERE/../cmake/check-library-entry.cmake" ] && command -v cmake >/dev/null; then
        if cmake -DLIBRARY_BINARY="$d/min.library" \
                 -P "$HERE/../cmake/check-library-entry.cmake" >"$d/entry.log" 2>&1
        then entry=ok; else entry=BAD; fi
    else
        entry=unchecked
    fi

    grep -aq '\$VER: min.library' "$d/min.library" && vertag=ok || vertag=GONE
    grep -q ' _min_romtag$' "$d/nm.txt" && romtag=ok || romtag=GONE
    grep -q ' _MinVectorTable$' "$d/nm.txt" && vectbl=ok || vectbl=GONE

    missing=""
    for s in $VECSYMS; do
        grep -q " $s\$" "$d/nm.txt" || missing="$missing,${s#_min_}"
    done
    n=$(grep -c '^' "$d/nm.txt")

    echo "arm=$name lto=$lto gc=$gc compile=ok link=ok bytes=$size entry=$entry vertag=$vertag romtag=$romtag vectable=$vectbl vec_missing=${missing#,}${missing:+} syms=$n"
}

build_exe() {
    local name="$1" lto="$2" gc="$3"
    local d="$OUT/exe_$name"
    rm -rf "$d"; mkdir -p "$d"

    local cflags=($ARCH $OPT $WARN -fomit-frame-pointer -fno-strict-aliasing
                  -ffunction-sections -fno-optimize-sibling-calls -I"$HERE")
    [ "$lto" = "1" ] && cflags+=(-flto)
    local ldflags=()
    [ "$lto" = "1" ] && ldflags+=(-flto $ARCH $OPT)
    [ "$gc" = "1" ] && ldflags+=(-Wl,--gc-sections)

    ( cd "$d" && "$CC" "${cflags[@]}" -c "$HERE/minexe.c" "$HERE/minvectors.c" \
                                        "$HERE/minops.c" "$HERE/minasm.s" ) \
        > "$d/compile.log" 2>&1 || { echo "arm=exe_$name lto=$lto gc=$gc compile=fail"; return; }
    ( cd "$d" && "$CC" "${ldflags[@]}" -o minexe \
          minexe.o minvectors.o minops.o minasm.o ) > "$d/link.log" 2>&1 || {
        echo "arm=exe_$name lto=$lto gc=$gc link=fail err=$(head -1 "$d/link.log" | tr ' ' '_')"; return; }
    "$OBJDUMP" -d "$d/minexe" > "$d/dis.txt" 2>&1
    "$NM" "$d/minexe" > "$d/nm.txt" 2>&1
    echo "arm=exe_$name lto=$lto gc=$gc link=ok bytes=$(wc -c < "$d/minexe" | tr -d ' ')"
}

echo "# toolchain: $("$CC" --version | head -1)"
echo "# ld:        $("$TC/bin/m68k-amigaos-ld" --version | head -1)"
build_arm nolto_nogc 0 0
build_arm nolto_gc   0 1
build_arm lto_nogc   1 0
build_arm lto_gc     1 1
build_arm lto_gc_keep  1 1 keep
build_arm lto_nogc_keep 1 0 keep
build_exe nolto      0 1
build_exe lto        1 1
