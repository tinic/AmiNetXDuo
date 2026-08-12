#!/usr/bin/env bash
#
# Measure a stack's periodic-timer rate and its packet turnaround, from
# outside it.
#
#   tests/compare/run-tickprobe.sh -s ours|roadshow [options]
#
# WHY
#
#   docs/RESEARCH.md 29.5 had Roadshow answering ICMP in 4.32 ms where we took
#   7, and two explanations fit: a faster periodic timer, or a cheaper receive
#   path.  tests/compare/tickprobe.c distinguishes them by putting a SANA-II
#   device underneath whichever stack is being measured and timestamping every
#   frame it hands over, inside BeginIO, from the E-Clock.  It answered
#   neither: our tick is the finer of the two and our turnaround through this
#   device is the shorter, so the 2.7 ms is below SANA-II.  See tickprobe.c.
#
#   Nothing of the foreign stack is read, disassembled or copied.  It is
#   located at run time, staged, given an interface, and watched.
#
# NO NETWORK.  There is no -n and no a2065.device: the guest's only interface
# is tcpdrill.device, so SLIRP, the host's routing and the emulator's own
# scheduling are all out of the measurement.
#
# MEASUREMENT RUN.  fsuae-run.sh is invoked with -x, which takes the emulator
# queue alone (tools/amiberry-run.sh, commit 81b8f8d).  A quantisation histogram
# taken while another agent's boot shares the machine is a histogram of that
# agent.
#
# OPTIONS
#   -s ours|roadshow   which stack (required)
#   -b DIR             build directory for `ours` (default build/cm)
#   -R DIR             Roadshow Workbench/ directory
#   -m MODEL           emulator profile (default A1200, the only timing one)
#   -t SECS            timeout (default 300)
#   -T TAG             run tag; results land in build/amiberry-testhd-<tag>/
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

STACK=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
RSDIR="${AMINETXDUO_CMP_ROADSHOW:-/tmp/rsdemo/Roadshow-Demo-1.15/Workbench}"
MODEL=A1200
TIMEOUT=300
TAG=""

while getopts "s:b:R:m:t:T:" opt; do
    case "$opt" in
        s) STACK="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        R) RSDIR="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        *) sed -n '3,36p' "$0" >&2; exit 2 ;;
    esac
done

[ -n "$STACK" ] || { sed -n '3,36p' "$0" >&2; exit 2; }
[ -n "$TAG" ] || TAG="tick-$STACK"

