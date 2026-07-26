#!/usr/bin/env bash
#
# Run the ported curl under FS-UAE, on SLIRP, against real URLs.
#
#   clients/curl/run-fsuae.sh [-m MODEL] [-t SECONDS] [-c CPU] [-k MHZ]
#                             [-b STACKBUILD] [-C CURLBUILD]
#
# Same shape as tests/tls/run-fetch.sh -- stage a2065.device, the interface
# configuration, bsdsocket.library and a driver that reads a command list --
# with two differences that are the whole reason this is a separate script:
#
#   * The driver is clients/curl/clientrun.c, not ToolsSmoke, because curl
#     needs a 256 KB stack and a Shell gives 4 KB.  See that file.
#
#   * mathieeedoubbas.library has to be staged.  curl uses doubles (the
#     progress meter, --max-time, --write-out), this toolchain's newlib
#     implements double arithmetic by calling that library, and it is NOT in
#     Kickstart 3.1 ROM -- verified against the 40.68 A1200 image, which
#     contains mathieeesingbas and nothing else.  A real Amiga has it in
#     LIBS: because every Workbench install ships it; a bare directory hard
#     drive does not.  Point AMINETXDUO_MATHIEEEDOUBBAS at one, or drop a
#     copy in build/mathieeedoubbas.library.
#
# NOT A BASELINE: it depends on the internet and on FS-UAE's SLIRP NAT.
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
CLOCK=""
STACK_BUILD="${AMINETXDUO_BUILD:-build/cm}"
CURL_BUILD="build/curl"

while getopts "m:t:c:k:b:C:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        b) STACK_BUILD="$OPTARG" ;;
        C) CURL_BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-k MHz] [-b stackbuild] [-C curlbuild]" >&2; exit 2 ;;
    esac
done

CURL="$ROOT/$CURL_BUILD/src/curl"
BSD="$ROOT/$STACK_BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$ROOT/$STACK_BUILD/src/tools/AddNetInterface"

for f in "$CURL" "$BSD" "$ADDIF"; do
    [ -f "$f" ] || { echo "missing $f -- build it first" >&2; exit 2; }
done

# ------------------------------------------------------------ the driver ---

. "$ROOT/tools/amiga-toolchain.sh"

RUNNER="$ROOT/build/clients/ClientRun"
mkdir -p "$ROOT/build/clients"
if [ ! -x "$RUNNER" ] || [ "$ROOT/clients/curl/clientrun.c" -nt "$RUNNER" ]; then
    echo "==> building ClientRun"
    "$AMIGA_GCC" -O2 -m68020 -fomit-frame-pointer -Wall -I"$AMIGA_NDK" \
                 -o "$RUNNER" "$ROOT/clients/curl/clientrun.c"
fi

# ------------------------------------------------------------- a2065 -------

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

# ------------------------------------------------ mathieeedoubbas.library --

MATH="${AMINETXDUO_MATHIEEEDOUBBAS:-}"
if [ -z "$MATH" ]; then
    for candidate in \
        "$ROOT/build/mathieeedoubbas.library" \
        "$HOME/amigaos/libs/mathieeedoubbas.library"
    do
        [ -f "$candidate" ] && { MATH="$candidate"; break; }
    done
fi

# ------------------------------------------------------------- staging ----

STAGE="$ROOT/build/curl-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$CURL"  "$STAGE/curl"
cp "$ADDIF" "$STAGE/AddNetInterface"

if [ -n "$MATH" ] && [ -f "$MATH" ]; then
    cp "$MATH" "$STAGE/libs/mathieeedoubbas.library"
    echo "==> mathieeedoubbas.library staged ($(wc -c < "$MATH" | tr -d ' ') bytes)"
else
    echo "!! NO mathieeedoubbas.library staged." >&2
    echo "   curl references it (newlib's double arithmetic does), it is not" >&2
    echo "   in Kickstart 3.1 ROM, and the startup opens it before main()." >&2
    echo "   Set AMINETXDUO_MATHIEEEDOUBBAS=<path> or drop one in build/." >&2
fi

if [ -n "${AMINETXDUO_CURL_COMMANDS:-}" ]; then
    cp "$AMINETXDUO_CURL_COMMANDS" "$STAGE/commands.txt"
    echo "==> command list: $AMINETXDUO_CURL_COMMANDS"
else
cat > "$STAGE/commands.txt" <<'EOF'
SYS:curl --version
SYS:AddNetInterface eth0
SYS:curl -sS -i http://example.com/
EOF
fi

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-curl}"

CPUARG=()
[ -z "$CPU" ]   || CPUARG+=(-c "$CPU")
[ -z "$CLOCK" ] || CPUARG+=(-k "$CLOCK")

exec "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
     "$RUNNER" "$STAGE/devs" "$STAGE/libs" "$STAGE/curl" \
     "$STAGE/AddNetInterface" "$STAGE/commands.txt"
