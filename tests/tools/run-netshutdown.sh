#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR NetShutdown, AND FOR WHAT IT DOES TO THE PROGRAMS
# THAT ARE USING THE NETWORK.
#
#   tests/tools/run-netshutdown.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                  [-N board] [-B backend] [-a address]
#                                  [-r TRANSCRIPT]
#
# A user reported it: "the other TCP/IP stacks send a CTRL_C signal to the
# processes that have the bsdsocket.library open.  This allows these
# applications to shutdown or close the library.  Of course this involves some
# grace period. [...] it doesn't really require a 'force' argument either.
# It's always 'force', but with the attempt to notify the applications."
#
# Ours took the interfaces down and stopped, so `httpd` serving a drawer, `nc`
# holding a listener, and anything sitting in WaitSelect() were all still there
# afterwards, holding a library whose network had been taken away underneath
# them.  Nothing tested it because nothing here had ever run a command while a
# service was live.
#
# What is asserted, and why each one needs a live machine:
#
#   1  two services are started and left running, httpd and nc.  Both hold
#      bsdsocket.library open, both are listening, and netstat -a says so
#      before anything is shut down.  This is the state the report is about.
#   2  ShutProbe (tests/tools/shutprobe.c) adds two holders of its own, one
#      blocked inside the library in WaitSelect() and one waiting on its own
#      signals outside it, reads bsdsocket.library's open count off the master
#      base, runs the command, and watches for the grace period.
#   3  the open count is the verdict.  Every OpenLibrary() adds one and the
#      stack goes down when the last is given back, so "the applications were
#      notified and closed" and "nothing happened" are two different numbers
#      rather than two readings of the same prose.
#   4  the interfaces are down afterwards, which is what NetShutdown did
#      before any of this and must keep doing.
#
# THE OPEN COUNT IS THE POINT.  A service that prints "shutting down" and stays
# resident holding a socket is the failure being tested for; only the count, or
# a later run of the same test, can tell that apart from one that left.
#
# BRIDGED, always.  The services here are real ones and the whole subject is
# what happens to live connections; a peerless backend would test a stack
# talking to itself.
#
# GOOD CASE: about 60 s wall, boot to verdict.  A run that reaches -t is a
# defect to diagnose, not a number to raise: the first assertion names the
# command that was still running when the emulator was killed.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=180
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
IFDEVICE="${AMINETXDUO_IFDEVICE:-a2065.device}"
ADDRESS=192.168.1.241
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
HTTPD_PORT=8080
NC_PORT=7099
GRACE=10
REPLAY=""

while getopts "m:t:b:N:B:a:g:r:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        # Assert a transcript that already exists instead of booting, for
        # developing the assertions and for showing they fail on a transcript
        # broken on purpose.  It proves nothing about the product, which is
        # why it prints that it did not run.
        r) REPLAY="$OPTARG" ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

# Two arms, two boots, because the first one ends with the stack gone and the
# second needs it there.
#
#   letgo     every program handles the signal and closes the library, which
#             is what the shutdown is for
#   stubborn  one of them does not, which Roadshow's manual says cannot be
#             prevented: "it is not possible for an Amiga program to be forced
#             to give up its network resources".  The interfaces must still go
#             down, the command must say which program held on, and it must
#             not pretend to have succeeded
#
# A suite that only ran the first arm could not tell a shutdown that reports
# the straggler from one that never noticed.
if [ -z "${AMINETXDUO_NETSHUT_ARM:-}" ] && [ -z "$REPLAY" ]; then
    rc=0
    for arm in letgo stubborn; do
        echo
        echo "######################## arm: $arm ########################"
        AMINETXDUO_NETSHUT_ARM="$arm" \
        AMINETXDUO_RUN_TAG="netshut-$arm" \
            bash "$0" "$@" || rc=1
    done
    echo
    [ "$rc" = 0 ] && echo "PASS: both arms" || echo "FAIL: see the arm above" >&2
    exit "$rc"
