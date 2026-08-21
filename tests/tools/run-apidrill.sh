#!/usr/bin/env bash
#
# The whole-vector-surface leak drill.
#
#   tests/tools/run-apidrill.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                               [-N board] [-B backend] [-n]
#
# The commands reach about thirty of bsdsocket.library's entry points.  The
# library has 150, and the ones no command touches are the ones nothing has
# ever watched.  ApiDrill (tests/tools/apidrill.c) calls every one of them in a
# loop and asserts the machine is where it started: AvailMem, NETSTATUS_HEALTH's
# allocations and sockets, and the three things that live on the CALLER rather
# than in the library -- tc_SigAlloc, SysBase->PortList, SysBase->SemaphoreList.
# AmigaOS reclaims none of them when a process exits, so an ambiguity about who
# owns one is a permanent loss until reboot.
#
# The staging is chosen so that every path the drill takes is local and
# immediate:
#
#   * eth0 is STATIC, not DHCP, so no client re-binds an address underneath a
#     measurement and no DHCP exchange is on the clock.
#   * DEVS:Internet/name_resolution names no server, so every resolver refusal
#     (gethostbyname of a name that is not there, getaddrinfo of one, the two
#     _r forms) is answered out of the local files and returns at once.  With a
#     server configured the same rows would be timing a DNS round trip, 64
#     times over, inside a measurement of the library.
#   * hosts/networks/protocols/services come from tests/netstack/devs, so the
#     get*by*() success paths have something to find.
#
# ApiDrill is the only command that runs before the reports, and nothing opens
# the library ahead of it, so the counters it reads are its own.
#
# WHAT IS ASSERTED
#
#   * tools/test-verdict.sh's usual three: a transcript exists, the guest
#     reached its own `N checks, M failures` line, and M is 0 with N above a
#     floor.
#   * the two `!noise` control rows read zero.  They are the identical sampling
#     bracket around no library call at all, at both iteration counts the table
#     uses, and they are what tells "this vector leaks" apart from "this
#     machine cannot be measured on today".
#   * coverage: how many of the 150 rows were actually called.  A row that
#     could not be called says so and is counted as uncovered; it does not
#     become a pass.
#   * the machine booted once.
#
# -n IS THE PROOF THE GATES FIRE
#
#   It runs the guest's BROKEN mode, which replaces the table with six rows
#   that each take a resource and do not give it back -- memory, a signal bit,
#   a socket, a message port, a semaphore -- plus one that asserts the wrong
#   return value for a stub.  Every gate has to go red, and this script has to
#   REJECT the run.  A -n that reports PASSED means the assertions have stopped
#   asserting, which is the state tests/tools/run-addifleak.sh was in for as
#   long as it existed.
#
# ENVIRONMENT
#
#   AMINETXDUO_APIDRILL_ITERS     override the guest's default iteration count
#   AMINETXDUO_APIDRILL_MINCOVER  rows that must have been called (default 145)
#
# BRIDGED, NEVER SLIRP.  -B names the host NIC to bridge onto and the string
# `slirp` is refused outright.  Nothing on the LAN has to answer -- every
# assertion is on what the guest prints -- but a board that came up on NAT is
# not a board, and the counters this drill reads are the library's behaviour on
# a real driver or they are nothing.
#
# -N PICKS THE BOARD, and its driver is staged to match: see sana2_stage below.
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.  Every other board's driver comes out
# of AMINETXDUO_SANA2_STORE or ~/amiga-assets/devs.
#
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
NEGATIVE=0

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
        echo "apidrill_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

# 150 rows, most of them two variants, is 240 measured brackets.  Measured on
# the A1200 profile: the drill itself takes 5.8 s and the whole run, boot
# included, 20 s of host wall clock (14 s for -n).  The ceilings below are
# those measured times with room for a slower host, and they are ceilings, not
# waits.  A run that reaches one has hung, and the row it hung on is the last
# `> N name vK` line in the transcript -- the drill flushes before every call
# for exactly that reason.  Raising a ceiling to get green would only hide it.
if [ "$TIMEOUT" = 0 ]; then
    if [ "$NEGATIVE" = 1 ]; then
        TIMEOUT=120
    else
        TIMEOUT=180
    fi
fi

MINCOVER="${AMINETXDUO_APIDRILL_MINCOVER:-145}"

