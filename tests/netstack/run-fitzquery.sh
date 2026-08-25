#!/usr/bin/env bash
#
# THE REGRESSION TEST FOR "the machine cannot see its own share".
#
#   tests/netstack/run-fitzquery.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT WENT WRONG
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=180
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"
FITZ="$ROOT/build/fitz/Fitz/fitz"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

[ -f "$FITZ" ] || {
    echo "missing $FITZ, run tests/endurance/fetch-fitz.sh" >&2; exit 2; }

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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/fitzquery-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
[ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$FITZ" "$STAGE/fitz"

cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
wait 4
&SYS:fitz serve RAM: name ramdisk
wait 6
SYS:fitz query
wait 2
SYS:fitz query
EOF

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-fitzquery}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/fitz"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

# ------------------------------------------------------------ assertions ---

FAILED=0

fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }

BOOTS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$BOOTS" -gt 1 ]; then
    fail "THE MACHINE REBOOTED: $BOOTS starts in one run"
elif [ "$BOOTS" -eq 1 ]; then
    pass "the machine booted exactly once"
else
    fail "no start in the transcript, the run did not get far enough to judge"
fi

if grep -q "no named services found on LAN" "$REPORT"; then
    fail "'fitz query' found nothing, the broadcast did not come back"
fi

if grep -qE "^  ramdisk [0-9]+ [0-9]+" "$REPORT"; then
    pass "'fitz query' listed this machine's own share"
    grep -E "^  ramdisk [0-9]+ [0-9]+" "$REPORT" | sed 's/^/       /'
else
    fail "'fitz query' did not list 'ramdisk'"
fi

LISTED=$(grep -cE "^  ramdisk [0-9]+ [0-9]+" "$REPORT" || true)
if [ "$LISTED" -ge 2 ]; then
    pass "both queries listed it ($LISTED)"
else
    fail "only $LISTED of 2 queries listed the share"
fi

if grep -q "Roadshow detected" "$REPORT"; then
    fail "Fitz took its Roadshow path, the result says nothing about ours"
fi

echo
if [ "$FAILED" = "0" ]; then
    echo "run-fitzquery: PASSED"
else
    echo "run-fitzquery: FAILED"
fi
exit "$FAILED"