fi

ARM="${AMINETXDUO_NETSHUT_ARM:-letgo}"

TOOLS="$ROOT/$BUILD/src/tools"
PROBES="$ROOT/$BUILD/tests/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-netshut}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"
RUN_RC=0

if [ -n "$REPLAY" ]; then
    [ -f "$REPLAY" ] || { echo "no such transcript: $REPLAY" >&2; exit 2; }
    REPORT="$REPLAY"
    echo "==> REPLAY of $REPORT: nothing was run, this only checks the checks"
else

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/NetShutdown" \
         "$TOOLS/netstat" "$TOOLS/httpd" "$TOOLS/nc" \
         "$PROBES/ShutProbe" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" \
                     "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/netshut-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/Public"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"

echo "Hello from an Amiga." > "$STAGE/Public/readme.txt"

# A board other than the a2065, the way run-httpd.sh takes one.
. "$ROOT/tools/sana2-stage.sh"
if [ "$BOARD" != a2065 ]; then
    if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ]; then
        _want=$(sana2_driver_for "$BOARD")
        _have=$(sana2_local_driver "$_want")
        [ -n "$_have" ] && [ -f "$_have" ] &&
            export AMINETXDUO_SANA2_DRIVER="$_have"
    fi
    sana2_stage "$BOARD" "$STAGE/devs"
    IFDEVICE="$SANA2_DEVICE"
    echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"
fi

# Static: this test asserts against the address it was given, and a lease that
# arrives late would make "the interface is up" a question about the lab's DHCP
# server rather than about the shutdown.
cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=$IFDEVICE
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
EOF

for t in AddNetInterface NetShutdown netstat ShowNetStatus httpd nc; do
    cp "$TOOLS/$t" "$STAGE/$t"
done
cp "$PROBES/ShutProbe" "$STAGE/ShutProbe"

# ShutProbe's third argument adds the holder that ignores the signal.
PROBE_ARGS="SYS:NetShutdown $GRACE"
if [ "$ARM" = stubborn ]; then PROBE_ARGS="$PROBE_ARGS deaf"; fi

# The two services are started detached and left running, which is the state
# the whole test is about; `wait` is ToolsSmoke's Delay(), long enough for both
# to reach their listen().
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:netstat -i
&SYS:httpd ROOT SYS:Public PORT $HTTPD_PORT >DH0:httpd.txt
&SYS:nc -l $NC_PORT -v -w 90 >DH0:nc.txt
wait 6
SYS:netstat -a
SYS:ShowNetStatus USERS
SYS:ShutProbe $PROBE_ARGS
SYS:netstat -i
SYS:netstat -a
EOF

# ------------------------------------------------------------------ run ---

echo "==> booting $MODEL under Amiberry, $BOARD bridged on $BACKEND"
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$BACKEND" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/NetShutdown" "$STAGE/netstat" \
    "$STAGE/ShowNetStatus" "$STAGE/httpd" "$STAGE/nc" "$STAGE/ShutProbe" \
    "$STAGE/Public"
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
show()  { block "$1" "$2" | sed 's/^/       /' >&2; }

want_rc() {
    local got; got=$(rc_of "$1" "$2")
    if [ "$got" = "$3" ]; then pass "$4 (rc $got)"
    else fail "$4: expected rc $3, got '${got:-nothing}'"; show "$1" "$2"
    fi
}

PROBE_CMD="SYS:ShutProbe SYS:NetShutdown $GRACE"
if [ "$ARM" = stubborn ]; then PROBE_CMD="$PROBE_CMD deaf"; fi

# One key=value line out of ShutProbe's block, or the empty string.
kv() { block "$PROBE_CMD" 1 | sed -n "s/^$1=\\(.*\\)$/\\1/p" | tail -1; }

