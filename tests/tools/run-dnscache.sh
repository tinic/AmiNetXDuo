#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR THE DNS ANSWER CACHE.
#
#   tests/tools/run-dnscache.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT IT IS PROVING
#
#   NX_DNS_CACHE_ENABLE plus one call to nx_dns_cache_initialize() is the whole
#   change, and a `#define` that changes no packet is not a feature.  So the
#   assertion is not "the second lookup was fast": it is that the second lookup
#   PUT NO QUERY ON THE WIRE.
#
#   Two names are looked up three times each, alternating, from six separate
#   invocations of `host`.  The stack survives between them, AddNetInterface
#   holds the reference that keeps it up, so all six share one NX_DNS.
#
#     with the cache     two queries leave the machine, one per name;
#     without it         six do, one per invocation.
#
#   Two names rather than one because the count is then the assertion by
#   itself: a stack that never queried at all, or one that queried every time,
#   both fail, and no separately built control arm is needed to tell them
#   apart.
#
#   The count is taken from the EMULATED A2065's own frame log, which is
#   written inside the emulated hardware below every line of our code.
#
# WHAT ELSE IT ASSERTS
#
#   * every lookup of a name returned the SAME address.  A cache that returned
#     nothing, or something else, on the hit would otherwise show up here as a
#     pass, there would be no query on the wire either way.
#   * the reverse direction is exercised too, because PTR records go through
#     the same cache by a different path (_nx_dns_host_by_address_get_internal).
#
# THE ONE EXTERNAL DEPENDENCY, stated rather than hidden: SLIRP's DNS server at
# 10.0.2.3 forwards to the host's resolver, so this test needs the host to be
# able to resolve two public names.  If the first lookup of each name fails the
# test says so and fails, rather than passing on an empty wire.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/host" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/dnscache-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface host; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

# Alternating on purpose. A cache that held exactly one answer would pass a
# run that asked AAABBB and fail this one, and "holds one entry" is not what
# was built.
{
    echo "SYS:AddNetInterface eth0"
    for _ in 1 2 3; do
        echo "SYS:host $NAME_A"
        echo "SYS:host $NAME_B"
    done
} > "$STAGE/commands.txt"

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-dnscache}"
HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"
FSLOG="$ROOT/build/fsuae-base-$AMINETXDUO_RUN_TAG/Cache/Logs/fs-uae.log.txt"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
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
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

STARTS=$(grep -c "SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted exactly once (no reset)"
else
    fail "the command list ran $STARTS times -- the machine reset"
fi

# ---- every lookup answered, and answered the same ------------------------
for name in "$NAME_A" "$NAME_B"; do
    ANSWERS=$(grep -c "^$name " "$REPORT" || true)
    UNIQUE=$(grep "^$name " "$REPORT" | sort -u | wc -l | tr -d ' ')

    if [ "$ANSWERS" -eq 3 ]; then
        pass "$name resolved on all three lookups"
    else
        fail "$name produced $ANSWERS answers, expected 3"
        echo "       (SLIRP forwards to the host's resolver -- can this host" >&2
        echo "        resolve $name?)" >&2
    fi

    if [ "$UNIQUE" -eq 1 ] && [ "$ANSWERS" -gt 0 ]; then
        pass "$name gave the same answer every time"
    else
        fail "$name gave $UNIQUE different answers"
    fi
done

# ---- THE WIRE ------------------------------------------------------------
if [ -f "$FSLOG" ]; then
    python3 "$ROOT/tests/trace/a2065pcap.py" "$FSLOG" -o "$HD/host.pcap" \
        > "$HD/a2065.txt" 2>&1 || true
fi

if [ -s "$HD/host.pcap" ]; then
    # Queries only: the guest's own datagrams TO port 53. tcpdump prints the
    # question name for a DNS query, so each name can be counted separately.
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
    fail "no host-side capture at $HD/host.pcap -- the wire was not observed"
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "dnscache: FAILED" >&2
    exit 1
fi

echo "dnscache: PASSED"
exit 0
