#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR Online AND Offline.
#
#   tests/tools/run-onoff.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                            [-A [-N board] [-B backend]] [-r TRANSCRIPT]
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
        r) REPLAY="$OPTARG" ;;
        *) sed -n '3,7p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-onoff}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"
RUN_RC=0

if [ -n "$REPLAY" ]; then
    [ -f "$REPLAY" ] || { echo "no such transcript: $REPLAY" >&2; exit 2; }
    REPORT="$REPLAY"
    echo "==> REPLAY of $REPORT: nothing was run, this only checks the checks"
else

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/Online" \
         "$TOOLS/Offline" "$TOOLS/netstat" "$TOOLS/ping" "$BSD"; do
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

STAGE="$ROOT/build/onoff-stage"
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

cp "$BSD"                   "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface Online Offline netstat ping; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

cat > "$STAGE/commands.txt" <<'EOF'
SYS:Offline eth0
SYS:Offline
SYS:AddNetInterface eth0
SYS:netstat -i
SYS:ping 10.0.2.2 -c 2 -t 10
SYS:Offline eth0
SYS:netstat -i
SYS:ping 10.0.2.2 -c 1 -t 5
SYS:Offline eth0
SYS:Online eth0
SYS:netstat -i
SYS:ping 10.0.2.2 -c 2 -t 10
SYS:Online eth0
SYS:Offline eth0 TIMEOUT 5
SYS:Online a2065.device
SYS:netstat -i
SYS:Offline eth0 UNIT 3
SYS:Offline nosuch0
SYS:Offline nosuch.device UNIT 7
SYS:netstat -i
EOF

# ------------------------------------------------------------------ run ---

set +e
if [ "$RUNNER" = "amiberry" ]; then
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/Online" "$STAGE/Offline" \
        "$STAGE/netstat" "$STAGE/ping"
else
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/Online" "$STAGE/Offline" \
        "$STAGE/netstat" "$STAGE/ping"
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

# How long that command took, in milliseconds.  Only TIMEOUT/N needs it.
ms_of() { block "$1" "$2" | sed -n 's/^----- rc [0-9-]*, \([0-9]*\) ms.*/\1/p'; }

show() { block "$1" "$2" | sed 's/^/       /' >&2; }

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

ifaces() { block "SYS:netstat -i" "$1"; }

eth0_line() { ifaces "$1" | grep -E '^eth0 ' || true; }

iface_up() { # nth-netstat description
    local line; line=$(eth0_line "$1")
    if [ -z "$line" ]; then
        fail "$2: eth0 is not listed at all"
        ifaces "$1" | sed 's/^/       /' >&2
    elif printf '%s\n' "$line" | grep -Eq '10\.0\.2\.15 +up'; then
        pass "$2"
    else
        fail "$2: got '$line'"
    fi
}

# ---- the run finished, and finished once --------------------------------
EXPECTED_BLOCKS=20
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

# ---- 1. with the stack down, Offline is a no-op and says so --------------
says "SYS:Offline eth0" 1 \
     "eth0 is already offline: the network is not running." \
     "with no stack, Offline says the network is not running"
want_rc "SYS:Offline eth0" 1 5 "and returns WARN, not a failure"

# ---- 2. NAME is required ------------------------------------------------
says "SYS:Offline" 1 "^Usage: Offline " "a call with no name prints the usage"
want_rc "SYS:Offline" 1 10 "and returns ERROR"

# ---- 3. the machine this test needs -------------------------------------
want_rc "SYS:AddNetInterface eth0" 1 0 "eth0 was added"
iface_up 1 "eth0 is up on 10.0.2.15 before anything is switched"

# ---- 4. and it passes traffic. the baseline for the two below ------------
says "SYS:ping 10.0.2.2 -c 2 -t 10" 1 ", 0% packet loss" \
     "ping 10.0.2.2 gets its replies while eth0 is up"
want_rc "SYS:ping 10.0.2.2 -c 2 -t 10" 1 0 "and ping succeeds"

# ---- 5. Offline: the sentence, the listing, and the traffic -------------
says "SYS:Offline eth0" 2 "^eth0 is offline$" "Offline reports eth0 offline"
want_rc "SYS:Offline eth0" 2 0 "and returns OK"

# Down, not gone: RemoveNetInterface unlists an interface, Offline must not.
LINE2=$(eth0_line 2)
if [ -z "$LINE2" ]; then
    fail "eth0 vanished from netstat -i, Offline is not a removal"
    ifaces 2 | sed 's/^/       /' >&2
elif printf '%s\n' "$LINE2" | grep -Eq ' down'; then
    pass "netstat -i still lists eth0, with the link down"
else
    fail "netstat -i still shows eth0's link up after Offline: '$LINE2'"
