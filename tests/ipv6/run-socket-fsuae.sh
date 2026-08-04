#!/usr/bin/env bash
#
# Run the AF_INET6 bsdsocket.library test under FS-UAE.
#
#   tests/ipv6/run-socket-fsuae.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#
# Stages LIBS:bsdsocket.library, LIBS:usergroup.library and the DEVS: config.
#
# NO DRIVER.  The test talks only over ::1, but the library will not bring a
# stack up with no interface to put it on, so the test installs one itself,
# tests/tcpdrill/tapdev.c, made at run time with MakeLibrary()/AddDevice().
# DEVS:NetInterfaces/tap0 names it and src/sana2/ brings it up through exactly
# the code a real card goes through.
#
# That is what lets this run anywhere tier 2 runs.  It used to need Commodore's
# a2065.device, which is not ours to ship, so the whole IPv6 surface was pinned
# to the one CI runner that had a copy.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=240
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/v6}"
# FS-UAE needs an X server; on a headless Linux box it dies in GLAD before the
# guest boots, so -A picks Amiberry, which runs genuinely headless. Same block
# run-fsuae.sh beside this one carries.
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

EXE="$ROOT/$BUILD/tests/ipv6/ipv6_socket_test"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"

for f in "$EXE" "$BSD" "$UG"; do
    [ -f "$f" ] || { echo "missing $f (build with -DAMINETXDUO_IPV6=ON)" >&2; exit 2; }
done

STAGE="$ROOT/build/ipv6-socket-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
# tests/tcpdrill's devs, not tests/netstack's: its NetInterfaces/tap0 names the
# device the test creates for itself, so nothing here needs a driver on disk.
cp -R "$ROOT/tests/tcpdrill/devs" "$STAGE/devs"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$UG"  "$STAGE/libs/usergroup.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-v6sock}"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

if [ "$RUNNER" = "amiberry" ]; then
    exec "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
         -t "$TIMEOUT" "${CPUARG[@]}" "$EXE" "$STAGE/devs" "$STAGE/libs"
fi

exec "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$EXE" "$STAGE/devs" "$STAGE/libs"
