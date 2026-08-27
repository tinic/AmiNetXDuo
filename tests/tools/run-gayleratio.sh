#!/usr/bin/env bash
#
# HOW FAST IS THIS CPU AGAINST THE CHIPSET, measured in the machine.
#
#   tests/tools/run-gayleratio.sh [-b builddir] [-t seconds] [-B backend]
#                                 [-r spins] [-a arm[,arm]] [-l]
#
# src/netdev/netdev_clock.c exists because a delay counted in bus reads is a
# measure of the CPU and not of time, and the number that separates the two
# machines is SPINS PER RASTER LINE: src/netdev/test/test_netdev_clock.c calls
# 256 the reference 14 MHz 68020 and 25600 an accelerator, and asserts that the
# Gayle hold is 300 ms on both.  That host test drives a simulated beam.
#
# This is the same ratio taken off a real emulated machine.  netdev_pcmcia.c
# publishes it through the probe record (ANXDIAG_CLOCK) and CheckNetDevice
# prints it, so the arm is one boot per CPU rate with one command in it.
#
# WHAT IT SETTLES, MEASURED ON THIS RIG 2026-08-27, Amiberry 149e9aa2:
#
#   default (A1200, cpu_multiplier=4)    13 MHz       16 spins per line
#   cpu_multiplier=16                    52 MHz       36
#   cpu_multiplier=64                   209 MHz       49
#   cpu_multiplier=256                  837 MHz       54
#   cpu_speed=max                    host-bound       27
#
# THE RATIO IS NOT REACHABLE UNDER EMULATION AND IT SATURATES.  Sixty-four
# times the nominal CPU rate buys 3.4 times the spins, and cpu_speed=max --
# which is host-bound rather than a rate at all -- comes in BELOW
# cpu_multiplier=64.  The loop body is a read of $DFF004, and a CPU access to
# the chipset is synchronised to the chipset's own clock in the emulator, so
# more cycles between accesses does not make an access cheaper.  The real
# machines are 256 (reference) and 25600 (accelerated): the emulator does not
# reach the SLOWER of the two, and is 16 times under it at the rate that is
# supposed to BE it.
#
# So the accelerated branch is proven in the host tier and only there:
# src/netdev/test/test_netdev_clock.c, `ctest -R netdev_clock`.  What this arm
# holds down instead is the part the emulator can answer -- the card claims at
# every rate, the measurement is monotonic in the rate, and the ceiling is a
# recorded number rather than a belief.  gayleratio_timed_path_binds= says, per
# arm, whether the beam clock decided anything at all: below us_per_line * 4
# spins the caller's own iteration count is the longer of the two and the wait
# is the old counted loop, which is every arm here.
#
# MEASURED ON THE ACCELERATED A1200, 2026-08-27.  amiga-1200.local is an A1200
# with a PiStorm32 and a 3c589 in the socket, and CheckNetDevice on it reports
#
#   spins_per_line=25   us_per_line=63   gayleratio_timed_path_binds=no
#   spins_per_line=27   us_per_line=63   gayleratio_timed_path_binds=no
#
# Two readings, either side of a power cycle, on the same machine.
#
# TWENTY-FIVE AND TWENTY-SEVEN, not 25600.  The accelerated constant in
# src/netdev/test/test_netdev_clock.c:176 is not what an accelerated machine
# measures, and neither is the 256 at :175 what a stock one does -- the whole
# emulated table above tops out at 54.  The reason is the same one that makes
# the table saturate: the loop body is a read of $DFF004, and a PiStorm does
# not accelerate the chipset, so the spin rate is a property of the Gayle and
# chip bus rather than of the CPU.  This machine is SLOWER per spin than the
# 14 MHz reference the counts were calibrated on -- 25 spins to a 63 us line
# is 2.5 us a spin against the reference 0.25 us -- so the unconditional floor
# in netdev_wait_begin() (us * 4 spins, 1200000 of them for the Gayle hold)
# decides every wait here and decides it LONG, not short.  The failure this
# clock was written against is the opposite of the one real hardware shows.
#
# WHAT REMAINS UNPROVEN: that any machine reaches a spin rate at which the beam
# clock binds.  Nothing in this lab does, emulated or physical.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

BUILD="${AMINETXDUO_BUILD:-build/cm}"
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
MODEL=A1200
BOARD=ne2000_pcmcia
TIMEOUT=0
ONLY=""
LIST=0