# ------------------------------------------------------------- fixtures ---
#
# Missing fixtures are an infrastructure failure with their own exit status.
# They are not a product result and must never be reported as one.

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
DRILL="$ROOT/$BUILD/tests/tools/ApiDrill"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/NetShutdown" "$DRILL" "$BSD"; do
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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/apidrill-stage"
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

# -N puts a board in the machine; this puts its driver in DEVS: and its name in
# DEVICE=.  Without it the line above stands whatever -N asked for, so every
# board but the A2065 opens a2065.device against hardware that is not there and
# the run reports a stack failure that is really a staging one.
. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"

# No name server, on purpose.  See the header: it is what makes every resolver
# refusal local and immediate instead of a SLIRP round trip inside a 64-call
# loop.  The file is present and empty rather than absent, so the difference is
# a decision and not a missing fixture.
cat > "$STAGE/devs/Internet/name_resolution" <<'NREOF'
# ApiDrill stages this file with no nameserver line on purpose: every resolver
# failure path in the drill then answers out of DEVS:Internet/hosts and returns
# at once, instead of timing a DNS round trip 64 times over.
NREOF

cp "$BSD"                   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/NetShutdown"     "$STAGE/NetShutdown"
cp "$DRILL"                 "$STAGE/ApiDrill"

DRILLARGS=""
[ -n "${AMINETXDUO_APIDRILL_ITERS:-}" ] && \
    DRILLARGS="ITERS $AMINETXDUO_APIDRILL_ITERS"
[ -n "${AMINETXDUO_APIDRILL_ONLY:-}" ] && \
    DRILLARGS="$DRILLARGS ONLY $AMINETXDUO_APIDRILL_ONLY"
[ "$NEGATIVE" = 1 ] && DRILLARGS="$DRILLARGS BROKEN"

cat > "$STAGE/commands.txt" <<EOF
SYS:ApiDrill $DRILLARGS
SYS:NetShutdown
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-apidrill}"

if [ "$NEGATIVE" = 1 ]; then
    echo "==> NEGATIVE CONTROL: this run must be REJECTED below"
fi

set +e
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE, ceiling ${TIMEOUT}s"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/ApiDrill" "$STAGE/NetShutdown"
RUN_RC=$?
set -e

HD="$(verdict_hd_amiberry)"
REPORT="$HD/tools.txt"

if [ ! -s "$REPORT" ]; then
    echo "INFRA: the guest wrote no $REPORT (emulator rc=$RUN_RC)." >&2
    if [ "$RUN_RC" = "124" ]; then
        echo "       The run hit its ceiling.  That is a defect in what was" >&2
        echo "       run, not a number to raise: the last '> N name vK' line" >&2
        echo "       in the transcript names the vector that hung." >&2
    fi
    exit 2
fi

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

