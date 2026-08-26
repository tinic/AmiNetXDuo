#!/usr/bin/env bash
#
# Every installer scenario, in order, on a real Workbench 3.1.
#
#   install/test/run-all.sh [-a ARCHIVE]
#
# The harness is install/test/run-workbench.sh.  Each scenario is a full
# install onto a freshly staged bare machine, then a power cycle that requires
# the stock Startup-Sequence to reach S:User-Startup on its own:
#
#   ICONS     the generated .info files, through the real icon.library
#   NOVICE    no questions at all; every default has to be right on its own
#   AVERAGE   the normal questions, all answered with the default
#   EXPERT    the only level drawing the unit-number, interface-name and
#             per-copy confirmation pages, and no other caller passes it
#   STATIC    "no" to DHCP, the only way into P_ask_ip and P_ip_parse
#   RERUN     installs three times; later passes must keep the configuration
#
# Runs are serialised by tools/amiberry-run.sh's lock -- about fifteen minutes.
# SPDX-License-Identifier: MIT

set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ARGS=("$@")

SCENARIOS=(NOVICE AVERAGE EXPERT STATIC RERUN)
declare -a RESULTS

ROOT=$(cd "$HERE/../.." && pwd)
GCC="${AMIGA_GCC:-$HOME/amigaos/tools/m68k-amigaos-gcc/bin/m68k-amigaos-gcc}"
NDK="${AMIGA_NDK:-$HOME/amigaos/tools/m68k-amigaos-gcc/m68k-amigaos/ndk-include}"

failures=0
skipped=0
# HOW MANY SCENARIOS REACHED A VERDICT.  Without it, `exit "$failures"` at the
# end reports 0 for a box where every scenario skipped -- no licensed
# Workbench, no ADFs, no peer -- and prints "everything that could run passed"
# over a run that installed nothing.  A skip can still be honest -- no ADFs, no
# peer -- so "any skip" is the wrong gate; the gate is whether a single
# scenario reached a verdict.
#
# SCENARIOS ONLY.  ICONS is a preamble -- it hands the generated .info files to
# the real icon.library and needs no Workbench -- and counting it here would
# let the one cheap check stand in for the five this file is named after.
passed=0

# --------------------------------------------------------------- the icons --
#
# Cheap and worth doing first: hand the generated .info files to the real
# icon.library rather than trusting the generator and its own reader to agree
# with each other.

echo
echo "############################################################"
echo "#  ICONS"
echo "############################################################"
python3 "$ROOT/install/tools/makeicon.py" "$ROOT/install" >/dev/null
python3 "$ROOT/install/tools/showicon.py" "$ROOT/install"/*.info >/dev/null
"$GCC" -O2 -m68020 -Wall -Wextra -I"$NDK" \
       -o "$ROOT/build/icontest" "$ROOT/install/test/icontest.c"
rm -rf "$ROOT/build/icons-stage"
mkdir -p "$ROOT/build/icons-stage/AmiNetXDuo"
cp "$ROOT/install"/*.info "$ROOT/build/icons-stage/AmiNetXDuo/"
if AMINETXDUO_RUN_TAG=icons "$ROOT/tools/amiberry-run.sh" -t 90 \
        "$ROOT/build/icontest" "$ROOT/build/icons-stage/AmiNetXDuo"; then
    RESULTS+=("  PASS  ICONS")
else
    RESULTS+=("  FAIL  ICONS")
    failures=$((failures + 1))
fi
for scenario in "${SCENARIOS[@]}"; do
    echo
    echo "############################################################"
    echo "#  $scenario"
    echo "############################################################"

    # What each scenario becomes now that run-workbench.sh is the harness.
    # STATIC and RERUN are not levels: RERUN is what -H already does, three
    # installs over one another, and STATIC is -S, which answers the DHCP
    # question no.  Both need a level where the questions are drawn at all.
    case "$scenario" in
        NOVICE|AVERAGE|EXPERT) opts=(-l "$scenario") ;;
        RERUN)                 opts=(-l AVERAGE -H) ;;
        STATIC)                opts=(-l AVERAGE -S) ;;
    esac

    "$HERE/run-workbench.sh" "${opts[@]}" "${ARGS[@]}"
    rc=$?
    case "$rc" in
        # run-workbench.sh's own codes: 2 is "this box cannot run this test",
        # 3 is "no second machine could reach the guest".  Neither is a pass
        # and neither is a defect in the installer, so they are their own
        # result rather than a failure that sends somebody reading the
        # installer script.
        0) RESULTS+=("  PASS  $scenario")
           passed=$((passed + 1)) ;;
        2) RESULTS+=("  SKIP  $scenario -- an ingredient is missing on this machine")
           skipped=$((skipped + 1)) ;;
        3) RESULTS+=("  SKIP  $scenario -- no second machine could reach the Amiga")
           skipped=$((skipped + 1)) ;;
        *) RESULTS+=("  FAIL  $scenario (exit $rc)")
           failures=$((failures + 1)) ;;
    esac
done

echo
echo "============================================================"
echo "  summary"
echo "============================================================"
printf '%s\n' "${RESULTS[@]}"
echo
echo "scenarios_passed=$passed"
echo "scenarios_failed=$failures"
echo "scenarios_skipped=$skipped"
if [ "$failures" != "0" ]; then
    echo "==> $failures check(s) failed"
    exit "$failures"
fi
# Nothing failed AND nothing ran.  77 is what tools/ci-arm.sh renders as
# SKIPPED and what the rest of the tree means by "no verdict in either
# direction"; this used to be 0 and read as a full installer sweep.
if [ "$passed" = "0" ]; then
    echo "==> NOTHING WAS TESTED: not one installer scenario reached a"
    echo "    verdict, so this says nothing about the installer in either"
    echo "    direction.  The SKIP lines above say what is missing."
    exit 77
fi
echo "==> everything that could run passed"
exit 0
