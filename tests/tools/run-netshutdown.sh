#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR NetShutdown, AND FOR WHAT IT DOES TO THE PROGRAMS
# THAT ARE USING THE NETWORK.
#
#   tests/tools/run-netshutdown.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                                  [-N board] [-B backend] [-a address]
#                                  [-r TRANSCRIPT]
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
        r) REPLAY="$OPTARG" ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

# Two arms, two boots, because the first one ends with the stack gone and the
# second needs it there.
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


STAGE="$ROOT/build/netshut-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/Public"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"

echo "Hello from an Amiga." > "$STAGE/Public/readme.txt"

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

kv() { block "$PROBE_CMD" 1 | sed -n "s/^$1=\\(.*\\)$/\\1/p" | tail -1; }

want_kv() { # key expected description
    local got; got=$(kv "$1")
    if [ "$got" = "$2" ]; then pass "$3 ($1=$got)"
    else fail "$3: $1 is '${got:-missing}', expected $2"
    fi
}

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

want_rc "SYS:AddNetInterface eth0" 1 0 "eth0 was added"

if block "SYS:netstat -i" 1 | grep -Eq "^eth0 .*$ADDRESS +up"; then
    pass "eth0 is up on $ADDRESS"
else
    fail "eth0 is not up before the shutdown"; show "SYS:netstat -i" 1
fi

listening_on() { awk -v p="$1" '$1 == "tcp" && $2 == p { n++ } END { print n+0 }'; }

for p in "$HTTPD_PORT" "$NC_PORT"; do
    if [ "$(block "SYS:netstat -a" 1 | listening_on "$p")" -gt 0 ]; then
        pass "a service is listening on port $p before the shutdown"
    else
        fail "nothing is listening on port $p: the services did not start"
        show "SYS:netstat -a" 1
    fi
done

for p in httpd nc; do
    if block "SYS:ShowNetStatus USERS" 1 | grep -Eq "^$p +[0-9]"; then
        pass "ShowNetStatus USERS lists $p with its socket count"
    else
        fail "ShowNetStatus USERS does not list $p"
        show "SYS:ShowNetStatus USERS" 1
    fi
done

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
    want_kv command_rc 5 "NetShutdown returned WARN with a program holding on"
else
    want_kv command_rc 0 "NetShutdown returned OK"
fi

want_kv select_broken 1 \
        "the holder blocked in WaitSelect() got SIGBREAKF_CTRL_C"
want_kv wait_broken 1 \
        "the holder waiting on its own signals got SIGBREAKF_CTRL_C"
want_kv select_closed 1 "and it closed bsdsocket.library"
want_kv wait_closed 1   "and so did the other one"

want_kv holders_needing_manual_break 0 \
        "neither holder had to be broken by the probe afterwards"

OPEN_AFTER=$(kv opencnt_after)
if [ -z "$OPEN_AFTER" ]; then
    fail "the probe reported no opencnt_after"
elif [ "$ARM" = stubborn ]; then
    want_kv deaf_still_holding 1 \
            "the program that ignores the signal is still there, untouched"
    if [ "$OPEN_AFTER" -eq 1 ]; then
        pass "and it is the only opener left (was $OPEN_BEFORE): every" \
             "program that does handle the signal let go"
    else
        fail "$OPEN_AFTER openers left (was $OPEN_BEFORE), expected just the" \
             "one that ignores the signal"
    fi
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

if block "$PROBE_CMD" 1 | grep -Eq '^eth0: stopped$'; then
    pass "NetShutdown took eth0 down"
else
    fail "NetShutdown never reported eth0 stopped"; show "$PROBE_CMD" 1
fi

echo "  --  netstat -i after the shutdown, for the reader: a stack here at"
echo "      all is one that netstat itself started."
block "SYS:netstat -i" 2 | sed 's/^/      /'

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