# ACCELERATED_TICKS_PER_LINE, src/netdev/test/test_netdev_clock.c:176.  The one
# number this arm has to reach, and it is defined there rather than here.
# 0 = report the ceiling and do not judge it, which is the default because the
# emulator cannot reach either real machine.  -r <n> asserts a floor, and is
# how this arm would be pointed at hardware that can.
WANT_SPINS="${AMINETXDUO_GAYLE_SPINS:-0}"

# The two real machines, from src/netdev/test/test_netdev_clock.c:175-176.
# Quoted so the table says what the gap is without anyone having to look.
REFERENCE_SPINS=256
ACCELERATED_SPINS=25600

while getopts "b:t:B:r:a:l" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        r) WANT_SPINS="$OPTARG" ;;
        a) ONLY="$OPTARG" ;;
        l) LIST=1 ;;
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

# arm      cpu    extra config             what it is
ARMS="
stock        68020  -                     13MHz-A1200
mult16       68020  cpu_multiplier=16     52MHz
mult64       68020  cpu_multiplier=64     209MHz
mult256      68020  cpu_multiplier=256    837MHz
unthrottled  68020  cpu_speed=max         host-bound
"

selected() { # arm
    case ",$ONLY," in
        ,,) return 0 ;;
        *",$1,"*) return 0 ;;
        *) return 1 ;;
    esac
}

if [ "$LIST" = 1 ]; then
    printf '%-13s %-6s %-21s %s\n' arm cpu config nominal
    printf '%s\n' "$ARMS" | while read -r a c e n; do
        [ -n "$a" ] || continue
        selected "$a" || continue
        printf '%-13s %-6s %-21s %s\n' "$a" "$c" "$e" "$n"
    done
    exit 0
fi

TOOLS="$ROOT/$BUILD/src/tools"
for f in "$TOOLS/ToolsSmoke" "$TOOLS/CheckNetDevice"; do
    [ -f "$f" ] || { echo "gayleratio_stage=missing:$f" >&2; exit 2; }
done

# shellcheck source=../../tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"
# shellcheck source=../../tools/emu-board.sh
. "$ROOT/tools/emu-board.sh"

sana2_select "$BOARD" "$ROOT/$BUILD"
[ -n "$SANA2_SEL_PATH" ] || {
    echo "gayleratio_stage=missing:anxnet.device in $BUILD" >&2; exit 2; }

STAGE="$ROOT/build/gayleratio-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/devs/Networks" "$STAGE/libs"
cp "$SANA2_SEL_PATH" "$STAGE/devs/Networks/$SANA2_SEL_DRIVER"
cp "$TOOLS/CheckNetDevice" "$STAGE/CheckNetDevice"
echo "SYS:CheckNetDevice" > "$STAGE/commands.txt"

PROOF=$(emu_board_log_proof "$BOARD")

RESULTS="$ROOT/build/gayleratio-results.txt"
: > "$RESULTS"
LOGDIR="$ROOT/build/gayleratio-logs"
rm -rf "$LOGDIR"; mkdir -p "$LOGDIR"

COUNT=0; FAILED=0; TOP=0; TOP_ARM=""; PREV=0; PREV_ARM=""; MONOTONIC=yes