want_kv() { # key expected description
    local got; got=$(kv "$1")
    if [ "$got" = "$2" ]; then pass "$3 ($1=$got)"
    else fail "$3: $1 is '${got:-missing}', expected $2"
    fi
}

# ---- the run finished ----------------------------------------------------
EXPECTED_BLOCKS=9
BLOCKS=$(grep -c '^===== SYS:' "$REPORT" || true)
if ! grep -q '^===== done, ' "$REPORT"; then
    LAST=$(grep '^===== SYS:' "$REPORT" | tail -1 | sed 's/^===== //; s/ =====$//')
    fail "THE RUN DID NOT FINISH: it stopped in '${LAST:-nothing ran}'," \
         "block $BLOCKS of $EXPECTED_BLOCKS (emulator rc=$RUN_RC," \
         "timeout ${TIMEOUT}s).  That command did not return."
else
    pass "the machine booted once and ran every command"
fi

# ---- 1. the machine, and the two services on it --------------------------
want_rc "SYS:AddNetInterface eth0" 1 0 "eth0 was added"

if block "SYS:netstat -i" 1 | grep -Eq "^eth0 .*$ADDRESS +up"; then
    pass "eth0 is up on $ADDRESS"
else
    fail "eth0 is not up before the shutdown"; show "SYS:netstat -i" 1
fi

# netstat -a lists sockets as "tcp <local port> <foreign> <state>", so the
# port is the second field and nothing else.  Matching it anywhere in the line
# would also match the byte counts and the foreign column, which is how the
# first version of this file passed both before and after the shutdown.
listening_on() { awk -v p="$1" '$1 == "tcp" && $2 == p { n++ } END { print n+0 }'; }

# Both services must be in it, or the shutdown below has nothing to shut down
# and every assertion after it is measuring an empty machine.
for p in "$HTTPD_PORT" "$NC_PORT"; do
    if [ "$(block "SYS:netstat -a" 1 | listening_on "$p")" -gt 0 ]; then
        pass "a service is listening on port $p before the shutdown"
    else
        fail "nothing is listening on port $p: the services did not start"
        show "SYS:netstat -a" 1
    fi
done

# ---- 2. and ShowNetStatus USERS names them -------------------------------
#
# The listing nothing else on the Amiga has: Roadshow's ShowNetStatus reports
# sockets without their owners and AmiTCP kept the list private, so this is
# the only place a user can find out which program to close.
for p in httpd nc; do
    if block "SYS:ShowNetStatus USERS" 1 | grep -Eq "^$p +[0-9]"; then
        pass "ShowNetStatus USERS lists $p with its socket count"
    else
        fail "ShowNetStatus USERS does not list $p"
        show "SYS:ShowNetStatus USERS" 1
    fi
done

# ---- 3. the probe took its measurement -----------------------------------
want_rc "$PROBE_CMD" 1 0 "ShutProbe ran to the end"
want_kv done 1 "and reported a complete measurement"

# httpd, nc, ShutProbe's two holders, and the reference AddNetInterface leaves
# behind on purpose: five, and never fewer than four for the test to mean
# anything.
OPEN_BEFORE=$(kv opencnt_before)
if [ -n "$OPEN_BEFORE" ] && [ "$OPEN_BEFORE" -ge 4 ]; then
    pass "$OPEN_BEFORE programs had bsdsocket.library open when it was told to stop"
else
    fail "opencnt_before is '${OPEN_BEFORE:-missing}': fewer openers than this" \
         "test stages, so it is not measuring what it claims to"
fi

if [ "$ARM" = stubborn ]; then
    # WARN and not OK.  A command that reports success while a program is
    # still holding the library has told a script the opposite of the truth,
    # and the script's next step is usually to reconfigure or reboot.
    want_kv command_rc 5 "NetShutdown returned WARN with a program holding on"
else
    want_kv command_rc 0 "NetShutdown returned OK"
fi