# ---- one boot -------------------------------------------------------------
STARTS=$(grep -c "^===== SYS:ApiDrill" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted once (no reset)"
elif [ "$STARTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run never reached ApiDrill"
fi

# ---- the noise floor ------------------------------------------------------
#
# Read before anything is concluded about a vector.  These two rows run the
# identical sampling bracket around no library call at all, so a non-zero
# reading is the machine and not the library, and every other row's number is
# then worth exactly nothing.
NOISE_BAD=0
for row in '!noise64' '!noise16'; do
    LINE=$(grep -E "^V -[0-9]+ $row " "$REPORT" | tail -1 || true)
    if [ -z "$LINE" ]; then
        fail "the drill printed no $row control row, so nothing established
       the noise floor and no other row's byte count can be read"
        NOISE_BAD=1
        continue
    fi
    echo "   , $LINE"
    B=$(printf '%s' "$LINE" | sed -E 's/.* bytes=(-?[0-9]+) .*/\1/')
    if [ "$B" = 0 ]; then
        pass "$row reads 0 bytes per call"
    else
        fail "$row reads $B bytes per call: this machine cannot be measured on"
        NOISE_BAD=1
    fi
done

# ---- the guest's own verdict ---------------------------------------------
set +e
# 240 brackets at up to ten gates each is 2205 checks in the normal run; the
# floor is what notices a drill that quietly stopped calling rows.  The
# negative control runs six rows on purpose and must not be held to it -- it is
# rejected by the broken-row gates at the bottom instead.
if [ "$NEGATIVE" = 1 ]; then MINCHECKS=1; else MINCHECKS=2000; fi
verdict_guest apidrill "$MINCHECKS" "$RUN_RC" "$REPORT"
VRC=$?
set -e
[ "$VRC" -eq 0 ] || FAILED=1

# ---- coverage -------------------------------------------------------------
#
# The half that a green transcript cannot show.  A drill that stopped calling
# rows still reports zero failures.
COVER=$(grep -E "^covered: [0-9]+ of [0-9]+ vectors" "$REPORT" | tail -1 || true)
if [ -z "$COVER" ]; then
    fail "the drill did not report its coverage"
else
    echo "   , $COVER"
    C_DONE=$(printf '%s' "$COVER" | sed -E 's/^covered: ([0-9]+) .*/\1/')
    C_ALL=$(printf  '%s' "$COVER" | sed -E 's/^covered: [0-9]+ of ([0-9]+) .*/\1/')
    C_UNC=$(printf  '%s' "$COVER" | sed -E 's/.*, ([0-9]+) uncovered.*/\1/')
    C_VAR=$(printf  '%s' "$COVER" | sed -E 's/.*, ([0-9]+) variants run/\1/')

    [ "$C_ALL" -eq 150 ] \
        && pass "the table has all 150 LVOs in it" \
        || fail "the table has $C_ALL rows, the library has 150"

    if [ "$NEGATIVE" != 1 ]; then
        [ "$C_DONE" -ge "$MINCOVER" ] \
            && pass "$C_DONE of $C_ALL vectors called ($C_UNC uncovered)" \
            || fail "only $C_DONE of $C_ALL vectors were called, wanted $MINCOVER"

        [ "$C_VAR" -ge "$((MINCOVER + 60))" ] \
            && pass "$C_VAR variants run" \
            || fail "only $C_VAR variants run: rows are being skipped at run time"
    fi

    echo
    echo "   vectors NOT called:"
    grep -E "^V [0-9]+ .*(UNCOVERED|NOTAPPLICABLE)" "$REPORT" \
        | sed 's/^/     /' || echo "     (none)"
    echo
fi

# ---- the rows that moved --------------------------------------------------
echo "   rows with a non-zero reading:"
awk '/^V [0-9-]+ / {
        z = 1
        for (i = 1; i <= NF; i++) {
            split($i, kv, "=")
            if (kv[1] == "bytes" || kv[1] == "alloc" || kv[1] == "sock" ||
                kv[1] == "sig" || kv[1] == "port" || kv[1] == "sem" ||
                kv[1] == "task") {
                if (kv[2] + 0 != 0) z = 0
            }
        }
        if (!z) print "     " $0
     }' "$REPORT" || true
echo

# ---- the negative control -------------------------------------------------
if [ "$NEGATIVE" = 1 ]; then
    BROKEN_ROWS=$(grep -cE "^V -1[0-9][0-9] !broken-" "$REPORT" || true)
    BROKEN_RED=$(grep -cE "^= -1[0-9][0-9] !broken-[a-z]+ LEAK" "$REPORT" || true)

    echo "   broken rows run: $BROKEN_ROWS, red: $BROKEN_RED"
    if [ "$BROKEN_ROWS" -lt 6 ]; then
        echo "apidrill: NEGATIVE CONTROL FAILED, only $BROKEN_ROWS broken rows ran" >&2
        exit 1
    fi
    if [ "$BROKEN_RED" -lt 6 ]; then
        echo "apidrill: NEGATIVE CONTROL FAILED, only $BROKEN_RED of $BROKEN_ROWS" >&2
        echo "          deliberately broken rows were caught; the gates are not" >&2
        echo "          gating and a green normal run proves nothing" >&2
        exit 1
    fi
    if [ "$FAILED" -ne 0 ]; then
        echo "apidrill: negative control PASSED, all $BROKEN_RED broken rows were caught and the run was rejected"
        exit 0
    fi
    echo "apidrill: NEGATIVE CONTROL FAILED, a run of deliberately broken rows was accepted" >&2
    exit 1
fi

if [ "$NOISE_BAD" -ne 0 ]; then
    echo "apidrill: INFRASTRUCTURE FAILURE, the noise floor is not zero" >&2
    exit 2
fi

if [ "$FAILED" -ne 0 ]; then
    echo "apidrill: FAILED" >&2
    exit 1
fi

echo "apidrill: PASSED"
exit 0
