#!/usr/bin/env bash
#
# TURNING THE mDNS RESPONDER ON AND OFF WHILE THE MACHINE RUNS.
#
#   tests/tools/run-mdnsctl.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT THIS IS FOR, AND WHY IT IS NOT tests/tools/run-mdns.sh
#
#   run-mdns.sh asks whether the responder is right: the label it derives, the
#   records it puts on the wire, what SLIRP does with them.  It has one
#   configuration and it never changes it.
#
#   This one asks whether MDNS= is a switch.  Until now it was read once, at
#   start-up, and an interface added by hand set the flag that NETSTATUS_IF_MDNS
#   is reported from without ever calling nx_mdns_enable(): the interface joined
#   no group, probed for no name and answered nothing, while ShowNetStatus said
#   it was answering .local.  A remove neither disabled it nor cleared the flag.
#
#   So the four things below are the whole test, and the third and the fourth
#   are the ones a flag that only pretends cannot pass:
#
#     1. up with no MDNS=      -- reported off, and the name does not resolve
#     2. MDNS=YES              -- reported on, and the name resolves
#     3. MDNS=NO               -- reported off, and it STOPS resolving
#     4. removed and re-added  -- reported off, because the file never asked
#
#   Between 2 and 3 the machine is turned on again, so an off/on pair is
#   exercised as well as an off: the module suspends its records rather than
#   deleting them and re-announces what it finds, so this is where a second
#   registration of the same services would show up.
#
# HOW "IT RESOLVES" IS DECIDED, AND WHAT IT IS WORTH
#
#   On the guest: `host amigatest.local`.  That is not a self-check dressed up
#   as one.  nx_mdns_host_address_get() walks the ENABLED interfaces and asks on
#   each, and answers out of the local cache for a name this machine has claimed
#   -- so with no interface enabled it has nowhere to ask and fails, and it
#   fails the same way for a name that was never probed for.  The command is
#   unmodified and reaches the module through gethostbyname() and
#   netstack_resolve(), which is the path every command shares.
#
#   On the host: tests/tools/mdnswatch.py sits on the host's REAL network,
#   joined to 224.0.0.251, for the whole run.  Whatever it hears got there
#   through the emulator's NAT, so a line naming this guest is another machine
#   seeing the name -- and the TTL tells the two events apart, an announcement
#   carries the RFC 6762 10 TTL and the goodbye that MDNS=NO sends carries zero.
#
#   THAT HALF IS REPORTED, NOT ASSERTED, and the reason is measured rather than
#   assumed: SLIRP is a user-mode NAT and rewrites the source port, and
#   _nx_mdns_packet_address_check() drops any mDNS datagram whose source port is
#   not 5353 (RFC 6762 6), so nothing the host sends can be answered by the
#   guest under this backend.  A bridged run would answer it and is not
#   available here.  A run that hears nothing exits 3, "not measured", rather
#   than red: whether a NAT relays multicast is a property of the emulator.
#
# ONE BOOT.  ToolsSmoke reopens its transcript from the top after a reset, so a
# reboot would turn "five reports" into a shorter list that still reads as a
# pass; the count of adds is asserted for that reason.
#
# SLIRP, deliberately: the guest-side half needs no peer, and a second bridged
# Amiga on this host would take the MAC of one that is already there.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# A TIMEOUT IS A DEFECT.  The ceiling below is twice the measured good case,
# which is stated beside it.  A run that burns it produces a partial transcript
# and proves nothing; the verdict names the command the transcript stops at.
# Raising -t is never the fix.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
# THE GOOD CASE IS 48 SECONDS, measured on playhouse3 (Amiberry 68020 A1200 on
# SLIRP): boot, seventeen commands and thirteen seconds of deliberate waiting
# for probing.  The ceiling is twice it, the same arithmetic
# tests/tools/run-ifconfigure.sh uses, because an emulator that costs more than
# double what it has been seen to cost is hung.  The run prints what it really
# took, so the number stays checkable.
TIMEOUT=96
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) sed -n '3,5p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" \
         "$TOOLS/RemoveNetInterface" "$TOOLS/ConfigureNetInterface" \
         "$TOOLS/ShowNetStatus" "$TOOLS/host" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# The configured name, and the single DNS label mDNS has to derive from it.
# Deliberately not equal, as run-mdns.sh has it: a responder that used HOSTNAME
# verbatim would claim "amigatest.home.lan.local" and every assertion below
# would fail rather than passing by accident.
CFG_HOSTNAME="amigatest.home.lan"
LABEL="amigatest"

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/mdnsctl-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# STATIC, and SLIRP's own numbers, for the reason run-ifconfigure.sh is static:
# a DHCP client is a second writer, and the A record the responder announces is
# whatever the interface holds, so a lease arriving mid-run would move it.
#
# AND NO MDNS= LINE AT ALL.  That is step 1's premise and step 4's: the file
# never asks for mDNS, so nothing in this run may report it except between the
# two commands that switch it on and off.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
IFEOF

