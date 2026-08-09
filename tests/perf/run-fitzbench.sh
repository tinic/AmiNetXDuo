#!/usr/bin/env bash
#
# Fitz bulk throughput, on a link that has latency.
#
#   tests/perf/run-fitzbench.sh [-H user@host] [-A addr] [-m MODEL] [-c CPU]
#                               [-b BUILDDIR] [-k KB] [-C CHUNK] [-r REPS]
#                               [-T TAG] [-t SECONDS] [-p PORT] [-s] [-x]
#                               [-a] [-B IFACE] [-N BOARD]
#                               [-w] [-l PCT] [-L PCT]
#
# WHAT IT MEASURES
#
#   The guest mounts a Fitz share served by a THIRD machine on the LAN and
#   FitzBench writes a file to it and reads it back, timed with ReadEClock().
#   Then it does the same against RAM:, which is the same program and the same
#   AmigaDOS with no network under it, the control that says how much of the
#   figure is ours.
#
#   READ IS THE FIGURE TO COMPARE ON.  tools/profiler/Profile over a real mount,
#   2026-08-02: the read arm leaves the guest CPU 60% idle, the write arm 22%,
#   which is what the write figure pricing buffer acceptance rather than
#   throughput looks like from the CPU's side.  Of the busy CPU on a read,
#   bsdsocket.library is 79%, Exec 9%, the driver 6% and Fitz itself 4%.
#   docs/BACKLOG.md has the split and the read-ahead sweep that came out of it.
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
#   -a is bridged AMIBERRY, run on the Linux host it is installed on, the
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
# -w ALSO MEASURES THE INBOUND LOSS RATE, which is what to gate on rather than
# the rate.  The peer captures its own egress and tests/perf/lossrate.py turns
# it into retransmissions over data segments sent, over the read phases: one
# number, taken where the counters are exact, and upstream of the throughput
# rather than downstream of everything.  -l fails the run above a raw loss
# percentage and -L above the percentage with spurious retransmissions
# removed; both imply -w.  What to set them to is a property of the rig and
# has to be measured on it, across 29 runs of 13 libraries on one rig the
# raw rate's within-library spread was 13% of its own mean and the worst pair
# differed by 58%, so a single run cannot carry a tight threshold and the
# useful gate is the median of three.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"
. "$ROOT/tests/perf/peercap.sh"

# NOT playhouse2, whatever its convenience: it is an LXC container on a veth,
# so its SYN-ACK carries an uncomputed TX-offload checksum that no NIC ever
# fixes up.  Our stack rejects it, correctly, and the run reads as "1
# connection made, 6 bad packets, 6 checksum errors" and no transfer, which
# looks like our defect and is not.  The peer must also be a THIRD machine:
# a frame the emulator's own host sends to the guest's MAC never comes back to
# that NIC's pcap capture (63).
# Checked after getopts, not here: both tests used to run before the option
# parse below, so -H was rejected before it was read and -H turo@playhouse2 went
# straight past the guard the env var could not.
PEER="${AMINETXDUO_FITZ_PEER:-}"
# ssh dials a name, the guest dials a number, and they are two spellings of one
# machine.  Derived from $PEER rather than defaulted, because the two used to be
# independent inputs with a validated name and an unvalidated address: the
# default was 192.168.1.184, which is the playhouse2 the case above refuses.  A
# run that staged the server on one machine and pointed the guest at another
# still exited 0, printing RESULT read FAILED beside a healthy RAM: control.
PEER_ADDR="${AMINETXDUO_FITZ_PEER_ADDR:-}"
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
NGDIR="${AMINETXDUO_CMP_AMITCPNG:-}"
EXTRALIBS="${AMINETXDUO_FITZ_EXTRALIBS:-}"
# -a IS USE_AMIBERRY, NOT AMIBERRY.  tools/amiberry-run.sh reads $AMIBERRY as
# the PATH to the emulator, and amiga-assets/env.sh exports it.  A plain
# `AMIBERRY=1` here keeps that export attribute, so the flag arrives in the
# child as a path of "1" and the run dies on "amiberry not found; set
# AMIBERRY=<path>" with the emulator installed and working.
USE_AMIBERRY=0
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BOARD=a2065
LOSSCAP=0
MAXLOSS=""
MAXEFF=""

while getopts "H:A:m:c:b:k:C:r:T:t:p:sxR:aB:N:wl:L:G:E:" opt; do
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
        G) NGDIR="$OPTARG" ;;
        E) EXTRALIBS="$OPTARG" ;;
        a) USE_AMIBERRY=1 ;;
        B) USE_AMIBERRY=1; IFACE="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        w) LOSSCAP=1 ;;
        l) LOSSCAP=1; MAXLOSS="$OPTARG" ;;
        L) LOSSCAP=1; MAXEFF="$OPTARG" ;;
        *) echo "usage: $0 [-H user@host] [-A addr] [-m model] [-c cpu]" \
                "[-b build] [-k KB] [-C chunk] [-r reps] [-T tag] [-t secs]" \
                "[-p port] [-s] [-x] [-R roadshowdir] [-a] [-B iface]" \
                "[-N board] [-w] [-l pct] [-L pct]" >&2
           exit 2 ;;
    esac
