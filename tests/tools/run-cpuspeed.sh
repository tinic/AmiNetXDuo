#!/usr/bin/env bash
# THE SAME BRING-UP, ON A CPU THE DELAY LOOPS WERE NOT WRITTEN FOR.
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

BUILD="${AMINETXDUO_BUILD:-build/cm}"
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"
TIMEOUT=0
ONLY_BOARD=""
ONLY_CPU=""
LIST=0

while getopts "b:t:B:N:c:l" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        N) ONLY_BOARD="$OPTARG" ;;
        c) ONLY_CPU="$OPTARG" ;;
        l) LIST=1 ;;
        *) echo "usage: $0 [-b builddir] [-t seconds] [-B backend]\
 [-N board[,board]] [-c cpu[,cpu]] [-l]" >&2; exit 2 ;;
    esac
done

ARMS="
a2065          68020  real
a2065          68030  real
a2065          68040  real
a2065          68060  real
a2065          68060  max
a2065          68060  clk209
ne2000_pcmcia  68020  real
ne2000_pcmcia  68030  real
ne2000_pcmcia  68040  real
ne2000_pcmcia  68060  real
ne2000_pcmcia  68060  max
ne2000_pcmcia  68060  clk209
"

selected() { # board cpu
    case ",$ONLY_BOARD," in
        ,,) ;;
        *",$1,"*) ;;
        *) return 1 ;;
    esac
    case ",$ONLY_CPU," in
        ,,) return 0 ;;
        *",$2,"*) return 0 ;;
        *) return 1 ;;
    esac
}

if [ "$LIST" = 1 ]; then
    printf '%-15s %-7s %s\n' board cpu speed
    printf '%s\n' "$ARMS" | while read -r b c s; do
        [ -n "$b" ] || continue
        selected "$b" "$c" || continue
        printf '%-15s %-7s %s\n' "$b" "$c" "$s"
    done
    exit 0
fi


EXE="$ROOT/$BUILD/tests/netstack/netstack_test"
[ -f "$EXE" ] || {
    echo "build $BUILD/tests/netstack/netstack_test first" >&2; exit 2; }

[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2; exit 2; }

. "$ROOT/tests/tools/bringup-verdict.sh"

RESULTS="$ROOT/build/cpuspeed-results.txt"
: > "$RESULTS"

run_arm() { # board cpu speed
    local board="$1" cpu="$2" speed="$3"
    local tag="matrix-cpu-$board-$cpu-$speed"
    local t="$TIMEOUT" rc report extra="" started elapsed

    if [ "$t" = 0 ]; then
        case "$board" in
            ne2000_pcmcia) t=420 ;;
            *)             t=180 ;;
        esac
    fi

    case "$speed" in
        max)    extra="cpu_speed=max" ;;
        clk209) extra="cpu_multiplier=64" ;;
    esac

    echo
    echo "=============================================================="
    echo "==> $board at $cpu, cpu_speed=$speed  (tag $tag, ceiling ${t}s)"
    echo "=============================================================="

    started=$(date +%s)
    (
        export AMINETXDUO_RUN_TAG="$tag"
        export AMINETXDUO_AMIBERRY_EXTRA="$extra"
        "$ROOT/tests/netstack/run-amiberry.sh" \
            -N "$board" -c "$cpu" -B "$BACKEND" -t "$t" -b "$BUILD"
    )
    rc=$?
    elapsed=$(( $(date +%s) - started ))

    report="$ROOT/build/amiberry-testhd-$tag/stdout.txt"

    local verdict=FAIL
    if bringup_verdict "$report" | sed 's/^/    /'; then
        verdict=PASS
    fi

    printf '%-15s %-7s %-5s %-5s run_rc=%-4s wall_s=%s\n' \
           "$board" "$cpu" "$speed" "$verdict" "$rc" "$elapsed" >> "$RESULTS"

    [ "$verdict" = PASS ]
}

FAILED=0
COUNT=0
while read -r board cpu speed; do
    [ -n "$board" ] || continue
    selected "$board" "$cpu" || continue
    COUNT=$((COUNT + 1))
    run_arm "$board" "$cpu" "$speed" || FAILED=$((FAILED + 1))
done <<EOF
$ARMS
EOF

echo
echo "======================== the matrix ==========================="
cat "$RESULTS"
echo "==============================================================="
echo "cpuspeed_arms=$COUNT cpuspeed_failed=$FAILED"

[ "$COUNT" -gt 0 ] || { echo "no arm selected" >&2; exit 2; }

if [ "$FAILED" = 0 ]; then
    echo "cpuspeed: PASS -- every arm came up and carried a packet"
    exit 0
fi

echo
echo "cpuspeed: FAIL -- $FAILED of $COUNT arms did not reach the network" >&2
echo >&2
echo "  Read the table above by COLUMN, not by row:" >&2
echo >&2
echo "  * red at every CPU including 68020    not this gate's defect.  The" >&2
echo "    build, the ROM, the driver or the backend.  Run one arm by hand" >&2
echo "    with -N <board> -c 68020 and fix that first." >&2
echo >&2
echo "  * green at 68020, red as the CPU rises  a delay that counts work" >&2
echo "    instead of time.  src/netdev/netdev_pcmcia.c pc_settle() turns" >&2
echo "    microseconds into 'n = us * 4', four bus reads to the microsecond," >&2
echo "    and el3.c, ne2000.c, netdev_cmds.c and netdev_isapnp.c each have a" >&2
echo "    sibling of it." >&2
echo >&2
echo "  * green at 68060/real, red at 68060/clk209  the RATE is the variable" >&2
echo "    and not the instruction set.  Same finding, stated exactly." >&2
echo >&2
echo "  * red on ne2000_pcmcia only              the claim path, netdev_pcmcia.c." >&2
echo "    Red on a2065 too and it is above the driver." >&2
echo >&2
echo "  The drives are at $ROOT/build/amiberry-testhd-matrix-cpu-*." >&2
exit 1
