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
TIMEOUT=120
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
for f in "$EXE" "$PREFS"; do
    [ -f "$f" ] || { echo "missing $f (build the cross tree first)" >&2; exit 2; }
done

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-netwireless}"

. "$ROOT/tools/test-verdict.sh"

set +e
"$ROOT/tools/amiberry-run.sh" -m "$MODEL" -t "$TIMEOUT" "$EXE" "$PREFS"
RUN_RC=$?
set -e

verdict_guest "netwireless" 13 "$RUN_RC" \
    "$(verdict_hd_amiberry)/stdout.txt" "$(verdict_serial_amiberry)" && exit 0
exit $?
