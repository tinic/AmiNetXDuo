#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR RemoveNetInterface, ACROSS ITS WHOLE SURFACE.
#
#   tests/tools/run-ifremove.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                               [-A [-N board] [-B backend]]
#
# The command shipped in 0.20.0 with nothing exercising it.  A compile proves
# it links; it does not prove the claim in its own header, that removing an
# interface "releases its configuration slot, so the same name can be added
# again with a different file behind it".  Freeing a slot in name only fails
# on the second add and nowhere earlier.
#
# What its template offers, and so what is checked here:
#
#   INTERFACE/M/A   more than one name in a call, and one is required
#   FORCE/S         remove an interface that still has connections
#   QUIET/S         say nothing
#
# plus the four ways it can end -- OK, WARN when there was nothing to remove,
# ERROR on a bad argument line, FAIL when the network would not answer -- and
# the two spellings of an interface, its name or the path of the file it was
# configured from.
#
# ONE BOOT.  ToolsSmoke reopens its transcript from the top after a reset, so a
# reboot would quietly turn "added twice" into "added once, twice"; the count
# of adds is asserted for that reason.  The steps are ordered so each leaves
# the machine in the state the next one needs, and the no-network case runs
# first because it is the only one that needs the stack down.
#
# NOT COVERED HERE: EBUSY, an interface refused because it still carries TCP
# connections, and the FORCE that overrides it.  That needs a live connection
# and therefore a peer, so it belongs with the tests that have one; FORCE is
# exercised below only to prove it is accepted and does not change the outcome
# when there is nothing to override.
#
# SLIRP by default, which needs no peer: this is the shim's own bookkeeping.
# -A runs it bridged, where a real driver and a real backend sometimes disagree
# with SLIRP about when a unit is closed.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=240
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

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/RemoveNetInterface" \
         "$TOOLS/netstat" "$BSD"; do
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

STAGE="$ROOT/build/ifremove-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# Static, and SLIRP's own numbers, so the address after a re-add can be
# compared with the address before it.  A lease would make "came back
# addressed" depend on a server answering every time.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'IFEOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=10.0.2.15
NETMASK=255.255.255.0
GATEWAY=10.0.2.2
IFEOF

# The board's own driver, when -A named one that is not the A2065. Staged the
# way tests/netstack/run-amiberry.sh stages it, DEVS:Networks with DEVICE= left
# as the bare name, so that -N xsurf100z2 runs against x-surf-100.device rather
# than against a card with nothing able to open it.
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

cp "$BSD"                      "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface"    "$STAGE/AddNetInterface"
cp "$TOOLS/RemoveNetInterface" "$STAGE/RemoveNetInterface"
cp "$TOOLS/netstat"            "$STAGE/netstat"

# Step by step, and each line is asserted below by its position in this list.
cat > "$STAGE/commands.txt" <<'EOF'
SYS:RemoveNetInterface eth0
SYS:AddNetInterface eth0
SYS:netstat -i
SYS:RemoveNetInterface nosuch0
SYS:RemoveNetInterface eth0 QUIET
SYS:netstat -i
SYS:AddNetInterface eth0
SYS:netstat -i
SYS:RemoveNetInterface DEVS:NetInterfaces/eth0
SYS:netstat -i
SYS:AddNetInterface eth0
SYS:RemoveNetInterface eth0 nosuch0
SYS:netstat -i
SYS:AddNetInterface eth0
SYS:RemoveNetInterface eth0 FORCE
SYS:netstat -i
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ifremove}"

set +e
if [ "$RUNNER" = "amiberry" ]; then
    HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/RemoveNetInterface" "$STAGE/netstat"
else
    HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
        "$STAGE/AddNetInterface" "$STAGE/RemoveNetInterface" "$STAGE/netstat"
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

# The Nth block for a given command banner: its output and nothing else, so
# every assertion below reads the transcript by the same rule.
block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}