run_arm() { # arm cpu extra nominal
    local arm="$1" cpu="$2" extra="$3" nominal="$4"
    local tag="gayleratio-$arm" t="$TIMEOUT" rc report uaelog spins us claimed
    local started

    [ "$t" != 0 ] || t=600

    echo
    echo "=============================================================="
    echo "==> $arm: $MODEL at $cpu, ${extra/-/no extra config}  ($nominal)"
    echo "=============================================================="

    started=$(date +%s)
    (
        export AMINETXDUO_RUN_TAG="$tag"
        [ "$extra" = - ] || export AMINETXDUO_AMIBERRY_EXTRA="$extra"
        "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$BACKEND" -m "$MODEL" \
            -c "$cpu" -t "$t" \
            "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
            "$STAGE/libs" "$STAGE/CheckNetDevice"
    ) > "$LOGDIR/$arm.log" 2>&1
    rc=$?
    tail -25 "$LOGDIR/$arm.log"

    report="$ROOT/build/amiberry-testhd-$tag/tools.txt"
    uaelog="$ROOT/build/amiberry-$tag.log"

    spins=$(sed -n 's/.*delay clock measured \([0-9][0-9]*\) spin(s) per raster line.*/\1/p' \
            "$report" 2>/dev/null | head -1)
    us=$(sed -n 's/.*A scan line was measured at \([0-9][0-9]*\) us.*/\1/p' \
         "$report" 2>/dev/null | head -1)

    claimed=no
    if [ -n "$PROOF" ] && grep -aqF "$PROOF" "$uaelog" 2>/dev/null; then
        claimed=yes
    fi

    printf 'arm=%s cpu=%s config=%s nominal=%s spins_per_line=%s us_per_line=%s card_claimed=%s run_rc=%s wall_s=%s log=%s\n' \
           "$arm" "$cpu" "$extra" "$nominal" "${spins:-none}" "${us:-none}" \
           "$claimed" "$rc" "$(( $(date +%s) - started ))" "$LOGDIR/$arm.log" \
           | tee -a "$RESULTS"

    # DID THE BEAM DECIDE ANYTHING?  netdev_wait_begin() takes the caller's own
    # count as an unconditional floor and pc_settle() passes us * 4, so the
    # clock only shortens a wait once a line holds more than us_per_line * 4
    # spins.  Below that the wait is the old counted loop with a measurement
    # running beside it, whatever the code path says.
    if [ -n "$spins" ] && [ -n "$us" ]; then
        if [ "$spins" -gt "$((us * 4))" ]; then
            echo "  gayleratio_timed_path_binds=yes (over $((us * 4)))"
        else
            echo "  gayleratio_timed_path_binds=no (floor is $((us * 4)) spins\
 a line; the caller's count is the whole wait)"
        fi
    fi

    COUNT=$((COUNT + 1))

    if [ -z "$spins" ]; then
        echo "FAIL: $arm printed no clock measurement at all" >&2
        FAILED=$((FAILED + 1))
        return 0
    fi

    # THE CARD HAS TO COME UP AT EVERY RATE.  That is the branch: the 300 ms
    # Gayle hold is a duration, and a loop that counts bus reads gets shorter
    # as the CPU gets faster.  A claim that fails at the top of the table is
    # the defect this whole clock exists to prevent.
    if [ "$claimed" != yes ]; then
        echo "FAIL: $arm never reached '$PROOF': the card was not claimed at\
 this rate" >&2
        FAILED=$((FAILED + 1))
    fi

    if [ "$spins" -gt "$TOP" ]; then TOP="$spins"; TOP_ARM="$arm"; fi

    # MONOTONICITY IS ASSERTED OVER THE MULTIPLIER ARMS ONLY.  cpu_speed=max is
    # not a point on the rate axis: it is "as fast as this host goes", it has
    # no nominal MHz, and it measures BELOW cpu_multiplier=64 here.  Grading it
    # as though it were the top of the ramp turns a property of the host into a
    # failed assertion.
    case "$extra" in
    cpu_multiplier=*|-)
        if [ "$PREV" -ne 0 ] && [ "$spins" -le "$PREV" ]; then
            echo "FAIL: $arm measured $spins spins per line, no more than\
 $PREV_ARM's $PREV: the rate is not the variable it is supposed to be" >&2
            MONOTONIC=no
            FAILED=$((FAILED + 1))
        fi
        PREV="$spins"; PREV_ARM="$arm"
        ;;
    esac
    return 0
}

while read -r arm cpu extra nominal; do
    [ -n "$arm" ] || continue
    selected "$arm" || continue
    run_arm "$arm" "$cpu" "$extra" "$nominal"
done <<EOF
$ARMS
EOF

echo
echo "===================== spins per raster line ====================="
cat "$RESULTS"
echo "================================================================"
echo "gayleratio_arms=$COUNT gayleratio_failed=$FAILED\
 gayleratio_top=$TOP top_arm=${TOP_ARM:-none}\
 gayleratio_want=$WANT_SPINS gayleratio_monotonic=$MONOTONIC"
echo "gayleratio_ceiling=$TOP reference=$REFERENCE_SPINS\
 accelerated=$ACCELERATED_SPINS\
 short_of_reference=$(( REFERENCE_SPINS / (TOP > 0 ? TOP : 1) ))x\
 short_of_accelerated=$(( ACCELERATED_SPINS / (TOP > 0 ? TOP : 1) ))x"
echo "gayleratio_host_tier=src/netdev/test/test_netdev_clock.c (ctest -R netdev_clock)"

[ "$COUNT" -gt 0 ] || { echo "result=refused"; echo "no arm ran" >&2; exit 2; }

if [ "$WANT_SPINS" != 0 ] && [ "$TOP" -lt "$WANT_SPINS" ]; then
    echo "FAIL: the fastest arm reached $TOP spins per raster line and -r asked\
 for $WANT_SPINS." >&2
    FAILED=$((FAILED + 1))
fi

if [ "$FAILED" = 0 ]; then
    echo "result=ok"
    exit 0
fi
echo "result=fail"
exit 1