case "$BUILD" in
    /*) ;;
    *)  BUILD="$ROOT/$BUILD" ;;
esac

# ------------------------------------------------------------- the probe ----

. "$ROOT/tools/amiga-toolchain.sh"

PROBE="$ROOT/build/tickprobe/TickProbe"
mkdir -p "$ROOT/build/tickprobe"
if [ ! -x "$PROBE" ] ||
   [ "$ROOT/tests/compare/tickprobe.c" -nt "$PROBE" ] ||
   [ "$ROOT/tests/tcpdrill/tapdev.c" -nt "$PROBE" ]; then
    echo "==> building TickProbe"
    case "${CPU:-}${MODEL:-}" in
        *68000*|*A500*|*A600*|*A2000*) _guest_arch="-m68000" ;;
        *)                             _guest_arch="-m68020" ;;
    esac
    "$AMIGA_GCC" -O2 "$_guest_arch" -fomit-frame-pointer -Wall -Wextra -noixemul \
        -I "$ROOT/tests/tcpdrill" -I "$ROOT/src/sana2" -I "$ROOT/include" \
        ${AMIGA_NDK:+-I "$AMIGA_NDK"} \
        -o "$PROBE" "$ROOT/tests/compare/tickprobe.c" \
                    "$ROOT/tests/tcpdrill/tapdev.c"
fi

# ------------------------------------------------------------- the stack ----

STAGE="$ROOT/build/tick-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp -R "$ROOT/tests/tcpdrill/devs/Internet" "$STAGE/devs/Internet"

case "$STACK" in
    ours)
        LIBBSD="$BUILD/src/bsdsocket/bsdsocket.library"
        [ -f "$LIBBSD" ] || { echo "missing $LIBBSD, build it first" >&2; exit 2; }
        cp "$LIBBSD" "$STAGE/libs/bsdsocket.library"
        [ -f "$BUILD/src/usergroup/usergroup.library" ] &&
            cp "$BUILD/src/usergroup/usergroup.library" "$STAGE/libs/"
        cp "$ROOT/tests/compare/tick-if.ours" "$STAGE/devs/NetInterfaces/tap0"
        NOTE="AmiNetXDuo from $BUILD"
        ;;
    roadshow)
        [ -d "$RSDIR" ] || { echo "no Roadshow at $RSDIR (-R DIR)" >&2; exit 2; }
        [ -f "$RSDIR/Libs/bsdsocket.library" ] || {
            echo "no bsdsocket.library under $RSDIR/Libs" >&2; exit 2; }
        # Their whole Libs/ drawer: AddNetInterface is not ours and opening
        # something it needs is not a thumb on the scale.
        cp -R "$RSDIR/Libs/." "$STAGE/libs/"
        cp "$RSDIR/C/AddNetInterface" "$STAGE/AddNetInterface"
        cp "$ROOT/tests/compare/tick-if.roadshow" "$STAGE/devs/NetInterfaces/tap0"
        # Roadshow's commands are built against a C library that opens the
        # maths libraries before main(), and a bare directory hard drive has
        # no LIBS: to find them in.
        for lib in mathieeedoubbas mathieeedoubtrans; do
            for cand in "$ROOT/build/$lib-os.library" "$ROOT/build/$lib.library"; do
                [ -f "$cand" ] && { cp "$cand" "$STAGE/libs/$lib.library"; break; }
            done
        done
        # Our own library configures DEVS:NetInterfaces when it opens; theirs
        # does not, and their AddNetInterface is the only thing that should
        # ever bring a Roadshow interface up.
        printf 'DH0:AddNetInterface DEVS:NetInterfaces/tap0\n' > "$STAGE/tickif.txt"
        NOTE="Roadshow from $RSDIR"
        ;;
    *)  echo "unknown stack '$STACK'" >&2; exit 2 ;;
esac

STAGED=("$STAGE/devs" "$STAGE/libs")
[ -f "$STAGE/AddNetInterface" ] && STAGED+=("$STAGE/AddNetInterface")
[ -f "$STAGE/tickif.txt" ]      && STAGED+=("$STAGE/tickif.txt")

echo "==> stack: $NOTE"

export AMINETXDUO_RUN_TAG="$TAG"

set +e
"$ROOT/tools/amiberry-run.sh" -x -m "$MODEL" -t "$TIMEOUT" \
    "$PROBE" "${STAGED[@]}" \
    > "$ROOT/build/tickprobe-$TAG.log" 2>&1
RC=$?
set -e

HD="$ROOT/build/amiberry-testhd-$TAG"
OUT="$ROOT/build/tickprobe-$TAG.txt"

if [ -f "$HD/tickprobe.txt" ]; then
    cp "$HD/tickprobe.txt" "$OUT"
    echo "==> samples: $OUT"
    grep '^#\|^!!' "$OUT" | head -40
else
    echo "!! no tickprobe.txt, the run did not get that far"
    [ -f "$HD/stdout.txt" ] && { echo "--- stdout ---"; cat "$HD/stdout.txt"; }
fi

echo
[ -f "$HD/tickif.out" ] && { echo "--- the stack's own interface command ---"
                             cat "$HD/tickif.out"; }

echo "emulator log: build/tickprobe-$TAG.log"
echo "serial log:   build/serial-$TAG.log"

exit "$RC"
