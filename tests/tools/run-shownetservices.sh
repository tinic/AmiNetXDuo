#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR ShowNetServices.
#
#   tests/tools/run-shownetservices.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                      [-A [-N board] [-B backend]]
#                                      [-r TRANSCRIPT]
#
# The mDNS browse, and nothing exercised it.  It is the one command in the set
# that blocks on purpose -- it listens for a window and then prints what
# arrived -- so the thing worth testing about it is not only what it says but
# how long it takes to say it.  A browse whose window is not honoured is
# either a command that returns before anything can answer or one that sits
# there, and both read as "it printed nothing" from the outside.
#
# What the template offers, and so what is checked here:
#
#   TYPE        an RFC 6763 4.1.2 service type, and the refusal of the two
#               things a user types instead ("http", "_http")
#   ALL/S       every type, and its refusal to take a TYPE as well
#   SECONDS/K/N the window, its range, and that it is what the command waits
#   TXT/S       accepted
#   QUIET/S     the progress lines suppressed, the answer still printed
#
# plus the exit codes: OK, WARN when nothing answered, ERROR on an argument
# that cannot be obeyed and on a machine with no stack running, FAIL when the
# stack has no mDNS at all.
#
# THE WINDOW IS THE ASSERTION.  ToolsSmoke records how long each command took,
# so SECONDS=1 and the default 3 are compared against the clock and against
# each other: a command that ignores SECONDS shows up as two equal times, and
# one that never waits shows up as a time under its own window.  That is the
# bounded-blocking case tested rather than avoided.
#
# THE ARGUMENT ERRORS RUN BEFORE THE STACK IS UP, deliberately.  The header
# says the command never starts the stack; every refusal below therefore has
# to be the argument being refused and not the network being missing, and each
# is asserted to say the one and not the other.
#
# GOOD CASE: 30 s wall on playhouse3 under SLIRP, measured, boot to verdict, of
# which 13 s is the command under test waiting out four windows on purpose.
# The default -t 90 is 3x the guest's share.  A run that reaches the timeout is
# a defect to diagnose, not a number to raise: the first assertion below names
# the command that was still running when the emulator was killed.
#
# NOT COVERED HERE:
#
#   * anything answering.  SLIRP has no peers, so every browse below comes back
#     empty, and what is asserted is the empty answer's wording, its WARN and
#     its timing.  A found service needs a responder on the same wire: -A -B
#     <interface> puts this on the host's LAN where Avahi and Bonjour answer,
#     but what they offer is not the same from one machine to the next, so
#     there is nothing there this file could assert.  tests/tools/run-mdns.sh
#     is the test with a peer, from the other end -- it watches this machine
#     answer somebody else's query.
#   * ALL's second window, for the same reason: it only opens when the first
#     window turned up a type, so on an empty network ALL is one window and
#     the branch that browses every type found is never entered.
#   * the machine whose stack has mDNS built in but disabled on every
#     interface (RETURN_WARN, "Add MDNS=YES ... then restart the stack").
#     MDNS is read out of the interface file at startup, so it cannot be both
#     on and off in one boot, and the enabled half is what the rest of this
#     file needs.
#   * TXT's output.  It prints the TXT records of an instance, and there are
#     no instances here; it is asserted only to be accepted.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=90
BUILD="${AMINETXDUO_BUILD:-build/cm}"
RUNNER=slirp
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
REPLAY=""

while getopts "m:t:b:AN:B:r:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        N) RUNNER=amiberry; BOARD="$OPTARG" ;;
        B) RUNNER=amiberry; IFACE="$OPTARG" ;;
        # Assert a transcript that already exists instead of booting.  For
        # developing the assertions below, and for showing that they fail on a
        # transcript broken on purpose.
        r) REPLAY="$OPTARG" ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-shownetservices}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"
RUN_RC=0

if [ -n "$REPLAY" ]; then
    [ -f "$REPLAY" ] || { echo "no such transcript: $REPLAY" >&2; exit 2; }
    REPORT="$REPLAY"
    echo "==> REPLAY of $REPORT: nothing was run, this only checks the checks"
else

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/ShowNetServices" \
         "$BSD"; do
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

STAGE="$ROOT/build/shownetservices-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# MDNS=YES, which is what puts NETSTATUS_IF_MDNS on the interface and is the
# whole precondition of this command: without it every browse below stops at
# "mDNS is not enabled on any interface" and asserts nothing about browsing.
# Static, so the bring-up costs no lease and the windows below are the only
# thing this test waits for.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
MDNS=YES
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

cp "$BSD" "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface ShowNetServices; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

