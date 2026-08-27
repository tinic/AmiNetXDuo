#!/usr/bin/env bash
#
# THE OPEN/EXPUNGE/REOPEN AND ONLINE/OFFLINE DRILL.
#
#   tests/tools/run-cycledrill.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                 [-N board] [-B backend] [-n]
#
# THE HALF THIS CANNOT REACH.
#
# The open report behind this drill is a guru after NetShutdown, and it came
# off a 3c589 -- a card no emulator models, so nothing here can boot one.
# Three cycles on a2065/A1200 match the task lists name for name and this is
# green; that is a fact about the a2065.  The 3c589 half is
# tests/tools/run-hwcard.sh's `shutdown` arm, which drives the real machine
# through its own httpd and asks ICMP and HTTP separately afterwards, because
# a machine that stopped and a stack that did not come back look the same
# from one probe.  There is no -A here: this harness builds a drive and boots
# it, and the machine that has the card is already switched on.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# shellcheck source=../../tools/serial-log.sh
. "$ROOT/tools/serial-log.sh"

MODEL=A1200
TIMEOUT=0
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD=a2065
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
NEGATIVE=0

CYCLES="${AMINETXDUO_CYCLE_CYCLES:-3}"
EXPUNGES="${AMINETXDUO_CYCLE_EXPUNGES:-2}"
SOCKETS="${AMINETXDUO_CYCLE_SOCKETS:-2}"
GUEST_CYCLES=""
GUEST_EXPUNGES=""

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

if [ "$NEGATIVE" = 1 ]; then
    GUEST_CYCLES=1
    GUEST_EXPUNGES=0
    echo "==> NEGATIVE CONTROL: this run must be REJECTED by the gates below"
fi

GUEST_CYCLES="${GUEST_CYCLES:-$CYCLES}"
GUEST_EXPUNGES="${GUEST_EXPUNGES:-$EXPUNGES}"

if [ "$TIMEOUT" = 0 ]; then
    TIMEOUT=$(( 180 + GUEST_CYCLES * 20 + GUEST_EXPUNGES * 60 ))
fi

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
DRILL="$ROOT/$BUILD/tests/tools/CycleDrill"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/netstat" "$TOOLS/ShowNetStatus" \
         "$TOOLS/NetShutdown" "$DRILL" "$BSD"; do
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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/cycledrill-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

CYCLE_DRIVER="${AMINETXDUO_CYCLE_DRIVER:-a2065.device}"
CYCLE_CARD="${AMINETXDUO_CYCLE_CARD:-}"
if [ "$CYCLE_DRIVER" != a2065.device ]; then
    cp "${AMINETXDUO_CYCLE_DRIVER_PATH:?set AMINETXDUO_CYCLE_DRIVER_PATH}" \
       "$STAGE/devs/$CYCLE_DRIVER"
fi

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=$CYCLE_DRIVER
${CYCLE_CARD:+CARD=$CYCLE_CARD}
UNIT=0
CONFIGURE=STATIC
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
IFEOF

cp "$BSD"                   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/netstat"         "$STAGE/netstat"
cp "$TOOLS/ShowNetStatus"   "$STAGE/ShowNetStatus"
cp "$TOOLS/NetShutdown"     "$STAGE/NetShutdown"
cp "$DRILL"                 "$STAGE/CycleDrill"

cat > "$STAGE/commands.txt" <<EOF
SYS:CycleDrill CYCLES $GUEST_CYCLES EXPUNGE $GUEST_EXPUNGES SOCKETS $SOCKETS
SYS:netstat -h
SYS:ShowNetStatus MEMORY
SYS:NetShutdown
SYS:CycleDrill REPORT
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-cycledrill}"

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
SERIAL=$(serial_log_path "$AMINETXDUO_RUN_TAG")

set +e
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/CycleDrill" "$STAGE/netstat" "$STAGE/ShowNetStatus" \
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

