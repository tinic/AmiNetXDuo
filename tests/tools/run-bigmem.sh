#!/usr/bin/env bash
# THE POOL ARITHMETIC, ON A MACHINE WITH REAL MEMORY IN IT.
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

BUILD="${AMINETXDUO_BUILD:-build/cm}"
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"
BOARD=a2065
TIMEOUT=240
LIST=0

while getopts "b:t:B:N:l" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        l) LIST=1 ;;
        *) echo "usage: $0 [-b builddir] [-t seconds] [-B backend]\
 [-N board] [-l]" >&2; exit 2 ;;
    esac
done

ARMS="
chip-only  A1200  68020  0  0    band
lab-a1200  A1200  68020  8  0    band
accel-32m  A3000  68030  8  32   saturated
accel-128m A3000  68030  8  128  saturated
"

if [ "$LIST" = 1 ]; then
    printf '%-11s %-6s %-7s %-9s %-8s %s\n' name model cpu fastmem_mb z3mem_mb pool
    printf '%s\n' "$ARMS" | while read -r n m c f z p; do
        [ -n "$n" ] || continue
        printf '%-11s %-6s %-7s %-9s %-8s %s\n' "$n" "$m" "$c" "$f" "$z" "$p"
    done
    exit 0
fi

EXE="$ROOT/$BUILD/tests/netstack/netstack_test"
[ -f "$EXE" ] || {
    echo "build $BUILD/tests/netstack/netstack_test first" >&2; exit 2; }
[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2; exit 2; }

. "$ROOT/tests/tools/bringup-verdict.sh"

RESULTS="$ROOT/build/bigmem-results.txt"
: > "$RESULTS"

POOL_CEILING=520
POOL_MIN=16

# The window ceiling, written out rather than derived, for the same reason
# tests/sockopt/sockopt_test.c writes it out: (512 / 8) * 1568.  That test only
# range-asserts FLOOR <= window <= CEILING, which passes whether or not the
# ceiling ever bound.  A saturated arm is the one machine where it MUST bind,
# and until this ran nothing had ever observed it doing so.
TCP_WINDOW_CEILING=100352

clamp_arm() { # name model cpu fastmem z3mem poolexpect
    local name="$1" model="$2" cpu="$3" fast="$4" z3="$5" expect="${6:-band}"
    local tag="matrix-clamp-$name" out win queue

    if [ ! -f "$ROOT/$BUILD/tests/sockopt/sockopt_test" ]; then
        printf '%-11s clamps SKIP  no sockopt_test in %s\n' \
               "$name" "$BUILD" >> "$RESULTS"
        return 0
    fi

    (
        export AMINETXDUO_RUN_TAG="$tag"
        export AMINETXDUO_FASTMEM="$fast"
        export AMINETXDUO_Z3MEM="$z3"
        "$ROOT/tests/sockopt/run-sockopt.sh" \
            -m "$model" -c "$cpu" -t "$TIMEOUT" -b "$BUILD"
    ) > "$ROOT/build/bigmem-clamps-$name.log" 2>&1

    out="$ROOT/build/amiberry-testhd-$tag/stdout.txt"
    win=$(sed -n 's/.*poolclamp_tcp_window=//p' "$out" 2>/dev/null | tail -1)
    queue=$(sed -n 's/.*poolclamp_udp_queue=//p' "$out" 2>/dev/null | tail -1)

    if [ -z "$win" ] || [ -z "$queue" ]; then
        printf '%-11s clamps FAIL  no poolclamp lines in %s\n' \
               "$name" "$out" >> "$RESULTS"
        return 1
    fi

    if [ "$expect" = saturated ] && [ "$win" != "$TCP_WINDOW_CEILING" ]; then
        printf '%-11s clamps FAIL  tcp_window=%-7s udp_queue=%-7s \
ceiling_bound=0 want=%s\n' \
               "$name" "$win" "$queue" "$TCP_WINDOW_CEILING" >> "$RESULTS"
        return 1
    fi

    printf '%-11s clamps PASS  tcp_window=%-7s udp_queue=%-7s ceiling_bound=%s\n' \
           "$name" "$win" "$queue" \
           "$([ "$win" = "$TCP_WINDOW_CEILING" ] && echo 1 || echo 0)" \
           >> "$RESULTS"
    return 0
}