# The six refusals come first, with no stack running, because that is where
# they prove the most: a command that validated its arguments after opening
# the stack would answer all six with "the network is not started".
cat > "$STAGE/commands.txt" <<'EOF'
SYS:ShowNetServices http
SYS:ShowNetServices _http
SYS:ShowNetServices _http._tcp ALL
SYS:ShowNetServices SECONDS=0
SYS:ShowNetServices SECONDS=61
SYS:ShowNetServices
SYS:AddNetInterface eth0
SYS:ShowNetServices
SYS:ShowNetServices SECONDS=1
SYS:ShowNetServices QUIET
SYS:ShowNetServices _http._tcp TXT
SYS:ShowNetServices ALL
EOF

# ------------------------------------------------------------------ run ---

set +e
if [ "$RUNNER" = "amiberry" ]; then
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/ShowNetServices"
else
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/ShowNetServices"
fi
RUN_RC=$?
set -e

fi  # not a replay

if [ ! -f "$REPORT" ]; then
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    [ "$RUN_RC" = 124 ] &&
        echo "       rc 124 is the ${TIMEOUT}s timeout: the machine never" \
             "got as far as writing one." >&2
    exit 1
fi

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}

rc_of() { block "$1" "$2" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'; }
ms_of() { block "$1" "$2" | sed -n 's/^----- rc [0-9-]*, \([0-9]*\) ms.*/\1/p'; }
show()  { block "$1" "$2" | sed 's/^/       /' >&2; }

want_rc() { # banner nth expected description
    local got; got=$(rc_of "$1" "$2")
    if [ "$got" = "$3" ]; then pass "$4 (rc $got)"
    else fail "$4: expected rc $3, got '${got:-nothing}'"; show "$1" "$2"
    fi
}

says() { # banner nth extended-regexp description
    if block "$1" "$2" | grep -Eq "$3"; then pass "$4"
    else fail "$4"; show "$1" "$2"
    fi
}

silent_about() { # banner nth extended-regexp description
    if block "$1" "$2" | grep -Eq "$3"; then fail "$4"; show "$1" "$2"
    else pass "$4"
    fi
}

# The window, in milliseconds, and what it must be: at least the seconds it was
# given, and not so much more that it is waiting on something else.  ToolsSmoke
# times each command from the outside, so this is the whole command, the browse
# and the two library calls around it.
waited() { # banner nth seconds description
    local ms; ms=$(ms_of "$1" "$2")
    local lo=$(( $3 * 1000 ))
    local hi=$(( $3 * 1000 + 4000 ))

    if [ -z "$ms" ]; then
        fail "$4: the command left no timing at all"
    elif [ "$ms" -lt "$lo" ]; then
        fail "$4: it came back in ${ms} ms, less than the ${3} s window it" \
             "was asked to listen for"
    elif [ "$ms" -gt "$hi" ]; then
        fail "$4: it took ${ms} ms for a ${3} s window, so it is waiting on" \
             "something other than the window"
    else
        pass "$4 (${ms} ms)"
    fi
}

# ---- the run finished, and finished once --------------------------------
EXPECTED_BLOCKS=12
BLOCKS=$(grep -c '^===== SYS:' "$REPORT" || true)
if ! grep -q '^===== done, ' "$REPORT"; then
    LAST=$(grep '^===== SYS:' "$REPORT" | tail -1 | sed 's/^===== //; s/ =====$//')
    fail "THE RUN DID NOT FINISH: it stopped in '${LAST:-nothing ran}'," \
         "block $BLOCKS of $EXPECTED_BLOCKS (emulator rc=$RUN_RC," \
         "timeout ${TIMEOUT}s).  That command did not return."
elif [ "$BLOCKS" -eq "$EXPECTED_BLOCKS" ]; then
    pass "the machine booted once and ran all $EXPECTED_BLOCKS commands"
else
    fail "expected $EXPECTED_BLOCKS command blocks, found $BLOCKS:" \
         "the machine rebooted, or the list was truncated"
fi

# ---- 1. a type that is not one ------------------------------------------
#
# The two spellings a user reaches for, and neither is a service type: RFC
# 6763 4.1.2 wants the underscore and the transport both.
says "SYS:ShowNetServices http" 1 \
     '^ShowNetServices: "http" is not a service type$' \
     "a bare service name is refused, quoting it"
want_rc "SYS:ShowNetServices http" 1 10 "and returns ERROR"
silent_about "SYS:ShowNetServices http" 1 "not (running|started)" \
     "and it is the type being refused, not the stack being down"

says "SYS:ShowNetServices _http" 1 \
     '^ShowNetServices: "_http" is not a service type$' \
     "an underscore without a transport is refused too"
want_rc "SYS:ShowNetServices _http" 1 10 "and returns ERROR"

# ---- 2. ALL takes no type ------------------------------------------------
says "SYS:ShowNetServices _http._tcp ALL" 1 \
     "^ShowNetServices: ALL browses every type, so it takes no type$" \
     "ALL with a type is refused, and says why"
want_rc "SYS:ShowNetServices _http._tcp ALL" 1 10 "and returns ERROR"

# ---- 3. SECONDS has a range, and both ends of it are guarded ------------
for n in 1 2; do
    case "$n" in
        1) B="SYS:ShowNetServices SECONDS=0";  W="below" ;;
        2) B="SYS:ShowNetServices SECONDS=61"; W="above" ;;
    esac
    says "$B" 1 "^ShowNetServices: SECONDS must be between 1 and 60$" \
         "a window $W the range is refused, with the range in the message"
    want_rc "$B" 1 10 "and returns ERROR"