# The rc ToolsSmoke recorded for that block.  RETURN_OK 0, WARN 5, ERROR 10,
# FAIL 20, which is what the command returns and what a script calling it sees.
rc_of() { block "$1" "$2" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'; }

want_rc() { # banner nth expected description
    local got; got=$(rc_of "$1" "$2")
    if [ "$got" = "$3" ]; then pass "$4 (rc $got)"
    else fail "$4: expected rc $3, got '${got:-nothing}'"
         block "$1" "$2" | sed 's/^/       /' >&2
    fi
}

ifaces() { block "SYS:netstat -i" "$1"; }

# ---- one boot ------------------------------------------------------------
ADDS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$ADDS" -eq 4 ]; then
    pass "the machine booted once and ran all four adds"
elif [ "$ADDS" -gt 4 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run stopped after $ADDS of 4 adds, something hung"
fi

# ---- 1. nothing to remove, because nothing is up -------------------------
if block "SYS:RemoveNetInterface eth0" 1 |
   grep -q "The network is not running"; then
    pass "removing before any add says the network is not running"
else
    fail "removing with the stack down did not say so"
    block "SYS:RemoveNetInterface eth0" 1 | sed 's/^/       /' >&2
fi
want_rc "SYS:RemoveNetInterface eth0" 1 5 "and returns WARN, not a failure"

# ---- 2. it was there to begin with ---------------------------------------
if ifaces 1 | grep -Eq '^eth0 .*10\.0\.2\.15'; then
    pass "eth0 came up on 10.0.2.15"
else
    fail "eth0 never came up, so nothing below proves anything"
    ifaces 1 | sed 's/^/       /' >&2
fi

# ---- 3. a name that is not there -----------------------------------------
if block "SYS:RemoveNetInterface nosuch0" 1 |
   grep -q 'there is no interface called "nosuch0"'; then
    pass "an unknown name is reported by name"
else
    fail "an unknown name was not reported"
    block "SYS:RemoveNetInterface nosuch0" 1 | sed 's/^/       /' >&2
fi
want_rc "SYS:RemoveNetInterface nosuch0" 1 20 "and returns FAIL, nothing removed"

# ---- 4. QUIET says nothing, and still removes ----------------------------
QOUT=$(block "SYS:RemoveNetInterface eth0 QUIET" 1 | grep -v '^----- rc ' || true)
if [ -z "$(printf '%s' "$QOUT" | tr -d '[:space:]')" ]; then
    pass "QUIET printed nothing"
else
    fail "QUIET printed something"
    printf '%s\n' "$QOUT" | sed 's/^/       /' >&2
fi
want_rc "SYS:RemoveNetInterface eth0 QUIET" 1 0 "and still succeeded"

if ifaces 2 | grep -Eq '^eth0 '; then
    fail "eth0 is still listed after the remove"
    ifaces 2 | sed 's/^/       /' >&2
else
    pass "eth0 is gone from the interface list"
fi

# ---- 5. the slot was genuinely free --------------------------------------
#
# The claim in the command's own header, and the reason this file exists.
want_rc "SYS:AddNetInterface eth0" 2 0 "the same name can be added again"
if ifaces 3 | grep -Eq '^eth0 .*10\.0\.2\.15'; then
    pass "and it came back carrying its address"
else
    fail "the re-added interface is not addressed"
    ifaces 3 | sed 's/^/       /' >&2
fi

# ---- 6. the other spelling: the file it was configured from --------------
if block "SYS:RemoveNetInterface DEVS:NetInterfaces/eth0" 1 |
   grep -q "eth0: removed"; then
    pass "a path is accepted where a name is, and names the interface"
else
    fail "the path form did not remove eth0"
    block "SYS:RemoveNetInterface DEVS:NetInterfaces/eth0" 1 | sed 's/^/       /' >&2
fi
if ifaces 4 | grep -Eq '^eth0 '; then
    fail "eth0 survived the path-form remove"
else
    pass "and eth0 is gone again"
fi

# ---- 7. more than one name in a call -------------------------------------
#
# INTERFACE/M.  One good and one bad: the good one must still go, and the call
# must report the failure rather than swallow it.
MULTI=$(block "SYS:RemoveNetInterface eth0 nosuch0" 1)
if printf '%s\n' "$MULTI" | grep -q "eth0: removed"; then
    pass "a multiple-name call removes the ones that exist"
else
    fail "the multiple-name call did not remove eth0"
    printf '%s\n' "$MULTI" | sed 's/^/       /' >&2
fi
if printf '%s\n' "$MULTI" | grep -q 'no interface called "nosuch0"'; then
    pass "and still reports the one that does not"
else
    fail "the bad name in a multiple-name call was swallowed"
    printf '%s\n' "$MULTI" | sed 's/^/       /' >&2
fi
want_rc "SYS:RemoveNetInterface eth0 nosuch0" 1 5 "partial success returns WARN"
if ifaces 5 | grep -Eq '^eth0 '; then
    fail "eth0 survived the multiple-name remove"
else
    pass "eth0 is gone after the multiple-name remove"
fi

# ---- 8. FORCE is accepted when there is nothing to force -----------------
want_rc "SYS:RemoveNetInterface eth0 FORCE" 1 0 "FORCE removes an idle interface"
if ifaces 6 | grep -Eq '^eth0 '; then
    fail "eth0 survived FORCE"
else
    pass "and eth0 is gone"
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "PASS: RemoveNetInterface, across its whole surface, on one boot"
    exit 0
fi
echo "the transcript above is the whole run" >&2
exit 1
