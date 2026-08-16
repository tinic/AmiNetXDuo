#!/usr/bin/env bash
#
# Run the AF_INET6 bsdsocket.library test, RFC 3542's whole surface.
#
#   tests/ipv6/run-socket.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                            [-N BOARD] [-B BACKEND]
#
# It was called run-socket-fsuae.sh and both halves of it drove
# tools/amiberry-run.sh.  -A picked between two branches that ran the same
# emulator.  fs-uae left the tree on 2026-08-04 and the name outlived it.
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
BOARD=a2065
# Both defaults are tools/amiberry-run.sh's own, repeated here only so -N and
# -B can override them.  With AMINETXDUO_AMIBERRY_BACKEND unset this is SLIRP,
# which is what the test wants: it talks to ::1 and needs no LAN.
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"

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
    verdict_guest "ipv6-socket" 120 "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
     -t "$TIMEOUT" "${CPUARG[@]}" "$EXE" "$STAGE/devs" "$STAGE/libs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
