#!/usr/bin/env bash
#
# DOES THIS HOST ANNOUNCE THE GROUPS IT LISTENS TO?
#
#   tests/ipv6/run-mld.sh [-B BACKEND] [-b BUILDDIR] [-m MODEL] [-t SECONDS]
#                         [-P user@peer] [-M MAC]
#
# WHAT IT PROVES, AND WHY IT NEEDS A REAL SEGMENT
#
# A node that joins a multicast group and reports nothing is invisible to a
# switch that snoops MLD.  The switch then has no reason to forward the group
# to that port, and the group that costs is a solicited-node address: it is
# where every neighbour solicitation addressed to this machine arrives.
#
# The failure is observable and nothing else about the machine is: a capture
# on the segment either has our Multicast Listener Report in it or it does
# not.  So this is a bridged test with a capture, and SLIRP would answer a
# different question -- there is no segment there and no second listener on
# it.
#
#   join      a report goes out when a group is joined, before any query has
#             been asked: for the solicited-node group at bring-up, and for
#             ff02::c when McastProbe asks for it
#   leave     a Done when McastProbe gives ff02::c back
#   query     a report after a query, in the version the querier used
#   suppress  a second host answering first, and our report not going out
#   exempt    ff02::1 never reported at all (RFC 9777 section 6)
#
# THE TWO OTHER MACHINES
#
#   THIS host runs the emulator and the capture.  tcpdump needs CAP_NET_RAW
#   here, which is the same grant amiberry already needs to bridge.
#
#   THE PEER (-P) is the querier and the second listener.  It has to be on
#   the same segment, and it needs a capable copy of python3:
#
#     cp $(command -v python3) ~/python3-cap
#     sudo /usr/sbin/setcap cap_net_raw+eip ~/python3-cap
#
#   Without a peer the run still checks join, leave and the exemption, and
#   says which assertions it could not reach.  It does not pass quietly.
#
# THE MAC IS FIXED AND MUST BE DISTINCT
#
# The solicited-node group under test is derived from it, and the lab has
# other Amigas on the same segment -- a shared MAC would put two machines'
# reports in one capture and the suppression assertion would read one as the
# other.  -M sets it.  The A2065 keeps only the last three octets and writes
# Commodore's 00:80:10 over the first three, so the address on the wire is
# not the one passed here; this script derives the group from what the card
# will actually use.
#
# OUTPUT IS key=value.  Exit 0 everything asserted held, 1 something did not,
# 2 a broken invocation or a missing prerequisite, 3 the run produced no
# capture to read.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BUILD="${AMINETXDUO_BUILD:-build/v6log}"
MODEL=A1200
TIMEOUT=260
PEER="${AMINETXDUO_MLD_PEER:-}"
MAC="02:41:4d:49:6d:1d"
TAG="mld"

while getopts "B:b:m:t:P:M:T:" opt; do
    case "$opt" in
        B) BACKEND="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        P) PEER="$OPTARG" ;;
        M) MAC="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        *) sed -n '3,6p' "$0" >&2; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

fail_setup() { echo "reason=$1"; echo "RESULT=refused"; exit 2; }

BSD="$BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$BUILD/src/tools/AddNetInterface"
SHOW="$BUILD/src/tools/ShowNetStatus"
SMOKE="$BUILD/src/tools/ToolsSmoke"
for f in "$BSD" "$ADDIF" "$SHOW" "$SMOKE"; do
    [ -f "$f" ] || fail_setup "build_missing:$f"
done

# tests/tools/mcastprobe.c is the application join and leave, and it has no
# CMake entry on purpose (it is a probe, not a command), so it is compiled
# here.  It is what an SSDP receiver does: join ff02::c, read, leave.
. "$ROOT/tools/amiga-toolchain.sh" > /dev/null 2>&1 || true
[ -n "${AMIGA_GCC:-}" ] || fail_setup "no_amiga_gcc"
PROBE="$ROOT/build/McastProbe"
"$AMIGA_GCC" -O2 -m68020 ${AMIGA_NDK:+-I"$AMIGA_NDK"} -o "$PROBE" \
    "$ROOT/tests/tools/mcastprobe.c" || fail_setup "mcastprobe_build_failed"

