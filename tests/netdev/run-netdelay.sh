#!/usr/bin/env bash
#
# THE ACCELERATOR ARM.  Does a netdev delay still take the time it asks for
# when the CPU is fast?
#
#   tests/netdev/run-netdelay.sh [-b BUILDDIR] [-m MODEL] [-t SECONDS]
#                                [-c CPU]...
#
# Every delay in src/netdev used to be a loop counted in bus reads, sized for a
# 14 MHz 68020.  A count of reads is a measure of THIS CPU and not of time, so
# the same loop behind an accelerator finishes in some fraction of the wait it
# was written to be -- and the longest of them holds a PC Card in reset for a
# minimum the card's documentation states in milliseconds.  netdev_clock.c
# replaced the counting with a wait measured against the raster beam.
#
# TWO CPUs, AND THE SECOND IS THE POINT.  At 68020 the old shape and the new
# one come out at much the same length, which is exactly why nobody saw this:
# the machine the numbers were chosen on is the machine they are right for.
# They separate at 68060, where the CPU is fast and the chipset is not, and
# that is the condition an accelerated Amiga puts the driver in.  Both arms
# must pass; the 68060 one is what turns red if the wait goes back to counting.
#
# NO CARD, NO BRIDGE, NO PEER.  The guest program compiles netdev_clock.c in
# and times it directly, so this needs nothing but an emulator and a ROM.  That
# is deliberate: the arms that need an a2065.device, a bridged interface and an
# ssh peer are the first to go unrunnable on a machine that has not been set
# up for them, and a regression gate for a timing defect should not be one of
# them.
#
# WHY IT IS NOT ENOUGH ON ITS OWN, and what backs it up:
#
#   src/netdev/test/test_netdev_clock.c   the same question with a beam the
#                                         test supplies, so it can be exact,
#                                         and can be a 14 MHz 68020 and an
#                                         accelerator in one binary.  Runs on
#                                         every push, needs no emulator.
#   tools/check-netdev-delays.sh          that the CALL SITES use the wait,
#                                         which no run-time test can see.
#
# key=value and an exit code, through tools/test-verdict.sh like every other
# guest harness here.
#
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

# The default pair, and the reason for each.  68020 is the machine every
# number in these files was chosen against, so it is the control: if the arm
# below fails there, the wait is wrong on the reference machine and not merely
# on a fast one.  68060 is the fastest CPU Amiberry emulates and stands in for
# the accelerators -- PiStorm32, Blizzard, ACA, Vampire -- that nobody here
# has one of.
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

    # Six checks: the E-Clock, the beam, the display mode, the clock's floor
    # and its ceiling, and the call site's floor.  A run that reached only
    # some of them is not a pass, which is what the count is for.
    verdict_guest "netdelay-$cpu" 6 "$run_rc" "$log" || rc=1

    sed -n 's/^\(beam:\|E-Clock\|300 ms from\|the pc_settle\|the same hold\)/  &/p' "$log"
done

exit $rc
