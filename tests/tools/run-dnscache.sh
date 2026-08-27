#!/usr/bin/env bash
# THE REGRESSION TEST FOR THE DNS ANSWER CACHE.
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

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/host" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

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

{
    echo "SYS:AddNetInterface eth0"
    for _ in 1 2 3; do
        echo "SYS:host $NAME_A"
        echo "SYS:host $NAME_B"
    done
} > "$STAGE/commands.txt"


export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-dnscache}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL, a2065 bridged on $IFACE"
set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/host"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FAILED=0
UNRUN=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }
skip() { echo "  --: $*"; UNRUN=$((UNRUN + 1)); }

STARTS=$(grep -c "SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted exactly once (no reset)"
else
    fail "the command list ran $STARTS times, the machine reset"
fi

for name in "$NAME_A" "$NAME_B"; do
    ANSWERS=$(grep -c "^$name " "$REPORT" || true)
    UNIQUE=$(grep "^$name " "$REPORT" | sort -u | wc -l | tr -d ' ')

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

if [ -s "$HD/host.pcap" ]; then
    tcpdump -r "$HD/host.pcap" -n "udp dst port 53" 2>/dev/null > "$HD/dns.txt" || true

    TOTAL=$(wc -l < "$HD/dns.txt" | tr -d ' ')
    echo "  DNS queries seen on the wire: $TOTAL"
    sed 's/^/       /' "$HD/dns.txt"

    for name in "$NAME_A" "$NAME_B"; do
        N=$(grep -c "$name" "$HD/dns.txt" || true)
        if [ "$N" -eq 1 ]; then
            pass "$name was asked for exactly ONCE in three lookups"
        else
            fail "$name was asked for $N times in three lookups, expected 1"
        fi
    done
else
    skip "no host-side capture: the wire was not observed, so the cache was
       measured only from what the guest printed"
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "dnscache: FAILED" >&2
    exit 1
fi

if [ "$UNRUN" -ne 0 ]; then
    echo "dnscache: SKIPPED, $UNRUN assertion group(s) did not run" >&2
    echo "  The wire is what this test is for.  Nothing else here can tell a" >&2
    echo "  cache that answered from memory from a stack that queried again" >&2
    echo "  and got the same answer, so this is not a pass." >&2
    exit 77
fi

echo "dnscache: PASSED"
exit 0
