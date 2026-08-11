#!/usr/bin/env bash
#
# HOW FAR A SOCKET GETS TOWARDS AN SMB SERVER, from a booted guest.
#
#   tests/tools/run-smbprobe.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                               [-N board] [-B backend] [-a address]
#                               [-s SMBHOST] [-r TRANSCRIPT]
#
# GitHub #3 says opening SMB2 to the LAN never responds; #4 says mounting SMB
# freezes the machine.  This is the bottom half of both questions and only the
# bottom half: ping, then `nc` to 445 and to 139, then the connection table.
# It shows that a socket to the server opens, which is as far as anything that
# is not a real Workbench can go -- whatever fails in those reports happens
# ABOVE the handshake, inside smb2-handler, and install/test/run-smbmount.sh is
# the gate for that.
#
# IT ASSERTS NOTHING.  The transcript is the result and the exit status is 0
# whatever the guest printed, which is why tests/HARNESSES files it as a bench
# rather than as coverage: there is nothing here that can go red.  Read it when
# run-smbmount.sh fails and the question is whether the packets got out at all.
#
# BRIDGED, always: the server is a machine on the LAN.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=180
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
IFDEVICE="${AMINETXDUO_IFDEVICE:-a2065.device}"
ADDRESS=192.168.1.241
SMBHOST="${AMINETXDUO_SMB_HOST:-192.168.1.72}"
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
REPLAY=""

while getopts "m:t:b:N:B:a:g:s:r:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        s) SMBHOST="$OPTARG" ;;
        # Print a transcript that already exists instead of booting.
        r) REPLAY="$OPTARG" ;;
        *) sed -n '5,7p' "$0" >&2; exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="${BUILD#./}" ;; esac

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

# Its own tag, and its own staging directory.  Both were run-netshutdown.sh's
# when this file was cut from it, so the two clobbered each other's drive and
# stage the moment anything ran them together.
export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-smbprobe}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"
RUN_RC=0

if [ -n "$REPLAY" ]; then
    [ -f "$REPLAY" ] || { echo "no such transcript: $REPLAY" >&2; exit 2; }
    REPORT="$REPLAY"
    echo "==> REPLAY of $REPORT: nothing was run"
else

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/netstat" \
         "$TOOLS/ping" "$TOOLS/nc" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" \
                     "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/smbprobe-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"

# A board other than the a2065, the way run-httpd.sh takes one.
. "$ROOT/tools/sana2-stage.sh"
if [ "$BOARD" != a2065 ]; then
    if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ]; then
        _want=$(sana2_driver_for "$BOARD")
        _have=$(sana2_local_driver "$_want")
        [ -n "$_have" ] && [ -f "$_have" ] &&
            export AMINETXDUO_SANA2_DRIVER="$_have"
    fi
    sana2_stage "$BOARD" "$STAGE/devs"
    IFDEVICE="$SANA2_DEVICE"
    echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"
fi

# Static: a lease that arrives late would make "the interface is up" a question
# about the lab's DHCP server rather than about the server being probed.
cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=$IFDEVICE
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
EOF

for t in AddNetInterface netstat ShowNetStatus ping nc; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:netstat -i
SYS:ping $SMBHOST -c 2 -t 10
SYS:nc -v -w 15 $SMBHOST 445
SYS:nc -v -w 15 $SMBHOST 139
SYS:netstat -a
EOF

# ------------------------------------------------------------------ run ---

echo "==> booting $MODEL under Amiberry, $BOARD bridged on $BACKEND"
echo "==> probing $SMBHOST"
set +e
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$BACKEND" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/netstat" "$STAGE/ShowNetStatus" \
    "$STAGE/ping" "$STAGE/nc"
RUN_RC=$?
set -e

fi  # not a replay

if [ ! -f "$REPORT" ]; then
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    [ "$RUN_RC" = 124 ] &&
        echo "       rc 124 is the ${TIMEOUT}s timeout: the machine never" \
             "got as far as writing one." >&2
    exit 1
fi

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

echo "This probe asserts nothing: the transcript above is the result."
exit 0
