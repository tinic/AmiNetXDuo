#!/usr/bin/env bash
#
# Run the bsdsocktest conformance suite against our bsdsocket.library, under
# WinUAE on a remote Windows host.  The WinUAE counterpart of run-winuae.sh.
#
#   tests/conformance/run-winuae.sh [-m MODEL] [-c CPU] [-t SECONDS]
#                                   [-T TAG] [-a "SUITE ARGS"] [-p] [-b BUILDDIR]
#
# WHY THIS EXISTS SEPARATELY, stated carefully because the obvious version of
# it is wrong: FS-UAE runs the HOST tier perfectly well.  The guest dials out
# to 10.0.2.2 and SLIRP carries it -- measured, 18 of 19 in the sendrecv
# category.  What SLIRP cannot do is let the helper dial BACK, so the one test
# where the peer connects to the guest fails there with ECONNREFUSED, and
# uae_slirp_redir is an empty function in every slirp backend FS-UAE ships
# (docs/RESEARCH.md 60).
#
# WinUAE bridges the A2065 onto a real adapter, so the guest takes a real DHCP
# lease and a third machine on the LAN can reach it.  Point -a "HOST <ip>" at
# a machine running the suite's helper -- a THIRD machine, not this one and
# not the Windows host: a frame the Windows host sends to the guest's MAC
# leaves its NIC and never returns to that NIC's own pcap capture, so it looks
# unreachable from there while being reachable from everywhere else.
#
# AMINETXDUO_WINUAE_A2065 must name the host adapter to bridge onto -- the
# bare pcap device name from pcap_findalldevs; tools/winuae-run.sh adds the
# rpcap:// prefix.  Without it this runs on SLIRP and the HOST tier cannot
# pass.
#
# THE HOST TIER'S BULK TESTS DO NOT RUN ON A HOST THAT COALESCES RECEIVES.
# WinUAE 6.0.3 copies a captured frame into a 4000-byte buffer without
# checking its length, so the first coalesced frame -- 11734 bytes on
# winbuilder, where both ends of the LAN are virtual -- kills the emulator
# mid-suite.  Roadshow dies identically, so this is not ours; docs/RESEARCH.md
# 63.4 has the measurement and the upstream commit that fixes it.
#
# -b (or AMINETXDUO_BUILD) picks the build tree the library comes from, so the
# floor build and an -DAMINETXDUO_IPV6=ON build can both be measured.
#
# -p runs conf_probe instead of the suite: the hand-written triage walk that
# prints rc and errno for every call, which is how you find out *why* a
# category collapsed.
#
# AMINETXDUO_CONF_HELPER=<user@host> restarts the suite's helper on that
# machine first, which the network tier needs; see the block that does it.
#
# -a is the suite's own ReadArgs line, e.g.
#      -a "LOOPBACK NOPAGE"
#      -a "CATEGORY socket NOPAGE VERBOSE"
#      -a "HOST 10.0.2.2 NOPAGE"
#
# Stages LIBS:bsdsocket.library and LIBS:usergroup.library from build/cm,
# DEVS:a2065.device plus the netstack DEVS: config, the suite binary and the
# argument line, then boots tools/fsuae-run.sh with conf_launcher as the
# program the Startup-Sequence runs.  See conf_launcher.c for why there is a
# launcher at all.
#
# FS-UAE ships its own bsdsocket.library emulation.  If it answered
# OpenLibrary() the whole run would be measuring WinUAE's host-socket shim
# instead of our stack, so this script sets bsdsocket_library = 0 via a host
# configuration dropped into the run's private base directory (FS-UAE reads
# it after resolving base_dir; the config tools/fsuae-run.sh writes cannot
# carry the option).
#
# The emulator log still says "bsdsocket.library installed" either way -- that
# line is printed when the emulation is *built*, not when it is registered.
# It is not the proof.  The proof is threefold and checked below / visible in
# the output:
#   1. Removing build/cm/.../bsdsocket.library from the staged LIBS: makes the
#      suite bail with "bsdsocket.library not available" -- so nothing else on
#      the machine answers OpenLibrary("bsdsocket.library", 4).
#   2. The TAP log's "# bsdsocket.library:" line reads AmiNetXDuo, our
#      SBTC_RELEASESTRPTR.  UAE's emulation answers "UAE <version>".
#   3. build/serial-<tag>.log fills with our ami_log netstack bring-up
#      (DHCP lease, resolver) as the library is opened.
#
# Results land in:
#   build/winuae-testhd-<tag>/bsdsocktest.log   the TAP log -- the result
#   build/winuae-testhd-<tag>/conf-out.txt      the suite's console summary
#   build/winuae-serial-<tag>.log               our ami_log output
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=600
CPU=""
# -T wins, then AMINETXDUO_RUN_TAG from the environment, then the default. The
# environment has to be honoured here: this script exports AMINETXDUO_RUN_TAG
# to tools/fsuae-run.sh, so taking the default unconditionally used to
# OVERWRITE a caller's tag -- two runs started with different
# AMINETXDUO_RUN_TAG values would silently share build/testhd-conformance and
# clobber each other's results.
TAG="${AMINETXDUO_RUN_TAG:-conformance}"
ARGS="NOPAGE"
PROBE=0

BOARD=a2065

