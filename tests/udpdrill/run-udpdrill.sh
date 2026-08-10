#!/usr/bin/env bash
#
# Run the UDP receive-path test under an emulator.
#
#   tests/udpdrill/run-udpdrill.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR] [-A]
#
# -A picks Amiberry, which runs genuinely headless; FS-UAE dies in GLAD without
# an X server. Same flag tests/sockopt/run-fsuae.sh carries.
#
# NO DRIVER. The test installs its own interface, tests/tcpdrill/tapdev.c,
# made at run time, and tests/tcpdrill/devs/NetInterfaces/tap0 names it, so
# nothing here needs a2065.device or anything on the wire.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"
RUNNER="${AMINETXDUO_RUNNER:-fsuae}"
TAG=udpdrill

while getopts "m:t:b:T:A" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        *) echo "usage: $0 [-m model] [-t secs] [-b dir] [-T tag] [-A]" >&2
           exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/tests/udpdrill/UdpDrill"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$EXE" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build it first" >&2; exit 2; }
done

STAGE="$ROOT/build/udpdrill-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/tcpdrill/devs" "$STAGE/devs"
cp "$BSD" "$STAGE/libs/bsdsocket.library"

export AMINETXDUO_RUN_TAG="$TAG"

if [ "$RUNNER" = "amiberry" ]; then
    HD="$ROOT/build/amiberry-testhd-$TAG"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -B slirp -m "$MODEL" -t "$TIMEOUT" \
        "$EXE" "$STAGE/devs" "$STAGE/libs" \
        > "$ROOT/build/udpdrill-$TAG.log" 2>&1 || true
else
    HD="$ROOT/build/amiberry-testhd-$TAG"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$EXE" "$STAGE/devs" "$STAGE/libs" \
        > "$ROOT/build/udpdrill-$TAG.log" 2>&1 || true
fi

echo
echo "================ udpdrill ================"
if [ -f "$HD/stdout.txt" ]; then
    cat "$HD/stdout.txt"
else
    echo "(no stdout.txt, the run did not get that far)"
fi
echo
echo "emulator log: build/udpdrill-$TAG.log"
