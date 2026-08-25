#!/usr/bin/env bash
#
# Build the endurance harness, and Fitz if its sources are present.
#
#   tests/endurance/build.sh
#
# Produces:
#
#   build/endurance/Endurance   the m68k harness: workload + pool timeline
#   build/endurance/fitz-serve  the host-side Fitz server (no FUSE needed)
#   build/endurance/fitz        Fitz for m68k, built HERE with debug at WARN
#
# SPDX-License-Identifier: MIT

set -euo pipefail

FITZ_ARCH="${FITZ_ARCH:-${SOAK_ARCH:--m68020}}"

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/endurance"
COMPAT="$ROOT/tests/conformance/compat"
FITZ="$ROOT/build/fitz/Fitz"
OUT="$ROOT/build/endurance"

. "$ROOT/tools/amiga-toolchain.sh"

mkdir -p "$OUT"

# ---- the harness ---------------------------------------------------------

echo "  CC endurance.c"
"$AMIGA_GCC" -O2 -Wall -Wextra $FITZ_ARCH -fomit-frame-pointer \
    -fno-strict-aliasing \
    -I"$ROOT/include" -I"$COMPAT" -I"$AMIGA_NDK" \
    -o "$OUT/Endurance" "$HERE/endurance.c"

"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" "$OUT/Endurance" || true

# ---- Fitz ----------------------------------------------------------------

if [ ! -d "$FITZ/src" ]; then
    echo "==> no build/fitz, run tests/endurance/fetch-fitz.sh for the Fitz arm"
    exit 0
fi

echo "  CC fitz-serve (host)"
(cd "$FITZ/src" && make fitz-serve >/dev/null)
cp "$FITZ/src/fitz-serve" "$OUT/fitz-serve"

# The released m68k binary, unchanged.
cp "$FITZ/fitz" "$OUT/fitz-release"
chmod +w "$OUT/fitz-release"

echo "  CC fitz (m68k, ADEBUG=5)"
FITZ_SRC="
    amiga-main.c amiga-client.c amiga-server.c amiga-common.c amiga-tzparse.c
    fitz-common.c fitz-common-server.c fitz-common-client.c
"
( cd "$FITZ/src" && \
  "$AMIGA_GCC" -std=c99 -Os $FITZ_ARCH -D__amigaos__ \
      -DPARSETZ -DROADSHOW_SONDERLOCKE -DDEBUG -DADEBUG=5 \
      -Wno-int-conversion -include sys/types.h \
      -I"$COMPAT" -I"$AMIGA_NDK" \
      -Wl,--allow-multiple-definition \
      -o "$OUT/fitz-debug" $FITZ_SRC "$HERE/fitz-kprintf.c" -lm )

"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" "$OUT/fitz-debug" || true

echo "==> $OUT"
