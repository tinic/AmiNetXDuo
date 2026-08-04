#!/usr/bin/env bash
#
# TWO OPENERS RACING FOR ONE ROW OF THE MULTICAST TABLE.
#
#   tests/tools/run-mcastrace.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                [-A [-N board] [-B backend]] [-n]
#
# WHAT THIS IS FOR
#
#   bsd_mcast_table[16] (src/bsdsocket/mcast.c) is one table for the machine
#   and a row is free precisely because bm_Sock is NULL.  bsd_mcast_join()
#   picks a free row inside the bsd_nx_enter() bracket; marking it used only
#   after bsd_nx_leave() lets a second base pick the same row in that window.
#   The loser's membership then exists in NetX Duo and nowhere else.
#
#   One process cannot reach this.  A base belongs to one Task and serialises
#   itself on its own nesting counter, so the two joins have to come from two
#   Processes with a base each, which is what McastRace is.
#
# THE ASSERTION
#
#   A join that returned 0 can be dropped.  That is the only thing the loser of
#   the race can observe, and it is enough: its row was overwritten, so
#   IP_DROP_MEMBERSHIP cannot find the membership it is holding.
#
#   Receiving a datagram is NOT the assertion.  NetX Duo refcounts membership
#   per NX_IP, so two joins for one group leave the count at 2 and one spurious
#   leave still leaves the group live for the other opener.  What the defect
#   destroys is the library's own socket-to-group mapping.
#
# WHY THE COUNT GATES ARE HALF THE FILE
#
#   The window is a handful of instructions.  A run that raced nothing finds
#   nothing wrong and would otherwise pass, so the guest reports how much
#   racing it did and this script demands a floor for both sides.  `-n` is the
#   control for exactly that: it asks the guest for one round and one shot,
#   leaves the gates where they were, and requires this script to REJECT the
#   run.  If -n ever reports PASSED the gates have stopped gating.
#
#   That control tests the gates.  The control for the DEFECT is to put it
#   back, move the three stores in bsd_mcast_join() below bsd_nx_leave() --
#   and see this script fail.  Measured on Amiberry/A1200, 200 shots:
#
#     row claimed inside the bracket   200 joined, 200 dropped,  0 stolen
#     stores moved back below it       200 joined, 137 dropped, 63 stolen
#
#   Every one of those 63 is errno 49, EADDRNOTAVAIL, on a group the sniper was
#   holding.  A longer run of the broken build stops finishing at all: at 400
#   shots the guest never reached its summary and the harness timed out.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# ENVIRONMENT
#
#   AMINETXDUO_RACE_ROUNDS   hammer round ceiling  (default 60000)
#   AMINETXDUO_RACE_SHOTS    sniper rounds         (default 200)
#   AMINETXDUO_RACE_NAP      ticks before a shot   (default 1)
#   AMINETXDUO_RACE_HOLD     ticks a shot is held  (default 1)
#   AMINETXDUO_RACE_MIN_HAMMER  hammer joins the gate demands (default 1500)
#   AMINETXDUO_RACE_MIN_SNIPER  sniper joins the gate demands (default 150)
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=0
BUILD="${AMINETXDUO_BUILD:-build/cm}"
# FS-UAE needs an X server; on a headless Linux box it dies in GLAD before the
# guest boots, so -A picks Amiberry, which runs genuinely headless.
RUNNER="${AMINETXDUO_RUNNER:-fsuae}"
BOARD=a2065
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"
NEGATIVE=0

ROUNDS="${AMINETXDUO_RACE_ROUNDS:-60000}"
SHOTS="${AMINETXDUO_RACE_SHOTS:-200}"
NAP="${AMINETXDUO_RACE_NAP:-1}"
HOLD="${AMINETXDUO_RACE_HOLD:-1}"
MIN_HAMMER="${AMINETXDUO_RACE_MIN_HAMMER:-1500}"
MIN_SNIPER="${AMINETXDUO_RACE_MIN_SNIPER:-150}"

# What the guest is asked for.  The same numbers in every run but the negative
# control, which is the point of there being two sets.
GUEST_ROUNDS=""
GUEST_SHOTS=""

while getopts "m:t:b:AN:B:n" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        n) NEGATIVE=1 ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-A [-N board] [-B backend]] [-n]" >&2; exit 2 ;;
    esac
done

if [ "$NEGATIVE" = 1 ]; then
    # One round and one shot, and the gates below are NOT lowered to match.
    # Lowering them would make the run self-consistent again and prove
    # nothing: what is tested here is that a run which passes every one of the
    # guest's own checks while racing almost nothing is still rejected.
    GUEST_ROUNDS=1
    GUEST_SHOTS=1
    echo "==> NEGATIVE CONTROL: this run must be REJECTED by the gates below"
fi

GUEST_ROUNDS="${GUEST_ROUNDS:-$ROUNDS}"
GUEST_SHOTS="${GUEST_SHOTS:-$SHOTS}"

