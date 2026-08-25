#!/usr/bin/env bash
# TWO OPENERS RACING FOR ONE ROW OF THE MULTICAST TABLE.
# BRIDGED, NEVER SLIRP.  -B names the host NIC to bridge onto and the string
# `slirp` is refused outright.  Nothing on the LAN has to answer -- the race is
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.  Every other board's driver comes out
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=0
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
NEGATIVE=0

ROUNDS="${AMINETXDUO_RACE_ROUNDS:-60000}"
SHOTS="${AMINETXDUO_RACE_SHOTS:-200}"
NAP="${AMINETXDUO_RACE_NAP:-1}"
HOLD="${AMINETXDUO_RACE_HOLD:-1}"
MIN_HAMMER="${AMINETXDUO_RACE_MIN_HAMMER:-1500}"
MIN_SNIPER="${AMINETXDUO_RACE_MIN_SNIPER:-150}"

GUEST_ROUNDS=""
GUEST_SHOTS=""

while getopts "m:t:b:N:B:n" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        n) NEGATIVE=1 ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board] [-B backend] [-n]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "mcastrace_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

if [ "$NEGATIVE" = 1 ]; then
    GUEST_ROUNDS=1
    GUEST_SHOTS=1
    echo "==> NEGATIVE CONTROL: this run must be REJECTED by the gates below"
fi

GUEST_ROUNDS="${GUEST_ROUNDS:-$ROUNDS}"
GUEST_SHOTS="${GUEST_SHOTS:-$SHOTS}"

if [ "$TIMEOUT" = 0 ]; then
    TIMEOUT=$(( 180 + GUEST_SHOTS * (NAP + HOLD) / 50 ))
fi

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
RACE="$ROOT/$BUILD/tests/tools/McastRace"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/netstat" \
         "$RACE" "$BSD"; do
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

STAGE="$ROOT/build/mcastrace-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
IFEOF

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"

cp "$BSD"                   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/netstat"         "$STAGE/netstat"
cp "$TOOLS/NetShutdown"     "$STAGE/NetShutdown"
cp "$RACE"                  "$STAGE/McastRace"

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:McastRace ROUNDS $GUEST_ROUNDS SHOTS $GUEST_SHOTS NAP $NAP HOLD $HOLD
SYS:netstat -h
SYS:NetShutdown
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-mcastrace}"

set +e
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/McastRace" "$STAGE/netstat" \
    "$STAGE/NetShutdown"
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

STARTS=$(grep -c "^===== SYS:AddNetInterface " "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted once (no reset)"
elif [ "$STARTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run never reached AddNetInterface"
fi

SUMMARY=$(grep -E "^McastRace: [0-9]+ check\(s\)" "$REPORT" | tail -1 || true)
if [ -z "$SUMMARY" ]; then
    fail "McastRace printed no summary, it did not finish"
else
    echo " , $SUMMARY"
    NFAILED=$(printf '%s' "$SUMMARY" | sed -E 's/.* ([0-9]+) failed$/\1/')

    if [ "$NFAILED" -eq 0 ]; then
        pass "no membership was lost to the other base"
    else
        fail "$NFAILED check(s) failed"
        grep -n "^FAIL: " "$REPORT" | sed 's/^/       /' >&2
    fi
fi

DID=$(grep -E "^did: [0-9]+ hammer joins" "$REPORT" | tail -1 || true)
if [ -z "$DID" ]; then
    fail "McastRace did not report what it did"
else
    echo " , $DID"
    D_HAM=$(printf '%s' "$DID" | sed -E 's/^did: ([0-9]+) hammer joins.*/\1/')
    D_SNI=$(printf '%s' "$DID" | sed -E 's/.*, ([0-9]+) sniper joins$/\1/')

    [ "$D_HAM" -ge "$MIN_HAMMER" ] \
        && pass "$D_HAM hammer joins (wanted $MIN_HAMMER)" \
        || fail "only $D_HAM hammer joins, wanted $MIN_HAMMER, the window was barely opened, so nothing here tested the race"

    [ "$D_SNI" -ge "$MIN_SNIPER" ] \
        && pass "$D_SNI sniper joins (wanted $MIN_SNIPER)" \
        || fail "only $D_SNI sniper joins, wanted $MIN_SNIPER, the second base barely ran, so nothing here tested the race"
fi

if grep -q "^FAIL: the sniper could not open a second base" "$REPORT"; then
    fail "the child never got a base of its own, one base cannot race itself"
else
    pass "the child ran on a bsdsocket.library base of its own"
fi

echo
if [ "$NEGATIVE" = 1 ]; then
    if [ "$FAILED" -ne 0 ]; then
        echo "mcastrace: negative control PASSED, the gates reject a run that did not race"
        exit 0
    fi
    echo "mcastrace: NEGATIVE CONTROL FAILED, a run with one round and one shot was accepted" >&2
    exit 1
fi

if [ "$FAILED" -ne 0 ]; then
    echo "mcastrace: FAILED" >&2
    exit 1
fi

echo "mcastrace: PASSED"
exit 0
