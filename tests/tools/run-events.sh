#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "the machine did something and could not say what".
#
#   tests/tools/run-events.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                             [-N board] [-B backend] [-a address]
#                             [-r TRANSCRIPT]
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
ADDRESS=
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
REPLAY=""

# shellcheck source=../../tools/emu-rig-lock.sh
. "$ROOT/tools/emu-rig-lock.sh"

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

if [ -z "$ADDRESS" ] && [ -z "$REPLAY" ]; then
    rig_claim_address "${AMINETXDUO_RIG_ADDR_PREFIX:-192.168.1}" \
                      "${AMINETXDUO_RIG_ADDR_FIRST:-200}" \
                      "${AMINETXDUO_RIG_ADDR_LAST:-254}" \
                      "run-events in $ROOT" || {
        echo "no free guest address; pass -a <addr> to pin one" >&2; exit 2; }
    ADDRESS="$RIG_ADDRESS"
    echo "guest_address=$ADDRESS"
fi

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-events}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"
RUN_RC=0

if [ -n "$REPLAY" ]; then
    [ -f "$REPLAY" ] || { echo "no such transcript: $REPLAY" >&2; exit 2; }
    REPORT="$REPLAY"
    echo "==> REPLAY of $REPORT: nothing was run, this only checks the checks"
else

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/ShowNetStatus" \
         "$TOOLS/NetShutdown" "$TOOLS/netstat" "$TOOLS/Offline" \
         "$TOOLS/Online" "$BSD"; do
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

STAGE="$ROOT/build/events-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"

# A board other than the a2065, the way run-netshutdown.sh takes one.
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

cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=$IFDEVICE
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
EOF

cat > "$STAGE/devs/NetInterfaces/eth9" <<EOF
DEVICE=nosuch.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=192.168.1.244
NETMASK=$NETMASK
EOF

for t in AddNetInterface ShowNetStatus NetShutdown netstat Offline Online; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
SYS:AddNetInterface eth9
SYS:ShowNetStatus EVENTS
SYS:Offline eth0
SYS:Online eth0
SYS:ShowNetStatus EVENTS
SYS:NetShutdown
SYS:ShowNetStatus EVENTS
SYS:netstat -i
EOF

# ------------------------------------------------------------------ run ---

rm -f "$REPORT"

echo "==> booting $MODEL under Amiberry, $BOARD bridged on $BACKEND"
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$BACKEND" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/ShowNetStatus" "$STAGE/NetShutdown" \
    "$STAGE/netstat" "$STAGE/Offline" "$STAGE/Online"
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

# ------------------------------------------------------------ assertions ---

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

# The Nth run of a command, as ToolsSmoke brackets them.
block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { exit }
        on { print }
    ' "$REPORT"
}

has() { # banner nth phrase description
    if block "$1" "$2" | grep -qF -- "$3"; then
        pass "$4"
    else
        fail "$4: '$3' is not in run $2 of $1"
    fi
}

hasnt() { # banner nth phrase description
    if block "$1" "$2" | grep -qF -- "$3"; then
        fail "$4: '$3' is in run $2 of $1"
    else
        pass "$4"
    fi
}

EV="SYS:ShowNetStatus EVENTS"

# ---- 1: it answers with the stack up -------------------------------------
has "$EV" 1 "the stack came up" \
    "the first read reports the bring-up"

has "$EV" 1 "the SANA-II device did not open" \
    "the interface whose device is absent is named"

# ---- 2: an interface cycle does not reset the history ---------------------
has "$EV" 2 "the stack came up" \
    "an Offline/Online cycle did not clear the record"

# ---- 3: it answers with the stack DOWN, which is the point ---------------
has "$EV" 3 "the stack began shutting down" \
    "the shutdown is readable after the stack has gone"
has "$EV" 3 "the reference that keeps the network standing was given back" \
    "and so is the release half of it"

# Reading it must not have restarted anything.
hasnt "$EV" 3 "is not in memory" \
    "the record survived the stack it describes"
if block "SYS:netstat -i" 1 | grep -qiE 'eth0.*UP'; then
    fail "the network came back: reading the ring restarted the stack"
else
    pass "the network stayed down after the ring was read"
fi

# ---- the words are the tool's and the codes are the library's ------------
if grep -qE '^ *[0-9]+\.[0-9]+ +(interface [0-9]+: )?event [0-9]+,' "$REPORT"
then
    fail "a code reached the machine with no sentence for it"
    grep -nE 'event [0-9]+,' "$REPORT" | sed 's/^/       /' >&2
else
    pass "every code printed had a sentence"
fi

echo
if [ "$FAILED" = 0 ]; then
    echo "PASS: the ring records and the command decodes, on the machine"
else
    echo "FAIL: see above" >&2
fi
exit "$FAILED"
