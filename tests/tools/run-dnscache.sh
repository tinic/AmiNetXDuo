#!/usr/bin/env bash
# THE REGRESSION TEST FOR THE DNS ANSWER CACHE.
#
#   tests/tools/run-dnscache.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR] [-B IFACE]
#
# THE WIRE IS WHAT THIS TEST IS FOR: nothing the guest prints can tell a cache
# that answered from memory from a stack that asked again and got the same
# answer.  It used to read $HD/host.pcap, which fs-uae wrote out of its SLIRP
# link and which nothing has written since fs-uae left the tree on 2026-08-04,
# so the strongest assertion group never ran and the harness reached exit 77
# every time however healthy the stack was.  The capture is now taken on the
# host, on the interface the guest is bridged onto, the way
# tests/tools/run-wirequiet.sh takes one, and a host that cannot take it
# refuses the run instead of passing it.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"
# BRIDGED.  The two names are resolved by whatever the DHCP server on the
# segment hands out, and nothing below knows or cares which server that is.
# It used to take the backend it inherited, which with nothing set is SLIRP,
# and a cache result taken there is the emulator's own resolver rather than
# ours.
IFACE="${AMINETXDUO_DNSCACHE_IFACE:-${AMINETXDUO_AMIBERRY_BACKEND:-ens18}}"

while getopts "m:t:b:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-B iface]" >&2; exit 2 ;;
    esac
done

kv() { printf '%s=%s\n' "$1" "$2"; }
refuse() { kv reason "$1"; kv RESULT refused; exit 2; }

case "$IFACE" in
    slirp|slirp_inbound)
        refuse "slirp_has_no_segment_to_capture: -B <interface>" ;;
esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/host" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

command -v tcpdump >/dev/null 2>&1 ||
    refuse "no tcpdump on this host, so the wire cannot be observed"
timeout 10 tcpdump -i "$IFACE" -c 1 -n >/dev/null 2>&1 || {
    rc=$?
    [ "$rc" = 124 ] ||
        refuse "tcpdump cannot read $IFACE as $(id -un); this needs no sudo on a host where it is set up, and running without it would assert nothing"
}

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
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

NAME_A="${AMINETXDUO_DNS_NAME_A:-example.com}"
NAME_B="${AMINETXDUO_DNS_NAME_B:-example.org}"


STAGE="$ROOT/build/dnscache-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface host; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

# NO STATIC NAME SERVER.  tests/netstack/devs/Internet/name_resolution still
# carries `nameserver 10.0.2.3`, fs-uae's SLIRP forwarder, which does not
# exist on a real segment: every lookup here was sent there first, waited a
# second, and was then re-sent to the server the lease supplies.  On the wire
# that is two packets per name, and the count below is a count of packets.
# The lease's server is the only one this test wants.
cat > "$STAGE/devs/Internet/name_resolution" <<'NREOF'
# Deliberately empty of servers: the DHCP lease supplies the only one, and a
# second, unreachable server would double every query on the wire.
NREOF

{
    echo "SYS:AddNetInterface eth0"
    for _ in 1 2 3; do
        echo "SYS:host $NAME_A"
        echo "SYS:host $NAME_B"
    done
} > "$STAGE/commands.txt"


export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-dnscache}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

# NOT under $HD: tools/amiberry-run.sh removes that directory as it stages the
# drive, so a capture started before the run and written there is deleted
# while tcpdump still holds the descriptor.
PCAP="$ROOT/build/dnscache-$AMINETXDUO_RUN_TAG.pcap"
TCPDUMPLOG="$ROOT/build/dnscache-$AMINETXDUO_RUN_TAG.tcpdump.log"
rm -f "$PCAP"

tcpdump -i "$IFACE" -n -s 256 -U -w "$PCAP" 'udp port 53' \
    > "$TCPDUMPLOG" 2>&1 &
TCPDUMP_PID=$!
capture_stop() {
    [ -n "${TCPDUMP_PID:-}" ] || return 0
    kill "$TCPDUMP_PID" 2>/dev/null || true
    wait "$TCPDUMP_PID" 2>/dev/null || true
    TCPDUMP_PID=""
}
trap capture_stop EXIT INT TERM HUP

for _ in 1 2 3 4 5 6 7 8 9 10; do
    grep -q 'listening on' "$TCPDUMPLOG" 2>/dev/null && break
    sleep 0.5
