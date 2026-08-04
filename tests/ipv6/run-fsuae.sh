#!/usr/bin/env bash
#
# Run the IPv6 link test under FS-UAE with an emulated A2065 on SLIRP.
#
#   tests/ipv6/run-fsuae.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#
# Stages the same DEVS: tree tests/netstack uses, its DEVS:NetInterfaces/eth0
# names no IPv6 keyword at all, which is the point: the default (CONFIGURE6
# absent == AUTO) is what almost every real installation will run.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=240
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/v6}"
# FS-UAE needs an X server; on a headless Linux box it dies in GLAD before the
# guest boots, so -A picks Amiberry, which runs genuinely headless.
RUNNER="${AMINETXDUO_RUNNER:-fsuae}"
BOARD=a2065
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"

while getopts "m:t:c:b:AN:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir] [-A [-N board] [-B backend]]" >&2
           exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/tests/ipv6/ipv6_link_test"
[ -f "$EXE" ] || {
    echo "build $BUILD/tests/ipv6/ipv6_link_test first (-DAMINETXDUO_IPV6=ON)" >&2
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

STAGE="$ROOT/build/ipv6-link-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-v6link}"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

if [ "$RUNNER" = "amiberry" ]; then
    exec "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
         -t "$TIMEOUT" ${CPU:+-c "$CPU"} "$EXE" "$STAGE/devs"
fi

exec "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$EXE" "$STAGE/devs"
