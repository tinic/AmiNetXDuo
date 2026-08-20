#!/usr/bin/env bash
#
# The crypto68k correctness binaries, on the machine.
#
#   tests/crypto68k/run-crypto.sh [-b BUILDDIR] [-t SECONDS] [-w SECONDS]
#
#     -t  ceiling for the two short programs   (default 300)
#     -w  ceiling for the wide modexp program  (default 600)
#
# WHY THIS EXISTS
#
# crypto68k_test, crypto68k_25519_test and crypto68k_ec_test have been built by
# every cross configuration since they were written and run by nothing.  The
# host tier covers the portable C in them (ctest: crypto68k_vectors,
# crypto68k_25519) and CANNOT cover what they are for: src/crypto68k's
# assembly is m68k and is different code per part -- MULU.L on a 68020 and up,
# four MULU.W on a 68000 and a 68060 -- so the check has to execute on a guest.
#
# NO LIBRARY, NO DRIVER, NO CARD, NO PEER.  Each program links the arithmetic
# and a reference implementation of the same arithmetic and compares them, so a
# ROM is the whole requirement and this runs wherever tier 2 runs.
#
# WHICH MACHINES, AND WHY NOT ALL OF THEM ON ALL OF THEM
#
#   68000 (A600)  the four-MULU.W path, which is the one nothing else runs.
#                 25519 and P-256 only.
#   68020 (A1200) the MULU.L path.  All three.
#
# crypto68k_test is the wide modexp pass and is 331 s of host wall clock on
# the 68020 arm, measured 2026-08-20; a 68000 is roughly four times that, and
# twenty minutes of nightly for a fourth witness of the same helpers the other
# two already exercise on that part is not the trade.  It runs on the 68020.
#
# Output is key=value plus one RESULT= line.  Exit: 0 pass, 1 a program
# failed, 2 refused before starting.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="${AMINETXDUO_BUILD:-build/cm}"
SHORT=300
WIDE=600

while getopts "b:t:w:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) SHORT="$OPTARG" ;;
        w) WIDE="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-t secs] [-w secs]" >&2; exit 2 ;;
    esac
done

DIR="$ROOT/$BUILD/tests/crypto68k"
for f in crypto68k_test crypto68k_25519_test crypto68k_ec_test; do
    [ -f "$DIR/$f" ] || {
        echo "crypto68k_refused=missing_$f"
        echo "RESULT=refused"
        exit 2
    }
done

# A600 is a real 68000 and needs a 68000 ROM; forcing -c 68000 onto the
# default A1200 builds a machine that does not exist and never reaches a
# Shell.  Same rule as tools/ci.sh's emulator stage.
if [ -z "${AMINETXDUO_KICKSTART_A600:-}" ]; then
    echo "crypto68k_note=AMINETXDUO_KICKSTART_A600 unset, the 68000 arm is skipped"
    ARMS=("A1200:68020")
else
    ARMS=("A1200:68020" "A600:68000")
fi

. "$ROOT/tools/test-verdict.sh"

fails=0
ran=0

one() { # model tag program floor timeout
    local model="$1" tag="$2" prog="$3" floor="$4" to="$5" rc log

    ran=$((ran + 1))
    export AMINETXDUO_RUN_TAG="c68k-$tag-$prog"
    log="$ROOT/$BUILD/crypto68k-$tag-$prog.log"

    "$ROOT/tools/amiberry-run.sh" -m "$model" -t "$to" "$DIR/$prog" \
        > "$log" 2>&1
    rc=$?

    if verdict_guest "$prog/$tag" "$floor" "$rc" \
            "$(verdict_hd_amiberry)/stdout.txt" \
            "$(verdict_serial_amiberry)"; then
        echo "crypto68k_$prog-$tag=PASS"
    else
        echo "crypto68k_$prog-$tag=FAIL rc=$rc log=$log"
        tail -25 "$log" >&2
        fails=$((fails + 1))
    fi
}

for arm in "${ARMS[@]}"; do
    model="${arm%%:*}"
    tag="${arm##*:}"

    # The floors are under the counts a whole run reports, not at them:
    # 25519 is 12630 checks and P-256 is 1620 as of 2026-08-20.  Raise them
    # when the tests grow, never lower them.
    one "$model" "$tag" crypto68k_25519_test 12000 "$SHORT"
    one "$model" "$tag" crypto68k_ec_test     1500 "$SHORT"

    [ "$tag" = "68020" ] || continue
    one "$model" "$tag" crypto68k_test        4900 "$WIDE"
done

echo "crypto68k_ran=$ran crypto68k_failed=$fails"
if [ "$fails" = 0 ]; then
    echo "RESULT=pass"
    exit 0
fi
echo "RESULT=fail"
exit 1