[ -n "${AMINETXDUO_KICKSTART:-}" ] || fail_setup "no_kickstart"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -f "${A2065:-/nonexistent}" ] || fail_setup "no_a2065_device"

command -v tcpdump > /dev/null || fail_setup "no_tcpdump"

# ------------------------------------------------- the addresses under test --
#
# a2065.cpp keeps the last three octets and writes 00:80:10 over the rest, so
# the card's address is Commodore's prefix and our tail.  The link-local is
# then RFC 4291 EUI-64 of that, and the solicited-node group is ff02::1:ff
# followed by its low 24 bits -- which are the same three octets, so the group
# is derivable without the guest having booted.
TAIL=$(printf '%s' "$MAC" | tr 'A-Z' 'a-z' | cut -d: -f4-6)
GUEST_MAC="00:80:10:$TAIL"
O4=$(printf '%s' "$TAIL" | cut -d: -f1)
O5=$(printf '%s' "$TAIL" | cut -d: -f2)
O6=$(printf '%s' "$TAIL" | cut -d: -f3)
SOLICITED="ff02::1:ff$O4:$O5$O6"
PROBE_GROUP="ff02::c"

echo "guest_mac=$GUEST_MAC solicited=$SOLICITED backend=$BACKEND peer=${PEER:-none}"

# ------------------------------------------------------------------- peer ---

peer_ok=0
if [ -n "$PEER" ]; then
    if ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" \
           'test -x ~/python3-cap && test -f ~/mldpeer.py' 2>/dev/null; then
        peer_ok=1
    else
        # Stage it and try once.  A peer that has the capability but not the
        # script is the ordinary case on a fresh lab machine.
        scp -q -o BatchMode=yes "$ROOT/tests/ipv6/mldpeer.py" "$PEER:~/mldpeer.py" 2>/dev/null
        ssh -o BatchMode=yes "$PEER" 'test -x ~/python3-cap' 2>/dev/null && peer_ok=1
    fi
fi
[ "$peer_ok" = 1 ] || echo "peer=unusable (no ~/python3-cap with CAP_NET_RAW)"

peer_send() {
    [ "$peer_ok" = 1 ] || return 0
    ssh -o BatchMode=yes "$PEER" "~/python3-cap ~/mldpeer.py --iface ${AMINETXDUO_MLD_PEER_IFACE:-ens18} $*" \
        >> "$ROOT/build/$TAG-peer.log" 2>&1
}

# ---------------------------------------------------------------- staging ---

STAGE="$ROOT/build/$TAG-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$ADDIF" "$STAGE/AddNetInterface"
cp "$SHOW"  "$STAGE/ShowNetStatus"
cp "$PROBE" "$STAGE/McastProbe"

cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=AUTO
EOF

# McastProbe is the application half of the table the solicited-node groups
# are in: IPV6_JOIN_GROUP and IPV6_LEAVE_GROUP through
# nxd_ipv6_multicast_interface_join()/_leave().  It runs late on purpose --
# the peer's queries have to have arrived first, so that the guest is in
# MLDv1 compatibility mode and the leave is a Done rather than a version 2
# state change.
#
# The mDNS responder would have been the obvious driver and is not usable
# here: NX_MDNS_ENABLE_IPV6 is off in this tree (CMakeLists.txt), so the
# responder joins no IPv6 group at all.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
wait 55
SYS:McastProbe
wait 25
SYS:ShowNetStatus
EOF

# ---------------------------------------------------------------- capture ---

PCAP="$ROOT/build/$TAG.pcap"
CAPPID="$ROOT/build/$TAG-tcpdump.pid"
rm -f "$PCAP" "$ROOT/build/$TAG-peer.log"

# `ip6` and not `icmp6`: an MLD message carries a Hop-by-Hop header, so its
# Next Header is 0 and tcpdump's icmp6 primitive -- which compares the fixed
# header's field and does not walk the chain -- matches none of them.  Losing
# an hour to that is what this comment is for.
#
# -U so the file is readable while the run is still going: the peer schedule
# below waits for the guest's first report rather than guessing when it boots.
setsid nohup tcpdump -i "$BACKEND" -s 256 -U -w "$PCAP" ip6 \
    > "$ROOT/build/$TAG-tcpdump.log" 2>&1 < /dev/null &