fi

# The assertion that a cosmetic Offline cannot pass.
PING2=$(block "SYS:ping 10.0.2.2 -c 1 -t 5" 1)
PRC2=$(rc_of "SYS:ping 10.0.2.2 -c 1 -t 5" 1)
if printf '%s\n' "$PING2" | grep -Eq '[0-9]+ packets transmitted, 0 received'; then
    pass "and nothing gets through: 0 received"
elif printf '%s\n' "$PING2" | grep -q ' received' ; then
    fail "an offline eth0 still passed traffic"
    show "SYS:ping 10.0.2.2 -c 1 -t 5" 1
else
    pass "and nothing gets through: ping could not send at all"
fi
if [ "$PRC2" = 0 ]; then
    fail "ping returned OK with eth0 offline"
else
    pass "and ping returns non-zero (rc $PRC2)"
fi

# ---- 6. asking twice ----------------------------------------------------
says "SYS:Offline eth0" 3 "^eth0 is already offline$" \
     "Offline on an interface already down says so"
want_rc "SYS:Offline eth0" 3 0 "and returns OK, not an error"

# ---- 7. Online, and the interface is usable again -----------------------
says "SYS:Online eth0" 1 "^eth0 is online, address 10\.0\.2\.15$" \
     "Online reports eth0 online with the address it had"
want_rc "SYS:Online eth0" 1 0 "and returns OK"
iface_up 3 "netstat -i has eth0 up on 10.0.2.15 again"
says "SYS:ping 10.0.2.2 -c 2 -t 10" 2 ", 0% packet loss" \
     "and the interface passes traffic again after the cycle"
want_rc "SYS:ping 10.0.2.2 -c 2 -t 10" 2 0 "with ping succeeding"

says "SYS:Online eth0" 2 "^eth0 is already online$" \
     "Online on an interface already up says so"
want_rc "SYS:Online eth0" 2 0 "and returns OK"

# ---- 8. TIMEOUT/N is accepted, and the bound is not spent ---------------
says "SYS:Offline eth0 TIMEOUT 5" 1 "^eth0 is offline$" \
     "TIMEOUT 5 is accepted and the interface goes down"
want_rc "SYS:Offline eth0 TIMEOUT 5" 1 0 "and returns OK"
TMS=$(ms_of "SYS:Offline eth0 TIMEOUT 5" 1)
if [ -n "$TMS" ] && [ "$TMS" -lt 3000 ]; then
    pass "and it returned in ${TMS} ms, well inside the 5 s it was given"
else
    fail "TIMEOUT 5 spent ${TMS:-?} ms: the wait is not ending on the state"
fi

# ---- 9. the other spelling of NAME: the driver an interface uses --------
says "SYS:Online a2065.device" 1 "^a2065.device unit 0 is interface eth0.$" \
     "a driver name resolves to the interface that uses it, and says so"
says "SYS:Online a2065.device" 1 "^eth0 is online, address 10\.0\.2\.15$" \
     "and switches that interface"
want_rc "SYS:Online a2065.device" 1 0 "returning OK"
iface_up 4 "netstat -i agrees eth0 is up"

# ---- 10. UNIT/N contradicting the interface file ------------------------
says "SYS:Offline eth0 UNIT 3" 1 \
     "^Offline: eth0 is a2065.device unit 0, and unit 3 was asked for$" \
     "UNIT that contradicts the file is refused, quoting both"
want_rc "SYS:Offline eth0 UNIT 3" 1 10 "and returns ERROR"

# ---- 11. a name that resolves to nothing --------------------------------
says "SYS:Offline nosuch0" 1 'Offline: nothing here is called "nosuch0"' \
     "an unknown name is reported by name"
says "SYS:Offline nosuch0" 1 "^ +eth0 +a2065.device unit 0$" \
     "and the interfaces that do exist are listed, with driver and unit"
want_rc "SYS:Offline nosuch0" 1 20 "and returns FAIL"

says "SYS:Offline nosuch.device UNIT 7" 1 \
     '^Offline: nothing here is "nosuch.device" on unit 7$' \
     "an unknown driver names the unit that was asked for"
want_rc "SYS:Offline nosuch.device UNIT 7" 1 20 "and returns FAIL"

# ---- 12. and none of that touched the interface -------------------------
iface_up 5 "eth0 is still up: the three refusals changed nothing"

echo
if [ "$FAILED" -eq 0 ]; then
    [ -n "$REPLAY" ] && { echo "checks pass against $REPORT (nothing was run)"; exit 0; }
    echo "PASS: Online and Offline, across their whole surface, on one boot"
    exit 0
fi
echo "the transcript above is the whole run" >&2
exit 1
