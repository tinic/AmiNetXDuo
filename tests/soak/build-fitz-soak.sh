#!/usr/bin/env bash
#
# Build the Fitz soak harness.
#
#   tests/soak/build-fitz-soak.sh
#
# Produces build/soak/FitzSoak, the m68k guest-side program.  Fitz itself is
# third-party and not vendored; it is fetched and built by
# tests/endurance/fetch-fitz.sh and tests/endurance/build.sh.  tests/conformance/
# compat must come first on the include path: GCC 15 cannot compile the NDK's
# inline/bsdsocket.h.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

# The guest CPU this is built for.  A 68020 binary on an A600 dies on its
# first 020 instruction, before any of the harness runs.
SOAK_ARCH="${SOAK_ARCH:--m68020}"

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/soak"
COMPAT="$ROOT/tests/conformance/compat"
OUT="$ROOT/build/soak"

. "$ROOT/tools/amiga-toolchain.sh"

mkdir -p "$OUT"

echo "  CC fitz_soak.c"
"$AMIGA_GCC" -O2 -Wall -Wextra $SOAK_ARCH -fomit-frame-pointer \
    -fno-strict-aliasing \
    -I"$ROOT/include" -I"$COMPAT" -I"$AMIGA_NDK" \
    -o "$OUT/FitzSoak" "$HERE/fitz_soak.c"

"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" "$OUT/FitzSoak" || true

# Fitz, via the endurance tree's own recipe.  Absent sources are not an error
# here: the message says which command fetches them.
if [ ! -d "$ROOT/build/fitz/Fitz/src" ]; then
    echo "==> no build/fitz, run tests/endurance/fetch-fitz.sh first"
    exit 0
fi

if [ ! -f "$ROOT/build/endurance/fitz-debug" ]; then
    echo "==> building Fitz via tests/endurance/build.sh"
    "$ROOT/tests/endurance/build.sh"
fi

echo "==> $OUT/FitzSoak"