# The sniper sleeps NAP+HOLD ticks per shot and the hammer runs until it stops,
# so the guest's own floor is SHOTS*(NAP+HOLD)/50 seconds.  The rest is boot.
if [ "$TIMEOUT" = 0 ]; then
    TIMEOUT=$(( 180 + GUEST_SHOTS * (NAP + HOLD) / 50 ))
fi

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
RACE="$ROOT/$BUILD/tests/tools/McastRace"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/netstat" \
         "$RACE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/mcastrace-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# STATIC, not DHCP: the run wants the interface up and unchanging for its whole
# length, and a lease renewal in the middle would reconfigure the interface the
# memberships are on.  10.0.2.15 and 10.0.2.2 are SLIRP's own numbers.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
IFEOF

cp "$BSD"                   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/netstat"         "$STAGE/netstat"
cp "$TOOLS/NetShutdown"     "$STAGE/NetShutdown"
cp "$RACE"                  "$STAGE/McastRace"

# AddNetInterface first and resident: IP_ADD_MEMBERSHIP with INADDR_ANY takes
# the first interface whose link is UP, so a run that started before the
# interface did would fail every join with EADDRNOTAVAIL and prove nothing.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:McastRace ROUNDS $GUEST_ROUNDS SHOTS $GUEST_SHOTS NAP $NAP HOLD $HOLD
SYS:netstat -h
SYS:NetShutdown
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-mcastrace}"

set +e
if [ "$RUNNER" = "amiberry" ]; then
    HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/McastRace" "$STAGE/netstat" \
        "$STAGE/NetShutdown"
else
    HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/McastRace" "$STAGE/netstat" \
        "$STAGE/NetShutdown"
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

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

# ---- one boot (docs/RESEARCH.md 25) --------------------------------------
#
# ToolsSmoke reopens the transcript from the top after a reset, so the first
# command appearing twice is a reboot and not a hang.  Two Processes writing
# one table with no lock is exactly the shape that gurus rather than misreports,
# so this is checked before anything the guest said about itself.
STARTS=$(grep -c "^===== SYS:AddNetInterface " "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted once (no reset)"
elif [ "$STARTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run never reached AddNetInterface"
fi

# ---- the guest's own checks ----------------------------------------------
SUMMARY=$(grep -E "^McastRace: [0-9]+ check\(s\)" "$REPORT" | tail -1 || true)
if [ -z "$SUMMARY" ]; then
    fail "McastRace printed no summary -- it did not finish"
else
    echo "  -- $SUMMARY"
    NFAILED=$(printf '%s' "$SUMMARY" | sed -E 's/.* ([0-9]+) failed$/\1/')

    if [ "$NFAILED" -eq 0 ]; then
        pass "no membership was lost to the other base"
    else
        fail "$NFAILED check(s) failed"
        grep -n "^FAIL: " "$REPORT" | sed 's/^/       /' >&2
    fi
fi

# ---- and it really raced --------------------------------------------------
#
# The half that matters.  Everything above passes on a run that joined once.
DID=$(grep -E "^did: [0-9]+ hammer joins" "$REPORT" | tail -1 || true)
if [ -z "$DID" ]; then
    fail "McastRace did not report what it did"
else
    echo "  -- $DID"
    D_HAM=$(printf '%s' "$DID" | sed -E 's/^did: ([0-9]+) hammer joins.*/\1/')
    D_SNI=$(printf '%s' "$DID" | sed -E 's/.*, ([0-9]+) sniper joins$/\1/')

    [ "$D_HAM" -ge "$MIN_HAMMER" ] \
        && pass "$D_HAM hammer joins (wanted $MIN_HAMMER)" \
        || fail "only $D_HAM hammer joins, wanted $MIN_HAMMER, the window was barely opened, so nothing here tested the race"

    [ "$D_SNI" -ge "$MIN_SNIPER" ] \
        && pass "$D_SNI sniper joins (wanted $MIN_SNIPER)" \
        || fail "only $D_SNI sniper joins, wanted $MIN_SNIPER, the second base barely ran, so nothing here tested the race"
fi

# ---- the second base really was a second base -----------------------------
if grep -q "^FAIL: the sniper could not open a second base" "$REPORT"; then
    fail "the child never got a base of its own -- one base cannot race itself"
else
    pass "the child ran on a bsdsocket.library base of its own"
fi

echo
if [ "$NEGATIVE" = 1 ]; then
    # Inverted: the run above raced almost nothing, so the gates had to reject
    # it.  A PASS here would mean they are not gating anything.
    if [ "$FAILED" -ne 0 ]; then
        echo "mcastrace: negative control PASSED -- the gates reject a run that did not race"
        exit 0
    fi
    echo "mcastrace: NEGATIVE CONTROL FAILED -- a run with one round and one shot was accepted" >&2
    exit 1
fi

if [ "$FAILED" -ne 0 ]; then
    echo "mcastrace: FAILED" >&2
    exit 1
fi

echo "mcastrace: PASSED"
exit 0
