#!/usr/bin/env bash
#
# THE POOL ARITHMETIC, ON A MACHINE WITH REAL MEMORY IN IT.
#
#   tests/tools/run-bigmem.sh [-b BUILDDIR] [-t SECONDS] [-B BACKEND]
#                             [-N BOARD] [-l]
#
# WHAT IT PROVES
#
#   That bring-up still works, and that the packet pool is still SANELY SIZED,
#   on a machine with 32 MB and 128 MB of Fast RAM in it.
#
# WHY IT EXISTS
#
#   src/netstack/netstack.c:541 sizes the pool off AvailMem():
#
#       packets = (avail / AMI_POOL_MEM_DIVISOR) / stride
#       clamped into [AMI_POOL_MIN_PACKETS, AMI_POOL_MAX_PACKETS] = [16, 512]
#
#   and the receive window, the acknowledgment threshold and the reader depth
#   are all shares of what that returns (include/aminetxduo/netstack.h:295-315).
#   So "how much memory does this machine have" is a real variable in the
#   stack's behaviour, and it is a variable this tree has only ever run at two
#   values: 0 MB and the lab A1200's 8 MB.  tools/amiberry-run.sh says as much
#   at its own AMINETXDUO_Z3MEM -- "an accelerated Amiga with 128 MB is a
#   configuration this 8 MB default never reaches" -- and nothing reached it.
#
#   SATURATION IS THE ASSERTION.  Below about 13.6 MB free, AvailMem()/16 is
#   what binds and the ceiling is never touched; the lab's 8 MB machine lands
#   around 370 packets.  Above it the arithmetic is supposed to saturate and
#   stop.  A gate that only asked for "at least 16" would be satisfied by a
#   pool that wrapped to nothing, because the MINIMUM clamp catches the wreck
#   on the way out and hides it.
#
#   So the two big arms are graded against EACH OTHER: quadruple the memory,
#   and the pool must not move.  That is a relation no single arm can assert,
#   and it separates arithmetic that saturated from arithmetic that is still
#   scaling and from arithmetic that overflowed into the floor -- three
#   outcomes a constant could not tell apart.
#
# THE BIG ARMS ARE AN A3000, AND THAT WAS MEASURED RATHER THAN ASSUMED
#
#   8 MB is the whole of Zorro II Fast RAM, so anything past it is Zorro III --
#   and Zorro III needs 32-BIT ADDRESSING.  A stock A1200 is
#   address_space_24 = true (cfgfile.cpp:9289) and the Z3 window is then never
#   mapped: the memory is in the config file, the guest never sees it, and the
#   run passes at 8 MB while its name claims 128.  A SILENT PASS, and exactly
#   the kind this file exists to refuse.
#
#   Asking for `address_space_24=false' on an A1200 does NOT fix it.  Measured
#   on 2026-08-25, one boot each, reading the guest's own pool line back:
#
#       A3000 68030  8 MB Fast + 32 MB Z3   pool 513 packets
#       A1200 68030  4 MB Fast + 32 MB Z3   pool 217 packets
#       A4000 68040  8 MB Fast + 32 MB Z3   pool 377 packets
#
#   Only the A3000 sees it.  217 is the A1200 with 4 MB and nothing else; 377
#   is the A4000 with 8 MB and nothing else.  Both of those runs were green and
#   both would have been a test of 32 MB that never had 32 MB in it.  So the
#   big arms boot an A3000, which needs AMINETXDUO_KICKSTART_A3000 -- and the
#   arm SAYS SO and skips rather than quietly falling back to a machine whose
#   memory does not exist.
#
#   THE FIGURE IS 513 AND NOT 512.  AMI_POOL_MAX_PACKETS is 512 and the clamp
#   does hold; netstack_test reports the pool's own total out of
#   nx_packet_pool_info_get(), and NetX Duo's accounting is one above the
#   number of packets asked for.  So the assertion is not `== 512'.  It is
#   SATURATION: the 32 MB arm and the 128 MB arm must agree exactly, and both
#   must be above the 8 MB arm.  Quadrupling the memory and getting the same
#   pool is the clamp working; getting a different one is the arithmetic still
#   scaling, and getting a SMALLER one is the overflow this arm is for -- which
#   a `>= 16' floor would have caught as a pass.
#
# COST: four boots, about three minutes.  Cheap enough to be worth having.
#
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