while getopts "m:c:t:T:a:b:N:p" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        a) ARGS="$OPTARG" ;;
        b) AMINETXDUO_BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        p) PROBE=1 ;;
        *) echo "usage: $0 [-m model] [-c cpu] [-t secs] [-T tag] [-a args]" \
                "[-b builddir] [-N board] [-p]" >&2
           exit 2 ;;
    esac
done

SUITE="$ROOT/build/bsdsocktest/bsdsocktest"
LAUNCHER="$ROOT/build/bsdsocktest/conf_launcher"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"

for f in "$SUITE" "$LAUNCHER"; do
    [ -f "$f" ] || { echo "missing $f -- run tests/conformance/build.sh" >&2; exit 2; }
done
for f in "$BSD" "$UG"; do
    [ -f "$f" ] || { echo "missing $f -- build bsdsocket_library usergroup_library" >&2; exit 2; }
done

. "$ROOT/tools/sana2-stage.sh"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ] && [ "$BOARD" = a2065 ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
    [ -n "$A2065" ] && [ -f "$A2065" ] || {
        echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
        exit 2
    }
fi

STAGE="$ROOT/build/conf-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
[ -z "$A2065" ] || cp "$A2065" "$STAGE/devs/a2065.device"
sana2_stage "$BOARD" "$STAGE/devs"
echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$UG"  "$STAGE/libs/usergroup.library"
cp "$SUITE" "$STAGE/bsdsocktest"
printf '%s\n' "$ARGS" > "$STAGE/conf-args"

# WinUAE's own bsdsocket.library emulation is off unless bsdsocket_emu is
# set, and tools/winuae-run.sh never sets it, so there is nothing to disable
# here -- the FS-UAE runner needs a Host.fs-uae for exactly that reason.

# ------------------------------------------------------------- the helper --
#
# AMINETXDUO_CONF_HELPER=<user@host> stages the vendored helper there and
# restarts it before the run.
#
# Not a convenience.  A helper left running across runs wedges, and the run
# after it fails in one of two ways that both look like the stack: `Bail out!
# Could not connect to host helper`, 0/142, or a suite that stops dead after
# test 41 -- the one where the helper dials back into the guest -- and times
# out.  Measured 2026-07-28 on both the floor and the IPv6 build, three of six
# runs each way, and gone once every run got a fresh helper.
#
# The restart goes through a script file rather than `ssh <host> <command>`.
# `pkill -f` sees the whole remote command line, so a pkill and the python it
# is about to start cannot share one: the pattern matches ssh's own shell and
# kills it before anything is launched.  In a file, only the helper matches.
if [ -n "${AMINETXDUO_CONF_HELPER:-}" ]; then
    HELPER_SRC="$ROOT/third_party/bsdsocktest/host/bsdsocktest_helper.py"
    [ -f "$HELPER_SRC" ] || { echo "missing $HELPER_SRC" >&2; exit 2; }
    HELPER_SH="$ROOT/build/conf-helper-start.sh"
    cat > "$HELPER_SH" <<'EOF'
#!/bin/sh
pkill -f bsdsocktest_helper 2>/dev/null
sleep 1
setsid nohup python3 /tmp/bsdsocktest_helper.py -v --bind 0.0.0.0 \
    > /tmp/helper.log 2>&1 < /dev/null &
sleep 2
pgrep -f bsdsocktest_helper > /dev/null
EOF
    echo "==> restarting the helper on $AMINETXDUO_CONF_HELPER"
    scp -q "$HELPER_SRC" "$AMINETXDUO_CONF_HELPER:/tmp/bsdsocktest_helper.py"
    scp -q "$HELPER_SH" "$AMINETXDUO_CONF_HELPER:/tmp/anxd-conf-helper.sh"
    ssh "$AMINETXDUO_CONF_HELPER" 'sh /tmp/anxd-conf-helper.sh' \
        || { echo "the helper did not start on $AMINETXDUO_CONF_HELPER" >&2
             exit 2; }
fi

export AMINETXDUO_RUN_TAG="$TAG"

set +e
if [ "$PROBE" = "1" ]; then
    # The probe needs no arguments and no big stack, so it is run directly
    # instead of through the launcher.
    "$ROOT/tools/winuae-run.sh" -N "$BOARD" -m "$MODEL" ${CPU:+-c "$CPU"} -t "$TIMEOUT" \
        "$ROOT/build/bsdsocktest/conf_probe" "$STAGE/devs" "$STAGE/libs"
    status=$?
    set -e
    exit "$status"
fi
"$ROOT/tools/winuae-run.sh" -N "$BOARD" -m "$MODEL" ${CPU:+-c "$CPU"} -t "$TIMEOUT" \
    "$LAUNCHER" "$STAGE/devs" "$STAGE/libs" "$STAGE/bsdsocktest" \
    "$STAGE/conf-args"
status=$?
set -e

echo "---- stack under test ----"
ident=$(grep -m1 "^# bsdsocket.library:" \
        "$ROOT/build/winuae-testhd-$TAG/bsdsocktest.log" 2>/dev/null || true)
case "$ident" in
    *AmiNetXDuo*) echo "$ident  (ours)" ;;
    "")           echo "!! no stack identification in the TAP log" ;;
    *)            echo "!! $ident -- NOT our library, results are meaningless" ;;
esac

exit "$status"
