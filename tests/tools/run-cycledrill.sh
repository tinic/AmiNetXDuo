#!/usr/bin/env bash
#
# THE OPEN/EXPUNGE/REOPEN AND ONLINE/OFFLINE DRILL.
#
#   tests/tools/run-cycledrill.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                 [-A [-N board] [-B backend]] [-n]
#
# WHAT THIS IS FOR
#
#   tests/soak/ and tests/endurance/ hold the stack in one state and hammer it.
#   That is not what kills a long-lived Amiga stack.  What kills it is cycling:
#   interfaces bounced Online and Offline, and the library opened, closed,
#   expunged and opened again.  There is no MMU, so an unclean expunge is
#   leaked signals and memory at best and a crash on the next open at worst,
#   and none of it shows on the first cycle.
#
#   CycleDrill (tests/tools/cycledrill.c) is the guest half.  It reads
#   NETSTATUS_HEALTH between cycles -- allocations outstanding, sockets alive,
#   packet buffers free, and the high-water mark of each, the same numbers
#   `netstat -h` and `ShowNetStatus MEMORY` print -- rather than inventing
#   instrumentation, and prints the trend rather than a pass/fail on one
#   reading.
#
# WHY THE DRILL RUNS BEFORE AddNetInterface AND NOT AFTER
#
#   bsd_lib_expunge() only proceeds at lib_OpenCnt 0, and AddNetInterface
#   deliberately leaks an OpenLibrary() reference to keep the network up.  Once
#   that command has run, no expunge on this machine can ever succeed again, so
#   a drill placed after it would print "declined" every cycle and pass.
#   CycleDrill is therefore the FIRST command in the list, brings the stack up
#   itself -- the first open starts it, interfaces and all -- and leaves one
#   reference held at the end so the `netstat -h` and `ShowNetStatus MEMORY`
#   that follow report on the stack it just cycled and not on a fresh one.
#
# WHAT IS ASSERTED HERE, AND WHY THE COUNTS MATTER MOST
#
#   A drill that stopped cycling would report no drift and look like a pass.
#   So the assertions are in two halves:
#
#     * the guest's own checks all passed (`0 failed`), and
#     * the guest really did the work: at least CYCLES*2 interface bounces,
#       CYCLES interface round trips, and EXPUNGES completed expunges.
#
#   `-n` is the negative control for exactly that.  It asks the drill for
#   CYCLES 1 EXPUNGE 0 while leaving the gates at the values they would have
#   had -- a run in which every guest check still passes, and all four count
#   gates have to fire -- and requires this script to REJECT it.  If -n ever
#   reports PASSED, the gates have stopped gating and the normal run proves
#   nothing.
#
# WHAT IT FOUND
#
#   An open/expunge/reopen cycle used to lose 12,612 bytes, dead linear over
#   eight cycles.  It was the DEVS:Internet netdb: ami_netdb_load() fills four
#   ami_alloc()ed tables held in file-scope statics, the statics went away with
#   the segment, and bsd_lib_expunge() never freed them.  The phase L control
#   is what named it -- a close/reopen cycle, same teardown, segment left
#   loaded, lost nothing -- and the gate below now reads zero on both phases.
#
# THE KNOWN DEFECT THIS CAN SEE
#
#   src/sana2/sana2_rx.c has a last-resort path that logs
#   `reader N did not stop; leaking its stack` and leaks 32 KB when a SANA-II
#   driver ignores AbortIO().  Commodore's a2065.device 2.16 genuinely does.
#   Every expunge cycle passes through that teardown, so the serial log is
#   grepped for it and the count is always printed.  It is NOT fatal by
#   default: it is a known driver behaviour, not a regression, and wiring the
#   normal suite red for it would only teach people to ignore this test.
#   AMINETXDUO_CYCLE_ORPHAN_FATAL=1 makes it fail, for a run that is
#   specifically about that path.
#
# ENVIRONMENT
#
#   AMINETXDUO_CYCLE_CYCLES     cycles       (default 3)
#   AMINETXDUO_CYCLE_EXPUNGES   expunges     (default 2)
#   AMINETXDUO_CYCLE_SOCKETS    sockets held across each bounce (default 2)
#   AMINETXDUO_CYCLE_ORPHAN_FATAL=1  a leaked reader stack fails the run
#   AMINETXDUO_CYCLE_LEAK_BUDGET     bytes a cycle may lose (1024)
#
#   The defaults keep this inside the normal suite.  A soak is
#   AMINETXDUO_CYCLE_CYCLES=50 AMINETXDUO_CYCLE_EXPUNGES=10, the way
#   AMINETXDUO_FUZZ_CASES raises the fuzzers.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
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

