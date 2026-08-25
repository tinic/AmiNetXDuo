#!/usr/bin/env bash
#
# Run tests/stack under FS-UAE: the API called from the stack a Shell command
# actually gets.
#
#   tests/stack/run-stack.sh [-m model] [-t seconds] [-b builddir] [-e]
#
#     -e  under Enforcer + MungWall (tools/enforcer-run.sh -m) instead of a
#         plain boot. That is the instrument for this: a Process's stack is an
#         AllocMem block, so overrunning it lands in MungWall's guard band and
#         is reported instead of merely corrupting something.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

MODEL="A1200"
TIMEOUT=900
BUILD="build/ci/default"
ENFORCE=0

IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

while getopts "m:t:b:B:e" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        e) ENFORCE=1 ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-B backend] [-e]" >&2; exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/tests/stack/stack_test"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$EXE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build stack_test bsdsocket_library" >&2; exit 2; }
done

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

TAG="${AMINETXDUO_RUN_TAG:-stack}"
STAGE="$ROOT/build/stack-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"

UG="$ROOT/$BUILD/src/usergroup/usergroup.library"
[ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"

BASEDIR="$ROOT/build/fsuae-base-$TAG"
mkdir -p "$BASEDIR/Configurations"
cat > "$BASEDIR/Configurations/Host.fs-uae" <<'EOF'
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
bsdsocket_library = 0
EOF

export AMINETXDUO_RUN_TAG="$TAG"

# ---------------------------------------------------------- the verdict ---
. "$ROOT/tools/test-verdict.sh"

verdict() {
    # 0 pass, 1 fail, 77 the guest skipped: all three are carried out.
    verdict_guest "stack" 12 "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$ROOT/build/testhd-$TAG/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

if [ "$ENFORCE" = "1" ]; then
    set +e
    "$ROOT/tools/enforcer-run.sh" -m -n -t "$TIMEOUT" \
         "$EXE" "$STAGE/devs" "$STAGE/libs"
    RUN_RC=$?
    set -e
    verdict "$RUN_RC"
fi

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
     "$EXE" "$STAGE/devs" "$STAGE/libs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