# ---- one boot (docs/RESEARCH.md 25) --------------------------------------
STARTS=$(grep -c "^===== SYS:CycleDrill CYCLES " "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted once (no reset)"
elif [ "$STARTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run never reached CycleDrill"
fi

# ---- the guest's own checks ----------------------------------------------
SUMMARY=$(grep -E "^CycleDrill: [0-9]+ check\(s\)" "$REPORT" | tail -1 || true)
if [ -z "$SUMMARY" ]; then
    fail "CycleDrill printed no summary, it did not finish"
else
    echo " , $SUMMARY"
    NCHECKS=$(printf '%s' "$SUMMARY" | sed -E 's/^CycleDrill: ([0-9]+) check.*/\1/')
    NFAILED=$(printf '%s' "$SUMMARY" | sed -E 's/.* ([0-9]+) failed$/\1/')

    if [ "$NFAILED" -eq 0 ]; then
        pass "every check in the guest passed ($NCHECKS of them)"
    else
        fail "$NFAILED of $NCHECKS guest checks failed"
        grep -n "^FAIL: " "$REPORT" | sed 's/^/       /' >&2
    fi
fi

# ---- and it really cycled -------------------------------------------------
DID=$(grep -E "^did: [0-9]+ opens" "$REPORT" | tail -1 || true)
if [ -z "$DID" ]; then
    fail "CycleDrill did not report what it did"
else
    echo " , $DID"
    D_OPENS=$(printf  '%s' "$DID" | sed -E 's/^did: ([0-9]+) opens.*/\1/')
    D_BOUNCE=$(printf '%s' "$DID" | sed -E 's/.*, ([0-9]+) bounces.*/\1/')
    D_TRIPS=$(printf  '%s' "$DID" | sed -E 's/.*, ([0-9]+) round trips.*/\1/')
    D_EXP=$(printf    '%s' "$DID" | sed -E 's/.*, ([0-9]+) expunges$/\1/')

    WANT_OPENS=$(( CYCLES * 4 + EXPUNGES * 2 + 1 ))
    WANT_BOUNCE=$(( CYCLES * 2 ))

    [ "$D_OPENS" -ge "$WANT_OPENS" ] \
        && pass "$D_OPENS library opens (wanted $WANT_OPENS)" \
        || fail "only $D_OPENS library opens, wanted $WANT_OPENS"

    [ "$D_BOUNCE" -ge "$WANT_BOUNCE" ] \
        && pass "$D_BOUNCE interface bounces (wanted $WANT_BOUNCE)" \
        || fail "only $D_BOUNCE interface bounces, wanted $WANT_BOUNCE"

    [ "$D_TRIPS" -ge "$CYCLES" ] \
        && pass "$D_TRIPS interface remove/re-add round trips (wanted $CYCLES)" \
        || fail "only $D_TRIPS interface round trips, wanted $CYCLES"

    if [ "$EXPUNGES" -gt 0 ]; then
        [ "$D_EXP" -ge "$EXPUNGES" ] \
            && pass "$D_EXP expunges completed (wanted $EXPUNGES)" \
            || fail "only $D_EXP expunges completed, wanted $EXPUNGES, the library was never unloaded, so nothing here tested the expunge path"
    else
        fail "EXPUNGES is 0: this run did not test the expunge path at all"
    fi
fi

# ---- the reports agree with the drill -------------------------------------
if grep -q "cannot read it" "$REPORT"; then
    fail "netstat/ShowNetStatus could not read the stack the drill left up"
else
    pass "netstat -h and ShowNetStatus read the stack the drill left running"
fi

# ---- memory not given back by a cycle -------------------------------------
LEAK_BUDGET="${AMINETXDUO_CYCLE_LEAK_BUDGET:-1024}"
for phase in expunge cold; do
    LEAKLINE=$(grep -E "^$phase leak: -?[0-9]+ bytes per cycle" "$REPORT" \
               | tail -1 || true)
    if [ -n "$LEAKLINE" ]; then
        LEAK=$(printf '%s' "$LEAKLINE" | sed -E 's/^[a-z]+ leak: (-?[0-9]+) .*/\1/')
        echo " , $LEAKLINE (budget $LEAK_BUDGET)"
        if [ "$LEAK" -le "$LEAK_BUDGET" ]; then
            pass "a $phase cycle gave its memory back"
        else
            fail "a $phase cycle lost $LEAK bytes, over the $LEAK_BUDGET budget"
        fi
    else
        fail "the drill printed no per-cycle $phase leak figure (the guest ran
       $GUEST_EXPUNGES expunge(s)), so nothing measured whether a $phase cycle
       gives its memory back"
    fi
done

# ---- what the shutdown left standing --------------------------------------
TCPLEFT=$(sed -n 's/^tcp: //p' "$REPORT" | tail -1)
LIBLEFT=$(sed -n 's/^library: //p' "$REPORT" | tail -1)

if [ -z "$TCPLEFT" ]; then
    fail "the report after NetShutdown did not run, so nothing was checked"
elif [ "$TCPLEFT" = absent ]; then
    pass "NetShutdown took TCP: down ($TCPLEFT)"
else
    fail "TCP: is still $TCPLEFT after NetShutdown, so no expunge can ever
       succeed on this machine again"
fi

[ -n "$LIBLEFT" ] && echo " , bsdsocket.library after NetShutdown: $LIBLEFT"

# ---- the known SANA-II reader leak ----------------------------------------
ORPHANS=0
if serial_log_have "${SERIAL:-}" "$BUILD" \
                   "orphaned SANA-II reader stacks"; then
    ORPHANS=$(grep -c "did not stop. Its stack leaks" "$SERIAL" || true)
    echo " , orphaned SANA-II reader stacks logged: $ORPHANS"
else
    ORPHANS=-1
    fail "orphaned SANA-II reader stacks were NOT CHECKED: the serial log is
       empty, so the assertion had no input.  32 KB a time goes unnoticed
       exactly here"
fi
if [ "$ORPHANS" -gt 0 ]; then
    echo "     (src/sana2/sana2_rx.c's last-resort path: a driver ignored AbortIO()."
    echo "      32 KB leaked per occurrence.)"
    if [ "${AMINETXDUO_CYCLE_ORPHAN_FATAL:-1}" = 1 ]; then
        fail "$ORPHANS SANA-II reader stack(s) orphaned, 32 KB each, never freed"
    fi
fi

echo
if [ "$NEGATIVE" = 1 ]; then
    if [ "$FAILED" -ne 0 ]; then
        echo "cycledrill: negative control PASSED, the gates reject a run that did not cycle"
        exit 0
    fi
    echo "cycledrill: NEGATIVE CONTROL FAILED, a run with no expunge and one cycle was accepted" >&2
    exit 1
fi

if [ "$FAILED" -ne 0 ]; then
    echo "cycledrill: FAILED" >&2
    exit 1
fi

echo "cycledrill: PASSED"
exit 0
