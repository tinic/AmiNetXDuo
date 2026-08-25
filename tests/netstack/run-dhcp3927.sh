#!/usr/bin/env bash
# Run the DHCP lifecycle / RFC 3927 test under FS-UAE on SLIRP.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/netstack"
MODEL=A1200
TIMEOUT=420
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
CONFIGURE=DHCP
MODE=lease

while getopts "m:t:c:b:C:M:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        C) CONFIGURE="$OPTARG" ;;
        M) MODE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t secs] [-c cpu] [-b dir] [-C configure] [-M mode]" >&2
           exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/tests/netstack/dhcp3927_test"
[ -f "$EXE" ] || { echo "build $BUILD/tests/netstack/dhcp3927_test first" >&2; exit 2; }

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

STAGE="$ROOT/build/dhcp3927-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/env"
cp -R "$HERE/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=$CONFIGURE
EOF

printf '%s' "$MODE" > "$STAGE/env/DHCP3927MODE"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-dhcp3927}"

. "$ROOT/tools/test-verdict.sh"

verdict() {
    verdict_guest "dhcp3927" 30 "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

echo "==> CONFIGURE=$CONFIGURE, DHCP3927MODE=$MODE"

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$EXE" "$STAGE/devs" "$STAGE/env"
RUN_RC=$?
set -e
verdict "$RUN_RC"
