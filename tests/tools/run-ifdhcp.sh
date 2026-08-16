#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR ConfigureNetInterface'S DHCP HALF.
#
#   tests/tools/run-ifdhcp.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                             [-A [-N board] [-B backend]]
#
# WHAT IT IS FOR
#
#   Before CONFIGURE and RELEASE, the only way to make a lease happen again was
#   Offline followed by Online, or NetShutdown, both of which restart every
#   interface on the machine.  There was no way at all to give a lease back.
#
#   Three claims, and each one is checked from OUTSIDE the command:
#
#     RELEASE drops the lease        ShowNetStatus stops printing the "lease
#                                    from" line, and a second RELEASE is
#                                    refused because there is nothing left to
#                                    release.  The refusal is the strong half:
#                                    it is a different invocation reading the
#                                    state the first one left.
#
#     CONFIGURE=DHCP takes one       an interface with no lease ends up
#                                    addressed, leased and able to ping.
#
#     CONFIGURE=DHCP renews one      on an interface that is already bound this
#                                    is RFC 2131 4.4.5's renewing state entered
#                                    early, and the command waits for NetX
#                                    Duo's raw state to come back to BOUND.
#                                    That only happens on a DHCPACK, so rc 0
#                                    inside the timeout is the server's answer
#                                    and not this machine's opinion -- and the
#                                    address must be the same one afterwards,
#                                    because a renewal that moved the address
#                                    is an allocation wearing the wrong name.
#
#   Every CONFIGURE=DHCP below carries an explicit TIMEOUT so that a server
#   which never answers costs a known number of seconds rather than the
#   60-second default four times over.  The ceiling at the top is sized from
#   that, and from the measured good case.
#
# THE ARGUMENT SURFACE, from the same template:
#
#   CONFIGURE/K                 DHCP, and nothing else -- AUTO is refused by
#                               name because Roadshow has it and this does not
#   RELEASE=RELEASEADDRESS/S    both spellings, as Roadshow has them
#   TIMEOUT/K/N                 rejected under ten seconds, and rejected
#                               without CONFIGURE, because there is nothing
#                               else here to wait for
#   NETMASK, GATEWAY            refused WITH CONFIGURE=DHCP: the server decides
#                               them, so accepting would be accepting something
#                               that is then written over
#
# WHY eth0 IS DHCP HERE, where tests/tools/run-ifconfigure.sh makes it STATIC:
# this is the half that has no meaning without a server, and SLIRP has one.
# That harness needs an interface with no second writer and this one needs the
# writer, so they are two files rather than two sections of one.
#
# ONE BOOT, and the count of adds says so.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
# THE GOOD CASE IS 21 SECONDS, measured on playhouse3 (Amiberry 68020 A1200 on
# SLIRP): boot, one add, three pings, four DHCP exchanges and twenty-four
# commands.  The ceiling is a boot of its own plus twice the measured work,
# which is what tests/tools/run-toolleak.sh uses and for the same reason: an
# emulator that costs more than double what it has been seen to cost is hung,
# and the ceiling is what says so rather than something to raise.  The run
# prints what it really took.
#
# A DHCP server that stops answering does NOT reach this ceiling: every
# CONFIGURE=DHCP below carries TIMEOUT 20, so it fails with a verdict instead
# of running out of wall clock, which is the difference between a result and
# an infrastructure fault.
TIMEOUT=105
BUILD="${AMINETXDUO_BUILD:-build/cm}"
RUNNER=slirp
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

while getopts "m:t:b:AN:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        N) RUNNER=amiberry; BOARD="$OPTARG" ;;
        B) RUNNER=amiberry; IFACE="$OPTARG" ;;
        *) sed -n '3,10p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" \
         "$TOOLS/ConfigureNetInterface" "$TOOLS/ShowNetStatus" \
         "$TOOLS/netstat" "$TOOLS/ping" "$BSD"; do
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

