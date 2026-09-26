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
#             per-copy confirmation pages; it enters non-default values
#   STATIC    "no" to DHCP, the only way into P_ask_ip and P_ip_parse
#   INVALID_STATIC enters an out-of-range address, requires the validation
#                   page to repeat, then corrects it before files are written
#   INVALID_NAMES does the same for the Expert interface and host-name fields
#   NO_DRIVERS declines the three supplied device images and proves an
#              existing vendor driver still boots unchanged
#   NO_BOOT    installs without startup lines, then starts it manually
#   SYSTEM_RERUN    installs the system layout twice in place
#   SYSTEM_RECONFIGURE installs twice and takes "Set them up again", proving
#                   replacement plus the S:Network-Startup.old recovery path
#   SYSTEM_MINIMAL  installs and boots the minimal profile
#   SYSTEM_MICRO    installs and boots the micro profile
#   XSURF100_MINIMAL_* / XSURF100_MICRO_* boot both reduced profiles with
#                   iComp's vendor x-surf-100.device, DHCP and static. The
#                   vendor driver changes its copy path if it sees AMITCP
#                   during OpenDevice; an A2065-only profile check misses it.
#   FULL_MINIMAL / MINIMAL_FULL / FULL_MICRO / MICRO_FULL exercise upgrades
#                   between profiles, including the private TLS pairing
#   DRAWER_FULL     refuses to disturb a foreign stack, then installs the
#                   full profile in its own drawer twice
#   DRAWER_MINIMAL  does the same with the minimal profile
#   DRAWER_MICRO    does the same with the micro profile
#   DRAWER_FULL_MINIMAL / DRAWER_MICRO_FULL cover the same profile edges
#                   without ever touching system libraries or configuration
#   ROADSHOW_* / AMITCPNG_* seed named existing-stack layouts, then either
#                   decline with a zero-diff oracle or replace while keeping
#                   editable configuration and the AmiTCP: assignment
#   EMU68_* inject deterministic device-tree results and prove the Installer
#                   creates genet/wifipi definitions and starts only Ethernet
#   CANCEL_DRIVERS clicks the real Abort button before writes and requires an
#                   exact zero-diff destination
#   MISSING_* corrupt the throw-away archive at four distinct preflights and
#                   require refusal before the destination changes
#   NO_CARD         proves a Novice install with no detected driver refuses
#                   without writing anything
#   STALE_PI_DRIVERS proves shipped Pi driver files from an earlier install
#                   are not mistaken for Pi hardware on another machine
#   HARDWARE_XSURF100 leaves out the vendor driver and proves the hardware
#                   probe selects the board and writes the anxnet DEVICE/CARD
#                   pair without a user manufacturing the answer
#   HARDWARE_PCMCIA does the same through card.resource and identity tuples
#   TERMINAL        opts into the browser services, reinstalls, and exercises
#                   them from a second machine
#   STATIC_NO_DRIVERS proves two non-default questions can be answered in one
#                   run while the existing vendor driver supplies the card
#
# AMINETXDUO_REQUIRE_ALL_SCENARIOS=1 makes any missing ingredient or peer a
# failure.  The release e2e stage sets it: a release gate may not go green
# after silently skipping a common installation.
#
# Runs are serialised by the emulator rig lock.  This is deliberately a long
# release gate: every row starts with a fresh Workbench and reaches a verdict.
# SPDX-License-Identifier: MIT

set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ARGS=("$@")

