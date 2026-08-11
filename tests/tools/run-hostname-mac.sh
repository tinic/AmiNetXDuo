#!/usr/bin/env bash
#
# TWO MACHINES, NO CONFIGURATION, TWO NAMES.
#
#   tests/tools/run-hostname-mac.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT WENT WRONG
#
#   A machine with nothing in DEVS:NetInterfaces (no ID=), nothing in
#   ENV:HOSTNAME, nothing in DEVS:Internet/name_resolution and no DHCP option
#   12 fell back to the literal "amiga", so every such machine on a segment
#   claimed amiga.local.  Seventy emulated guests did it at once and the RFC
#   6762 9 probe took a working machine's name off the air while its address
#   kept answering.
#
#   ami_ns_name_after_card() now takes the last three octets of the card's
#   hardware address: 00:80:10:49:00:01 becomes "amiga-490001".
#
# THE ASSERTION THAT MATTERS IS THE SECOND BOOT
#
#   One machine coming up as amiga-490001 shows the derivation runs.  It does
#   not show the collision is gone -- a constant would pass that.  So this
#   boots twice with two hardware addresses and requires two names.  The A2065
#   keeps the last three bytes of AMINETXDUO_AMIBERRY_MAC and overwrites the
#   first three with Commodore's 00:80:10 (tools/amiberry-run.sh), which is
#   exactly the half the name is cut from.
#
# WHAT SLIRP CANNOT SHOW, AND IS NOT CLAIMED
#
#   Each guest has its own NAT and its own 10.0.2.15; the two never share a
#   link layer, so neither can see the other's probe and "they did not collide
#   on the wire" is not measurable here.  What is measurable, and is what the
#   change is: each machine derives a different name from its own card, says
#   the same name through `hostname`, ShowNetStatus and gethostname(), and
#   answers to it on .local.  The mDNS half is answered out of the responder's
#   own local cache with no packet, which is the same thing run-mdns.sh relies
#   on and the reason it works behind a NAT.
#
#   The bridged backend would show the collision directly and is deliberately
#   not used: a guest on the lab LAN claiming amiga.local is the fault under
#   repair, and running it would inflict it on whoever is on that segment.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
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

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/ShowNetStatus" \
         "$TOOLS/hostname" "$TOOLS/host" "$BSD"; do
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

# The two cards.  Only the last three octets survive the A2065, and they are
# what the name is cut from, so the two differ there: one in the last octet
# alone, which is the pair a two-machine bench actually has, and one in all
# three so that a derivation using only part of the tail is caught too.
MAC_A="02:41:4d:49:00:01"; WANT_A="amiga-490001"
MAC_B="02:41:4d:ab:cd:ef"; WANT_B="amiga-abcdef"

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }

# One boot: stage a machine with NOTHING naming it, run the three commands
# that report a name, and ask the resolver for the name we expect it to have
# derived.  Writes the transcript path to stdout.
boot() {
    local tag="$1" mac="$2" want="$3"
    local stage="$ROOT/build/hostname-mac-stage-$tag"

    rm -rf "$stage"
    mkdir -p "$stage/libs"
    cp -R "$ROOT/tests/netstack/devs" "$stage/devs"
    cp "$A2065" "$stage/devs/a2065.device"
    cp "$BSD"   "$stage/libs/bsdsocket.library"
    for t in AddNetInterface ShowNetStatus hostname host; do
        cp "$TOOLS/$t" "$stage/$t"
    done

    # The responder, which is off unless an interface file asks for it.
    echo "MDNS=YES" >> "$stage/devs/NetInterfaces/eth0"

    # And NOTHING that names the machine: no ID= in the interface file, no
    # `hostname` line in name_resolution, no ENV:HOSTNAME.  Asserted rather
    # than assumed, because a stray line in the shared fixture would make this
    # whole run test the configured path by accident.
    if grep -qi '^[[:space:]]*ID=' "$stage/devs/NetInterfaces/eth0" ||
       grep -qi '^[[:space:]]*hostname' "$stage/devs/Internet/name_resolution"
    then
        echo "the fixture names the machine; this test needs one that does not" >&2
        exit 2
    fi

    {
        echo "SYS:AddNetInterface eth0"
        echo "wait 3"
        echo "SYS:hostname"
        echo "SYS:ShowNetStatus"
        echo "SYS:host $want.local"
    } > "$stage/commands.txt"

    export AMINETXDUO_RUN_TAG="hostname-mac-$tag"
    export AMINETXDUO_AMIBERRY_MAC="$mac"

    echo "==> booting $MODEL, A2065 on SLIRP, MAC $mac" >&2
    set +e
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$stage/commands.txt" "$stage/devs" "$stage/libs" \
        "$stage/AddNetInterface" "$stage/ShowNetStatus" "$stage/hostname" \
        "$stage/host" >&2
    local rc=$?
    set -e

    local report="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG/tools.txt"
    [ -f "$report" ] || {
        echo "FAIL: guest $tag wrote no $report (run rc=$rc)" >&2
        return 1
    }
    echo "$report"
}

