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
# WHICH MACHINES, AND WHAT IT COSTS
#
# Measured on Kickstart 3.1 under Amiberry, 2026-08-20, host wall clock:
#
#   crypto68k_25519_test    16636 checks    133 s on the 68020
#   crypto68k_ec_test        1730 checks    306 s on the 68020
#   crypto68k_test           4965 checks    331 s on the 68020
#
# A 68000 is roughly four times a 68020 here, so the arms are not symmetric
# and the reason is arithmetic rather than coverage:
#
#   68020 (A1200)  all three, about thirteen minutes.
#   68000 (A600)   crypto68k_25519_test alone, about nine.  It is the fe_mul
#                  four-MULU.W path, which nothing else in tier 2 runs at all.
#                  The other two on that part would be half an hour of nightly
#                  for a second witness of helpers the 68020 arm already
#                  covers.
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

    # The floors are under the counts a whole run reports and not at them, so
    # a test that grows does not turn the gate red; a test that stops halfway
    # still does.  Measured 2026-08-20: 16636, 1730, 4965.  Raise them when
    # the tests grow, never lower them.
    #
    # The ceilings are the measured times with room, not round numbers picked
    # to be safe: 133 s and 306 s on the 68020, four times that on the 68000.
    if [ "$tag" = "68000" ]; then
        one "$model" "$tag" crypto68k_25519_test 16000 "$((SHORT * 3))"
        continue
    fi

    one "$model" "$tag" crypto68k_25519_test 16000 "$SHORT"
    one "$model" "$tag" crypto68k_ec_test     1500 "$WIDE"
    one "$model" "$tag" crypto68k_test        4900 "$WIDE"
done

echo "crypto68k_ran=$ran crypto68k_failed=$fails"
if [ "$fails" = 0 ]; then
    echo "RESULT=pass"
    exit 0
fi
echo "RESULT=fail"
exit 1
