#!/usr/bin/env bash
#
# Build the tbdye/bsdsocktest conformance suite with our toolchain.
#
#   tests/conformance/build.sh [-u] [-b BUILDDIR]
#
# -u  fetch/update the checkout before building
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/conformance"
SRC="$ROOT/third_party/bsdsocktest"
UPSTREAM="https://github.com/tbdye/bsdsocktest.git"
UPDATE=0
BUILDDIR="$ROOT/build/cm"

while getopts "ub:" opt; do
    case "$opt" in
        u) UPDATE=1 ;;
        b) case "$OPTARG" in /*) BUILDDIR="$OPTARG" ;;
                             *)  BUILDDIR="$ROOT/$OPTARG" ;; esac ;;
        *) echo "usage: $0 [-u] [-b BUILDDIR]" >&2; exit 2 ;;
    esac
done

# -e, not -d: a git submodule's .git is a file, and re-cloning over one fails.
if [ ! -e "$SRC/.git" ]; then
    echo "==> fetching $UPSTREAM into third_party/bsdsocktest"
    mkdir -p "$ROOT/third_party"
    git clone --depth 50 "$UPSTREAM" "$SRC"
elif [ "$UPDATE" = "1" ]; then
    git -C "$SRC" pull --ff-only
fi

. "$ROOT/tools/amiga-toolchain.sh"
TOOLCHAIN="$AMIGA_TOOLCHAIN_ROOT"
NDK="$AMIGA_NDK"
CC="$AMIGA_GCC"

CFLAGS=(-O2 -Wall -Wno-error=incompatible-pointer-types
        -m68020 -fomit-frame-pointer -fno-strict-aliasing
        -I"$HERE/compat" -I"$NDK"
        -include sys/types.h -include strings.h -include stdio.h
        -Dstricmp=strcasecmp
        -Dprintf=iprintf -Dsprintf=siprintf
        -Dvfprintf=vfiprintf -Dvsnprintf=vsniprintf)

OBJ="$ROOT/build/bsdsocktest/obj"
OUT="$ROOT/build/bsdsocktest/bsdsocktest"
mkdir -p "$OBJ"

# nullglob: compat/ currently holds headers only, so compat/*.c may match
# nothing.  Without it the unmatched pattern would be passed to the compiler
# verbatim.
shopt -s nullglob
sources=("$SRC"/src/*.c "$HERE"/compat/*.c "$ROOT/src/common/ami_udivdi3.c")
shopt -u nullglob

objs=()
for c in "${sources[@]}"; do
    o="$OBJ/$(basename "${c%.c}").o"
    if [ ! -f "$o" ] || [ "$c" -nt "$o" ]; then
        echo "  CC $(basename "$c")"
        "$CC" "${CFLAGS[@]}" -c -o "$o" "$c"
    fi
    objs+=("$o")
done

echo "  LD $(basename "$OUT")"
"$CC" -m68020 -o "$OUT" "${objs[@]}"

COMMON="$BUILDDIR/src/common/libaminetxduo_common.a"
if [ -f "$COMMON" ]; then
    echo "  CC conf_launcher.c"
    "$CC" -O2 -m68020 -fomit-frame-pointer -I"$ROOT/include" -I"$NDK" \
          -o "$ROOT/build/bsdsocktest/conf_launcher" \
          "$HERE/conf_launcher.c" "$COMMON"
else
    echo " , skipping conf_launcher: no $COMMON, build it or pass -b" >&2
fi

echo "  CC conf_probe.c"
"$CC" -O2 -m68020 -fomit-frame-pointer -fno-strict-aliasing \
      -I"$HERE/compat" -I"$NDK" -include sys/types.h \
      -o "$ROOT/build/bsdsocktest/conf_probe" "$HERE/conf_probe.c"

echo "==> $OUT"
"$TOOLCHAIN/bin/m68k-amigaos-size" "$OUT" || true
