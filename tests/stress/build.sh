#!/usr/bin/env bash
#
# Build the four-process Fitz stress harness and the tools it runs.
#
#   tests/stress/build.sh
#
# Produces:
#
#   build/stress/FitzStress     the m68k supervisor: four workers, one mount
#   build/stress/comparetree    Fitz's own tree comparator, for m68k
#   build/stress/fitz-serve     the host-side Fitz server (no FUSE needed)
#   build/stress/fitz           Fitz for m68k, debug at WARN
#
# SPDX-License-Identifier: MIT

set -euo pipefail

STRESS_ARCH="${STRESS_ARCH:--m68020}"

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/stress"
COMPAT="$ROOT/tests/conformance/compat"
FITZ="$ROOT/build/fitz/Fitz"
OUT="$ROOT/build/stress"

. "$ROOT/tools/amiga-toolchain.sh"

mkdir -p "$OUT"

# ---- the supervisor ------------------------------------------------------

echo "  CC fitzstress.c"
"$AMIGA_GCC" -O2 -Wall -Wextra $STRESS_ARCH -fomit-frame-pointer \
    -fno-strict-aliasing \
    -I"$ROOT/include" -I"$COMPAT" -I"$AMIGA_NDK" \
    -o "$OUT/FitzStress" "$HERE/fitzstress.c"

"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" "$OUT/FitzStress" || true

# ---- Fitz and its comparator ---------------------------------------------

if [ ! -d "$FITZ/src" ]; then
    echo "==> no build/fitz, run tests/endurance/fetch-fitz.sh"
    exit 0
fi

echo "  CC comparetree (m68k)"
"$AMIGA_GCC" -std=c99 -O2 $STRESS_ARCH -D__amigaos__ \
    -Wno-int-conversion \
    -I"$COMPAT" -I"$AMIGA_NDK" \
    -o "$OUT/comparetree" "$FITZ/src/amiga-comparetree.c"

"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" "$OUT/comparetree" || true

echo "  CC fitz-serve (host)"
(cd "$FITZ/src" && make -B fitz-serve >/dev/null)
cp "$FITZ/src/fitz-serve" "$OUT/fitz-serve"

cp "$FITZ/fitz" "$OUT/fitz-release"
chmod +w "$OUT/fitz-release"

echo "  CC fitz (m68k, ADEBUG=5)"
FITZ_SRC="
    amiga-main.c amiga-client.c amiga-server.c amiga-common.c amiga-tzparse.c
    fitz-common.c fitz-common-server.c fitz-common-client.c
"
( cd "$FITZ/src" && \
  "$AMIGA_GCC" -std=c99 -Os $STRESS_ARCH -D__amigaos__ \
      -DPARSETZ -DROADSHOW_SONDERLOCKE -DDEBUG -DADEBUG=5 \
      -Wno-int-conversion -include sys/types.h \
      -I"$COMPAT" -I"$AMIGA_NDK" \
      -o "$OUT/fitz-debug" $FITZ_SRC "$ROOT/tests/endurance/fitz-kprintf.c" -lm )

"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" "$OUT/fitz-debug" || true

echo "==> $OUT"