check() {
    local tag="$1" want="$2" report="$3"

    echo
    echo "================= guest $tag, expecting $want ================="
    cat "$report"
    echo "=============================================================="

    # `hostname` with no argument prints the name on a line of its own.
    if grep -qx "$want" "$report"; then
        echo "guest_${tag}_hostname=$want"
    else
        echo "guest_${tag}_hostname=MISSING"
        fail "guest $tag: \`hostname\` did not print $want"
    fi

    # The premise, not the claim: nothing configured a name, so what follows
    # is about the fallback.  It holds whether or not the fallback works, and
    # is here to catch a fixture that starts naming the machine.
    if grep -q "not named by anything" "$report"; then
        echo "guest_${tag}_source=derived"
    else
        echo "guest_${tag}_source=OTHER"
        fail "guest $tag: something named the machine, so nothing was derived"
    fi

    # ShowNetStatus reads gethostname() off the running library, so this is
    # the second consumer agreeing with the first.
    if grep -q "^Host name: *$want" "$report"; then
        echo "guest_${tag}_shownetstatus=$want"
    else
        echo "guest_${tag}_shownetstatus=MISSING"
        fail "guest $tag: ShowNetStatus does not say $want"
    fi

    # And the responder claimed it: the third consumer, and the one the
    # collision was in.
    if grep -qi "^$want.local has address " "$report"; then
        echo "guest_${tag}_mdns=$want.local"
    else
        echo "guest_${tag}_mdns=MISSING"
        fail "guest $tag: $want.local did not resolve"
    fi

    # The name it must NOT have, read off the line that says what the
    # RESPONDER claimed.  Not off `hostname`: with the derivation removed the
    # machine printed "localhost" there and "amiga.local" here, which is the
    # divergence that made the fault hard to see from outside and is why this
    # assertion is on the mDNS line rather than on the command.
    if grep -q "^Known here as: *amiga.local" "$report"; then
        echo "guest_${tag}_claimed=amiga.local"
        fail "guest $tag: the responder still claimed amiga.local"
    else
        echo "guest_${tag}_claimed=$(sed -n 's/^Known here as: *\([^ ]*\).*/\1/p' \
                                     "$report" | head -1)"
    fi
}

REPORT_A=$(boot a "$MAC_A" "$WANT_A") || FAILED=1
REPORT_B=$(boot b "$MAC_B" "$WANT_B") || FAILED=1

[ -n "${REPORT_A:-}" ] && check a "$WANT_A" "$REPORT_A"
[ -n "${REPORT_B:-}" ] && check b "$WANT_B" "$REPORT_B"

# The point of the whole run, and read back out of the two transcripts rather
# than out of the two expectations, which are constants and would agree with
# each other whatever the guests did.
GOT_A=$(grep -oE '^amiga-[0-9a-f]{6}$' "${REPORT_A:-/dev/null}" | head -1 || true)
GOT_B=$(grep -oE '^amiga-[0-9a-f]{6}$' "${REPORT_B:-/dev/null}" | head -1 || true)

echo
echo "got_a=${GOT_A:-NONE}"
echo "got_b=${GOT_B:-NONE}"
if [ -n "$GOT_A" ] && [ -n "$GOT_B" ] && [ "$GOT_A" != "$GOT_B" ]; then
    echo "names_differ=yes"
else
    echo "names_differ=no"
    fail "the two machines did not take two different names"
fi

echo "result=$([ "$FAILED" -eq 0 ] && echo PASS || echo FAIL)"
exit "$FAILED"
