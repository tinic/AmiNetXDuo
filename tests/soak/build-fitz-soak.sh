#!/usr/bin/env bash
#
# Build the Fitz soak harness.
#
#   tests/soak/build-fitz-soak.sh
#
# Produces build/soak/FitzSoak, the m68k guest-side program: it starts Fitz,
# drives it, samples the stack and writes the timeline.
#
# It also needs Fitz itself, which is third-party and not vendored.  Fitz is
# fetched and built by tests/endurance/fetch-fitz.sh and tests/endurance/
# build.sh; both are used here as they stand rather than duplicated, so there
# is one recipe for building somebody else's program and not two that can
# drift apart.
#
# Compiled the way tests/endurance/build.sh compiles Endurance: against the
# Roadshow NDK headers with tests/conformance/compat first on the include
# path, so the regenerated inline/bsdsocket.h wins over the NDK one, which
# GCC 15 cannot compile.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/soak"
COMPAT="$ROOT/tests/conformance/compat"
OUT="$ROOT/build/soak"

. "$ROOT/tools/amiga-toolchain.sh"

mkdir -p "$OUT"

echo "  CC fitz_soak.c"
"$AMIGA_GCC" -O2 -Wall -Wextra -m68020 -fomit-frame-pointer \
    -fno-strict-aliasing \
    -I"$ROOT/include" -I"$COMPAT" -I"$AMIGA_NDK" \
    -o "$OUT/FitzSoak" "$HERE/fitz_soak.c"

"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" "$OUT/FitzSoak" || true

# Fitz, via the endurance tree's own recipe.  Absent sources are not an error
# here: the message says which command fetches them.
if [ ! -d "$ROOT/build/fitz/Fitz/src" ]; then
    echo "==> no build/fitz -- run tests/endurance/fetch-fitz.sh first"
    exit 0
fi

if [ ! -f "$ROOT/build/endurance/fitz-debug" ]; then
    echo "==> building Fitz via tests/endurance/build.sh"
    "$ROOT/tests/endurance/build.sh"
fi

echo "==> $OUT/FitzSoak"
