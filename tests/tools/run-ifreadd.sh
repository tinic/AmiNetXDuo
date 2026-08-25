#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "AddInterfaceTagList() gives back an interface that
# is bare, and ConfigureInterfaceTagList() addresses it".
#
#   tests/tools/run-ifreadd.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                              [-N BOARD] [-B IFACE]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

while getopts "m:t:b:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board] [-B backend]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "ifreadd_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
PROBE="$ROOT/$BUILD/tests/tools/IfProbe"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/netstat" \
         "$TOOLS/NetShutdown" "$PROBE" "$BSD"; do
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

STAGE="$ROOT/build/ifreadd-stage"
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

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/netstat"         "$STAGE/netstat"
cp "$TOOLS/NetShutdown"     "$STAGE/NetShutdown"
cp "$PROBE"                 "$STAGE/IfProbe"

cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
SYS:netstat -r
SYS:IfProbe
SYS:netstat -r
SYS:NetShutdown
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-ifreadd}"

set +e
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/netstat" "$STAGE/NetShutdown" \
    "$STAGE/IfProbe"
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

routes() {
    awk -v want="$1" '
        /^===== SYS:netstat -r =====/ { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { exit }
        on { print }
    ' "$REPORT"
}

# ---- one boot (docs/RESEARCH.md 25) --------------------------------------
STARTS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted once (no reset)"
elif [ "$STARTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: the command list restarted"
else
    fail "the run did not get as far as bringing eth0 up"
fi

# ---- what it had ---------------------------------------------------------
BEFORE=$(routes 1)
if printf '%s\n' "$BEFORE" | grep -Eq '^10\.0\.2\.0 +\* +255\.255\.255\.0 '; then
    pass "eth0 started on 10.0.2.0/24"
else
    fail "eth0 did not start with the /24, nothing to lose, so nothing proved"
    printf '%s\n' "$BEFORE" | sed 's/^/       /' >&2
fi

if printf '%s\n' "$BEFORE" | grep -Eq '^default +10\.0\.2\.2 '; then
    pass "and with the default route through 10.0.2.2"
else
    fail "eth0 started with no default route, nothing to lose, so nothing proved"
    printf '%s\n' "$BEFORE" | sed 's/^/       /' >&2
fi

# ---- bare, as the published API says -------------------------------------
if grep -q "^bare after add: address 0\.0\.0\.0 netmask 0\.0\.0\.0, bare, correctly" "$REPORT"; then
    pass "the re-added interface came back with no address and no mask"
else
    fail "the re-added interface came back already addressed"
    grep -n "^bare after add:" "$REPORT" | sed 's/^/       /' >&2
fi

# ---- and the configure addressed it --------------------------------------
if grep -q "^netmask after the round trip: .*, the mask it had, correctly" "$REPORT"; then
    pass "IFQ_NetMask is the mask ConfigureInterfaceTagList() was given"
else
    fail "the configure did not honour IFC_NetMask, the classful guess again"
    grep -n "^netmask after the round trip:" "$REPORT" | sed 's/^/       /' >&2
fi

AFTER=$(routes 2)
if printf '%s\n' "$AFTER" | grep -Eq '^10\.0\.2\.0 +\* +255\.255\.255\.0 '; then
    pass "and the attached route is the /24 the configure asked for"
else
    fail "the attached route came back as something else, the classful mask"
    printf '%s\n' "$AFTER" | sed 's/^/       /' >&2
fi

if printf '%s\n' "$AFTER" | grep -Eq '^default +'; then
    fail "a default route reappeared on its own, neither vector may install one"
    printf '%s\n' "$AFTER" | sed 's/^/       /' >&2
else
    pass "the default route is gone, as it must be, AddRouteTagList() puts it back"
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "ifreadd: FAILED" >&2
    exit 1
fi

echo "ifreadd: PASSED"
exit 0
