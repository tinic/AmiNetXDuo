#!/usr/bin/env bash
#
# Run FreeBSD's adopted tests/sys/netinet cases under Amiberry.
#
#   tests/atf/run-atf.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                        [-N BOARD] [-B BACKEND]
#
# NO DRIVER AND NO CARD.  Everything the adopted cases do is over
# INADDR_LOOPBACK; the tap0 interface exists only because the library will not
# bring a stack up without one, so this asks for no board.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=240
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
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

EXE="$ROOT/$BUILD/tests/atf/AtfTcpSocket"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"

for f in "$EXE" "$BSD" "$UG"; do
    [ -f "$f" ] || { echo "missing $f (build the cross tree first)" >&2; exit 2; }
done

STAGE="$ROOT/build/atf-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/tcpdrill/devs" "$STAGE/devs"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$UG"  "$STAGE/libs/usergroup.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-atf}"

# ---------------------------------------------------------- the verdict ---
#
# The guest's own counters, not its exit status.  tcp_socket.c carries 21
# ATF_REQUIRE and atf_main.c counts one check each, so the floor is 18: under
# a whole run, because ATF_REQUIRE is fatal to its case and a case that stops
# early still prints a summary.
. "$ROOT/tools/test-verdict.sh"

RUNARG=()
[ -z "$CPU" ]   || RUNARG+=(-c "$CPU")
[ -z "$BOARD" ] || RUNARG+=(-N "$BOARD")
[ -z "$IFACE" ] || RUNARG+=(-B "$IFACE")

set +e
"$ROOT/tools/amiberry-run.sh" -m "$MODEL" -t "$TIMEOUT" "${RUNARG[@]}" \
     "$EXE" "$STAGE/devs" "$STAGE/libs"
RUN_RC=$?
set -e

verdict_guest "atf" 18 "$RUN_RC" \
    "$(verdict_hd_amiberry)/stdout.txt" \
    "$(verdict_serial_amiberry)"
exit $?
