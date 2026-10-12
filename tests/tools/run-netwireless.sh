#!/usr/bin/env bash
#
# The NetWireless tools against tests/tools/wifistub.c, a Wi-Fi SANA-II unit
# the guest makes at run time.  Amiberry has no Pi Wi-Fi; this proves the
# device core, the scan that is never aborted, and the Shell output.
#
#   tests/tools/run-netwireless.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) sed -n '7p' "$0" >&2; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

EXE="$BUILD/tests/tools/WifiStub"
PREFS="$BUILD/src/tools/NetWirelessPrefs"
CX="$BUILD/src/tools/NetWireless"
WAITBREAK="$BUILD/tests/tools/WaitBreak"
WM="$BUILD/tests/tools/WmStub"
for f in "$EXE" "$PREFS" "$CX" "$WM" "$WAITBREAK"; do
    [ -f "$f" ] || { echo "missing $f (build the cross tree first)" >&2; exit 2; }
done

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-netwireless}"

# An interface file the way the installer writes one: DEVICE and UNIT after
# more than 2 KB of comments.
STAGE="$BUILD/netwireless-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/devs/NetInterfaces" "$STAGE/c"
cp "$WM" "$STAGE/c/WirelessManager"
{
    for i in $(seq 1 40); do
        echo "; comment line $i of the header the installer writes ahead of the keys"
    done
    echo "DEVICE=wifistub.device"
    echo "UNIT=0"
    echo "CONFIGURE=DHCP"
} > "$STAGE/devs/NetInterfaces/wifitest"
# The PC Card Wi-Fi, found by its driver's name when nothing is given.
printf '%s\n' "DEVICE=prism2.device" "UNIT=0" "CONFIGURE=DHCP" \
    > "$STAGE/devs/NetInterfaces/wifi0"
# A self-contained installation's own Devs drawer, assigned in the guest.
mkdir -p "$STAGE/anxd/Devs/NetInterfaces"
printf '%s\n' "DEVICE=prism2.device" "UNIT=0" \
    > "$STAGE/anxd/Devs/NetInterfaces/pcmcia"

. "$ROOT/tools/test-verdict.sh"

set +e
"$ROOT/tools/amiberry-run.sh" -m "$MODEL" -t "$TIMEOUT" "$EXE" "$PREFS" "$CX" \
    "$WAITBREAK" \
    "$STAGE/devs" "$STAGE/c" "$STAGE/anxd"
RUN_RC=$?
set -e

verdict_guest "netwireless" 59 "$RUN_RC" \
    "$(verdict_hd_amiberry)/stdout.txt" "$(verdict_serial_amiberry)" && exit 0
exit $?
