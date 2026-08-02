#!/usr/bin/env bash
#
# Prove the profiler, end to end, in one command.
#
#   tools/profiler/selftest.sh [-b BUILDDIR] [-m MODEL] [-c CPU] [-t SECONDS]
#
# Runs Profile against profspin under FS-UAE and then asks
# tools/profiler/profreport.py whether the samples landed where the program
# says it was.  Two questions, and a sampling profiler that gets either wrong
# still produces a ranking that reads perfectly well:
#
#   CONTAINMENT      the samples are inside the kernels that were running.
#                    This is the exception-frame test.  Read the frame two
#                    bytes off and every recorded value is half an SR
#                    concatenated with half a PC -- a number that lands
#                    somewhere and resolves to something.  It scores
#                    approximately zero here rather than a bit less.
#
#   PROPORTIONALITY  each kernel's share of the samples matches its share of
#                    the wall clock profspin measured for itself.  A sampler
#                    can be correctly aimed and still fire only when something
#                    else periodic is running; this is what catches that.
#
# RUN IT ON MORE THAN ONE CPU, because the frame is what is in question:
#
#   tools/profiler/selftest.sh -b build/p20                 68020, 8-byte frame
#   tools/profiler/selftest.sh -b build/p00 -m A500         68000, 6-byte frame
#
# and with AMINETXDUO_KICKSTART pointing at a ROM the model can boot -- an
# A1200 ROM in an A500 does not fail as a bad ROM, it fails as an emulator
# that dies host-side before the guest runs.
#
# ON A REAL MACHINE there is no script: copy Profile and profspin across and
#
#   Profile OUT=RAM:spin.prof profspin RANGES=RAM:spin.ranges
#
# then bring both files back and run the profreport.py line this script
# prints.  That is the whole point of the tool -- an emulator above a 68020
# has no usable cycle model, so the interesting answer only exists on iron.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="$ROOT/build/p20"
MODEL=A1200
CPU=""
TIMEOUT=400

while getopts "b:m:c:t:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-m model] [-c cpu] [-t seconds]" >&2
           exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
BIN="$BUILD/tools/profiler"

for f in Profile profspin profspin.map; do
    [ -f "$BIN/$f" ] || {
        echo "missing $BIN/$f" >&2
        echo "  cmake --build $BUILD --parallel --target Profile profspin" >&2
        exit 2
    }
done

AMIGA_TOOLCHAIN_QUIET=1 . "$ROOT/tools/amiga-toolchain.sh"

echo "==> Profile $BIN/profspin under $MODEL${CPU:+ (CPU $CPU)}"

# xvfb-run because FS-UAE wants a real GL context and exits in a second
# without one, and -x because a shared run exits early in a way that looks
# exactly like a crash in the code under test.
xvfb-run -a "$ROOT/tools/fsuae-run.sh" -x -t "$TIMEOUT" -m "$MODEL" \
    ${CPU:+-c "$CPU"} \
    -a "OUT=DH0:spin.prof FOLDED=DH0:spin.folded profspin RANGES=DH0:spin.ranges" \
    "$BIN/Profile" "$BIN/profspin"

HD="$ROOT/build/testhd"
for f in spin.prof spin.ranges; do
    [ -f "$HD/$f" ] || { echo "the run produced no $f" >&2; exit 1; }
done

echo
exec "$ROOT/tools/profiler/profreport.py" "$HD/spin.prof" \
    --exe "$BIN/profspin" --map "$BIN/profspin.map" --objdir "$BIN" \
    --nm "${AMIGA_TOOLCHAIN_ROOT}/bin/m68k-amigaos-nm" \
    --ndk "${AMIGA_NDK:-}" \
    --contain "$HD/spin.ranges" \
    --folded "$HD/spin-full.folded" --trace "$HD/spin.json"
