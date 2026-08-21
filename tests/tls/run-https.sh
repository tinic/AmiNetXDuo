#!/usr/bin/env bash
#
# Run the real-HTTPS handshake under Amiberry on a bridged card.
#
#   tests/tls/run-https.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                          [-N BOARD] [-B IFACE]
#
# Same staging as tests/netstack/run-amiberry.sh, DEVS:NetInterfaces/eth0,
# DEVS:Internet/* and a SANA-II driver, because this test brings the whole
# stack up through netstack_startup() before it opens a socket.
#
# BRIDGED, NEVER SLIRP.  This harness had no -B at all, so it inherited
# amiberry-run.sh's slirp default and AMINETXDUO_AMIBERRY_BACKEND was the only
# way to put it on a wire.  It reaches a public HTTPS server, which is the one
# thing NAT through a stub cannot do: the run could only burn its ceiling.  -B
# names the host NIC and the string `slirp` is refused by name.
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
TIMEOUT=300
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/tls}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"

while getopts "m:t:c:b:k:N:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir] [-N board] [-B backend]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "tls-https_backend=refused:$IFACE" >&2
        echo "This harness fetches a real HTTPS URL and is bridged only." >&2
        echo "-B names a host interface." >&2
        exit 2
        ;;
esac

EXE="$ROOT/$BUILD/tests/tls/tls_https"
[ -f "$EXE" ] || { echo "build $BUILD/tests/tls/tls_https first" >&2; exit 2; }

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

STAGE="$ROOT/build/tls-https-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

# The shared fixture names SLIRP's forwarder, 10.0.2.3, which is nothing on a
# real segment: the resolver waits it out before it falls back on the server
# the lease carried, thirty seconds per lookup.  The lease is the only source
# here, so the file names none.
cat > "$STAGE/devs/Internet/name_resolution" <<'NREOF'
# No nameserver line: the DHCP lease on the bridge carries one, and naming
# SLIRP's dead 10.0.2.3 here costs the resolver's whole failover time on every
# lookup before it gets used.
domain localdomain
NREOF

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

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-tlshttps}"

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
    # A FLOOR OF 1 IS NOT A FLOOR.  23 is the count of a clean bridged run,
    # A1200 on ens18, 2026-08-20 -- measured, not read off the w_check() calls
    # in tls_https.c.  A run that makes fewer stopped somewhere.
    verdict_guest "tls-https" 23 "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

CPUARG=()
[ -z "$CPU" ] || CPUARG+=(-c "$CPU")
[ -z "${CLOCK:-}" ] || CPUARG+=(-k "$CLOCK")

# AMINETXDUO_PROFILE=1 runs the same thing under tools/profiler/Profile, which
# needs no recompilation of the target: the profile lands on the drive as
# DH0:tls.prof for tools/profiler/profreport.py.
if [ "${AMINETXDUO_PROFILE:-0}" = "1" ]; then
    case "$BUILD" in /*) PROF="$BUILD/tools/profiler/Profile" ;;
                      *) PROF="$ROOT/$BUILD/tools/profiler/Profile" ;; esac
    [ -x "$PROF" ] || { echo "build the Profile target first" >&2; exit 2; }
    set +e
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
         -t "$TIMEOUT" "${CPUARG[@]}" \
         -a "OUT=DH0:tls.prof FOLDED=DH0:tls.folded tls_https" \
         "$PROF" "$EXE" "$STAGE/devs"
    RUN_RC=$?
    set -e
    verdict "$RUN_RC"
fi

set +e
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
     -t "$TIMEOUT" "${CPUARG[@]}" \
     "$EXE" "$STAGE/devs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