STAGE="$ROOT/build/ifdhcp-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# DHCP, which is the whole point here.  SLIRP hands out 10.0.2.15 from
# 10.0.2.2, so both ends of every exchange below are known numbers.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
IFEOF

if [ "$RUNNER" = amiberry ]; then
    . "$ROOT/tools/sana2-stage.sh"

    if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
        _want=$(sana2_driver_for "$BOARD")
        _have=$(sana2_local_driver "$_want")
        [ -n "$_have" ] && [ -f "$_have" ] &&
            export AMINETXDUO_SANA2_DRIVER="$_have"
    fi

    sana2_stage "$BOARD" "$STAGE/devs"
    echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"
fi

cp "$BSD"                         "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface"       "$STAGE/AddNetInterface"
cp "$TOOLS/ConfigureNetInterface" "$STAGE/ConfigureNetInterface"
cp "$TOOLS/ShowNetStatus"         "$STAGE/ShowNetStatus"
cp "$TOOLS/netstat"               "$STAGE/netstat"
cp "$TOOLS/ping"                  "$STAGE/ping"

cat > "$STAGE/commands.txt" <<'EOF'
SYS:ConfigureNetInterface eth0 RELEASE
SYS:AddNetInterface eth0
SYS:ping 10.0.2.2 -c 2 -t 20
SYS:netstat -i
SYS:ShowNetStatus eth0
SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20
SYS:netstat -i
SYS:ShowNetStatus eth0
SYS:ping 10.0.2.2 -c 2 -t 20
SYS:ConfigureNetInterface eth0 RELEASE
SYS:ShowNetStatus eth0
SYS:ConfigureNetInterface eth0 RELEASE
SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20
SYS:netstat -i
SYS:ShowNetStatus eth0
SYS:ping 10.0.2.2 -c 2 -t 20
SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP NETMASK 255.255.255.0
SYS:ConfigureNetInterface eth0 CONFIGURE=AUTO
SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 3
SYS:ConfigureNetInterface eth0 TIMEOUT 30
SYS:ConfigureNetInterface nosuch0 RELEASE
SYS:ShowNetStatus eth0
SYS:ConfigureNetInterface eth0 QUIET RELEASEADDRESS
SYS:ShowNetStatus eth0
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ifdhcp}"

STARTED=$(date +%s)
set +e
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
if [ "$RUNNER" = "amiberry" ]; then
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/ConfigureNetInterface" \
        "$STAGE/ShowNetStatus" "$STAGE/netstat" "$STAGE/ping"
else
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/ConfigureNetInterface" \
        "$STAGE/ShowNetStatus" "$STAGE/netstat" "$STAGE/ping"
fi
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

# A TIMEOUT IS A DEFECT, NOT A RESULT.  A partial transcript makes every
# assertion below read the wrong block, and the early ones would still pass.
WANTED=$(grep -c . "$STAGE/commands.txt")
RAN=$(grep -c '^===== ' "$REPORT" || true)
if [ "$RAN" -lt "$WANTED" ]; then
    STUCK=$(sed -n "$((RAN + 1))p" "$STAGE/commands.txt")
    echo "INFRA: the guest ran $RAN of $WANTED commands in ${ELAPSED}s against a" \
         "${TIMEOUT}s ceiling." >&2
    echo "       It stopped at: ${STUCK:-<past the end of the list>}" >&2
    echo "       That command hung.  Raising -t is not the fix; the run above" \
         "measured nothing." >&2
    exit 2
fi

block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}

