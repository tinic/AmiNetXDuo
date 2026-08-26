#!/usr/bin/env bash
#
# Prove the profiler, end to end, in one command.
#
#   tools/profiler/selftest.sh [-b BUILDDIR] [-m MODEL] [-c CPU] [-t SECONDS]
#
# RUN IT ON MORE THAN ONE CPU, because the frame is what is in question:
#
#   tools/profiler/selftest.sh -b build/p20                 68020, 8-byte frame
#   tools/profiler/selftest.sh -b build/p00 -m A500         68000, 6-byte frame
#
# and with AMINETXDUO_KICKSTART pointing at a ROM the model can boot, an
# A1200 ROM in an A500 does not fail as a bad ROM, it fails as an emulator
# that dies host-side before the guest runs.
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

# There is no exclusive lane left: it lived in the FS-UAE runner's slot lock,
# and Amiberry has none.  Keep the machine idle by hand before believing a
# number taken here -- a contended host has already corrupted one set of
# figures in this project.
"$ROOT/tools/amiberry-run.sh" -t "$TIMEOUT" -m "$MODEL" \
    ${CPU:+-c "$CPU"} \
    -a "OUT=DH0:spin.prof FOLDED=DH0:spin.folded profspin RANGES=DH0:spin.ranges" \
    "$BIN/Profile" "$BIN/profspin"

HD="$ROOT/build/amiberry-testhd-${AMINETXDUO_RUN_TAG:-amiberry}"
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