echo "hostname $CFG_HOSTNAME" >> "$STAGE/devs/Internet/name_resolution"

# One service, so that the off/on pair has something whose double registration
# would be visible: the responder registers what this file declares the first
# time an interface is enabled, and must not do it again on the second.
cat > "$STAGE/devs/Internet/service_discovery" <<'SDEOF'
# written by tests/tools/run-mdnsctl.sh
_http._tcp    80
SDEOF

cp "$BSD"                          "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface RemoveNetInterface ConfigureNetInterface \
         ShowNetStatus host; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

# Every line is asserted below by its position in this list.  The waits are for
# probing, which is three packets 250 ms apart after a randomised first delay
# (RFC 6762 8.1): without them a name that had not been claimed yet would be
# reported as a name that cannot be claimed.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:ShowNetStatus eth0
SYS:host $LABEL.local TIMEOUT 5
SYS:ConfigureNetInterface eth0 MDNS=YES
wait 4
SYS:ShowNetStatus eth0
SYS:host $LABEL.local TIMEOUT 5
SYS:ConfigureNetInterface eth0 MDNS=NO
wait 3
SYS:ShowNetStatus eth0
SYS:host $LABEL.local TIMEOUT 5
SYS:ConfigureNetInterface eth0 MDNS=YES
wait 4
SYS:ShowNetStatus eth0
SYS:host $LABEL.local TIMEOUT 5
SYS:RemoveNetInterface eth0 FORCE
SYS:AddNetInterface eth0
wait 2
SYS:ShowNetStatus eth0
SYS:host $LABEL.local TIMEOUT 5
EOF

# ------------------------------------------------------- the host watcher ---
#
# Sized against the LOCK QUEUE and not against the run: the emulator runs in
# this tree are serialised, and a watcher that lived for TIMEOUT seconds would
# routinely be dead before the guest booted, and its silence would then be read
# as "SLIRP dropped it".  Same lesson as run-mdns.sh and run-sntp.sh.
#
# It only listens.  --respond is not passed: nothing the host sends can be
# answered under SLIRP anyway (the source port is rewritten and RFC 6762 6
# makes the module drop it), so answering would put packets on somebody's
# network for no measurement.
WATCHLOG="$ROOT/build/mdnsctl-watch.log"
WATCH_PID=""
if python3 "$ROOT/tests/tools/mdnswatch.py" \
       --log "$WATCHLOG" --seconds "$((TIMEOUT + 3600))" --selftest \
       > "$ROOT/build/mdnsctl-watch.out" 2>&1 &
then
    WATCH_PID=$!
fi
cleanup_watch() {
    [ -n "$WATCH_PID" ] && kill -TERM "$WATCH_PID" 2>/dev/null || true
}
trap cleanup_watch EXIT INT TERM HUP
sleep 2
kill -0 "$WATCH_PID" 2>/dev/null || WATCH_PID=""

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-mdnsctl}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

STARTED=$(date +%s)
echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/RemoveNetInterface" \
    "$STAGE/ConfigureNetInterface" "$STAGE/ShowNetStatus" "$STAGE/host"
RUN_RC=$?
set -e
ELAPSED=$(( $(date +%s) - STARTED ))

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
note() { echo "  --: $*"; }

# An instrument that did not work is not a defect in the thing it was pointed
# at.  Its own counter and its own exit status, 3, so "could not measure" is
# never read as "measured and wrong".
BROKEN=0
infra() { echo "INFRA: $*" >&2; BROKEN=$((BROKEN + 1)); }

# A TIMEOUT IS A DEFECT, NOT A RESULT.  A partial transcript makes every
# assertion below read the wrong block, and the first few would still pass.
WANTED=$(grep -c . "$STAGE/commands.txt")
RAN=$(grep -c '^===== ' "$REPORT" || true)
WAITS=$(grep -c '^wait ' "$STAGE/commands.txt" || true)
if [ "$RAN" -lt "$((WANTED - WAITS))" ]; then
    echo "INFRA: the guest ran $RAN of $((WANTED - WAITS)) commands in ${ELAPSED}s" \
         "against a ${TIMEOUT}s ceiling." >&2
    echo "       That command hung.  Raising -t is not the fix; the run above" \
         "measured nothing." >&2
    exit 2
fi

echo "  --: the run took ${ELAPSED}s against a ${TIMEOUT}s ceiling"

# The Nth block for a given command banner: its output and nothing else.
block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}

says() { # banner nth pattern description
    if block "$1" "$2" | grep -Eq -- "$3"; then
        pass "$4"
    else
        fail "$4"
        block "$1" "$2" | sed 's/^/       /' >&2
    fi
}

denies() { # banner nth pattern description
    if block "$1" "$2" | grep -Eq -- "$3"; then
        fail "$4"
        block "$1" "$2" | sed 's/^/       /' >&2
    else
        pass "$4"
    fi
}