# ------------------------------------------------------------- the arms ----
#
# name  model  cpu  fastmem-MB  z3mem-MB  pool-expectation
#
#   band        AvailMem() is what binds and the exact figure is a property of
#               the machine, so only the clamps are asserted.
#   saturated   the ceiling is supposed to be doing the work.  Asserted
#               against the OTHER saturated arm rather than against a
#               constant: see the header on why the figure is 513.
#
# THE 0 MB ARM IS NOT PADDING.  It is the 1 MB floor in
# src/sana2/sana2_internal.h, and it is the only arm where the MINIMUM clamp is
# the one under load.  Keeping it beside the 128 MB arm is what makes the pair
# a range rather than two anecdotes.
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

# AMI_POOL_MAX_PACKETS is 512 (include/aminetxduo/netstack.h:317) and
# netstack_test reports nx_packet_pool_info_get()'s total, which is one above
# the number asked for -- 513 on a saturated machine, measured.  The ceiling
# here therefore has slack in it and is a SANITY bound, not the assertion; the
# assertion that matters is saturation, below.  The literals are repeated
# rather than read from the header on purpose: a gate that computes its
# expectation from the thing under test cannot fail when that thing changes.
POOL_CEILING=520
POOL_MIN=16

# WHAT THE POOL'S CLAMPS PRODUCED, ON THE SAME MEMORY PROFILE.
#
# netstack_test above pings.  It never opens a socket, so the receive window
# and the UDP queue that ami_bsd_tcp_window() and bsd_udp_queue_max() derive
# from the pool -- and CLAMP -- were the one thing this matrix did not read,
# on the one machine where the ceilings bind.  sockopt_test drives the shipped
# library over a run-time SANA-II device, so it needs no card and runs on
# every arm here; it prints poolclamp_tcp_window= and poolclamp_udp_queue=.
#
# A missing binary is a skip and not a failure: run-bigmem is asked for by
# tools/ci.sh matrix with a build that may not carry tests/sockopt.
clamp_arm() { # name model cpu fastmem z3mem
    local name="$1" model="$2" cpu="$3" fast="$4" z3="$5"
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

    printf '%-11s clamps PASS  tcp_window=%-7s udp_queue=%s\n' \
           "$name" "$win" "$queue" >> "$RESULTS"
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

    # THE PER-ARM CHECK IS ONLY THE CLAMPS.  Saturation is a relation BETWEEN
    # arms and cannot be decided from one, so it is asserted after the loop.
    local poolverdict=PASS
    if [ -z "$packets" ] || [ "$packets" = none ]; then
        poolverdict="FAIL(no_pool_line)"
    elif [ "$packets" -lt "$POOL_MIN" ] || [ "$packets" -gt "$POOL_CEILING" ]; then
        poolverdict="FAIL(${packets}_outside_${POOL_MIN}_${POOL_CEILING})"
    fi

    printf '%-11s %-6s %-7s fast=%-4s z3=%-4s %-5s pool=%-5s %-24s %s run_rc=%s\n' \
           "$name" "$model" "$cpu" "$fast" "$z3" "$verdict" "${packets:-none}" \
           "$poolverdict" "$expect" "$rc" >> "$RESULTS"

    # Remembered for the saturation check below.
    eval "POOL_${name//-/_}=\${packets:-0}"

    clamp_arm "$name" "$model" "$cpu" "$fast" "$z3" || verdict=FAIL

    [ "$verdict" = PASS ] && [ "$poolverdict" = PASS ]
}

FAILED=0
COUNT=0
SKIPPED_BIG=0
while read -r name model cpu fast z3 expect; do
    [ -n "$name" ] || continue

    # A3000 NEEDS ITS OWN ROM.  quickstart=A3000 selects the MACHINE, not the
    # ROM, so an A1200 Kickstart boots A3000 hardware and the mismatch shows up
    # later as a device that will not open -- which reads as a defect in the
    # driver.  tools/amiberry-run.sh says the same thing at its own kickstart
    # block.  Skipping loudly beats a red arm that is really a missing file.
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

# ---------------------------------------------------------- saturation ----
#
# THE ASSERTION THAT NEEDS TWO ARMS.  Quadrupling the memory and getting the
# same pool is the clamp working.  A different figure is the arithmetic still
# scaling past its ceiling; a SMALLER one is the overflow this whole arm is
# for, and a `>= 16' floor grades that as a pass because the minimum clamp
# rescues the wreck on the way out.
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
echo "  bring-up red only on the accel arms  the stack does not survive a" >&2
echo "    machine this size.  src/netstack/netstack.c:541 and everything" >&2
echo "    sized off it: window ceiling, ack threshold, reader depth." >&2
echo >&2
echo "  The drives are at $ROOT/build/amiberry-testhd-matrix-mem-*." >&2
exit 1
