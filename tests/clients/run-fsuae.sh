#!/usr/bin/env bash
#
# Run the client access-pattern test under FS-UAE.
#
#   tests/clients/run-fsuae.sh [-m MODEL] [-c CPU] [-t SECONDS] [-T TAG]
#                              [-b BUILDDIR]
#
# Stages LIBS:bsdsocket.library and LIBS:usergroup.library out of the build
# tree plus DEVS:a2065.device and the netstack config, exactly as
# tests/ipv6/run-socket.sh does.  Everything the test does happens over
# 127.0.0.1, but the library still needs an interface to bring the stack up on.
#
# FS-UAE's own bsdsocket.library emulation is turned off the same way
# tests/conformance/run-conformance.sh turns it off, a host configuration dropped
# into the run's private base directory, so a pass cannot be WinUAE's
# host-socket shim answering instead of ours.
#
# Results: build/testhd-<tag>/stdout.txt
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
CPU=""
TAG="${AMINETXDUO_RUN_TAG:-clients}"

while getopts "m:c:t:T:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        b) AMINETXDUO_BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-c cpu] [-t secs] [-T tag] [-b builddir]" >&2
           exit 2 ;;
    esac
done

EXE="$ROOT/build/clients/client_patterns"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"

[ -f "$EXE" ] || { echo "missing $EXE, run tests/clients/build.sh" >&2; exit 2; }
for f in "$BSD" "$UG"; do
    [ -f "$f" ] || { echo "missing $f, build bsdsocket_library usergroup_library" >&2; exit 2; }
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

STAGE="$ROOT/build/clients-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$UG"  "$STAGE/libs/usergroup.library"

export AMINETXDUO_RUN_TAG="$TAG"

exec "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" ${CPU:+-c "$CPU"} -t "$TIMEOUT" \
     "$EXE" "$STAGE/devs" "$STAGE/libs"
