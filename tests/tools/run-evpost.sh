#!/usr/bin/env bash
# Can a socket event signal a Task that has exited?
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.  Every other board's driver comes out
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

. "$ROOT/tools/test-verdict.sh"

MODEL=A1200
TIMEOUT=0
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
UNFIXED=0

while getopts "m:t:b:N:B:u" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        u) UNFIXED=1 ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board] [-B backend] [-u]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "evpost_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

[ "$TIMEOUT" = 0 ] && TIMEOUT=360

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
PROBE="$ROOT/$BUILD/tests/tools/EvPost"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/NetShutdown" "$PROBE" "$BSD"; do
    [ -f "$f" ] || {
        echo "INFRA: missing $f, build the tree first" >&2
        exit 2
    }
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
    echo "INFRA: no a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

MAC="${AMINETXDUO_EVPOST_MAC:-02:41:4d:49:00:e5}"
HOST="${AMINETXDUO_EVPOST_NAME:-anxdev}"

STAGE="$ROOT/build/evpost-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

mkdir -p "$STAGE/devs/NetInterfaces" "$STAGE/devs/Internet"
cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
IFEOF

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"

printf 'hostname %s\n' "$HOST" > "$STAGE/devs/Internet/name_resolution"

cp "$BSD"                   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/NetShutdown"     "$STAGE/NetShutdown"
cp "$PROBE"                 "$STAGE/EvPost"

cat > "$STAGE/commands.txt" <<EOF
SYS:EvPost
SYS:NetShutdown
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-evpost}"
export AMINETXDUO_AMIBERRY_MAC="$MAC"

if [ "$UNFIXED" = 1 ]; then
    echo "==> UNFIXED-BUILD MODE: this run must report uaf=yes"
fi

set +e
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE, ceiling ${TIMEOUT}s"
echo "==> MAC $MAC, hostname $HOST"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/EvPost" "$STAGE/NetShutdown"
RUN_RC=$?
set -e

HD="$(verdict_hd_amiberry)"
REPORT="$HD/tools.txt"

if [ ! -s "$REPORT" ]; then
    echo "INFRA: the guest wrote no $REPORT (emulator rc=$RUN_RC)." >&2
    if [ "$RUN_RC" = "124" ]; then
        echo "       The run hit its ceiling.  That is a defect in what was" >&2
        echo "       run, not a number to raise: the last 'evpost/' line in" >&2
        echo "       the transcript says how far the probe got." >&2
    fi
    exit 2
fi

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

echo
echo "   what the guest reported:"
grep -E "^evpost/" "$REPORT" | sed 's/^/     /' || true
echo

val() { grep -E "^evpost/$1=" "$REPORT" | tail -1 | sed -E "s/^evpost\/$1=//" | tr -d '\r'; }

STARTS=$(grep -c "^===== SYS:EvPost" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted once (no reset)"
elif [ "$STARTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run never reached EvPost"
fi

CLAIMED=$(val claimed)
if [ "$CLAIMED" = "yes" ]; then
    pass "the freed Task block was reclaimed with AllocAbs, so the read is ours"
else
    fail "AllocAbs could not own the block (claimed=$CLAIMED): nothing measured"
fi

QUIET=$(val quiet)
if [ "$QUIET" = "00000000" ]; then
    pass "the claimed block stays zero while no datagram is sent"
else
    fail "the claimed block read $QUIET with no stimulus: this is noise, not a
       measurement, and the after= reading means nothing"
fi

WOKE=$(val control_woke)
TRIGGER=$(val trigger)
if [ "$WOKE" = "yes" ]; then
    pass "a LIVE client was woken by the same stimulus (trigger=$TRIGGER)"
else
    fail "the live control client was NOT woken (trigger=$TRIGGER).
       Either no datagram arrived, in which case nothing was measured, or a
       sweeper cleared sb_Task on a task that is alive, which is worse than
       the bug it fixes."
fi

CLOSED=$(val control_closed)
[ "$CLOSED" = "yes" ] \
    && pass "the live control closed its socket and library normally" \
    || fail "the live control did not finish (control_closed=$CLOSED)"

MISSED=$(val soak_missed)
ROUNDS=$(val soak_rounds)
SECS=$(val soak_seconds)
WAKES=$(val soak_wakes)
FIRST=$(val soak_first_miss)
if [ "$MISSED" = "0" ]; then
    pass "a WaitSelect() client woke on all $ROUNDS rounds over ${SECS}s ($WAKES wakeups)"
else
    fail "a live WaitSelect() client missed $MISSED of $ROUNDS rounds, first at
       round $FIRST.  If the misses start at one round and never stop, a sweep
       cleared sb_Task on a task that is alive, and that is worse than the
       defect this change is about."
fi

UAF=$(val uaf)
if [ "$UNFIXED" = 1 ]; then
    if [ "$UAF" = "yes" ]; then
        pass "uaf=yes: the demonstration still demonstrates"
    else
        fail "uaf=$UAF on a build that is supposed to have the defect.
       The demonstration has stopped demonstrating, and a default run of this
       script proves nothing."
    fi
else
    if [ "$UAF" = "no" ]; then
        pass "uaf=no: the event found sb_Task already NULL"
    else
        fail "uaf=$UAF: Signal() wrote into a freed struct Task"
    fi
fi

FASTUAF=$(val fast_uaf)
FASTTICKS=$(val fast_ticks)
FASTCLAIM=$(val fast_claimed)
echo "   residual: an orphan stimulated ${FASTTICKS} ticks after its Task died"
echo "             reads fast_uaf=$FASTUAF (claimed=$FASTCLAIM)"
if [ "$UNFIXED" != 1 ] && [ "$FASTUAF" = "no" ] && [ "$FASTCLAIM" = "yes" ]; then
    echo "             -- the sweep got there first this time; the window is"
    echo "                bounded by the sweep period, not closed by it"
fi

TASKS=$(val tasks)
SCAN=$(val scan_us)
if [ -n "$SCAN" ] && [ -n "$TASKS" ]; then
    echo "   cost: $TASKS tasks on Exec's lists, ${SCAN} us per liveness scan"
    echo "         so a sweep of N child bases costs N x ${SCAN} us once a second"
fi

set +e
verdict_guest evpost 12 "$RUN_RC" "$REPORT"
VRC=$?
set -e
[ "$VRC" -eq 0 ] || FAILED=1

if [ "$FAILED" -ne 0 ]; then
    echo "evpost: FAILED" >&2
    exit 1
fi

echo "evpost: PASSED"
exit 0
