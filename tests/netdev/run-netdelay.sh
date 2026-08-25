#!/usr/bin/env bash
# THE ACCELERATOR ARM.  Does a netdev delay still take the time it asks for
# when the CPU is fast?
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-build/cm}"
MODEL=A1200
TIMEOUT=180
CPUS=()

while getopts "b:m:t:c:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPUS+=("$OPTARG") ;;
        *) echo "usage: $0 [-b builddir] [-m model] [-t seconds] [-c cpu]..." >&2
           exit 2 ;;
    esac
done

[ ${#CPUS[@]} -gt 0 ] || CPUS=(68020 68060)

EXE="$ROOT/$BUILD/tests/netdev/netdelay_test"
[ -f "$EXE" ] || {
    echo "build $BUILD/tests/netdev/netdelay_test first" >&2
    exit 2
}

. "$ROOT/tools/test-verdict.sh"

rc=0

for cpu in "${CPUS[@]}"; do
    log="$ROOT/build/netdelay-$cpu.log"

    echo
    echo "==> netdelay at $cpu on $MODEL"

    AMINETXDUO_RUN_TAG="netdelay-$cpu" \
        "$ROOT/tools/amiberry-run.sh" -m "$MODEL" -c "$cpu" -t "$TIMEOUT" \
        "$EXE" > "$log" 2>&1
    run_rc=$?

    verdict_guest "netdelay-$cpu" 6 "$run_rc" "$log" || rc=1

    sed -n 's/^\(beam:\|E-Clock\|300 ms from\|the pc_settle\|the same hold\)/  &/p' "$log"
done

exit $rc
