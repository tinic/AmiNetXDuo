#!/usr/bin/env bash
#
# Run the IPv6 link test under Amiberry with an emulated A2065.
#
#   tests/ipv6/run-link.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                          [-N BOARD] [-B BACKEND]
#
# It was called run-fsuae.sh and both halves of it drove
# tools/amiberry-run.sh; -A picked between two branches running the same
# emulator.  fs-uae left the tree on 2026-08-04 and the name outlived it, the
# way tests/ipv6/run-socket.sh's did.
#
# -B says what the card is wired to.  Unset, tools/amiberry-run.sh's own
# default applies; this file no longer names SLIRP itself.
#
# Stages the same DEVS: tree tests/netstack uses, its DEVS:NetInterfaces/eth0
# names no IPv6 keyword at all, which is the point: the default (CONFIGURE6
# absent == AUTO) is what almost every real installation will run.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=240
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/v6}"
BOARD=a2065
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

EXE="$ROOT/$BUILD/tests/ipv6/ipv6_link_test"
[ -f "$EXE" ] || {
    echo "build $BUILD/tests/ipv6/ipv6_link_test first (-DAMINETXDUO_IPV6=ON)" >&2
    exit 2
}

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

STAGE="$ROOT/build/ipv6-link-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-v6link}"

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
    verdict_guest "ipv6-link" 8 "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

RUNARG=(-N "$BOARD")
[ -z "$CPU" ]   || RUNARG+=(-c "$CPU")
[ -z "$IFACE" ] || RUNARG+=(-B "$IFACE")

set +e
"$ROOT/tools/amiberry-run.sh" -m "$MODEL" -t "$TIMEOUT" "${RUNARG[@]}" \
     "$EXE" "$STAGE/devs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
