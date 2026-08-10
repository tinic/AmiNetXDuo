#!/usr/bin/env bash
#
# Run the real-HTTPS handshake under FS-UAE with an emulated A2065 on SLIRP.
#
#   tests/tls/run-https.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#
# Same staging as tests/netstack/run-fsuae.sh, DEVS:NetInterfaces/eth0,
# DEVS:Internet/* and a SANA-II a2065.device, because this test brings the
# whole stack up through netstack_startup() before it opens a socket.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/tls}"

while getopts "m:t:c:b:k:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir]" >&2; exit 2 ;;
    esac
done

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
    verdict_guest "tls-https" 1 "$1" \
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
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
         -a "OUT=DH0:tls.prof FOLDED=DH0:tls.folded tls_https" \
         "$PROF" "$EXE" "$STAGE/devs"
    RUN_RC=$?
    set -e
    verdict "$RUN_RC"
fi

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$EXE" "$STAGE/devs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