SCENARIOS=(
    NOVICE AVERAGE EXPERT STATIC INVALID_STATIC INVALID_NAMES
    NO_DRIVERS NO_BOOT
    SYSTEM_RERUN SYSTEM_RECONFIGURE SYSTEM_MINIMAL SYSTEM_MICRO
    XSURF100_MINIMAL_DHCP XSURF100_MINIMAL_STATIC
    XSURF100_MICRO_DHCP XSURF100_MICRO_STATIC
    FULL_MINIMAL MINIMAL_FULL FULL_MICRO MICRO_FULL
    DRAWER_FULL DRAWER_MINIMAL DRAWER_MICRO
    DRAWER_FULL_MINIMAL DRAWER_MICRO_FULL
    ROADSHOW_LEAVE ROADSHOW_REPLACE AMITCPNG_LEAVE AMITCPNG_REPLACE
    EMU68_GENET EMU68_WIFI EMU68_BOTH EMU68_BOTH_MICRO
    CANCEL_DRIVERS MISSING_CORE MISSING_DRIVER MISSING_PROBE MISSING_MINIMAL
    NO_CARD STALE_PI_DRIVERS HARDWARE_XSURF100 HARDWARE_PCMCIA
    TERMINAL STATIC_NO_DRIVERS
    SNTP SNTP_INVALID SNTP_RERUN
    SPACES SPACES_DRAWER
)
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
    # Every non-level row names the exact visible choice or existing-install
    # path it owns.  Drawer rows install twice because the second pass is the
    # reinstall of our own self-contained stack; they also begin with a
    # foreign stack and prove that declining replacement writes nothing.
    case "$scenario" in
        NOVICE|AVERAGE)        opts=(-l "$scenario") ;;
        EXPERT)                opts=(-l EXPERT -E) ;;
        STATIC)                opts=(-l AVERAGE -S) ;;
        INVALID_STATIC)        opts=(-l AVERAGE -V) ;;
        INVALID_NAMES)         opts=(-l EXPERT -I) ;;
        NO_DRIVERS)            opts=(-l AVERAGE -J) ;;
        NO_BOOT)               opts=(-l AVERAGE -B) ;;
        SYSTEM_RERUN)          opts=(-l AVERAGE -R) ;;
        SYSTEM_RECONFIGURE)    opts=(-l AVERAGE -U) ;;
        SYSTEM_MINIMAL)        opts=(-l AVERAGE -p minimal) ;;
        SYSTEM_MICRO)          opts=(-l AVERAGE -p micro) ;;
        XSURF100_MINIMAL_DHCP) opts=(-l AVERAGE -p minimal -N xsurf100z3) ;;
        XSURF100_MINIMAL_STATIC) opts=(-l AVERAGE -p minimal -N xsurf100z3 -S) ;;
        XSURF100_MICRO_DHCP)   opts=(-l AVERAGE -p micro -N xsurf100z3) ;;
        XSURF100_MICRO_STATIC) opts=(-l AVERAGE -p micro -N xsurf100z3 -S) ;;
        FULL_MINIMAL)          opts=(-l AVERAGE -x full-minimal) ;;
        MINIMAL_FULL)          opts=(-l AVERAGE -x minimal-full) ;;
        FULL_MICRO)            opts=(-l AVERAGE -x full-micro) ;;
        MICRO_FULL)            opts=(-l AVERAGE -x micro-full) ;;
        DRAWER_FULL)           opts=(-l AVERAGE -D -g -p drawer) ;;
        DRAWER_MINIMAL)        opts=(-l AVERAGE -D -p minimal) ;;
        DRAWER_MICRO)          opts=(-l AVERAGE -D -p micro) ;;
        DRAWER_FULL_MINIMAL)   opts=(-l AVERAGE -D -x full-minimal) ;;
        DRAWER_MICRO_FULL)     opts=(-l AVERAGE -D -x micro-full) ;;
        ROADSHOW_LEAVE)        opts=(-l AVERAGE -f roadshow-leave) ;;
        ROADSHOW_REPLACE)      opts=(-l AVERAGE -f roadshow-replace) ;;
        AMITCPNG_LEAVE)        opts=(-l AVERAGE -f amitcpng-leave) ;;
        AMITCPNG_REPLACE)      opts=(-l AVERAGE -f amitcpng-replace) ;;
        EMU68_GENET)           opts=(-l AVERAGE -e genet) ;;
        EMU68_WIFI)            opts=(-l AVERAGE -e wifi) ;;
        EMU68_BOTH)            opts=(-l AVERAGE -e both) ;;
        EMU68_BOTH_MICRO)      opts=(-l AVERAGE -e both -p micro) ;;
        CANCEL_DRIVERS)        opts=(-l AVERAGE -c drivers) ;;
        MISSING_CORE)          opts=(-l AVERAGE -m core) ;;
        MISSING_DRIVER)        opts=(-l AVERAGE -m driver) ;;
        MISSING_PROBE)         opts=(-l AVERAGE -m probe) ;;
        MISSING_MINIMAL)       opts=(-l AVERAGE -m minimal) ;;
        NO_CARD)               opts=(-l NOVICE -C) ;;
        STALE_PI_DRIVERS)      opts=(-l NOVICE -z) ;;
        HARDWARE_XSURF100)     opts=(-l AVERAGE -M -N xsurf100z2) ;;
        HARDWARE_PCMCIA)       opts=(-l AVERAGE -M -N ne2000_pcmcia) ;;
        TERMINAL)              opts=(-l AVERAGE -H) ;;
        STATIC_NO_DRIVERS)     opts=(-l AVERAGE -S -J) ;;
        SNTP)                  opts=(-l AVERAGE -s yes) ;;
        SNTP_INVALID)          opts=(-l AVERAGE -s invalid) ;;
        SNTP_RERUN)            opts=(-l AVERAGE -s yes -R) ;;
SPACES)                opts=(-l AVERAGE -W) ;;
SPACES_DRAWER)         opts=(-l AVERAGE -D -W) ;;
    esac

    AMINETXDUO_RUN_TAG="matrix-$scenario" \
        "$ROOT/install/test/run-workbench.sh" "${opts[@]}" "${ARGS[@]}"
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
if [ "${AMINETXDUO_REQUIRE_ALL_SCENARIOS:-0}" = "1" ] &&
   [ "$skipped" != "0" ]; then
    echo "==> RELEASE GATE INCOMPLETE: $skipped common scenario(s) skipped"
    exit 1
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
