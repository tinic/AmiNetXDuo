#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR OUTBOUND SOURCE SELECTION.
#
#   tests/tools/run-srcsel.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                             [-N board] [-B backend]
#
# WHAT IT IS PROVING, HALF ONE: THE SOURCE THE CALLER NAMED
#
#   bind() names a local address and the send direction has to honour it.
#   NetX Duo binds a socket to a port only, so the library maps the bound
#   address to the index nxd_udp_socket_source_send() takes for a datagram and
#   nxd_tcp_client_socket_source_connect() takes for a connect.
#
#   The assertion is on the source address the RECEIVER saw, not on the send
#   returning len, the send returned len before any of this existed, with
#   whatever source NetX picked.  The TCP arm asserts the address the
#   ACCEPTING end reports, for the same reason.
#
#   It also holds the refusal.  A destination the bound address cannot reach
#   is ENETUNREACH, for a datagram, which used to be dropped inside
#   _nx_ip_packet_send() with the send already reported successful, and for a
#   connect(), before the SYN.
#
# WHAT IT IS PROVING, HALF TWO: THE SOURCE NOTHING NAMED
#
#   RFC 6724 source address selection, src/ipv6/ipv6_srcsel.c, which decides
#   what an unbound AF_INET6 socket sends from.  tests/ipv6/host drives its
#   rules one at a time over a hand-built NX_IP, which is the only way to reach
#   most of them; this is the check that the same code, in the shipped library,
#   on a booted machine with a real interface, answers the same.
#
#   Three destinations and three different answers: a global on this node's own
#   prefix takes the global source, a link-local destination takes the
#   link-local, and ::1 takes ::1.  Getting the same address for all three is
#   what a first-match walk over the address list does, and is the defect.
#
#   The fourth is the refusal: a global on a prefix that is not on-link, with
#   no default router, has no outgoing interface and therefore no source.
#
# WHAT IT DOES NOT PROVE.  The case the source connect exists for is two
# interfaces, source on one, route out of the other, and the guest here
# has one.  What is measured is the single-interface half.
#
# BRIDGED, NEVER SLIRP.  -B names the host interface to bridge onto and the
# script refuses the string `slirp` outright.  Every assertion here is on what
# the GUEST prints and nothing on the LAN has to answer, so the bridge is not
# carrying test traffic -- it is there because slirp is not a network and a
# result taken over it says nothing about the stack.
#
# ADDRESSES.  A /24 nothing on the host's LAN uses for IPv4, and the RFC 3849
# documentation prefix for IPv6, so no address here is claimed or answered for
# anywhere.  The IPv6 four are constants shared with tests/tools/srcprobe.c:
#
#   2001:db8:6724:1::10/64   ADDRESS6, the guest's own          SELF6
#   2001:db8:6724:1::1       same /64, on-link                  ONLINK6
#   2001:db8:6724:9::1       a different /64, no route to it    OFF6
#   fe80::1                  a link-local destination           LL6
#
#   Changing one means changing both files.  CONFIGURE6=STATIC and no
#   GATEWAY6, deliberately: it makes the whole arm independent of whether the
#   lab has a router advertising anything this minute, and it is what leaves
#   OFF6 with no route.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# OUTPUT IS key=value AND AN EXIT CODE.  Nothing here reads prose, from the
# guest or from itself; srcprobe.c prints one `key=ok` or `key=fail:<errno>`
# line per check and the keys below are matched exactly.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=420
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD=a2065
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

while getopts "m:t:b:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board] [-B backend]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "srcsel_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
PROBE="$ROOT/$BUILD/tests/tools/SrcProbe"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$PROBE" "$BSD"; do
    [ -f "$f" ] || { echo "srcsel_stage=missing:$f" >&2; exit 2; }
done

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
    echo "srcsel_stage=missing:a2065.device" >&2
    echo "Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# The probe is given `self` on its command line before the guest boots, so a
# DHCP lease is not knowable then and the IPv4 side is static too.
SELF="${AMINETXDUO_SRC_SELF:-10.78.0.2}"
PEER="${AMINETXDUO_SRC_PEER:-10.78.0.1}"
MASK="${AMINETXDUO_SRC_MASK:-255.255.255.0}"