done

# NOT playhouse2, whatever its convenience: it is an LXC container on a veth, so
# its SYN-ACK carries an uncomputed TX-offload checksum that no NIC ever fixes
# up.  Our stack rejects it, correctly, and the run reads as "1 connection made,
# 6 bad packets, 6 checksum errors" and no transfer, which looks like our defect
# and is not.  The peer must also be a THIRD machine: a frame the emulator's own
# host sends to the guest's MAC never comes back to that NIC's pcap capture (63).
case "$PEER" in
    *playhouse2*)
        echo "playhouse2 cannot serve this: VMs on one Proxmox host never cross" >&2
        echo "a NIC, so its TX checksums are never computed and our stack rejects" >&2
        echo "them, it reads as 6 bad packets and no transfer.  Use another." >&2
        exit 2 ;;
esac
[ -n "$PEER" ] || [ -n "$PEER_ADDR" ] || {
    echo "set AMINETXDUO_FITZ_PEER=<user@host> or pass -H, a third machine on" >&2
    echo "real hardware, not this emulator's host and not an LXC container." >&2
    echo "-A <addr> alone is enough if a fitz-serve is already listening." >&2
    exit 2
}

[ "$USE_AMIBERRY" = "0" ] || [ "$SLIRP" = "0" ] || {
    echo "-a and -s are different emulators; pick one" >&2; exit 2; }

# Resolve the emulator NOW, not after staging a drive and starting a server on
# the peer: a bad environment must cost a second, not a boot and a timeout.
if [ "$USE_AMIBERRY" = "1" ]; then
    . "$ROOT/tools/amiberry-resolve.sh"
    amiberry_resolve || exit 2
fi

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"
BENCH="$ROOT/$BUILD/tests/perf/FitzBench"
FITZ="$ROOT/build/fitz/Fitz/fitz"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$BSD" "$BENCH"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done
[ -f "$FITZ" ] || {
    echo "missing $FITZ, run tests/endurance/fetch-fitz.sh" >&2; exit 2; }

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
    [ -n "$PEER_ADDR" ] || {
        echo "no peer: give -A <addr> for a server that is already listening," >&2
        echo "or -H <user@host> to have this script start one" >&2; exit 2; }
    SERVER_ADDR="$PEER_ADDR"
    cleanup() { :; }
elif [ "$SLIRP" = "1" ]; then
    SERVER_ADDR=10.0.2.2
    LOCAL_SERVE="$ROOT/build/endurance/fitz-serve"
    [ -x "$LOCAL_SERVE" ] || {
        echo "missing $LOCAL_SERVE, run tests/endurance/build.sh" >&2; exit 2; }
    SHARE="$ROOT/build/fitzbench-share-$TAG"
    rm -rf "$SHARE"; mkdir -p "$SHARE"
    PEERLOG="$ROOT/build/fitzbench-$TAG-peer.log"
    "$LOCAL_SERVE" "$SHARE" PORT "$PORT" > "$PEERLOG" 2>&1 &
    PEER_PID=$!
    cleanup() { kill -TERM "$PEER_PID" 2>/dev/null || true; }
