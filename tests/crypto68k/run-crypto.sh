#!/usr/bin/env bash
# The crypto68k correctness binaries, on the machine.
# NO LIBRARY, NO DRIVER, NO CARD, NO PEER.  Each program links the arithmetic
# and a reference implementation of the same arithmetic and compares them, so a
# ROM is the whole requirement and this runs wherever tier 2 runs.
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
        echo "$prog-$tag=PASS"
    else
        echo "$prog-$tag=FAIL rc=$rc log=$log"
        tail -25 "$log" >&2
        fails=$((fails + 1))
    fi
}

for arm in "${ARMS[@]}"; do
    model="${arm%%:*}"
    tag="${arm##*:}"

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
