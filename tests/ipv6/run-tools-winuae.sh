#!/usr/bin/env bash
#
# ping and traceroute over IPv6 on a bridged real network, under WinUAE.
#
#   tests/ipv6/run-tools-winuae.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                  [-T TAG] [-6 ADDR] [-4 ADDR]
#
# WHAT THIS PROVES THAT tests/ipv6/run-tools-fsuae.sh CANNOT.  FS-UAE's SLIRP
# forwards nothing beyond itself: `traceroute 8.8.8.8` is `*` at every hop
# there for IPv4 as well, so every trace that harness can run terminates in
# one hop because the target was one hop away.  RESEARCH 67 could show the
# hop-limit ramp only by reading it off the emulated A2065's frames.  Here the
# guest takes a SLAAC address from the LAN's own router advertisement and the
# ramp is proved the other way round: by routers on the Internet decrementing
# it and returning ICMPv6 time exceeded, one distinct address per hop.
#
# The run needs, and checks for, four things:
#
#   * a pcap device to bridge onto, in AMINETXDUO_WINUAE_A2065;
#   * a WinUAE built from master, 6.0.3 dies on the first coalesced frame,
#     RESEARCH 63.4, with tools/winuae/a2065-multicast-loopback.patch on
#     top of it.  Without that patch the guest gets no IPv6 address at all;
#     the patch header says why;
#   * a router advertisement on the LAN.  Without one the guest has a
#     link-local address and nothing else, which can reach no off-link
#     destination and cannot trace anywhere; that is reported as a skip with
#     exit 3, not as a failure, because it is the network's answer and not
#     the stack's.
#   * an IPv6 destination that answers echo requests.  2606:4700:4700::1111
#     does; 2001:4860:4860::8888 was measured NOT answering from here, so a
#     silent target reads as a broken stack.  -6 picks another.
#
# IPv4 runs alongside every IPv6 case rather than in a separate script: the
# question this answers is whether the IPv6 work left IPv4 alone, and the only
# way to say that about the same boot is to ask both in it.
#
# Exit 0 all clear, 1 an assertion failed, 3 the LAN has no IPv6 to test on.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=420
BUILD="${AMINETXDUO_BUILD:-build/v6}"
TAG="${AMINETXDUO_RUN_TAG:-v6wire}"
V6TARGET="2606:4700:4700::1111"
V4TARGET="1.1.1.1"