rc_of() { block "$1" "$2" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'; }

want_rc() { # banner nth expected description
    local got; got=$(rc_of "$1" "$2")
    if [ "$got" = "$3" ]; then pass "$4 (rc $got)"
    else fail "$4: expected rc $3, got '${got:-nothing}'"
         block "$1" "$2" | sed 's/^/       /' >&2
    fi
}

says() { # banner nth pattern description
    if block "$1" "$2" | grep -Eq -- "$3"; then
        pass "$4"
    else
        fail "$4"
        block "$1" "$2" | sed 's/^/       /' >&2
    fi
}

says_not() { # banner nth pattern description
    if block "$1" "$2" | grep -Eq -- "$3"; then
        fail "$4"
        block "$1" "$2" | sed 's/^/       /' >&2
    else
        pass "$4"
    fi
}

ifaces() { block "SYS:netstat -i" "$1"; }
status() { block "SYS:ShowNetStatus eth0" "$1"; }

# The address on eth0 out of the Nth `netstat -i`.
address() { ifaces "$1" | awk '$1 == "eth0" { print $3; exit }'; }

pinged() { # nth description
    local out
    out=$(block "SYS:ping 10.0.2.2 -c 2 -t 20" "$1")
    if printf '%s\n' "$out" | grep -q '2 received' &&
       [ "$(rc_of "SYS:ping 10.0.2.2 -c 2 -t 20" "$1")" = "0" ]; then
        pass "$2"
    else
        fail "$2: no reply came back over eth0"
        printf '%s\n' "$out" | sed 's/^/       /' >&2
    fi
}

# ---- one boot ------------------------------------------------------------
ADDS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$ADDS" -eq 1 ]; then
    pass "the machine booted once and ran the whole list"
elif [ "$ADDS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run never got as far as bringing eth0 up, something hung"
fi

# ---- 1. nothing to release, because nothing is up ------------------------
says "SYS:ConfigureNetInterface eth0 RELEASE" 1 "The network is not running" \
     "releasing before any add says the network is not running"
want_rc "SYS:ConfigureNetInterface eth0 RELEASE" 1 5 "and returns WARN"

# ---- 2. the lease it booted with -----------------------------------------
#
# The control.  If SLIRP does not hand out a lease, nothing below is about
# this command.
pinged 1 "eth0 came up by DHCP and the gateway answers over it"
LEASED=$(address 1)
if [ "$LEASED" = "10.0.2.15" ]; then
    pass "the DHCP server gave it 10.0.2.15"
else
    fail "eth0 is on '${LEASED:-nothing}', not the 10.0.2.15 SLIRP hands out"
    ifaces 1 | sed 's/^/       /' >&2
fi
says "SYS:ShowNetStatus eth0" 1 "lease +from 10\.0\.2\.2" \
     "and ShowNetStatus says which server it came from"

# ---- 3. CONFIGURE=DHCP on a bound interface is a RENEWAL -----------------
#
# rc 0 means the command watched NetX Duo's raw state leave RENEWING and come
# back to BOUND, which happens on a DHCPACK and on nothing else.  A server that
# never answered would leave it in RENEWING until the timeout.
want_rc "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20" 1 0 \
        "CONFIGURE=DHCP on a bound interface is answered inside the timeout"
says "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20" 1 \
     "lease renewed" \
     "and says it renewed rather than allocated, which is what it did"
RENEWED=$(address 2)
if [ "$RENEWED" = "$LEASED" ]; then
    pass "and the address did not move ($RENEWED): a renewal keeps it"
else
    fail "the renewal changed the address ($LEASED -> ${RENEWED:-nothing}), which"\
         "makes it an allocation under the wrong name"
    ifaces 2 | sed 's/^/       /' >&2
fi
says "SYS:ShowNetStatus eth0" 2 "lease +from 10\.0\.2\.2" \
     "and the interface still holds a lease afterwards"
pinged 2 "and still carries traffic"

# ---- 4. RELEASE gives it back --------------------------------------------
want_rc "SYS:ConfigureNetInterface eth0 RELEASE" 2 0 "RELEASE is accepted"
says "SYS:ConfigureNetInterface eth0 RELEASE" 2 "the lease is released" \
     "and says so"

# The observable half, read by a different command: ShowNetStatus prints a
# "lease from" line only for an interface that holds one.
says_not "SYS:ShowNetStatus eth0" 3 "lease +from" \
         "and ShowNetStatus no longer reports a lease on eth0"

# The other observable half, read by a second invocation of the command
# itself: there is nothing left to release, and it says which.
says "SYS:ConfigureNetInterface eth0 RELEASE" 3 "has no lease to release" \
     "a second RELEASE is refused because the first one really dropped it"
want_rc "SYS:ConfigureNetInterface eth0 RELEASE" 3 20 "and returns FAIL"

# ---- 5. CONFIGURE=DHCP on an interface with no lease is an ALLOCATION ----
want_rc "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20" 2 0 \
        "CONFIGURE=DHCP after a release takes a lease again"
says "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20" 2 \
     "lease taken" \
     "and says it allocated rather than renewed, which is what it did"
RETAKEN=$(address 3)
if [ "$RETAKEN" = "10.0.2.15" ]; then
    pass "and the interface is addressed again ($RETAKEN)"
else
    fail "eth0 is on '${RETAKEN:-nothing}' after re-acquiring"
    ifaces 3 | sed 's/^/       /' >&2
fi
says "SYS:ShowNetStatus eth0" 4 "lease +from 10\.0\.2\.2" \
     "and ShowNetStatus reports a lease once more"
pinged 3 "and the gateway answers over the re-acquired lease"

# ---- 6. the argument lines that cannot be acted on -----------------------
says "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP NETMASK 255.255.255.0" 1 \
     "takes its netmask and gateway from the server" \
     "NETMASK with CONFIGURE=DHCP is refused, and says why"
want_rc "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP NETMASK 255.255.255.0" 1 10 \
        "and returns ERROR"

says "SYS:ConfigureNetInterface eth0 CONFIGURE=AUTO" 1 \
     "CONFIGURE takes DHCP and nothing else" \
     "CONFIGURE=AUTO is refused by name"
want_rc "SYS:ConfigureNetInterface eth0 CONFIGURE=AUTO" 1 10 "and returns ERROR"

says "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 3" 1 \
     "is too short to tell anything about the network" \
     "a TIMEOUT of less than ten seconds is refused"
want_rc "SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 3" 1 10 \
        "and returns ERROR"

says "SYS:ConfigureNetInterface eth0 TIMEOUT 30" 1 \
     "needs CONFIGURE=DHCP" \
     "a TIMEOUT with nothing to wait for is refused"
want_rc "SYS:ConfigureNetInterface eth0 TIMEOUT 30" 1 10 "and returns ERROR"

says "SYS:ConfigureNetInterface nosuch0 RELEASE" 1 \
     'there is no interface called "nosuch0"' \
     "RELEASE on a name that is not there is reported by name"
want_rc "SYS:ConfigureNetInterface nosuch0 RELEASE" 1 20 "and returns FAIL"

# ...and the interface is still leased after all five refusals.
says "SYS:ShowNetStatus eth0" 5 "lease +from 10\.0\.2\.2" \
     "and none of the five refusals touched the lease"

# ---- 7. RELEASEADDRESS is the same switch, and QUIET is honoured ---------
QOUT=$(block "SYS:ConfigureNetInterface eth0 QUIET RELEASEADDRESS" 1 |
       grep -v '^----- rc ' || true)
if [ -z "$(printf '%s' "$QOUT" | tr -d '[:space:]')" ]; then
    pass "QUIET printed nothing"
else
    fail "QUIET printed something"
    printf '%s\n' "$QOUT" | sed 's/^/       /' >&2
fi
want_rc "SYS:ConfigureNetInterface eth0 QUIET RELEASEADDRESS" 1 0 \
        "and RELEASEADDRESS was accepted"

# QUIET printed nothing, so the only evidence that RELEASEADDRESS is the same
# switch as RELEASE is what it left behind.
says_not "SYS:ShowNetStatus eth0" 6 "lease +from" \
         "and the lease is gone: RELEASEADDRESS is the same switch as RELEASE"

echo
echo "==> the whole run took ${ELAPSED}s against a ${TIMEOUT}s ceiling"
if [ "$FAILED" -eq 0 ]; then
    echo "PASS: ConfigureNetInterface's DHCP half, on one boot"
    exit 0
fi
echo "the transcript above is the whole run" >&2
exit 1