# Shared with srcprobe.c; see the block comment above.
SELF6="${AMINETXDUO_SRC_SELF6:-2001:db8:6724:1::10/64}"

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/srcsel-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$SELF
NETMASK=$MASK
GATEWAY=$PEER
CONFIGURE6=STATIC
ADDRESS6=$SELF6
STATE=up
IFEOF

cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$PROBE" "$STAGE/SrcProbe"

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:SrcProbe $SELF $PEER
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-srcsel}"

# A FRESH MAC EVERY RUN, not one pinned to this harness.  A repeated MAC means
# a repeated link-local, and a router or switch that still holds the previous
# run's neighbour entry can answer for a guest that never finished bringing the
# address up -- which is the shape of failure this arm is looking for.  The
# byte can be pinned with AMINETXDUO_SRCSEL_RUNBYTE when a run has to be
# repeated on the same address.
#
# 02:41:4d:4c:<runbyte>:7a: fourth byte 0x4c keeps this clear of both the
# derived space in tools/amiberry-run.sh (0x49) and run-cardsweep6.sh's (0x4b).
RUNBYTE="${AMINETXDUO_SRCSEL_RUNBYTE:-$(printf '%02x' $((RANDOM % 256)))}"
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_AMIBERRY_MAC:-02:41:4d:4c:$RUNBYTE:7a}"

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "srcsel_model=$MODEL"
echo "srcsel_board=$BOARD"
echo "srcsel_backend=$IFACE"
echo "srcsel_mac=$AMINETXDUO_AMIBERRY_MAC"
echo "srcsel_self=$SELF"
echo "srcsel_self6=$SELF6"

set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/SrcProbe"
RUN_RC=$?
set -e

echo "srcsel_run_rc=$RUN_RC"

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "srcsel_report=missing"; echo "srcsel=FAILED"; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FAILED=0
fail() { echo "$1=FAIL"; FAILED=1; }
pass() { echo "$1=ok"; }

# The probe running twice means the machine reset, which a wrong source index
# into nx_ip_interface[] is one way to produce.
STARTS=$(grep -c "SYS:SrcProbe" "$REPORT" || true)
echo "srcsel_probe_starts=$STARTS"
if [ "$STARTS" -eq 1 ]; then
    pass srcsel_no_reset
else
    fail srcsel_no_reset
fi

CHECKS=$(sed -n 's/^srcprobe_checks=//p' "$REPORT" | head -1)
FAILS=$(sed -n 's/^srcprobe_failures=//p' "$REPORT" | head -1)
echo "srcsel_probe_checks=${CHECKS:-none}"
echo "srcsel_probe_failures=${FAILS:-none}"

if [ -z "${FAILS:-}" ]; then
    fail srcsel_probe_summary
elif [ "$FAILS" -eq 0 ]; then
    pass srcsel_probe_summary
else
    fail srcsel_probe_summary
    grep '=fail:' "$REPORT" || true
fi

# Named individually so a silently skipped arm cannot pass by absence.  The
# IPv6 keys are the RFC 6724 half.
for want in \
    v4_udp_bound_iface_src \
    v4_udp_bound_loopback_src \
    v4_udp_unbound_sends \
    v4_udp_iface_to_loopback_unreach \
    v4_connect_loopback_from_loopback \
    v4_getsockname_unbound_route \
    v4_getsockname_unbound_dgram \
    v4_getsockname_unbound_iface \
    v4_connect_loopback_from_iface_unreach \
    v4_connect_iface_from_iface \
    v4_accepted_peer_is_bound_src \
    v4_unbound_connect_route \
    v4_bind_foreign_eaddrnotavail \
    v6_socket \
    v6_address_ready \
    v6_connect_onlink \
    v6_src_onlink_is_global \
    v6_connect_linklocal \
    v6_src_linklocal_is_linklocal \
    v6_src_linklocal_is_not_global \
    v6_connect_loopback \
    v6_src_loopback_is_loopback \
    v6_offlink_no_route_refused
do
    if grep -Fqx "$want=ok" "$REPORT"; then
        pass "srcsel_$want"
    else
        fail "srcsel_$want"
    fi
done

echo
if [ "$FAILED" -ne 0 ]; then
    echo "srcsel=FAILED"
    exit 1
fi

echo "srcsel=PASSED"
exit 0