while getopts "m:t:b:T:6:4:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        6) V6TARGET="$OPTARG" ;;
        4) V4TARGET="$OPTARG" ;;
        *) sed -n '3,5p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in
    /*) BDIR="$BUILD" ;;
    *)  BDIR="$ROOT/$BUILD" ;;
esac
TOOLS="$BDIR/src/tools"

[ -n "${AMINETXDUO_WINUAE_A2065:-}" ] || {
    echo "AMINETXDUO_WINUAE_A2065 is unset: this test is only meaningful" >&2
    echo "bridged onto a real adapter. Set it to the pcap device name," >&2
    echo "\\Device\\NPF_{...}, from pcap_findalldevs on the Windows host." >&2
    exit 2; }

STAGED_TOOLS="AddNetInterface ShowNetStatus netstat ping traceroute"
for t in $STAGED_TOOLS ToolsSmoke; do
    [ -f "$TOOLS/$t" ] || { echo "missing $TOOLS/$t, build $BUILD first" >&2
                            exit 2; }
done

CACHE="$BDIR/CMakeCache.txt"
if [ -f "$CACHE" ] && ! grep -q "^AMINETXDUO_IPV6:BOOL=ON" "$CACHE"; then
    echo "$BUILD was configured without IPv6." >&2
    exit 2
fi

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

# --------------------------------------------------------------- staging ---

STAGE="$ROOT/build/v6wire-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BDIR/src/bsdsocket/bsdsocket.library" "$STAGE/libs/bsdsocket.library"
for t in $STAGED_TOOLS; do cp "$TOOLS/$t" "$STAGE/$t"; done

# CONFIGURE6=AUTO, not STATIC: the address has to come from the LAN's own
# router advertisement, or the global one this test traces from would be an
# address this file invented and no router would route it.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=AUTO
EOF

# The wait is not padding.  A SLAAC address is TENTATIVE until duplicate
# address detection finishes and a tentative address cannot be a source, so a
# ping issued straight after the interface comes up fails with "no route to
# that network" while everything is in fact working.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
wait 10
SYS:ShowNetStatus INTERFACES
SYS:ShowNetStatus ALL
SYS:ping $V4TARGET -c 2 -t 10
SYS:traceroute $V4TARGET -m 20 -q 1 -w 3 -n
SYS:ping $V6TARGET -c 3 -t 10
SYS:traceroute $V6TARGET -m 20 -q 1 -w 3 -n
EOF

# ------------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="$TAG"
export AMINETXDUO_WINUAE_EXE="${AMINETXDUO_WINUAE_EXE:-C:\\winuae-patched\\winuae64.exe}"
HD="$ROOT/build/winuae-testhd-$TAG"

echo "==> $BUILD on $MODEL, A2065 bridged onto ${AMINETXDUO_WINUAE_A2065}"
echo "==> CONFIGURE6=AUTO; v6 target $V6TARGET, v4 target $V4TARGET"
set +e
"$ROOT/tools/winuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    $(for t in $STAGED_TOOLS; do echo "$STAGE/$t"; done)
RUN_RC=$?
set -e

RAW="$HD/tools.txt"
[ -f "$RAW" ] || { echo "FAIL: the guest wrote no $RAW (run rc=$RUN_RC)" >&2
                   exit 1; }

REPORT="$ROOT/build/v6wire-report-$TAG.txt"
tr -d '\r' < "$RAW" > "$REPORT"

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

# ------------------------------------------------------------ assertions ---

FAILED=0
ok()  { echo "  ok:      $*"; }
bad() { echo "  FAIL:    $*" >&2; FAILED=$((FAILED + 1)); }

block() {
    awk -v want="===== $1 =====" '
        index($0, want) == 1 { inb = 1; next }
        inb && /^----- rc /   { exit }
        inb                   { print }
    ' "$REPORT"
}
have() { block "$1" | grep -qiF, "$2"; }
want() { if have "$1" "$2"; then ok "$3"; else bad "$3"; fi; }

# Hops that answered, one address per line, from a traceroute block.  A hop
# that did not answer prints `*` and contributes nothing.
hops() {
    block "$1" | awk '$1 ~ /^[0-9]+$/ && $2 != "*" { print $2 }'
}

# ---- the LAN's own answer, before anything is judged --------------------
#
# A guest with only fe80:: has nowhere off-link to go.  That is the network
# saying it has no IPv6, not the stack failing, and the difference has to be
# reported rather than turned into a red assertion.
GLOBAL=$(block "SYS:ShowNetStatus INTERFACES" |
         grep -oE '[0-9a-f]{1,4}:[0-9a-f:]*::?[0-9a-f:]*' |
         grep -viE '^(fe80|fd|fc|ff)' | head -1 || true)
if [ -z "$GLOBAL" ]; then
    echo "SKIP: the guest has no global IPv6 address."
    echo "      Either this LAN sends no router advertisement, or the"
    echo "      emulator is not carrying multicast, see"
    echo "      tools/winuae/a2065-multicast-loopback.patch."
    echo "      Nothing off-link can be tested from here; IPv4 was:"
    block "SYS:ping $V4TARGET -c 2 -t 10" | tail -3
    exit 3
fi
ok "the LAN advertised a prefix; the guest autoconfigured $GLOBAL"

want "SYS:ShowNetStatus INTERFACES" "fe80::" "the link-local address is up"

# ---- control: IPv4 on the same boot -------------------------------------
want "SYS:ping $V4TARGET -c 2 -t 10" "2 received" \
     "IPv4 ping $V4TARGET got both replies"

V4HOPS=$(hops "SYS:traceroute $V4TARGET -m 20 -q 1 -w 3 -n" | sort -u | wc -l | tr -d ' ')
V4LAST=$(hops "SYS:traceroute $V4TARGET -m 20 -q 1 -w 3 -n" | tail -1)
if [ "$V4HOPS" -ge 3 ]; then
    ok "IPv4 traceroute crossed $V4HOPS distinct routers"
else
    bad "IPv4 traceroute saw $V4HOPS distinct routers, wanted 3 or more"
fi
[ "$V4LAST" = "$V4TARGET" ] && ok "IPv4 traceroute reached $V4TARGET" \
                            || bad "IPv4 traceroute ended at '$V4LAST'"

# ---- IPv6 off-link, through a router and not to a neighbour -------------
want "SYS:ping $V6TARGET -c 3 -t 10" "3 received" \
     "IPv6 ping $V6TARGET got all three replies"

V6HOPS=$(hops "SYS:traceroute $V6TARGET -m 20 -q 1 -w 3 -n" | sort -u | wc -l | tr -d ' ')
V6LAST=$(hops "SYS:traceroute $V6TARGET -m 20 -q 1 -w 3 -n" | tail -1)

# THE ASSERTION THIS SCRIPT EXISTS FOR.  Three or more distinct addresses
# means three or more routers each decremented the hop limit to zero and
# returned ICMPv6 time exceeded, which is the ramp proved end to end.
if [ "$V6HOPS" -ge 3 ]; then
    ok "IPv6 traceroute crossed $V6HOPS distinct routers, the hop-limit
           ramp works against real routers, not just on the wire"
else
    bad "IPv6 traceroute saw $V6HOPS distinct routers, wanted 3 or more"
fi
[ "$V6LAST" = "$V6TARGET" ] && ok "IPv6 traceroute reached $V6TARGET" \
                            || bad "IPv6 traceroute ended at '$V6LAST'"

# The first hop must be a router and not the guest itself: a trace whose
# hop 1 is the destination proves nothing about the ramp.
V6FIRST=$(hops "SYS:traceroute $V6TARGET -m 20 -q 1 -w 3 -n" | head -1)
[ -n "$V6FIRST" ] && [ "$V6FIRST" != "$V6TARGET" ] \
    && ok "hop 1 is a router ($V6FIRST), not the destination" \
    || bad "hop 1 was '$V6FIRST', the destination was one hop away"

echo
if [ "$FAILED" -gt 0 ]; then
    echo "FAILED: $FAILED assertion(s)"
    exit 1
fi
echo "PASS: IPv6 and IPv4 both reach off-link destinations through routers"
exit 0
