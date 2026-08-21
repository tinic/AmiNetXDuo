#!/usr/bin/env bash
#
# Run the full-link crypto68k LTO regression probe, without TLS traffic.
#
#   -DAMINETXDUO_CRYPTO68K_LTO_PROBE=ON
#
# The executable only opens tls.library.  Library init selects the CPU and
# runs c68k_p256_self_check(), so the run needs an A1200 ROM and nothing else:
# no bsdsocket.library, interface, driver, network or peer.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="${AMINETXDUO_BUILD:-build/cm}"
TIMEOUT=60

while getopts "b:t:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-t seconds]" >&2; exit 2 ;;
    esac
done

PROBE="$ROOT/$BUILD/tests/crypto68k/crypto68k_lto_probe"
TLSLIB="$ROOT/$BUILD/src/tlslib/tls.library"
CACHE="$ROOT/$BUILD/CMakeCache.txt"

if [ ! -f "$CACHE" ] ||
   ! grep -qx 'AMINETXDUO_CRYPTO68K_LTO_PROBE:BOOL=ON' "$CACHE" ||
   ! grep -qx 'AMINETXDUO_LTO:BOOL=ON' "$CACHE"; then
    echo "lto_probe_refused=configure_with_probe_and_lto"
    echo "RESULT=refused"
    exit 2
fi

for path in "$PROBE" "$TLSLIB"; do
    [ -f "$path" ] || {
        echo "lto_probe_refused=missing_$(basename "$path")"
        echo "RESULT=refused"
        exit 2
    }
done

echo "lto_probe_crypto_lto=ON"

STAGE="$ROOT/$BUILD/crypto68k-lto-probe-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp "$TLSLIB" "$STAGE/libs/tls.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-c68k-lto-probe}"
set +e
"$ROOT/tools/amiberry-run.sh" -m A1200 -t "$TIMEOUT" "$PROBE" "$STAGE/libs"
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
    echo "lto_probe=PASS"
    echo "RESULT=pass"
    exit 0
fi

echo "lto_probe=FAIL rc=$rc"
echo "RESULT=fail"
exit 1
