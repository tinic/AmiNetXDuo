#!/usr/bin/env bash
#
# THE SAME BRING-UP, ON A CPU THE DELAY LOOPS WERE NOT WRITTEN FOR.
#
#   tests/tools/run-cpuspeed.sh [-b BUILDDIR] [-t SECONDS] [-B BACKEND]
#                               [-N BOARD[,BOARD...]] [-c CPU[,CPU...]] [-l]
#
# WHAT IT PROVES
#
#   That an interface comes ONLINE and CARRIES A PACKET on a machine faster
#   than the 68020 this tree was written against -- 68030, 68040, 68060, and
#   the emulator's unthrottled setting, which is the closest thing here to an
#   accelerator.  Per board, per CPU, one boot each.
#
# WHY IT EXISTS
#
#   src/netdev counts BUS READS as a proxy for time.  netdev_pcmcia.c's
#   pc_settle() takes microseconds and turns them into `n = us * 4`, four reads
#   to the microsecond, which is a 14 MHz 68020 reading Gayle.  el3.c, ne2000.c,
#   netdev_cmds.c and netdev_isapnp.c all have a sibling of it.  On a machine
#   several times that speed every one of those loops returns early, and the
#   card is driven past a window the hardware needs.
#
#   NOTHING IN THIS TREE HAD EVER RUN A FAST CPU.  Not one arm, on any board,
#   at any tier.  So the whole class was invisible: the loops were correct for
#   the only machine that ever executed them, and there was no gate that could
#   hold an opinion about any other.  Three defects of this shape were found by
#   hand, on real hardware, in one evening -- which is what a matrix with a hole
#   this size costs.
#
#   The A1200's 68EC020 is the baseline arm rather than an omission: a run that
#   is red at 68060 and red at 68020 too is a rig fault or a broken build, and
#   the pair is what tells those apart from the thing this gate is for.
#
# WHAT THIS ARM CANNOT DO, MEASURED, AND SAID HERE SO NOBODY TRUSTS IT TOO FAR
#
#   IT DOES NOT REPRODUCE pc_settle().  This was measured on 2026-08-25 with
#   the `n = us * 4' loop still in the tree, unfixed, and every arm below was
#   green.  The reason is structural and no knob reaches it.
#
#   The window around netstack_startup(), which contains the PCMCIA claim and
#   therefore pc_settle(300000), timed off the stamped serial log on
#   ne2000_pcmcia at 68030:
#
#       cpu_multiplier    4 (13 MHz, stock A1200)     452 ms
#       cpu_multiplier   16 (52 MHz)                  390 ms
#       cpu_multiplier   64 (209 MHz)                 574 ms
#       cpu_multiplier  192 (628 MHz)                1714 ms
#
#   The loop does not get SHORTER as the CPU gets faster.  Past about 52 MHz
#   it gets LONGER, because an emulated bus read costs the HOST a roughly
#   fixed amount of work, so `us * 4' reads take host-time proportional to the
#   count and the emulated clock barely enters into it.  On a PiStorm the
#   ratio is the whole defect -- the CPU is tens of times faster and the Gayle
#   bus is not -- and under emulation that ratio cannot be created, because
#   the bus is not a slower thing that the CPU outruns.  On top of that the
#   emulated card has no settle time to violate: a software model answers a
#   register write immediately, so a delay that is too short has nothing to be
#   too short FOR.
#
#   So this arm covers the CPU-MODEL half of the class and not the CPU-RATE
#   half: code that behaves differently at 68030/68040/68060 -- a different
#   instruction set, caches that are on, alignment, anything gated on the
#   model -- and any path that hangs, expires or races when the processor
#   changes underneath it.  The timing-ratio half is
#   docs/TEST-MATRIX.md's "uncoverable without real hardware", and it is
#   listed there rather than quietly implied to be covered here.
#
#   THIS IS STILL WORTH BOOTING.  Before it, no arm in this tree had ever run
#   ANY CPU but the A1200's 68EC020, on any board, at any tier -- so the
#   entire class, both halves, had a coverage of zero.
#
# IT IS A REGRESSION GATE AND NOT A BENCHMARK
#
#   Nothing here gates on a rate, a duration or a byte count, and nothing is
#   compared against a recorded baseline.  The assertion is
#   tests/tools/bringup-verdict.sh: link up, an address, and an ICMP echo that
#   something off this machine answered.  A faster arm that comes up slower is
#   not a failure; an arm that does not come up is, at any speed.
#
#   Rates belong in tests/perf, where a baseline file and the rules for moving
#   it already exist.  A speed matrix that gates on throughput would go red on
#   host load and be turned off within a week.
#
# THE TWO BOARDS, AND WHY THESE TWO
#
#   ne2000_pcmcia   THE CODE THE DELAYS LIVE IN.  Our anxnet.device drives it
#                   as CARD=pcmcia (tools/sana2-stage.sh), so the claim path
#                   through netdev_pcmcia.c -- pc_settle() and the CIS walk --
#                   is what this boots.  It needs a Gayle machine and a 68020
#                   or better (tools/amiberry-run.sh:196-224), which every arm
#                   here is.
#   a2065           THE CONTROL.  A LANCE on the Zorro bus, driven by
#                   Commodore's own a2065.device, with no timing loop of ours
#                   anywhere in it.  If both boards go red at 68060 the fault
#                   is in the stack or the rig; if only the PCMCIA one does,
#                   it is where this file says it is.
#
# THE THREE KNOBS, BECAUSE -c ALONE IS NOT A SPEED
#
#   -c 68040 sets cpu_model, which is the INSTRUCTION SET.  It does not make
#   the machine quicker: Amiberry still paces a 68040 at the A1200's clock, so
#   an arm that only moved -c would be a model sweep wearing a speed sweep's
#   name.  Two more settings go in on top, both through
#   AMINETXDUO_AMIBERRY_EXTRA -- the documented escape hatch in
#   tools/amiberry-run.sh -- so this file writes no key that script does not
#   already know:
#
#     cpu_speed=max      m68k_speed = -1 (cfgfile.cpp:6807), the CPU
#                        unthrottled against a chipset still at Amiga rates.
#                        Measured worth about 1.24x here, not the tens the
#                        name suggests.
#     cpu_multiplier=64  the knob tools/amiberry-run.sh spells -k, measured
#                        linear at 3.27 MHz a step: 64 is nominally 209 MHz.
#                        This one really does raise emulated cycles per
#                        emulated second, which is the closest an emulator
#                        gets to an accelerator, and it is why the arm exists
#                        in this shape rather than as `-c' alone.
#
# COST: twelve boots, about three minutes.  Cheap enough for a nightly and
# nearly cheap enough for a push.  -N and -c narrow it.
#
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

