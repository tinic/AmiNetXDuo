#!/usr/bin/env bash
#
# Fitz bulk throughput, on a link that has latency.
#
#   tests/perf/run-fitzbench.sh [-H user@host] [-A addr] [-m MODEL] [-c CPU]
#                               [-b BUILDDIR] [-k KB] [-C CHUNK] [-r REPS]
#                               [-T TAG] [-t SECONDS] [-p PORT] [-s] [-x]
#                               [-a] [-B IFACE] [-N BOARD]
#
# WHAT IT MEASURES
#
#   The guest mounts a Fitz share served by a THIRD machine on the LAN and
#   FitzBench writes a file to it and reads it back, timed with ReadEClock().
#   Then it does the same against RAM:, which is the same program and the same
#   AmigaDOS with no network under it -- the control that says how much of the
#   figure is ours.
#
# WHY A BRIDGED EMULATOR AND NOT FS-UAE
#
#   Every throughput conclusion this project drew before this script came from
#   SLIRP, whose bandwidth-delay product is nearly zero, or from loopback,
#   where it is exactly zero.  docs/RESEARCH.md 64.6 spells out what that makes
#   unanswerable: a receive window cannot be shown to matter on a link with no
#   delay term, whatever the CPU is doing.  A bridged emulator puts the A2065
#   on a real adapter, so the guest takes a real DHCP lease and the peer is a
#   real machine several hops of real hardware away.
#
#   -a is bridged AMIBERRY, run on the Linux host it is installed on -- the
#   emulator is local, so this script has to BE on that machine.  -B names the
#   host NIC (ens18) and tools/amiberry-run.sh reads the backend back out of the
#   emulator log, so a run that quietly fell back to NAT fails rather than
#   printing a number.
#
#   The default is bridged WINUAE over ssh to winbuilder, which is where this
#   script started.
#
#   -s runs it on FS-UAE/SLIRP against a server on this Mac instead.  That is a
#   smoke test for the harness, not a measurement: read nothing into the
#   numbers it prints.
#
# THE PEER MUST BE A THIRD MACHINE.  A frame the emulator's host sends to the
# guest's MAC leaves its NIC and never comes back to that NIC's own pcap
# capture, so a server on the host is unreachable from the guest while being
# reachable from everywhere else (docs/RESEARCH.md 63).  That holds for
# Amiberry's uaenet_pcap exactly as it holds for WinUAE's.
#
# THE PATCHED WINUAE IS THE DEFAULT HERE.  6.0.3 copies a captured frame into a
# 4000-byte buffer without checking its length and dies on the first coalesced
# receive, which a bulk transfer produces immediately; C:\winuae-patched is a
# build from master that does not (63.4, 63.5).
#
# REPEATABILITY.  Three reps per direction inside one boot, and the boot itself
# is cheap enough to repeat.  Quote the mean and the spread, and compare
# libraries rather than quoting an absolute rate: warp mode is on, so the
# emulated CPU has no defined speed and only ratios between runs mean anything.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# NOT playhouse2, whatever its convenience: it is an LXC container on a veth,
# so its SYN-ACK carries an uncomputed TX-offload checksum that no NIC ever
# fixes up.  Our stack rejects it -- correctly -- and the run reads as "1
# connection made, 6 bad packets, 6 checksum errors" and no transfer, which
# looks like our defect and is not.  The peer must also be a THIRD machine:
# a frame the emulator's own host sends to the guest's MAC never comes back to
# that NIC's pcap capture (63).
PEER="${AMINETXDUO_FITZ_PEER:-}"
case "$PEER" in
    *playhouse2*)
        echo "playhouse2 cannot serve this: VMs on one Proxmox host never cross" >&2
        echo "a NIC, so its TX checksums are never computed and our stack rejects" >&2
        echo "them -- it reads as 6 bad packets and no transfer.  Use another." >&2
        exit 2 ;;
