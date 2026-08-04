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
# INCLUDE PATHS.  Everything m68k here is compiled the way
# tests/endurance/build.sh compiles Fitz: tests/conformance/compat FIRST, so
# the regenerated inline/bsdsocket.h wins over the NDK's, which GCC 15 cannot
# compile.  FitzStress opens no socket and would not need it; comparetree and
# Fitz do, and one rule for all three is one thing to remember.
#
# comparetree is `amiga-comparetree` in Fitz's own Makefile, where it is
# commented out of the `amiga:` target because that target is built with vbcc.
# Its rule (`$(AMIGA_CC) amiga-comparetree.c -o amiga-comparetree -lamiga`) is
# reproduced here against this tree's GCC instead.  -lamiga is vbcc's; the
# bebbo toolchain has the same entry points in libc, so it is dropped.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/stress"
COMPAT="$ROOT/tests/conformance/compat"
FITZ="$ROOT/build/fitz/Fitz"
OUT="$ROOT/build/stress"

. "$ROOT/tools/amiga-toolchain.sh"

mkdir -p "$OUT"

# ---- the supervisor ------------------------------------------------------

echo "  CC fitzstress.c"
"$AMIGA_GCC" -O2 -Wall -Wextra -m68020 -fomit-frame-pointer \
    -fno-strict-aliasing \
    -I"$ROOT/include" -I"$COMPAT" -I"$AMIGA_NDK" \
    -o "$OUT/FitzStress" "$HERE/fitzstress.c"

"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" "$OUT/FitzStress" || true

# ---- Fitz and its comparator ---------------------------------------------

if [ ! -d "$FITZ/src" ]; then
    echo "==> no build/fitz -- run tests/endurance/fetch-fitz.sh"
    exit 0
fi

echo "  CC comparetree (m68k)"
"$AMIGA_GCC" -std=c99 -O2 -m68020 -D__amigaos__ \
    -Wno-int-conversion \
    -I"$COMPAT" -I"$AMIGA_NDK" \
    -o "$OUT/comparetree" "$FITZ/src/amiga-comparetree.c"

"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" "$OUT/comparetree" || true

# -B, not plain make.  build/fitz is copied between hosts of different
# architectures, and the binary travels with it: make then finds a fitz-serve
# newer than every source, declares it up to date, and the run ships a Mach-O
# to a Linux peer, where it fails as a shell syntax error seven lines into the
# ELF header.
echo "  CC fitz-serve (host)"
(cd "$FITZ/src" && make -B fitz-serve >/dev/null)
cp "$FITZ/src/fitz-serve" "$OUT/fitz-serve"

cp "$FITZ/fitz" "$OUT/fitz-release"
chmod +w "$OUT/fitz-release"

# Ours, with the diagnostics on, the released binary has debug compiled out
# and its client retries EAGAIN on a blocking socket ten times in silence, so
# on that binary the defect this harness hunts is only ever visible as a
# connection that eventually died.  See tests/endurance/build.sh.
echo "  CC fitz (m68k, ADEBUG=5)"
FITZ_SRC="
    amiga-main.c amiga-client.c amiga-server.c amiga-common.c amiga-tzparse.c
    fitz-common.c fitz-common-server.c fitz-common-client.c
"
( cd "$FITZ/src" && \
  "$AMIGA_GCC" -std=c99 -Os -m68020 -D__amigaos__ \
      -DPARSETZ -DROADSHOW_SONDERLOCKE -DDEBUG -DADEBUG=5 \
      -Wno-int-conversion -include sys/types.h \
      -I"$COMPAT" -I"$AMIGA_NDK" \
      -o "$OUT/fitz-debug" $FITZ_SRC "$ROOT/tests/endurance/fitz-kprintf.c" -lm )

"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" "$OUT/fitz-debug" || true

echo "==> $OUT"
