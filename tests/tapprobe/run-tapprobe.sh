#!/usr/bin/env bash
#
# Run tapprobe under FS-UAE against one stack.
#
#   tests/tapprobe/run-tapprobe.sh [-s anxd|roadshow] [-t SECS] [-b BUILD]
#                                  [-r ROADSHOW_DIR]
#
# anxd is the calibration case: our own library, whose copy hook we wrote, so
# the numbers the probe reports for it are the scale everything else is read
# against.  roadshow stages the demo's Workbench tree -- bsdsocket.library,
# usergroup.library and the network commands -- and lets AddNetInterface start
# the stack.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
STACK=anxd
TIMEOUT=300
MODEL=A1200
BUILD="${AMINETXDUO_BUILD:-build/cm}"
RSDIR="${ROADSHOW_DIR:-$HOME/roadshow-wb}"

while getopts "s:t:b:r:m:" opt; do
    case "$opt" in
        s) STACK="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        r) RSDIR="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        *) echo "usage: $0 [-s anxd|roadshow] [-t secs] [-b build] [-r dir]" >&2
           exit 2 ;;
    esac
done

PROBE="$ROOT/$BUILD/tests/tapprobe/TapProbe"
[ -f "$PROBE" ] || { echo "missing $PROBE -- build it first" >&2; exit 2; }

TAG="tapprobe-$STACK"
[ "$MODEL" = "A1200" ] || TAG="$TAG-$MODEL"
STAGE="$ROOT/build/$TAG-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/devs/NetInterfaces" "$STAGE/libs" "$STAGE/rs"

IFFILE="$ROOT/tests/tapprobe/tap0"
[ "$STACK" = "amitcpng" ] && IFFILE="$ROOT/tests/tapprobe/tap0-ng"
cp "$IFFILE" "$STAGE/devs/NetInterfaces/tap0"
cp -R "$ROOT/tests/tcpdrill/devs/Internet" "$STAGE/devs/Internet"

UP=""

case "$STACK" in
anxd)
    cp "$ROOT/$BUILD/src/bsdsocket/bsdsocket.library" "$STAGE/libs/"
    ;;
roadshow)
    [ -d "$RSDIR/Libs" ] || {
        echo "no Roadshow Workbench tree at $RSDIR" >&2; exit 2; }
    cp "$RSDIR/Libs/bsdsocket.library"  "$STAGE/libs/"
    cp "$RSDIR/Libs/usergroup.library"  "$STAGE/libs/"
    # Roadshow's own Internet database wins over ours when it is present.
    rm -rf "$STAGE/devs/Internet"
    cp -R "$RSDIR/Devs/Internet" "$STAGE/devs/Internet"
    for cmd in AddNetInterface ConfigureNetInterface Online ShowNetStatus \
               GetNetStatus NetShutdown RemoveNetInterface ping arp; do
        [ -f "$RSDIR/C/$cmd" ] && cp "$RSDIR/C/$cmd" "$STAGE/rs/"
    done
    UP="DH0:rs/AddNetInterface DEVS:NetInterfaces/tap0"
    ;;
amitcpng)
    NGDIR="${AMITCPNG_DIR:-$HOME/amitcpng/AmiTCP_NG/data}"
    [ -d "$NGDIR/Libs" ] || {
        echo "no AmiTCP_NG tree at $NGDIR" >&2; exit 2; }
    cp "$NGDIR/Libs/bsdsocket.library" "$STAGE/libs/"
    [ -d "$NGDIR/Devs/Internet" ] && {
        rm -rf "$STAGE/devs/Internet"
        cp -R "$NGDIR/Devs/Internet" "$STAGE/devs/Internet"; }
    [ -d "$NGDIR/db" ] && cp -R "$NGDIR/db" "$STAGE/db"
    for cmd in AddNetInterface ConfigureNetInterface Online ShowNetStatus \
               GetNetStatus NetShutdown RemoveNetInterface Offline ping; do
        [ -f "$NGDIR/C/$cmd" ] && cp "$NGDIR/C/$cmd" "$STAGE/rs/"
    done
    UP="DH0:rs/AddNetInterface DEVS:NetInterfaces/tap0"
    ;;
*)  echo "unknown stack: $STACK" >&2; exit 2 ;;
esac

# argv does not survive the boot shell, so the mode travels in a file.
printf '%s\n%s\n' "$STACK" "$UP" > "$STAGE/mode.txt"

echo "==> stack: $STACK"

export AMINETXDUO_RUN_TAG="$TAG"

set +e
xvfb-run -a "$ROOT/tools/fsuae-run.sh" -x -m "$MODEL" -t "$TIMEOUT" \
    "$PROBE" "$STAGE/devs" "$STAGE/libs" "$STAGE/rs" "$STAGE/mode.txt" \
    $( [ -d "$STAGE/db" ] && echo "$STAGE/db" ) \
    > "$ROOT/build/$TAG.log" 2>&1
RC=$?
set -e

HD="$ROOT/build/testhd-$TAG"

echo
echo "================ tapprobe: $STACK ================"
if [ -f "$HD/tapprobe.txt" ]; then
    cat "$HD/tapprobe.txt"
else
    echo "(no tapprobe.txt -- the run did not get that far)"
    [ -f "$HD/stdout.txt" ] && { echo "--- stdout ---"; cat "$HD/stdout.txt"; }
fi

echo
echo "emulator log: build/$TAG.log"
echo "serial log:   build/serial-$TAG.log"
echo "guest files:  $HD"

exit "$RC"