# ---- 4. the applications were notified, and left -------------------------
#
# The report, as four numbers.  A stack that only disables the link scores
# zero on all four.
want_kv select_broken 1 \
        "the holder blocked in WaitSelect() got SIGBREAKF_CTRL_C"
want_kv wait_broken 1 \
        "the holder waiting on its own signals got SIGBREAKF_CTRL_C"
want_kv select_closed 1 "and it closed bsdsocket.library"
want_kv wait_closed 1   "and so did the other one"

want_kv holders_needing_manual_break 0 \
        "neither holder had to be broken by the probe afterwards"

# The count is what "the refcount drops to zero - hopefully" means.  httpd and
# nc are the two that matter here: they are ordinary programs that were told
# nothing and are expected to leave on the signal alone.
OPEN_AFTER=$(kv opencnt_after)
if [ -z "$OPEN_AFTER" ]; then
    fail "the probe reported no opencnt_after"
elif [ "$ARM" = stubborn ]; then
    # Exactly the deaf holder.  Zero would mean it was got rid of, which
    # nothing on this machine is allowed to do to a program that never asked
    # to be stopped; more than one would mean a cooperative program was left
    # behind as well.
    want_kv deaf_still_holding 1 \
            "the program that ignores the signal is still there, untouched"
    if [ "$OPEN_AFTER" -eq 1 ]; then
        pass "and it is the only opener left (was $OPEN_BEFORE): every" \
             "program that does handle the signal let go"
    else
        fail "$OPEN_AFTER openers left (was $OPEN_BEFORE), expected just the" \
             "one that ignores the signal"
    fi
    # And the command has to name it, or the user is left to guess which
    # program to close.
    if block "$PROBE_CMD" 1 | grep -Eq 'did not let go'; then
        pass "NetShutdown reported the program that did not let go"
    else
        fail "NetShutdown said nothing about the program still holding on"
        show "$PROBE_CMD" 1
    fi
elif [ "$OPEN_AFTER" -eq 0 ]; then
    pass "bsdsocket.library has no openers left: everything let go, and the" \
         "stack went down with the last of them"
else
    fail "bsdsocket.library still has $OPEN_AFTER openers (was $OPEN_BEFORE):" \
         "the programs using the network were not told it had stopped"
fi

# ---- 5. and the interfaces went down, which it always did ----------------
#
# From what NetShutdown printed, not from the netstat after it: a shutdown that
# succeeded took bsdsocket.library with it, and the next program to open the
# library starts the stack again from DEVS:NetInterfaces.  netstat is such a
# program, so the table it prints afterwards describes a new stack rather than
# the one that was shut down.  It is dumped below for the reader.
if block "$PROBE_CMD" 1 | grep -Eq '^eth0: stopped$'; then
    pass "NetShutdown took eth0 down"
else
    fail "NetShutdown never reported eth0 stopped"; show "$PROBE_CMD" 1
fi

echo "  --  netstat -i after the shutdown, for the reader: a stack here at"
echo "      all is one that netstat itself started."
block "SYS:netstat -i" 2 | sed 's/^/      /'

# ---- 6. and the services are gone, not merely quiet ----------------------
#
# The restarted stack has no httpd and no nc in it, so a listener on either
# port would have to be the original process still running.
for p in "$HTTPD_PORT" "$NC_PORT"; do
    if [ "$(block "SYS:netstat -a" 2 | listening_on "$p")" -gt 0 ]; then
        fail "port $p is still listening after NetShutdown"
        show "SYS:netstat -a" 2
    else
        pass "nothing is listening on port $p any more"
    fi
done

echo
if [ "$FAILED" -eq 0 ]; then
    [ -n "$REPLAY" ] && { echo "checks pass against $REPORT (nothing was run)"; exit 0; }
    echo "PASS: NetShutdown stops the network and the programs using it let go"
    exit 0
fi
echo "the transcript above is the whole run" >&2
exit 1