# CYCLES and EXPUNGES are what the gates at the bottom demand; GUEST_* is what
# the drill is actually asked for.  They are the same number in every run but
# the negative control, which is the point of there being two.
CYCLES="${AMINETXDUO_CYCLE_CYCLES:-3}"
EXPUNGES="${AMINETXDUO_CYCLE_EXPUNGES:-2}"
SOCKETS="${AMINETXDUO_CYCLE_SOCKETS:-2}"
GUEST_CYCLES=""
GUEST_EXPUNGES=""

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
    # The guest is asked for less than the gates below demand, and the gates
    # are NOT lowered to match.  Lowering them would make the run
    # self-consistent again and prove nothing: what is being tested is that a
    # drill which passes every one of its own checks while doing almost no
    # cycling still gets rejected, and all four count gates have to fire.
    GUEST_CYCLES=1
    GUEST_EXPUNGES=0
    echo "==> NEGATIVE CONTROL: this run must be REJECTED by the gates below"
fi

GUEST_CYCLES="${GUEST_CYCLES:-$CYCLES}"
GUEST_EXPUNGES="${GUEST_EXPUNGES:-$EXPUNGES}"

# Every cycle is a bounce, a socket round trip and an interface round trip;
# every expunge is a full stack teardown and bring-up, which is the expensive
# half.  These are ceilings, not waits: a passing run finishes well inside.
if [ "$TIMEOUT" = 0 ]; then
    TIMEOUT=$(( 180 + GUEST_CYCLES * 20 + GUEST_EXPUNGES * 60 ))
fi

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
DRILL="$ROOT/$BUILD/tests/tools/CycleDrill"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/netstat" "$TOOLS/ShowNetStatus" \
         "$TOOLS/NetShutdown" "$DRILL" "$BSD"; do
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

STAGE="$ROOT/build/cycledrill-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# STATIC, not DHCP, for the same reason run-ifreadd.sh is static: a DHCP
# interface has a client that may re-bind and write an address back, so a
# transcript showing one proves nothing about what the cycle did.  It also
# takes the DHCP exchange off the clock of every expunge cycle, which is what
# makes EXPUNGES=10 affordable.  10.0.2.15 and 10.0.2.2 are SLIRP's own
# numbers, taken statically rather than leased.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
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

# CycleDrill first, and nothing that opens bsdsocket.library before it: an
# expunge needs lib_OpenCnt 0, and any earlier command would have taken it off
# zero.  The two reports after it read the stack the drill left running.
cat > "$STAGE/commands.txt" <<EOF
SYS:CycleDrill CYCLES $GUEST_CYCLES EXPUNGE $GUEST_EXPUNGES SOCKETS $SOCKETS
SYS:netstat -h
SYS:ShowNetStatus MEMORY
SYS:NetShutdown
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-cycledrill}"

set +e
if [ "$RUNNER" = "amiberry" ]; then
    HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
    SERIAL="$ROOT/build/amiberry-serial-$AMINETXDUO_RUN_TAG.log"
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/CycleDrill" "$STAGE/netstat" "$STAGE/ShowNetStatus" \
        "$STAGE/NetShutdown"
else
    HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"
    SERIAL=""
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/CycleDrill" "$STAGE/netstat" "$STAGE/ShowNetStatus" \
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
# command appearing twice is a reboot and not a hang.  An expunge that took the
# machine with it lands here first.
STARTS=$(grep -c "^===== SYS:CycleDrill " "$REPORT" || true)
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
    fail "CycleDrill printed no summary -- it did not finish"
else
    echo "  -- $SUMMARY"
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
#
# The half that matters.  Everything above passes on a drill that opened the
# library once and stopped.
DID=$(grep -E "^did: [0-9]+ opens" "$REPORT" | tail -1 || true)
if [ -z "$DID" ]; then
    fail "CycleDrill did not report what it did"