else
    # Ask the peer for its own address rather than resolving the name here.
    # Local resolution answers for whatever this host's resolver believes, which
    # on a Mac is not the lab's DNS and on any host can be stale; the machine
    # ssh actually landed on cannot be wrong about its own interfaces.
    PEER_ADDRS=$(ssh "$PEER" \
        "ip -4 -o addr show scope global 2>/dev/null | awk '{print \$4}' | cut -d/ -f1" \
        2>/dev/null)
    [ -n "$PEER_ADDRS" ] || {
        echo "$PEER did not report an address of its own; pass -A <addr>" >&2
        exit 2; }
    if [ -z "$PEER_ADDR" ]; then
        PEER_ADDR=$(printf '%s\n' "$PEER_ADDRS" | head -1)
        [ "$(printf '%s\n' "$PEER_ADDRS" | wc -l)" -eq 1 ] || {
            echo "note: $PEER holds more than one address, using $PEER_ADDR" >&2
            echo "      pass -A to choose the one the guest's bridge reaches" >&2; }
    else
        # The guest cannot report reaching the wrong machine: it times out, and
        # the RAM: control arm passes beside it, so the run reads as a partial
        # success and exits 0.  Catch the mismatch here instead.
        printf '%s\n' "$PEER_ADDRS" | grep -qx "$PEER_ADDR" || {
            echo "$PEER does not hold $PEER_ADDR -- the server would be staged" >&2
            echo "on one machine and the guest pointed at another.  It holds:" >&2
            printf '%s\n' "$PEER_ADDRS" | sed 's/^/  /' >&2
            exit 2; }
    fi

    SERVER_ADDR="$PEER_ADDR"
    PEERLOG="$ROOT/build/fitzbench-$TAG-peer.log"
    # Matched on our own port, not on the binary: a bare fitz-serve pattern
    # takes out every other run on the peer, and this is a shared machine.
    # The bracket is not decoration either -- pkill -f matches the remote
    # shell's own command line, so an unbracketed pattern kills the connection
    # that issued it.
    PEER_KILL="pkill -f '[f]itz-serve .* PORT $PORT' || true"
    ssh "$PEER" "$PEER_KILL" >/dev/null 2>&1 || true
    ssh "$PEER" "rm -rf $PEER_DIR; mkdir -p $PEER_DIR;
                 nohup $PEER_BIN $PEER_DIR PORT $PORT > /tmp/fitzbench-peer.log 2>&1 &
                 sleep 1; ps -o args= -C fitz-serve" > "$PEERLOG" 2>&1
    cat "$PEERLOG"
    cleanup() { ssh "$PEER" "$PEER_KILL" >/dev/null 2>&1 || true; }
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
if [ -n "$NGDIR" ]; then
    # AmiTCP_NG, the same swap the -R arm makes.  Two differences from
    # Roadshow: it ships no usergroup.library, so ours goes in beside it, and
    # its commands want mathieeedoubbas and rexxsyslib in LIBS:, which a bare
    # boot shell does not have -- pass them with -L.
    [ -f "$NGDIR/Libs/bsdsocket.library" ] || {
        echo "no AmiTCP_NG at $NGDIR" >&2; exit 2; }
    cp "$NGDIR/Libs/bsdsocket.library" "$STAGE/libs/bsdsocket.library"
    [ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"
    cp "$NGDIR/C/AddNetInterface" "$STAGE/AddNetInterface"
    cp "$NGDIR/C/GetNetStatus"    "$STAGE/NetStat"
elif [ -n "$ROADSHOW" ]; then
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
[ -n "$ROADSHOW$NGDIR" ] || cp "$TOOLS/netstat" "$STAGE/NetStat"
# Libraries the stack under test needs and the boot shell has no LIBS: for.
if [ -n "$EXTRALIBS" ]; then
    for _l in "$EXTRALIBS"/*.library; do
        [ -f "$_l" ] && cp "$_l" "$STAGE/libs/"
    done
fi
cp "$FITZ"  "$STAGE/fitz"
cp "$BENCH" "$STAGE/FitzBench"

# `&` is SYS_Asynch: a Fitz mount stays resident as a DOS handler and never
# returns, so the benchmark line after it would never run otherwise.  The RAM:
# arm is last and deliberately in the same boot, it prices this program,
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

CAPDIR="$ROOT/build/losscap-$TAG"
CAPTURING=0
if [ "$LOSSCAP" = "1" ]; then
    if [ -z "$PEER" ]; then
        echo "-w needs an ssh-able peer (-H): the capture is taken there" >&2
        exit 2
    fi
    peercap_start "$PEER" "$PORT" "$CAPDIR" "$TAG" && CAPTURING=1
fi

set +e
if [ "$SLIRP" = "1" ]; then
    HD="$ROOT/build/testhd-$TAG"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/NetStat" "$STAGE/fitz" "$STAGE/FitzBench"
elif [ "$USE_AMIBERRY" = "1" ]; then
    # Amiberry is local, so this branch only works ON the machine it is
    # installed on, there is no ssh half the way winuae-run.sh has one.
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

[ "$CAPTURING" = "1" ] && { peercap_stop "$PEER" "$CAPDIR" "$TAG" || CAPTURING=0; }

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

# The emulator's exit status is the backend assertion's only way out, and this
# read it into a variable it used in one error message.  A `-B ens99` run is
# rejected by tools/amiberry-run.sh, comes up on SLIRP, reaches the peer
# through NAT anyway -- SLIRP forwards to real addresses -- and produced
# `read 230 write 348` with `!! ASKED FOR 'ens99' AND DID NOT GET IT` in the
# same log and rc=0 out of this script.  A zero bandwidth-delay path has no
# window, no RTT and no loss, so those are not throughput figures; stop before
# printing any.
if [ "$RUN_RC" != "0" ]; then
    echo "FAIL: the emulator run failed (rc=$RUN_RC); no figure from it is" >&2
    echo "usable.  Its own output above says which assertion refused it." >&2
    exit "$RUN_RC"
fi

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

if ! grep -q "fitzbench: RESULT" "$REPORT"; then
    echo "FAIL: no RESULT line, the benchmark did not finish" >&2
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
# run, under ip:, under tcp: and again in the SANA-II interface statistics --
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

LOSS_RC=0
if [ "$CAPTURING" = "1" ]; then
    LOSSARGS=(--per-phase)
    [ -z "$MAXLOSS" ] || LOSSARGS+=(--max-loss "$MAXLOSS")
    [ -z "$MAXEFF" ]  || LOSSARGS+=(--max-effective-loss "$MAXEFF")
    set +e
    peercap_report "$CAPDIR" "$TAG" "${LOSSARGS[@]}"
    LOSS_RC=$?
    set -e
fi

exit "$LOSS_RC"
