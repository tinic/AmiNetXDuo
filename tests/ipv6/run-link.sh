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
# BRIDGED, NEVER SLIRP.  -B named nothing, so an unset one fell through to
# tools/amiberry-run.sh's own default, which is SLIRP: the file stopped saying
# the word and went on running there.  The default is the bridge now and the
# string `slirp` is refused outright.
#
# It matters here more than most.  Half of what ipv6_link_test reports is what
# a ROUTER did -- whether one advertised itself, what prefix it offered, which
# address it left as the default -- and those are findings rather than checks
# precisely because a link may have no router.  On a real segment there is one,
# so the findings carry information; behind NAT they are a fixed nothing.
#
# Stages the same DEVS: tree tests/netstack uses, its DEVS:NetInterfaces/eth0
# names no IPv6 keyword at all, which is the point: the default (CONFIGURE6
# absent == AUTO) is what almost every real installation will run.
#
# -N PICKS THE BOARD, and its driver is staged to match: see sana2_stage below.
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.  Every other board's driver comes out
# of AMINETXDUO_SANA2_STORE or ~/amiga-assets/devs.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=240
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/v6}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

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

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "ipv6-link_backend=refused:$IFACE" >&2
        echo "This harness is bridged only.  -B names a host interface." >&2
        exit 2
        ;;
esac

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

# -N puts a board in the machine; this puts its driver in DEVS: and its name
# in DEVICE=.  Without it the shared interface file keeps DEVICE=a2065.device
# whatever -N asked for, so every other board opens a2065.device against
# hardware that is not there.
. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"

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

RUNARG=(-N "$BOARD" -B "$IFACE")
[ -z "$CPU" ] || RUNARG+=(-c "$CPU")

set +e
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -m "$MODEL" -t "$TIMEOUT" "${RUNARG[@]}" \
     "$EXE" "$STAGE/devs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