else
    echo "  -- $DID"
    D_OPENS=$(printf  '%s' "$DID" | sed -E 's/^did: ([0-9]+) opens.*/\1/')
    D_BOUNCE=$(printf '%s' "$DID" | sed -E 's/.*, ([0-9]+) bounces.*/\1/')
    D_TRIPS=$(printf  '%s' "$DID" | sed -E 's/.*, ([0-9]+) round trips.*/\1/')
    D_EXP=$(printf    '%s' "$DID" | sed -E 's/.*, ([0-9]+) expunges$/\1/')

    # Two bounces per cycle: the standalone one and the one the sockets are
    # held across.
    # Phase E and phase L each open EXPUNGES times.
    # The 4 is cycledrill.c's NEST_OPENS and has to track it: a base costs the
    # calling Task signal bits, so the drill cannot nest more than a handful.
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

    # The one that cannot be waved through: an expunge that never happened is
    # the whole subject of this file not being tested.
    if [ "$EXPUNGES" -gt 0 ]; then
        [ "$D_EXP" -ge "$EXPUNGES" ] \
            && pass "$D_EXP expunges completed (wanted $EXPUNGES)" \
            || fail "only $D_EXP expunges completed, wanted $EXPUNGES -- the library was never unloaded, so nothing here tested the expunge path"
    else
        fail "EXPUNGES is 0: this run did not test the expunge path at all"
    fi
fi

# ---- the reports agree with the drill -------------------------------------
#
# `netstat -h` reads the stack CycleDrill left running.  It is here because a
# held reference that is not actually holding anything would print the
# unavailable message instead, and the drill itself could not tell.
if grep -q "cannot read it" "$REPORT"; then
    fail "netstat/ShowNetStatus could not read the stack the drill left up"
else
    pass "netstat -h and ShowNetStatus read the stack the drill left running"
fi

# ---- memory not given back by a cycle -------------------------------------
#
# The drill reports AvailMem() at the same instant of the first and the last
# cycle of phase E and of phase L, divided by the cycles between them, so
# anything paid once cancels and what is left is per-cycle.  Both read zero:
# an expunge/reopen cycle and a close/reopen cycle each give back everything
# they took.  Three runs of eight cycles spread -6 to +3 bytes, which is
# allocator fragmentation and not a trend, so the budget is 1 KB.
LEAK_BUDGET="${AMINETXDUO_CYCLE_LEAK_BUDGET:-1024}"
for phase in expunge cold; do
    LEAKLINE=$(grep -E "^$phase leak: -?[0-9]+ bytes per cycle" "$REPORT" \
               | tail -1 || true)
    if [ -n "$LEAKLINE" ]; then
        LEAK=$(printf '%s' "$LEAKLINE" | sed -E 's/^[a-z]+ leak: (-?[0-9]+) .*/\1/')
        echo "  -- $LEAKLINE (budget $LEAK_BUDGET)"
        if [ "$LEAK" -le "$LEAK_BUDGET" ]; then
            pass "a $phase cycle gave its memory back"
        else
            fail "a $phase cycle lost $LEAK bytes, over the $LEAK_BUDGET budget"
        fi
    elif [ "$GUEST_EXPUNGES" -ge 2 ]; then
        fail "the drill did not report a per-cycle $phase leak figure"
    fi
done

# ---- the known SANA-II reader leak ----------------------------------------
#
# Only Amiberry keeps a serial log here, and ami_log() has no other sink, so
# under FS-UAE this cannot be looked for at all.  Saying so beats printing a
# zero that means "not checked".
ORPHANS=0
if [ -z "${SERIAL:-}" ] || [ ! -f "$SERIAL" ]; then
    echo "  -- orphaned SANA-II reader stacks: NOT CHECKED (no serial log; use -A)"
    ORPHANS=-1
else
    ORPHANS=$(grep -c "did not stop; leaking its stack" "$SERIAL" || true)
    echo "  -- orphaned SANA-II reader stacks logged: $ORPHANS"
fi
if [ "$ORPHANS" -gt 0 ]; then
    echo "     (src/sana2/sana2_rx.c's last-resort path: a driver ignored AbortIO()."
    echo "      32 KB leaked per occurrence.  Known, not a regression.)"
    if [ "${AMINETXDUO_CYCLE_ORPHAN_FATAL:-0}" = 1 ]; then
        fail "a SANA-II reader was orphaned and ORPHAN_FATAL is set"
    fi
fi

echo
if [ "$NEGATIVE" = 1 ]; then
    # Inverted: the run above asked for one cycle and no expunge, so the gates
    # had to reject it.  A PASS here would mean they are not gating anything.
    if [ "$FAILED" -ne 0 ]; then
        echo "cycledrill: negative control PASSED -- the gates reject a run that did not cycle"
        exit 0
    fi
    echo "cycledrill: NEGATIVE CONTROL FAILED -- a run with no expunge and one cycle was accepted" >&2
    exit 1
fi

if [ "$FAILED" -ne 0 ]; then
    echo "cycledrill: FAILED" >&2
    exit 1
fi

echo "cycledrill: PASSED"
exit 0
