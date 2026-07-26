#!/usr/bin/env bash
#
# Run the tcpdrill scripts under FS-UAE.
#
#   tests/tcpdrill/run-tcpdrill.sh [-m MODEL] [-t SECS] [-b BUILD] [-T TAG]
#                                  [-s SCRIPT]
#
# ONE BOOT PER SCRIPT FILE.  Every case in the file runs inside a single
# emulator run, because build/.fsuae.lock serialises runs and the queue is
# deep.  Cases are independent -- each opens its own socket and each uses a
# different peer ISN -- so the only thing they share is the stack, which is
# the point.
#
# NO NETWORK.  There is no `-n` here and no a2065.device: the guest's only
# interface is tcpdrill.device, which TcpDrill creates in its own address
# space, so nothing in this test depends on FS-UAE's SLIRP, on the host's
# routing, or on anything outside the emulated machine.  See tests/tcpdrill/
# tapdev.h for why a host-side injector is not possible here at all.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
TAG="tcpdrill"
SCRIPT="$ROOT/tests/tcpdrill/scripts/tcp.drill"

while getopts "m:t:b:T:s:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        s) SCRIPT="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t secs] [-b build] [-T tag] [-s script]" >&2
           exit 2 ;;
    esac
done

BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
DRILL="$ROOT/$BUILD/tests/tcpdrill/TcpDrill"

for f in "$BSD" "$DRILL" "$SCRIPT"; do
    [ -f "$f" ] || { echo "missing $f -- build it first" >&2; exit 2; }
done

STAGE="$ROOT/build/tcpdrill-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/tcpdrill/devs" "$STAGE/devs"
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
cp "$DRILL"  "$STAGE/TcpDrill"
cp "$SCRIPT" "$STAGE/drill.txt"

echo "==> stack:  $BUILD"
echo "==> script: $SCRIPT ($(grep -c '^case ' "$SCRIPT") cases)"

export AMINETXDUO_RUN_TAG="$TAG"

set +e
"$ROOT/tools/fsuae-run.sh" -m "$MODEL" -t "$TIMEOUT" \
    "$DRILL" "$STAGE/devs" "$STAGE/libs" "$STAGE/drill.txt" \
    > "$ROOT/build/tcpdrill-$TAG.log" 2>&1
RC=$?
set -e

HD="$ROOT/build/testhd-$TAG"

echo
echo "================ tcpdrill ================"
if [ -f "$HD/tcpdrill.txt" ]; then
    cat "$HD/tcpdrill.txt"
else
    echo "(no tcpdrill.txt -- the run did not get that far)"
    [ -f "$HD/stdout.txt" ] && { echo "--- stdout ---"; cat "$HD/stdout.txt"; }
fi

echo
echo "emulator log: build/tcpdrill-$TAG.log"
echo "serial log:   build/serial-$TAG.log"
echo "guest files:  $HD"

exit "$RC"
