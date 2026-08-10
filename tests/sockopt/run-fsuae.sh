#!/usr/bin/env bash
#
# Run the socket-option test under FS-UAE.
#
#   tests/sockopt/run-fsuae.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#
# Stages LIBS:bsdsocket.library, LIBS:usergroup.library and the DEVS: config.
#
# NO DRIVER.  Almost nothing here goes on the wire, an option is set and read
# back, but the library will not bring a stack up with no interface to put it
# on, so the test installs one itself: tests/tcpdrill/tapdev.c, made at run
# time with MakeLibrary()/AddDevice().  DEVS:NetInterfaces/tap0 names it.
#
# That is what lets this run anywhere tier 2 runs, rather than only where
# Commodore's a2065.device is.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=240
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/sockopt}"
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

EXE="$ROOT/$BUILD/tests/sockopt/sockopt_test"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"

for f in "$EXE" "$BSD" "$UG"; do
    [ -f "$f" ] || { echo "missing $f (build the cross tree first)" >&2; exit 2; }
done

STAGE="$ROOT/build/sockopt-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
# tests/tcpdrill's devs, not tests/netstack's: its NetInterfaces/tap0 names the
# device the test creates for itself, so nothing here needs a driver on disk.
cp -R "$ROOT/tests/tcpdrill/devs" "$STAGE/devs"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$UG"  "$STAGE/libs/usergroup.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-sockopt}"

# ---------------------------------------------------------- the verdict ---
#
# This used to end in `exec <runner>`, so the script's exit status was the
# guest's own return code: a guest that opened nothing, ran no checks and
# returned 0 was a pass, and so was one whose transcript never arrived.
# tools/test-verdict.sh reads the guest's own counters instead, puts a floor
# under the number of checks, and fails loudly and by name when there is no
# transcript at all.
. "$ROOT/tools/test-verdict.sh"

verdict() {
    # 0 pass, 1 fail, 77 the guest skipped: all three are carried out.
    verdict_guest "sockopt" 100 "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

if [ "$RUNNER" = "amiberry" ]; then
    set +e
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
         -t "$TIMEOUT" "${CPUARG[@]}" "$EXE" "$STAGE/devs" "$STAGE/libs"
    RUN_RC=$?
    set -e
    verdict "$RUN_RC"
fi

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$EXE" "$STAGE/devs" "$STAGE/libs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