esac
[ -n "$PEER" ] || {
    echo "set AMINETXDUO_FITZ_PEER=<user@host> -- a third machine on real" >&2
    echo "hardware, not this emulator's host and not an LXC container" >&2
    exit 2
}
PEER_ADDR="${AMINETXDUO_FITZ_PEER_ADDR:-192.168.1.184}"
PEER_DIR="${AMINETXDUO_FITZ_PEER_DIR:-/tmp/fitzbench-share}"
PEER_BIN="${AMINETXDUO_FITZ_PEER_BIN:-\$HOME/fitzsrc/fitz-serve}"
MODEL=A3000
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
KB=512
CHUNK=32768
REPS=3
TAG="${AMINETXDUO_RUN_TAG:-fitzbench}"
TIMEOUT=400
PORT="${AMINETXDUO_FITZ_PORT:-17712}"
SLIRP=0
ACCURATE=0
ROADSHOW=""
AMIBERRY=0
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BOARD=a2065

while getopts "H:A:m:c:b:k:C:r:T:t:p:sxR:aB:N:" opt; do
    case "$opt" in
        H) PEER="$OPTARG" ;;
        A) PEER_ADDR="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        k) KB="$OPTARG" ;;
        C) CHUNK="$OPTARG" ;;
        r) REPS="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        s) SLIRP=1 ;;
        x) ACCURATE=1 ;;
        R) ROADSHOW="${OPTARG:-/tmp/rsdemo/Roadshow-Demo-1.15/Workbench}" ;;
        a) AMIBERRY=1 ;;
        B) AMIBERRY=1; IFACE="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        *) echo "usage: $0 [-H user@host] [-A addr] [-m model] [-c cpu]" \
                "[-b build] [-k KB] [-C chunk] [-r reps] [-T tag] [-t secs]" \
                "[-p port] [-s] [-x] [-R roadshowdir] [-a] [-B iface] [-N board]" >&2
           exit 2 ;;
    esac
done

[ "$AMIBERRY" = "0" ] || [ "$SLIRP" = "0" ] || {
    echo "-a and -s are different emulators; pick one" >&2; exit 2; }

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"
BENCH="$ROOT/$BUILD/tests/perf/FitzBench"
FITZ="$ROOT/build/fitz/Fitz/fitz"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$BSD" "$BENCH"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
done
[ -f "$FITZ" ] || {
    echo "missing $FITZ -- run tests/endurance/fetch-fitz.sh" >&2; exit 2; }

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

# ------------------------------------------------------------- the server ---
#
# SLIRP reaches this Mac at 10.0.2.2; bridged reaches a real address, and the
# server has to be somewhere the guest's frames actually go.

if [ -z "$PEER" ]; then
    # The server is somebody else's problem: it is already listening at -A.
    SERVER_ADDR="$PEER_ADDR"
    cleanup() { :; }
elif [ "$SLIRP" = "1" ]; then
    SERVER_ADDR=10.0.2.2
    LOCAL_SERVE="$ROOT/build/endurance/fitz-serve"
    [ -x "$LOCAL_SERVE" ] || {
        echo "missing $LOCAL_SERVE -- run tests/endurance/build.sh" >&2; exit 2; }
    SHARE="$ROOT/build/fitzbench-share-$TAG"
    rm -rf "$SHARE"; mkdir -p "$SHARE"
    PEERLOG="$ROOT/build/fitzbench-$TAG-peer.log"
    "$LOCAL_SERVE" "$SHARE" PORT "$PORT" > "$PEERLOG" 2>&1 &
    PEER_PID=$!
    cleanup() { kill -TERM "$PEER_PID" 2>/dev/null || true; }
else
    SERVER_ADDR="$PEER_ADDR"
    PEERLOG="$ROOT/build/fitzbench-$TAG-peer.log"
    # The bracket in the pattern is not decoration: pkill -f matches the
    # remote shell's own command line, which contains the pattern, so an
    # unbracketed one kills the connection that issued it.
    ssh "$PEER" "pkill -f '[f]itz-serve' || true" >/dev/null 2>&1 || true
    ssh "$PEER" "rm -rf $PEER_DIR; mkdir -p $PEER_DIR;
                 nohup $PEER_BIN $PEER_DIR PORT $PORT > /tmp/fitzbench-peer.log 2>&1 &
                 sleep 1; ps -o args= -C fitz-serve" > "$PEERLOG" 2>&1
    cat "$PEERLOG"
    cleanup() { ssh "$PEER" "pkill -f '[f]itz-serve' || true" >/dev/null 2>&1 || true; }
