#!/usr/bin/env bash
# The guest data plane must follow the sending socket's IP_MULTICAST_LOOP.
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2
BUILD=${AMINETXDUO_BUILD:-build/cm}
BACKEND=${AMINETXDUO_AMIBERRY_BACKEND:-ens18}
TIMEOUT=240
ADDRESS=
TAG=mcastloop
while getopts "b:B:t:a:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-B bridged-nic] [-t seconds] [-a address]" >&2; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
case "$BACKEND" in slirp|slirp_inbound) echo "use a bridged a2065 interface" >&2; exit 2 ;; esac

TOOLS="$BUILD/src/tools"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
for file in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$BSD"; do
    [ -f "$file" ] || { echo "missing build output: $file" >&2; exit 2; }
done
[ -n "${AMINETXDUO_KICKSTART:-}" ] || { echo "no Kickstart configured" >&2; exit 2; }
A2065=${AMINETXDUO_A2065:-}
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -f "$A2065" ] || { echo "no a2065.device" >&2; exit 2; }

. "$ROOT/tools/amiga-toolchain.sh" > /dev/null 2>&1 || true
[ -n "${AMIGA_GCC:-}" ] || { echo "no Amiga compiler" >&2; exit 2; }
. "$ROOT/tools/emu-rig-lock.sh"
if [ -z "$ADDRESS" ]; then
    rig_claim_address "${AMINETXDUO_RIG_ADDR_PREFIX:-192.168.1}" \
        "${AMINETXDUO_RIG_ADDR_FIRST:-200}" \
        "${AMINETXDUO_RIG_ADDR_LAST:-254}" "run-mcastloop in $ROOT" || exit 2
    ADDRESS="$RIG_ADDRESS"
fi

STAGE=$(mktemp -d "$ROOT/build/mcastloop-stage.XXXXXX") || exit 2
trap 'rm -rf -- "$STAGE"' EXIT
mkdir -p "$STAGE/devs/NetInterfaces" "$STAGE/libs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
"$AMIGA_GCC" -O2 -m68020 ${AMIGA_NDK:+-I"$AMIGA_NDK"} \
    -o "$STAGE/McastProbe" "$ROOT/tests/tools/mcastprobe.c" || exit 2
printf 'DEVICE=a2065.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=%s\nNETMASK=255.255.255.0\n' \
    "$ADDRESS" > "$STAGE/devs/NetInterfaces/eth0"
{
    echo 'SYS:AddNetInterface eth0'
    echo 'SYS:McastProbe LOOP'
} > "$STAGE/commands.txt"

REPORT="$ROOT/build/amiberry-testhd-$TAG/tools.txt"
rm -f -- "$REPORT"
(
    export AMINETXDUO_RUN_TAG="$TAG"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -B "$BACKEND" -m A1200 \
        -t "$TIMEOUT" "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" \
        "$STAGE/devs" "$STAGE/libs" "$TOOLS/AddNetInterface" \
        "$STAGE/McastProbe"
)
RUN_RC=$?
if [ ! -s "$REPORT" ] || ! tr -d '\r' < "$REPORT" | grep -q '^===== done'; then
    echo "mcastloop: no complete guest transcript (run rc=$RUN_RC)" >&2
    exit 2
fi
if ! tr -d '\r' < "$REPORT" | grep -q '^loop: 16 checks, 0 failures$'; then
    echo "mcastloop: sender loopback failed; see $REPORT" >&2
    exit 1
fi
echo "mcastloop: PASSED ($REPORT)"
