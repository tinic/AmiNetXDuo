#!/usr/bin/env bash
#
# Run the shared-library load test under FS-UAE with an A2065 on SLIRP.
#
#   tests/libraries/run-fsuae.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#
# -b (or AMINETXDUO_BUILD) picks the build tree, so the floor build and an
# -DAMINETXDUO_IPV6=ON build can both be run without renaming directories.
#
# Stages LIBS:bsdsocket.library, LIBS:usergroup.library, DEVS:a2065.device and
# the DEVS:NetInterfaces / DEVS:Internet config, then boots.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=180
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:c:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir]" >&2; exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/tests/libraries/library_test"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"

for f in "$EXE" "$BSD" "$UG"; do
    [ -f "$f" ] || { echo "missing $f, build library_test bsdsocket_library usergroup_library" >&2; exit 2; }
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

STAGE="$ROOT/build/libraries-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$UG"  "$STAGE/libs/usergroup.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-libraries}"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

exec "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$EXE" "$STAGE/devs" "$STAGE/libs"