fi
trap cleanup EXIT INT TERM HUP

echo "==> fitz-serve on $SERVER_ADDR:$PORT"

# ---------------------------------------------------------------- staging ---

STAGE="$ROOT/build/fitzbench-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
# -R swaps the whole stack, library and starter both.  It is the discriminator
# for "is this rig or is this us": a figure Roadshow also cannot beat on the
# same emulator, the same bridge and the same peer is not ours to fix.
if [ -n "$ROADSHOW" ]; then
    [ -f "$ROADSHOW/Libs/bsdsocket.library" ] || {
        echo "no Roadshow at $ROADSHOW" >&2; exit 2; }
    cp "$ROADSHOW/Libs/bsdsocket.library" "$STAGE/libs/bsdsocket.library"
    cp "$ROADSHOW/Libs/usergroup.library" "$STAGE/libs/usergroup.library"
    cp "$ROADSHOW/C/AddNetInterface"      "$STAGE/AddNetInterface"
    cp "$ROADSHOW/C/GetNetStatus"         "$STAGE/NetStat"
else
    cp "$BSD"   "$STAGE/libs/bsdsocket.library"
    [ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"
    cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
fi
[ -n "$ROADSHOW" ] || cp "$TOOLS/netstat" "$STAGE/NetStat"
cp "$FITZ"  "$STAGE/fitz"
cp "$BENCH" "$STAGE/FitzBench"

# `&` is SYS_Asynch: a Fitz mount stays resident as a DOS handler and never
# returns, so the benchmark line after it would never run otherwise.  The RAM:
# arm is last and deliberately in the same boot -- it prices this program,
# AmigaDOS and the emulator's current mood with no network under any of it, and
# a network figure is only worth reading beside it.
STATARGS="-s"
[ -z "$ROADSHOW" ] || STATARGS=""

# NetStat runs on BOTH sides of the network arm.  Its counters are cumulative,
# so bytes/sec can be had from FitzBench alone but the PACKET rate cannot: only
# the difference across the timed window divided by that window is a rate, and
# the packet rate is the number the emulator's delivery pacing shows up in.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
wait 6
&SYS:fitz mount $SERVER_ADDR:$PORT FITZ:
wait 10
SYS:NetStat $STATARGS
SYS:FitzBench FITZ: KB=$KB CHUNK=$CHUNK REPS=$REPS
SYS:NetStat $STATARGS
SYS:FitzBench RAM: KB=$KB CHUNK=$CHUNK REPS=$REPS
EOF

# -------------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="$TAG"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")
# -x drops warp and asks for cycle accounting.  Warp makes the guest's speed a
# function of host load, which shows up directly as run-to-run spread; real
# speed costs wall clock and buys repeatability.
[ "$ACCURATE" = "0" ] || CPUARG+=(-x)

set +e
if [ "$SLIRP" = "1" ]; then
    HD="$ROOT/build/testhd-$TAG"
    "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/NetStat" "$STAGE/fitz" "$STAGE/FitzBench"
elif [ "$AMIBERRY" = "1" ]; then
    # Amiberry is local, so this branch only works ON the machine it is
    # installed on -- there is no ssh half the way winuae-run.sh has one.
    # amiberry-run.sh has no warp to drop, so -x has nothing to do here.
    HD="$ROOT/build/amiberry-testhd-$TAG"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" ${CPU:+-c "$CPU"} \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/NetStat" "$STAGE/fitz" "$STAGE/FitzBench"
else
    HD="$ROOT/build/winuae-testhd-$TAG"
    export AMINETXDUO_WINUAE_EXE="${AMINETXDUO_WINUAE_EXE:-C:\\winuae-patched\\winuae64.exe}"
    export AMINETXDUO_WINUAE_A2065="${AMINETXDUO_WINUAE_A2065:-\\Device\\NPF_{B0F2CE29-E3DB-4AB0-B55A-0BEDA6D1A48C}}"
    "$ROOT/tools/winuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/NetStat" "$STAGE/fitz" "$STAGE/FitzBench"
fi
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

if ! grep -q "fitzbench: RESULT" "$REPORT"; then
    echo "FAIL: no RESULT line -- the benchmark did not finish" >&2
    exit 1
fi

echo "==> results ($MODEL${CPU:+/$CPU}, $KB KB, chunk $CHUNK, $REPS reps)"
grep "fitzbench: RESULT\|fitzbench: file=" "$REPORT" | sed 's/^/    /'

# ------------------------------------------------------------ packet rate ---
#
# Bytes/sec is FitzBench's own figure.  Packets/sec is not derivable from it,
# and it is the one the emulator's delivery pacing shows up in: Amiberry hands
# the guest received frames from the hsync handler and looks at transmit every
# 16th hsync (src/a2065.cpp), so there is a ceiling on frames/sec that has
# nothing to do with how fast the guest's TCP is.
#
# The bracket holds both directions, the warm-up pair and the verify read, so
# what comes out of it is a count and a mean size over the whole network arm,
# not a per-direction rate.  A per-direction rate wants a capture: the mean
# frame size here mixes a write's full segments with the read's bare ACKs.
#
# ONLY THE ip: BLOCK IS READ.  NetStat prints "packets sent" three times per
# run -- under ip:, under tcp: and again in the SANA-II interface statistics --
# and taking them positionally reads the driver's counter as the second run's
# IP counter, which comes out negative.
echo
awk -v kb="$KB" -v reps="$REPS" '
    /^===== / { cmd = $0; infitz = (cmd ~ /FitzBench FITZ:/); inip = 0; inif = 0 }

    cmd ~ /NetStat/ && /^ip:/    { inip = 1; next }
    cmd ~ /NetStat/ && /^eth[0-9]/ { inif = 1; next }
    /^[^\t ]/                    { inip = 0; inif = 0 }

    inip && /packets sent/     { b = $4; gsub(/[()]/, "", b)
                                 ns[n_s++] = $1 + 0; nb[n_s - 1] = b + 0 }
    inip && /packets received/ { b = $4; gsub(/[()]/, "", b)
                                 nr[n_r++] = $1 + 0; nrb[n_r - 1] = b + 0 }
    inif && /packets received/ { fr[n_f] = $3 + 0; fs[n_f++] = $6 + 0 }

    infitz && /RESULT write kbs_mean=/ { sub(/.*kbs_mean=/, ""); wkbs = $1 + 0 }
    infitz && /RESULT read kbs_mean=/  { sub(/.*kbs_mean=/, ""); rkbs = $1 + 0 }

    END {
        if (n_s < 2 || n_r < 2 || wkbs == 0) { exit 0 }
        dps = ns[1] - ns[0];  dbs = nb[1] - nb[0]
        dpr = nr[1] - nr[0];  dbr = nrb[1] - nrb[0]
        printf "==> what crossed the wire (NetStat pair around the network arm)\n"
        printf "    ip tx %d packets / %d bytes", dps, dbs
        if (dps > 0) printf ", mean %d bytes", dbs / dps
        printf "\n"
        printf "    ip rx %d packets / %d bytes", dpr, dbr
        if (dpr > 0) printf ", mean %d bytes", dbr / dpr
        printf "\n"
        if (n_f >= 2)
            printf "    a2065 frames: %d sent, %d received\n",
                   fs[1] - fs[0], fr[1] - fr[0]
        # FitzBench moves KB each way once to warm up and then once per rep, so
        # the bytes in the bracket are known and the measured rates turn them
        # into the seconds they took.  Both directions ran back to back, so
        # this is the frame rate the card sustained across the arm rather than
        # either direction on its own.
        secs = (reps + 1) * (kb / wkbs + kb / rkbs) + 64 / rkbs
        if (secs > 0 && n_f >= 2)
            printf "    ~%d frames/s sustained over ~%.1f s of transfer\n",
                   ((fs[1] - fs[0]) + (fr[1] - fr[0])) / secs, secs
    }
' "$REPORT"

# The scheduler block is only there against a library that has NETSTATUS_HEALTH.
if grep -q "^scheduler:" "$REPORT"; then
    echo
    echo "==> was the machine ever held (last NetStat)"
    awk '/^scheduler:/ { s = ""; g = 1; next }
         g && /^\t/    { s = s $0 "\n"; next }
         g             { g = 0; last = s }
         END           { printf "%s", (g ? s : last) }' "$REPORT" | sed 's/^/  /'
fi

exit 0
