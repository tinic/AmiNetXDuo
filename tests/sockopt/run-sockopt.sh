#!/usr/bin/env bash
#
# Run the socket-option test under Amiberry.
#
#   tests/sockopt/run-sockopt.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                                [-N BOARD] [-B BACKEND]
#
# It was called run-fsuae.sh and both halves of it drove
# tools/amiberry-run.sh; -A picked between two branches running the same
# emulator.  fs-uae left the tree on 2026-08-04 and the name outlived it, the
# way tests/ipv6/run-socket.sh's did.
#
# Stages LIBS:bsdsocket.library, LIBS:usergroup.library and the DEVS: config.
#
# NO DRIVER, AND NO CARD EITHER.  Almost nothing here goes on the wire, an
# option is set and read back, but the library will not bring a stack up with
# no interface to put it on, so the test installs one itself:
# tests/tcpdrill/tapdev.c, made at run time with MakeLibrary()/AddDevice().
# DEVS:NetInterfaces/tap0 names it.
#
# That is what lets this run anywhere tier 2 runs, rather than only where
# Commodore's a2065.device is.
#
# SO IT ASKS FOR NO BOARD.  It used to pass -N a2065 and stage no driver for
# it, which put a card in the machine that nothing ever opened.  On a host
# with AMINETXDUO_AMIBERRY_BACKEND set, tools/amiberry-run.sh's backend
# assertion then looked for a `UAENET: '<iface>' open successful` line that
# only appears when the GUEST opens the driver, did not find one, and failed
# the run -- over a transcript ending `113 checks, 0 failures, PASS`.  A run
# that uses no card must not claim one; -N is still here for anyone who wants
# to put the same test on a real link.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=240
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/sockopt}"
# No card unless one is asked for, see the note above.  -N puts one in and -B
# says what it is wired to; with -N and no -B the backend is
# tools/amiberry-run.sh's own default.
BOARD=""
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-}"

while getopts "m:t:c:b:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir]" \
                "[-N board] [-B backend]" >&2
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

RUNARG=()
[ -z "$CPU" ]   || RUNARG+=(-c "$CPU")
[ -z "$BOARD" ] || RUNARG+=(-N "$BOARD")
[ -z "$IFACE" ] || RUNARG+=(-B "$IFACE")

set +e
"$ROOT/tools/amiberry-run.sh" -m "$MODEL" -t "$TIMEOUT" "${RUNARG[@]}" \
     "$EXE" "$STAGE/devs" "$STAGE/libs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
