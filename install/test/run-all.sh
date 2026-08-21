#!/usr/bin/env bash
#
# Every installer scenario, in order, on a real Workbench 3.1.
#
#   install/test/run-all.sh [-a ARCHIVE]
#
# The harness is install/test/run-workbench.sh.  This used to call
# install/test/run-installer-fsuae.sh, which was deleted with the rest of the
# fs-uae harnesses; `set -uo pipefail` has no -e, so all five scenarios scored
# 127, the script exited 5, and nothing invoked it, so nothing ever saw that.
#
# Each scenario is a full install onto a freshly staged bare machine:
#
#   NOVICE    no questions asked at all, the card is auto-detected from
#             DEVS: and every default has to be right on its own
#   AVERAGE   the questions a normal install asks, all answered with the
#             default
#   EXPERT    the same plus the unit number, the interface name and the
#             confirmation page for every copy and every file written
#   STATIC    "no" to the DHCP question, which is the only way into the four
#             P_ask_ip prompts and into P_ip_parse.  run-workbench.sh -S,
#             which answers that page by the LABEL of its second button --
#             installdrive.c already had that mechanism for -H, and the
#             DRIVE_NO_ON_YESNO page counter this scenario used to wait on is
#             set by nothing and is an index, so it would drift the first time
#             a question moved.  The four prompts are answered with their own
#             defaults; the run asserts CONFIGURE=STATIC and all four values
#             reached DEVS:.
#   RERUN     installs over itself; the second pass has to notice the existing
#             configuration and keep it.  This is run-workbench.sh -H, which
#             installs three times.
#
# It starts with ICONS, which hands the generated .info files to the real
# icon.library rather than trusting the generator to grade its own homework.
#
# Each then power-cycles the installed machine and requires the stock
# Startup-Sequence to reach S:User-Startup and bring the network up on its own.
#
# EXPERT is the reason this file is worth repairing rather than deleting: it is
# the only level that draws the unit-number and interface-name prompts
# (Install-AmiNetXDuo:783-804) and the confirmation page on each copylib, and
# no caller anywhere passes -l EXPERT.
#
# Runs are serialised by tools/amiberry-run.sh's lock, so this takes a while --
# budget about fifteen minutes.
#
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