done
grep -q 'listening on' "$TCPDUMPLOG" 2>/dev/null || {
    capture_stop
    sed 's/^/       /' "$TCPDUMPLOG" >&2
    refuse "tcpdump did not start on $IFACE"; }
echo "==> capturing udp port 53 on $IFACE into $PCAP"

echo "==> booting $MODEL, a2065 bridged on $IFACE"
set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/host"
RUN_RC=$?
set -e

sleep 1
capture_stop
trap - EXIT INT TERM HUP

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

STARTS=$(grep -c "SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted exactly once (no reset)"
else
    fail "the command list ran $STARTS times, the machine reset"
fi

# `has address `, NOT `^$name `.  On a dual-stack segment `host` prints an
# IPv6 line as well, so three lookups produced five or six lines and two
# distinct ones, and this read that as "five answers, two of them different".
# Measured bridged on 2026-08-27: example.com 3 A lines, one value, plus 2
# AAAA lines -- the FIRST lookup of a name carries no AAAA.  The cache
# question is about the A answer, and the AAAA lines are a separate one.
for name in "$NAME_A" "$NAME_B"; do
    ANSWERS=$(grep -c "^$name has address " "$REPORT" || true)
    UNIQUE=$(grep "^$name has address " "$REPORT" | sort -u | wc -l | tr -d ' ')

    if [ "$ANSWERS" -eq 3 ]; then
        pass "$name resolved on all three lookups"
    else
        fail "$name produced $ANSWERS answers, expected 3"
        echo "       (the resolver is the one the segment's DHCP server" >&2
        echo "        handed out; can it resolve $name?)" >&2
    fi

    if [ "$UNIQUE" -eq 1 ] && [ "$ANSWERS" -gt 0 ]; then
        pass "$name gave the same answer every time"
    else
        fail "$name gave $UNIQUE different answers"
    fi
done

# ---------------------------------------------------------------- the wire ---
#
# The whole point of the file.  The guest's frames are picked out of the
# capture by its own source address, which is read off the bring-up line
# rather than assumed: the segment's DHCP server hands out whatever it has.
#
# `A?` AND ONLY `A?`.  On a dual-stack segment `host` asks for AAAA as well,
# and a filter that matched the name anywhere counted both kinds of query
# against a ceiling meant for one.  The AAAA count is reported beside it and
# asserted on separately.
GUESTIP=$(sed -n 's/^.*online, address \([0-9][0-9.]*\).*$/\1/p' "$REPORT" |
          head -1)
kv guest_ip "${GUESTIP:-none}"

if [ -z "$GUESTIP" ]; then
    fail "the guest never printed an address, so nothing identifies its queries"
elif [ ! -s "$PCAP" ]; then
    fail "the capture $PCAP is empty; the wire was not observed"
else
    QUERIES="$ROOT/build/dnscache-$AMINETXDUO_RUN_TAG.queries.txt"
    tcpdump -r "$PCAP" -n "udp dst port 53 and src host $GUESTIP" \
        > "$QUERIES" 2>/dev/null || true

    kv wire_queries "$(grep -c . "$QUERIES" || true)"
    echo "  what the guest asked for:"
    sed 's/^/       /' "$QUERIES"

    for name in "$NAME_A" "$NAME_B"; do
        esc=$(printf '%s' "$name" | sed 's/\./\\./g')
        A=$(grep -cE " A\\? $esc\\.? " "$QUERIES" || true)
        AAAA=$(grep -cE " AAAA\\? $esc\\.? " "$QUERIES" || true)

        kv "wire_a_$name" "$A"
        kv "wire_aaaa_$name" "$AAAA"

        if [ "$A" -eq 1 ]; then
            pass "$name: exactly ONE A query on the wire for three lookups"
        else
            fail "$name: $A A queries on the wire for three lookups, expected 1"
        fi

        if [ "$AAAA" -le 1 ]; then
            pass "$name: $AAAA AAAA query on the wire for three lookups"
        else
            fail "$name: $AAAA AAAA queries on the wire for three lookups, expected at most 1"
        fi
    done
fi

kv run_rc "$RUN_RC"
kv pcap   "$PCAP"

echo
if [ "$FAILED" -ne 0 ]; then
    kv RESULT fail
    exit 1
fi

kv RESULT pass
exit 0