echo $! > "$CAPPID"
sleep 2

# Kill by the pid we started and never by pattern: this lab runs more than one
# harness at a time and a bare pkill takes somebody else's capture with it.
stop_capture() {
    [ -f "$CAPPID" ] || return 0
    kill "$(cat "$CAPPID")" 2>/dev/null
    rm -f "$CAPPID"
    sleep 1
}
trap 'stop_capture' EXIT

if ! kill -0 "$(cat "$CAPPID")" 2>/dev/null; then
    cat "$ROOT/build/$TAG-tcpdump.log" >&2
    fail_setup "tcpdump_did_not_start"
fi

# ------------------------------------------------------------------- run ---

SERIAL="$ROOT/build/amiberry-serial-$TAG.log"
rm -f "$SERIAL"

AMINETXDUO_RUN_TAG="$TAG" AMINETXDUO_AMIBERRY_MAC="$MAC" \
    "$ROOT/tools/amiberry-run.sh" \
    -N a2065 -B "$BACKEND" -m "$MODEL" -t "$TIMEOUT" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
    "$STAGE/ConfigureNetInterface" "$STAGE/ShowNetStatus" "$STAGE/commands.txt" \
    > "$ROOT/build/$TAG.out" 2>&1 &
RUNPID=$!

# ------------------------------------------------------- the peer schedule --
#
# Anchored on the guest's FIRST report and not on the clock: a boot is thirty
# to fifty seconds depending on what else this host is doing, and a query sent
# before the interface exists proves nothing and reads as a failure.
#
# That anchor is also the first assertion: the report is sent on join, while
# the address it belongs to is still tentative, which is the case the whole
# feature exists for.
guest_seen() {
    tcpdump -r "$PCAP" -nne "ether src $GUEST_MAC and ip6" 2>/dev/null | head -1 | grep -q .
}

anchor=0
for _ in $(seq 1 120); do
    if guest_seen; then anchor=1; break; fi
    sleep 1
done

if [ "$anchor" = 0 ]; then
    echo "guest_first_frame=none"
else
    echo "guest_first_frame=seen"
fi

if [ "$peer_ok" = 1 ] && [ "$anchor" = 1 ]; then

    # Version 2 first, because that is the version a host starts in.
    sleep 10
    peer_send query-v2 --mrd 4000
    echo "peer=query-v2"

    # Version 1, which puts the guest into compatibility mode: the Done on
    # leave and the suppression below only exist there.  This one is also the
    # control half of the suppression pair -- a query nobody else answers,
    # which the guest must answer.
    sleep 14
    peer_send query-v1 --mrd 4000
    echo "peer=query-v1-control"

    # The same query again, with this host answering it first.  The two
    # windows differ in one thing, so the second one being silent is
    # suppression and not a guest that has stopped talking.
    sleep 16
    peer_send query-v1 --mrd 8000
    sleep 1
    peer_send report-v1 --group "$SOLICITED"
    echo "peer=query-v1-suppressed"

    # Nothing after this.  McastProbe joins and leaves ff02::c at about
    # guest+55, and another host answering a query for that group inside
    # those few seconds would clear this host's last-reporter flag and there
    # would legitimately be no Done to find.  So no query is asked while it
    # runs.
fi

wait "$RUNPID"
run_rc=$?
sleep 3
stop_capture

echo "run_rc=$run_rc"

[ -s "$PCAP" ] || { echo "RESULT=nocapture"; exit 3; }

# ------------------------------------------------------------------ verdict --

python3 "$ROOT/tests/ipv6/mldcheck.py" --pcap "$PCAP" \
    --guest-mac "$GUEST_MAC" --solicited "$SOLICITED" --group "$PROBE_GROUP"
rc=$?

if [ "$peer_ok" != 1 ]; then
    echo "not_exercised=query_answer_v1,query_answer_v2,suppression (no peer)"
fi

exit "$rc"
