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
# Endurance is compiled the way tests/clients/build.sh compiles
# client_patterns: against the Roadshow NDK headers with
# tests/conformance/compat first on the include path, so the regenerated
# inline/bsdsocket.h wins over the NDK one, which GCC 15 cannot compile.
# Fitz needs exactly the same treatment for exactly the same reason -- its
# amiga-server.c calls SetSocketSignals(), and the NDK's inline for that one
# is the specific macro that fails.
#
# WHY BUILD FITZ RATHER THAN ONLY RUN THE SHIPPED BINARY
#
# The released binary has debug compiled out.  Its client treats EAGAIN on a
# blocking socket as retryable (src/amiga-client.c, checkretry()) and retries
# ten times before giving up, silently -- so on the released binary the defect
# this harness hunts is visible only as a connection that eventually dies.
# Built with -DADEBUG=5 the same code prints
#
#     * EAGAIN
#     * recv error err=-1 len=<n> errno=<e>
#
# through kprintf() to the serial port, which tools/fsuae-run.sh captures into
# build/serial*.log.  Both binaries are produced; run-fitz.sh takes the debug
# one by default and -r switches to the released one, so "did our own build
# change the answer?" is one flag rather than an argument.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/endurance"
COMPAT="$ROOT/tests/conformance/compat"
FITZ="$ROOT/build/fitz/Fitz"
OUT="$ROOT/build/endurance"

. "$ROOT/tools/amiga-toolchain.sh"

mkdir -p "$OUT"

# ---- the harness ---------------------------------------------------------

echo "  CC endurance.c"
"$AMIGA_GCC" -O2 -Wall -Wextra -m68020 -fomit-frame-pointer \
    -fno-strict-aliasing \
    -I"$ROOT/include" -I"$COMPAT" -I"$AMIGA_NDK" \
    -o "$OUT/Endurance" "$HERE/endurance.c"

"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" "$OUT/Endurance" || true

# ---- Fitz ----------------------------------------------------------------

if [ ! -d "$FITZ/src" ]; then
    echo "==> no build/fitz -- run tests/endurance/fetch-fitz.sh for the Fitz arm"
    exit 0
fi

# The host server.  fitz-serve is the half of Fitz that does NOT need FUSE
# (its own Makefile splits the two binaries for that reason), which is what
# makes the Amiga-as-client arrangement the one that works on any host.
echo "  CC fitz-serve (host)"
(cd "$FITZ/src" && make fitz-serve >/dev/null)
cp "$FITZ/src/fitz-serve" "$OUT/fitz-serve"

# The released m68k binary, unchanged.
cp "$FITZ/fitz" "$OUT/fitz-release"
chmod +w "$OUT/fitz-release"

# And ours, with the diagnostics turned on.
#
# -DAUTHENTICATION is deliberately absent: it pulls in speck64be.asm, which is
# vasm source this tree cannot assemble, and encryption is off in every run
# here anyway (Fitz's own readme calls it "prohibitively slow on a 7 MHz
# 68000").  -DPARSETZ and -DROADSHOW_SONDERLOCKE match the shipped build.
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
      -o "$OUT/fitz-debug" $FITZ_SRC "$HERE/fitz-kprintf.c" -lm )

"$AMIGA_TOOLCHAIN_ROOT/bin/m68k-amigaos-size" "$OUT/fitz-debug" || true

echo "==> $OUT"