STATUS="SYS:ShowNetStatus eth0"
LOOKUP="SYS:host $LABEL.local TIMEOUT 5"
ON="SYS:ConfigureNetInterface eth0 MDNS=YES"
OFF="SYS:ConfigureNetInterface eth0 MDNS=NO"

# ---- one boot ------------------------------------------------------------

ADDS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$ADDS" -eq 2 ]; then
    pass "the machine booted once and added the interface twice, as listed"
else
    fail "AddNetInterface ran $ADDS times, not 2: the machine reset"
fi

# ---- 1: up, and the file never asked ------------------------------------

says   "$STATUS" 1 '^ *mDNS +no$'   "1: with no MDNS= in the file, it reports off"
denies "$LOOKUP" 1 "has address"    "1: and $LABEL.local does not resolve"

# ---- 2: on --------------------------------------------------------------

says "$ON"     1 "answering .local here" "2: MDNS=YES was accepted"
says "$STATUS" 2 '^ *mDNS +yes, answering \.local$' \
                                         "2: and it now reports on"
says "$LOOKUP" 2 "^$LABEL\.local has address " \
                                         "2: and $LABEL.local resolves"

# It has to be the address the interface actually holds, not merely an answer.
says "$LOOKUP" 2 "^$LABEL\.local has address 10\.0\.2\.15" \
                                         "2: with the address eth0 carries"

# ---- 3: off, which is the case a pretending flag cannot pass -------------

says   "$OFF"    1 "no longer answering .local here" "3: MDNS=NO was accepted"
says   "$STATUS" 3 '^ *mDNS +no$'  "3: and it reports off again"
denies "$LOOKUP" 3 "has address"   "3: and $LABEL.local has STOPPED resolving"

# ---- 2b: and back on, so an off/on pair is proved as well as an off ------

says "$ON"     2 "answering .local here" "3b: MDNS=YES again was accepted"
says "$STATUS" 4 '^ *mDNS +yes, answering \.local$' \
                                         "3b: it reports on after an off/on pair"
says "$LOOKUP" 4 "^$LABEL\.local has address 10\.0\.2\.15" \
                                         "3b: and the name resolves again"

# ---- 4: removed and re-added ---------------------------------------------
#
# The interface goes with mDNS on, so this is the flag AND the responder: an
# add that copied the last occupant's answer, or one that set the flag without
# enabling anything, both fail here.

says   "$STATUS" 5 '^ *mDNS +no$' \
    "4: an interface removed and re-added does not claim mDNS it was not asked for"
denies "$LOOKUP" 5 "has address" \
    "4: and $LABEL.local does not resolve after the re-add"

# ---- what another machine saw --------------------------------------------

cleanup_watch
WATCH_PID=""
sleep 1

echo
echo "=============== what the HOST's network heard ======================="
[ -f "$WATCHLOG" ] && cat "$WATCHLOG"
echo "====================================================================="

if [ -z "${WATCHLOG:-}" ] || [ ! -f "$WATCHLOG" ] ||
   grep -q "INSTRUMENT UNAVAILABLE" "$WATCHLOG"; then
    infra "the host watcher could not bind UDP 5353, so nothing off this
       machine was measured"
elif ! grep -q "mdnswatch.local" "$WATCHLOG"; then
    infra "the watcher never saw its own calibration query, so it was not
       listening and its silence means nothing"
else
    HEARD=$(grep -c "$LABEL" "$WATCHLOG" || true)
    ALIVE=$(grep "$LABEL" "$WATCHLOG" | grep -c "ttl=[1-9]" || true)
    BYE=$(grep "$LABEL" "$WATCHLOG" | grep -c "ttl=0\b" || true)

    if [ "$HEARD" -eq 0 ]; then
        infra "SLIRP relayed nothing carrying '$LABEL' onto the host's own
       network, so whether another machine could see this name was not
       measured here.  The guest-side verdict above stands on its own."
    else
        note "SLIRP relays: $HEARD message(s) naming '$LABEL' reached the
       host's real network, which is another machine seeing them"
        if [ "$ALIVE" -gt 0 ]; then
            pass "another machine saw $LABEL.local announced with a live TTL"
        else
            fail "'$LABEL' reached the host LAN but never with a live TTL"
        fi
        if [ "$BYE" -gt 0 ]; then
            pass "and saw the RFC 6762 10.1 goodbye, TTL zero, when it stopped"
        else
            note "no TTL-zero record reached the host LAN; the goodbye is sent
       250 ms after MDNS=NO and the relay may simply not have carried it"
        fi
    fi
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "mdnsctl: FAILED" >&2
    exit 1
fi

if [ "$BROKEN" -ne 0 ]; then
    echo "mdnsctl: PASSED on the guest, $BROKEN host-side measurement(s) not" >&2
    echo "         taken; see INFRA above" >&2
    exit 3
fi

echo "mdnsctl: PASSED"
exit 0
