#!/usr/bin/env bash
#
# Run the netstack bring-up test under FS-UAE with an emulated A2065 on SLIRP.
#
#   tests/netstack/run-fsuae.sh [-m MODEL] [-t SECONDS]
#
# Stages DEVS:NetInterfaces/eth0, DEVS:Internet/* and a SANA-II a2065.device
# onto the test hard drive, then hands off to tools/fsuae-run.sh -n.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/netstack"
MODEL=A1200
TIMEOUT=180

while getopts "m:t:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds]" >&2; exit 2 ;;
    esac
done

EXE="$ROOT/build/cm/tests/netstack/netstack_test"
[ -f "$EXE" ] || { echo "build build/cm/tests/netstack/netstack_test first" >&2; exit 2; }

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

STAGE="$ROOT/build/netstack-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$HERE/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# Keep the staging drive, serial log and emulator config clear of any other
# run happening at the same time.
export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-netstack}"

exec "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "$EXE" "$STAGE/devs"
