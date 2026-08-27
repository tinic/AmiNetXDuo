#!/usr/bin/env bash
# THE REGRESSION TEST FOR "the machine is out of memory and the message blames
# the cable".
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# shellcheck source=../../tools/serial-log.sh
. "$ROOT/tools/serial-log.sh"

TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "t:b:" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done

ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
for f in "$ADDIF" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

KICK="${AMINETXDUO_KICKSTART_A2000:-}"
[ -n "$KICK" ] && [ -f "$KICK" ] || {
    echo "Kickstart 2.04 needed.  Set AMINETXDUO_KICKSTART_A2000=<rom>." >&2
    exit 2
}

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}


STAGE="$ROOT/build/oommsg-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
cp "$A2065"  "$STAGE/devs/a2065.device"
cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
EOF


MEM="fastmem_size=0;chipmem_size=1;bogomem_size=0"
export AMINETXDUO_AMIBERRY_EXTRA="${AMINETXDUO_AMIBERRY_EXTRA:+$AMINETXDUO_AMIBERRY_EXTRA;}$MEM"
export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-oommsg}"
export AMINETXDUO_A2065="$A2065"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting a 512 KB A2000 on Kickstart 2.04"
set +e
"$ROOT/tools/amiberry-run.sh" -m A2000 -c 68000 -N a2065 -t "$TIMEOUT" \
    -a eth0 "$ADDIF" "$STAGE/devs" "$STAGE/libs"
RUN_RC=$?
set -e

REPORT="$HD/stdout.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "================= what AddNetInterface printed ====================="
cat "$REPORT"
echo "===================================================================="
echo


FAILED=0
UNRUN=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }
skip() { echo "  --: $*"; UNRUN=$((UNRUN + 1)); }

expect() {
    local what="$1"; shift
    if grep -qiF -- "$*" "$REPORT"; then
        pass "$what"
    else
        fail "$what, nothing printed \"$*\""
    fi
}

reject() {
    local what="$1"; shift
    if grep -qiF -- "$*" "$REPORT"; then
        fail "$what, \"$*\" was printed and should not have been"
        grep -niF -- "$*" "$REPORT" | sed 's/^/       /' >&2
    else
        pass "$what"
    fi
}

expect "the INTERFACE argument reached the guest" "eth0: a2065.device unit 0"

expect "it reports the start as failed"    "the network did not start"
expect "and names memory as the reason"    "bytes are free. The stack needs about"

FREE=$(sed -n 's/^ *\([0-9][0-9]*\) bytes are free.*/\1/p' "$REPORT" | head -1)
if [ -z "$FREE" ]; then
    fail "no free-byte figure was printed"
elif [ "$FREE" -gt 0 ] && [ "$FREE" -lt 204800 ]; then
    pass "it quotes $FREE bytes free, under the 200K the branch tested"
else
    fail "the free figure '$FREE' is not a plausible reading on this machine"
fi

reject "it does not send anyone to look at the cable" "cable"
reject "and does not claim the card is fine"          "The card is fine"

RC=$(cat "$HD/.done" 2>/dev/null || echo "")
if [ "$RC" = "20" ]; then
    pass "AddNetInterface exited 20"
else
    fail "AddNetInterface exited '$RC', not 20, a failure reported as success"
fi

# THE LAST ASSERTION NEEDS A LOGGING BUILD, and it used to fail without one
# rather than say so.  serial_log_have only asks whether the file has bytes in
# it, and amiberry-run.sh now echoes a run token into the first line of every
# transcript, so on an AMINETXDUO_LOG=OFF build the log is 35 bytes -- not
# empty, and carrying nothing this can grep for.  What the seven assertions
# above check is the WORDING a user sees, which a shipping build produces; the
# stack's own AMI_ERROR is a different instrument and its absence is not a
# defect in the message.
SERIAL=$(serial_log_path "$AMINETXDUO_RUN_TAG")
# `|| true`, not `|| echo unknown`: serial_log_state PRINTS the state and
# returns 1 for `off`, so the fallback appended a second line and the message
# read "AMINETXDUO_LOG=off\nunknown".
LOGSTATE=$(serial_log_state "$BUILD" 2>/dev/null) || true
if [ "$LOGSTATE" != on ]; then
    skip "the stack's own refusal was NOT CHECKED: $BUILD is\
 AMINETXDUO_LOG=$LOGSTATE, so netstack_startup writes nothing.  The seven\
 assertions above are what a user sees and they did run"
elif ! serial_log_have "$SERIAL" "$BUILD" "the stack's own refusal"; then
    fail "the stack's own refusal was NOT CHECKED: the serial log is empty"
elif grep -qi "netstack_startup failed" "$SERIAL"; then
    pass "the stack refused for the reason the text describes"
else
    fail "the serial log has no netstack_startup failure, the run failed elsewhere"
    tail -20 "$SERIAL" | sed 's/^/       /' >&2
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "oommsg: FAILED" >&2
    exit 1
fi

if [ "$UNRUN" -ne 0 ]; then
    echo "oommsg: PASSED, $UNRUN assertion(s) did not run (above)"
    exit 0
fi

echo "oommsg: PASSED"
exit 0