run_arm() { # name model cpu fastmem z3mem poolexpect
    local name="$1" model="$2" cpu="$3" fast="$4" z3="$5" expect="$6"
    local tag="matrix-mem-$name"
    local rc report packets verdict=FAIL out

    echo
    echo "=============================================================="
    echo "==> $name: $model $cpu, ${fast} MB Fast, ${z3} MB Zorro III, pool $expect"
    echo "=============================================================="

    (
        export AMINETXDUO_RUN_TAG="$tag"
        export AMINETXDUO_FASTMEM="$fast"
        export AMINETXDUO_Z3MEM="$z3"
        "$ROOT/tests/netstack/run-amiberry.sh" \
            -N "$BOARD" -m "$model" -c "$cpu" -B "$BACKEND" \
            -t "$TIMEOUT" -b "$BUILD"
    )
    rc=$?

    report="$ROOT/build/amiberry-testhd-$tag/stdout.txt"

    out=$(bringup_verdict "$report")
    printf '%s\n' "$out" | sed 's/^/    /'
    printf '%s\n' "$out" | grep -q '^bringup_result=PASS' && verdict=PASS

    packets=$(printf '%s\n' "$out" | sed -n 's/^bringup_pool_packets=//p' | tail -1)

    local poolverdict=PASS
    if [ -z "$packets" ] || [ "$packets" = none ]; then
        poolverdict="FAIL(no_pool_line)"
    elif [ "$packets" -lt "$POOL_MIN" ] || [ "$packets" -gt "$POOL_CEILING" ]; then
        poolverdict="FAIL(${packets}_outside_${POOL_MIN}_${POOL_CEILING})"
    fi

    printf '%-11s %-6s %-7s fast=%-4s z3=%-4s %-5s pool=%-5s %-24s %s run_rc=%s\n' \
           "$name" "$model" "$cpu" "$fast" "$z3" "$verdict" "${packets:-none}" \
           "$poolverdict" "$expect" "$rc" >> "$RESULTS"

    eval "POOL_${name//-/_}=\${packets:-0}"

    clamp_arm "$name" "$model" "$cpu" "$fast" "$z3" "$expect" || verdict=FAIL

    [ "$verdict" = PASS ] && [ "$poolverdict" = PASS ]
}

FAILED=0
COUNT=0
SKIPPED_BIG=0
while read -r name model cpu fast z3 expect; do
    [ -n "$name" ] || continue

    if [ "$model" = A3000 ] && [ -z "${AMINETXDUO_KICKSTART_A3000:-}" ]; then
        echo
        echo "!! $name needs an A3000 Kickstart and AMINETXDUO_KICKSTART_A3000" >&2
        echo "!! is not set.  Zorro III memory maps on NO other machine here" >&2
        echo "!! (measured: A1200 and A4000 both ignore it), so this arm is" >&2
        echo "!! skipped rather than run on a machine without the memory." >&2
        printf '%-11s %-6s SKIP  no AMINETXDUO_KICKSTART_A3000\n' \
               "$name" "$model" >> "$RESULTS"
        SKIPPED_BIG=$((SKIPPED_BIG + 1))
        continue
    fi

    COUNT=$((COUNT + 1))
    run_arm "$name" "$model" "$cpu" "$fast" "$z3" "$expect" || FAILED=$((FAILED + 1))
done <<EOF
$ARMS
EOF

if [ "$SKIPPED_BIG" = 0 ]; then
    p32="${POOL_accel_32m:-0}"
    p128="${POOL_accel_128m:-0}"
    p8="${POOL_lab_a1200:-0}"

    if [ "$p32" != "$p128" ]; then
        echo "pool_saturation=FAIL 32m=$p32 128m=$p128 differ" >> "$RESULTS"
        FAILED=$((FAILED + 1))
    elif [ "$p32" -le "$p8" ]; then
        echo "pool_saturation=FAIL 32m=$p32 is not above the 8 MB arm ($p8)" \
             >> "$RESULTS"
        FAILED=$((FAILED + 1))
    else
        echo "pool_saturation=PASS 32m=$p32 == 128m=$p128, above 8 MB ($p8)" \
             >> "$RESULTS"
    fi
fi

echo
echo "==================== the memory matrix ========================"
cat "$RESULTS"
echo "==============================================================="
echo "bigmem_arms=$COUNT bigmem_failed=$FAILED"

if [ "$FAILED" = 0 ]; then
    echo "bigmem: PASS -- bring-up works and the pool clamp holds at every size"
    exit 0
fi

echo
echo "bigmem: FAIL -- $FAILED of $COUNT arms" >&2
echo >&2
echo "  pool_saturation=FAIL, 32m and 128m DIFFER    the sizing arithmetic" >&2
echo "    is still scaling past AMI_POOL_MAX_PACKETS.  If the 128 MB figure" >&2
echo "    is the SMALLER of the two, that is the overflow this arm exists" >&2
echo "    for: src/netstack/netstack.c:541, avail/divisor/stride." >&2
echo >&2
echo "  pool_saturation=FAIL, 32m not above the 8 MB arm  the Zorro III" >&2
echo "    memory never reached the guest.  It maps on an A3000 and on" >&2
echo "    neither an A1200 nor an A4000 (measured); check that the run" >&2
echo "    really booted an A3000 with AMINETXDUO_KICKSTART_A3000." >&2
echo >&2
echo "  ceiling_bound=0 on a saturated arm  the pool is big enough for the" >&2
echo "    window ceiling to bind and it did not: the window came out at" >&2
echo "    something other than (512 / 8) * 1568.  src/netstack/netstack.c:541" >&2
echo "    and the SO_RCVBUF answer in src/bsdsocket/sockopt.c." >&2
echo >&2
echo "  bring-up red only on the accel arms  the stack does not survive a" >&2
echo "    machine this size.  src/netstack/netstack.c:541 and everything" >&2
echo "    sized off it: window ceiling, ack threshold, reader depth." >&2
echo >&2
echo "  The drives are at $ROOT/build/amiberry-testhd-matrix-mem-*." >&2
exit 1