# ------------------------------------------------------------- the arms ----
#
# board  cpu  speed
#
# `speed' is one of
#
#   real     the machine's own clock: an A1200's 14 MHz, which is what the
#            delay loops in src/netdev were calibrated against
#   max      cpu_speed=max, the CPU unthrottled
#   clk209   cpu_multiplier=64, nominally 209 MHz
#
# 68060 gets all three, because the set isolates INSTRUCTION SET from RATE.  A
# 68060 at the A1200's clock executes the same code the 68020 arm does at the
# same speed; the same 68060 at 209 MHz does not.  An arm that is green at
# 68060/real and red at 68060/clk209 has named the RATE as the variable, which
# no single arm can do, and that is a distinction worth two extra boots.
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

# ------------------------------------------------------------ the rig ------

EXE="$ROOT/$BUILD/tests/netstack/netstack_test"
[ -f "$EXE" ] || {
    echo "build $BUILD/tests/netstack/netstack_test first" >&2; exit 2; }

[ -n "${AMINETXDUO_KICKSTART:-}" ] || {
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2; exit 2; }

. "$ROOT/tests/tools/bringup-verdict.sh"

RESULTS="$ROOT/build/cpuspeed-results.txt"
: > "$RESULTS"

# ONE AT A TIME, AND NEVER IN PARALLEL.  Every arm is the same guest with the
# same MAC, and on a bridged backend two of them at once are one machine to the
# LAN's DHCP server and to every neighbour cache on it: the second run answers
# on the first one's lease and a broken arm reads as a passing one.  The run
# tags differ so the drives, the serial logs and the serial ports do not
# collide (tools/amiberry-run.sh derives all three from the tag), which is a
# different problem and not this one.
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

    # THE GUEST'S OWN CHECKS, BY NAME.  run-amiberry.sh has already applied
    # tools/test-verdict.sh -- a floor under the check count and no failures --
    # and that is not the assertion this arm wants: netstack_test's wire check
    # is conditional, so a guest that never got a lease runs one check fewer
    # and clears the floor.  See the head of tests/tools/bringup-verdict.sh.
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

# NAME THE SHAPE, because the reader's next question is always the same one.
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
