#!/usr/bin/env bash
#
# A DHCP lease against a Wi-Fi association that comes late: tests/tools/
# wifigate.c, a unit in front of the A2065 that drops every frame until it
# associates, ASSOC seconds after AddNetInterface started it.  The lease
# must come inside LIMIT seconds of the association.
#
#   tests/tools/run-wifigate.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                               [-A ASSOC] [-L LIMIT] [-R LEAVE]
#
# SLIRP: its DHCP server answers at once, so the time to the lease is the
# client's alone.  Output is key=value; the verdict is the exit code.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=420
BUILD="${AMINETXDUO_BUILD:-build/cm}"
ASSOC=70
LIMIT=15
LEAVE=0

while getopts "m:t:b:A:L:R:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        A) ASSOC="$OPTARG" ;;
        L) LIMIT="$OPTARG" ;;
        R) LEAVE="$OPTARG" ;;
        *) sed -n '8,9p' "$0" >&2; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

EXE="$BUILD/tests/tools/WifiGate"
TOOLS="$BUILD/src/tools"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
for f in "$EXE" "$TOOLS/AddNetInterface" "$TOOLS/GetNetStatus" \
         "$TOOLS/NetShutdown" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f (build the cross tree first)" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" \
                     "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

STAGE="$BUILD/wifigate-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
rm -f "$STAGE/devs/NetInterfaces/"*
cp "$A2065" "$STAGE/devs/a2065.device"
printf '%s\n' "DEVICE=wifigate.device" "UNIT=0" "CONFIGURE=DHCP" \
    > "$STAGE/devs/NetInterfaces/wifi0"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface GetNetStatus NetShutdown; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-wifigate}"

. "$ROOT/tools/test-verdict.sh"

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
    -a "IFACE=wifi0 ASSOC=$ASSOC LIMIT=$LIMIT LEAVE=$LEAVE" \
    "$EXE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
    "$STAGE/GetNetStatus" "$STAGE/NetShutdown"
RUN_RC=$?
set -e

OUT="$(verdict_hd_amiberry)/stdout.txt"
[ -f "$OUT" ] && grep -E '^[a-z_]+=' "$OUT" || true
verdict_guest "wifigate" 5 "$RUN_RC" "$OUT" "$(verdict_serial_amiberry)" && exit 0
exit $?
