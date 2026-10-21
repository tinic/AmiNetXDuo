#!/usr/bin/env bash
# Run the AF_INET6 bsdsocket.library test, RFC 3542's whole surface.
# SPDX-License-Identifier: MIT

set -euo pipefail

# The interface this harness uses names a SANA-II device the guest
# creates at run time, so no boot script can bring it up; the guest
# names it itself once the device exists.  See tap_bring_up().

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=240
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/v6}"
NOTAP=0
ADDRCONFIG=ipv4
GUEST_ARG=addrconfig4
MIN_CHECKS=120

while getopts "m:t:c:b:na:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        n) NOTAP=1 ;;
        a) ADDRCONFIG="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir] [-n] [-a ipv4|ula|linklocal|ipv6]" >&2
           exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/tests/ipv6/ipv6_socket_test"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"

for f in "$EXE" "$BSD" "$UG"; do
    [ -f "$f" ] || { echo "missing $f (build with -DAMINETXDUO_IPV6=ON)" >&2; exit 2; }
done

STAGE="$ROOT/build/ipv6-socket-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/tcpdrill/devs" "$STAGE/devs"
# Explicit address modes make ADDRCONFIG independent of AUTO/DAD timing.
case "$ADDRCONFIG" in
    ipv4) echo 'CONFIGURE6=OFF' >> "$STAGE/devs/NetInterfaces/tap0" ;;
    ula)
        printf '\nCONFIGURE6=STATIC\nADDRESS6=fd00::10/64\n' >> "$STAGE/devs/NetInterfaces/tap0"
        GUEST_ARG=addrconfig6 ;;
    linklocal)
        # A link-local address alone does not count for AI_ADDRCONFIG.
        echo 'CONFIGURE6=LINKLOCAL' >> "$STAGE/devs/NetInterfaces/tap0"
        GUEST_ARG=addrconfig4 ;;
    ipv6)
        cat > "$STAGE/devs/NetInterfaces/tap0" <<'IFACE'
DEVICE=tcpdrill.device
UNIT=0
CONFIGURE=NONE
CONFIGURE6=STATIC
ADDRESS6=fd00::10/64
IFACE
        GUEST_ARG=addrconfig6only
        MIN_CHECKS=18 ;;
    *) echo "unknown address mode: $ADDRCONFIG" >&2; exit 2 ;;
esac

# -n: no tap0 configuration, as on a real machine.  The test has to finish
# on its own there, not wait for a packet only tap0 would loop back.
if [ "$NOTAP" = 1 ]; then
    rm -f "$STAGE/devs/NetInterfaces/tap0"
fi
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$UG"  "$STAGE/libs/usergroup.library"

if [ "$NOTAP" = 1 ]; then
    export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-v6sock-notap}"
else
    export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-v6sock}"
fi

. "$ROOT/tools/test-verdict.sh"

verdict() {
    verdict_guest "ipv6-socket" "$MIN_CHECKS" "$1" \
        "$(verdict_hd_amiberry)/stdout.txt" \
        "$(verdict_serial_amiberry)" && exit 0
    exit $?
}

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

set +e
"$ROOT/tools/amiberry-run.sh" -m "$MODEL" \
     -t "$TIMEOUT" "${CPUARG[@]}" -a "$GUEST_ARG" "$EXE" "$STAGE/devs" "$STAGE/libs"
RUN_RC=$?
set -e
verdict "$RUN_RC"