done

# ---- 4. and none of that needed a stack, but browsing does --------------
#
# Same command line as the live one below; the only difference is that nothing
# has started the network yet.
says "SYS:ShowNetServices" 1 "not (running|started)" \
     "with no stack, a browse says the network is not running"
want_rc "SYS:ShowNetServices" 1 10 "and returns ERROR"
silent_about "SYS:ShowNetServices" 1 "^Asking " \
     "and it never opened a window to listen in"

# ---- 5. the machine this test needs -------------------------------------
want_rc "SYS:AddNetInterface eth0" 1 0 "eth0 came up, with MDNS=YES"

# ---- 6. the meta-query: what kinds of service are here ------------------
#
# RFC 6763 9.  Nothing answers on SLIRP, which is the point of the wording
# being asserted: a browse that found nothing has to say so as a browse that
# found nothing, not as an error.
says "SYS:ShowNetServices" 2 \
     "^Asking what this network offers, listening 3 seconds\.\.\.$" \
     "a browse with no type asks what the network offers, and for how long"
says "SYS:ShowNetServices" 2 "^Nothing answered in 3 seconds\.$" \
     "and says that nothing answered, in the same number of seconds"
want_rc "SYS:ShowNetServices" 2 5 "returning WARN, not an error"
waited "SYS:ShowNetServices" 2 3 "it listened for its full three seconds"

# ---- 7. SECONDS is the window, not decoration ---------------------------
says "SYS:ShowNetServices SECONDS=1" 1 \
     "^Asking what this network offers, listening 1 second\.\.\.$" \
     "SECONDS=1 is echoed as one second, singular"
says "SYS:ShowNetServices SECONDS=1" 1 "^Nothing answered in 1 second\.$" \
     "and the answer counts the same second"
want_rc "SYS:ShowNetServices SECONDS=1" 1 5 "returning WARN"
waited "SYS:ShowNetServices SECONDS=1" 1 1 "and it waited one second"

# The two together: a command that ignored SECONDS would take the same time
# twice, and neither timing assertion above would catch it on its own.
SHORT=$(ms_of "SYS:ShowNetServices SECONDS=1" 1)
LONG=$(ms_of "SYS:ShowNetServices" 2)
if [ -n "$SHORT" ] && [ -n "$LONG" ] && [ $(( LONG - SHORT )) -ge 1500 ]; then
    pass "and the 3 s browse took ${LONG} ms against the 1 s browse's ${SHORT}"
else
    fail "the 1 s and 3 s browses took ${SHORT:-?} ms and ${LONG:-?} ms:" \
         "SECONDS is not what the command waits for"
fi

# ---- 8. QUIET drops the progress, not the answer ------------------------
silent_about "SYS:ShowNetServices QUIET" 1 "^Asking " \
     "QUIET says nothing about what it is doing"
says "SYS:ShowNetServices QUIET" 1 "^Nothing answered in 3 seconds\.$" \
     "but still answers the question that was asked"
want_rc "SYS:ShowNetServices QUIET" 1 5 "returning WARN"

# ---- 9. a type, and TXT alongside it ------------------------------------
says "SYS:ShowNetServices _http._tcp TXT" 1 \
     "^Asking for _http\._tcp, listening 3 seconds\.\.\.$" \
     "a well-formed type is asked for by name"
says "SYS:ShowNetServices _http._tcp TXT" 1 \
     "^Nothing answered for _http\._tcp in 3 seconds\.$" \
     "and the empty answer names the type too"
want_rc "SYS:ShowNetServices _http._tcp TXT" 1 5 "returning WARN"
waited "SYS:ShowNetServices _http._tcp TXT" 1 3 "after its full window"

# ---- 10. ALL on an empty network ----------------------------------------
#
# One window, not two: the second only opens for types the first turned up.
says "SYS:ShowNetServices ALL" 1 \
     "^Asking what this network offers, listening 3 seconds\.\.\.$" \
     "ALL starts with the same meta-query"
silent_about "SYS:ShowNetServices ALL" 1 "^Asking after " \
     "and does not open a second window when the first found no types"
want_rc "SYS:ShowNetServices ALL" 1 5 "returning WARN"
waited "SYS:ShowNetServices ALL" 1 3 "having waited one window and not two"

echo
if [ "$FAILED" -eq 0 ]; then
    [ -n "$REPLAY" ] && { echo "checks pass against $REPORT (nothing was run)"; exit 0; }
    echo "PASS: ShowNetServices, its arguments and its window, on one boot"
    exit 0
fi
echo "the transcript above is the whole run" >&2
exit 1
