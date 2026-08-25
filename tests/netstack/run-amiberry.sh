#!/usr/bin/env bash
# Run the netstack bring-up test under Amiberry on Linux.
# a2065.device is not ours to ship either: AMINETXDUO_A2065=<path>, or drop a
# copy in build/a2065.device.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/netstack"
MODEL=""
TIMEOUT=0
CPU=""
BOARD=a2065
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:c:b:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir] [-N board] [-B backend]" >&2; exit 2 ;;
    esac
done

if [ -z "$MODEL" ]; then
    case "$BOARD" in
        xsurf100z3) MODEL=A3000 ;;
        *)          MODEL=A1200 ;;
    esac
fi

if [ "$TIMEOUT" = 0 ]; then
    case "$BOARD" in
        ne2000_pcmcia) TIMEOUT=420 ;;
        *)             TIMEOUT=180 ;;
    esac
fi

EXE="$ROOT/$BUILD/tests/netstack/netstack_test"
[ -f "$EXE" ] || { echo "build $BUILD/tests/netstack/netstack_test first" >&2; exit 2; }

. "$ROOT/tools/sana2-stage.sh"

if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] && export AMINETXDUO_SANA2_DRIVER="$_have"
fi

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ] && [ "$BOARD" = "a2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
    [ -n "$A2065" ] && [ -f "$A2065" ] || {
        echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
        exit 2
    }
fi

STAGE="$ROOT/build/amiberry-netstack-stage-$BOARD"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$HERE/devs" "$STAGE/devs"
[ -z "$A2065" ] || cp "$A2065" "$STAGE/devs/a2065.device"

sana2_stage "$BOARD" "$STAGE/devs"
echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-netstack-$BOARD}"

. "$ROOT/tools/test-verdict.sh"

verdict() {
    verdict_guest "netstack" 12 "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$BACKEND" -m "$MODEL" \
     -t "$TIMEOUT" "${CPUARG[@]}" "$EXE" "$STAGE/devs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
