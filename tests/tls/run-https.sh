#!/usr/bin/env bash
# Run the real-HTTPS handshake under Amiberry on a bridged card.
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

cat > "$STAGE/devs/Internet/name_resolution" <<'NREOF'
# No nameserver line: the DHCP lease on the bridge carries one, and a second
# server this file cannot reach costs the resolver's failover time on every
# lookup before it gets used.
domain localdomain
NREOF

. "$ROOT/tools/sana2-stage.sh"
if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
    _want=$(sana2_driver_for "$BOARD")
    _have=$(sana2_local_driver "$_want")
    [ -n "$_have" ] && [ -f "$_have" ] &&
        export AMINETXDUO_SANA2_DRIVER="$_have"
fi
sana2_stage "$BOARD" "$STAGE/devs"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-tlshttps}"

. "$ROOT/tools/test-verdict.sh"

verdict() {
    verdict_guest "tls-https" 23 "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

CPUARG=()
[ -z "$CPU" ] || CPUARG+=(-c "$CPU")
[ -z "${CLOCK:-}" ] || CPUARG+=(-k "$CLOCK")

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
