#!/usr/bin/env bash
#
# A LIVE AMIGA ON THE LAN, IN ONE COMMAND.
#
#   tools/demo.sh [-b BUILDDIR] [-B BACKEND] [-m MODEL] [-n NAME] [-p PORT]
#                 [-t SECONDS]
#
# Boots an emulated Amiga bridged onto the real network, running httpd with the
# WebSocket terminal, and prints the address it leased.  For showing somebody
# the thing rather than testing it: nothing here asserts anything.
#
#   http://<address>/           the Public drawer, WebDAV-writable
#   http://<address>/terminal   an AmigaDOS Shell in a browser, NO PASSWORD
#   http://amiga.local/         the same machine by name, -n renames it
#
# The interface is staged with MDNS=YES and the drive with a hostname, because
# neither is a default: a demo reached only by its DHCP lease is one somebody
# has to be told the address of again tomorrow.
#
# WHY BRIDGED AND NOT SLIRP
#
#   A demo has to be reachable from the person's own machine.  tests/tools/
#   run-wsterm.sh forwards a port out of slirp instead, which is right for a
#   test and useless for showing anyone.
#
# THE MAC IS NOT THE ONE YOU ASK FOR
#
#   amiberry-run.sh takes AMINETXDUO_AMIBERRY_MAC, and the a2065's LANCE then
#   reports a DIFFERENT address on the wire -- 00:80:10:49:xx:xx, derived from
#   the unit.  Looking for the MAC you set finds nothing, which is a good way
#   to lose fifteen minutes.  This script reads the one the emulator logged.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-build/cm}"
BACKEND="${AMINETXDUO_DEMO_BACKEND:-ens18}"
MODEL=A1200
PORT=80
WINDOW=28800
NAME="${AMINETXDUO_DEMO_NAME:-amiga}"

while getopts "b:B:m:n:p:t:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        n) NAME="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        t) WINDOW="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-B backend] [-m model] [-n name]" \
                "[-p port] [-t seconds]" >&2; exit 2 ;;
    esac
done

# The asset store carries the ROMs and the drivers, and exports the Kickstart
# each model needs.  Forgetting it boots a machine with no ROM, which fails
# with a message about the ROM and nothing about the cause.
[ -f "$HOME/amiga-assets/env.sh" ] && . "$HOME/amiga-assets/env.sh"

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
PAGE="$ROOT/src/tools/web/terminal.html"
A2065="${AMINETXDUO_A2065:-$HOME/amiga-assets/devs/a2065.device}"

for f in "$TOOLS/httpd" "$BSD" "$PAGE" "$A2065"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first, or set AMINETXDUO_A2065" >&2; exit 2; }
done

# ------------------------------------------------------------- staging ---
# The shape tests/tools/run-wsterm.sh stages, which is the shape
# tests/tools/run-httpd.sh stages before it.  Never hand-assemble one.

STAGE="$ROOT/build/demo-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/Public/Docs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
# fetch needs tls.library for https, and tls.library needs a trust store, or
# every https URL fails with something that reads like a broken download.
[ -f "$ROOT/$BUILD/src/tlslib/tls.library" ] &&
    cp "$ROOT/$BUILD/src/tlslib/tls.library" "$STAGE/libs/"
mkdir -p "$STAGE/devs/Internet"
[ -f "$ROOT/third_party/cacert/cacert.pem" ] &&
    cp "$ROOT/third_party/cacert/cacert.pem" "$STAGE/devs/Internet/certificates"
cp "$PAGE"  "$STAGE/terminal.html"

# MDNS=YES so the machine is reachable by name.  A demo whose address is a
# DHCP lease is a demo somebody has to be told the address of again tomorrow.
# MDNS= is per interface and defaults to off, so this line is the whole of
# turning it on.
cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
MDNS=YES
EOF

# And a name to answer to.  The staged name_resolution has none, so without
# this the responder claims whatever DHCP or the interface ID produced and the
# address printed below is the only way anyone reaches it.
echo "hostname $NAME" >> "$STAGE/devs/Internet/name_resolution"

echo "Hello from an Amiga." > "$STAGE/Public/readme.txt"
echo "<html><body><h1>Amiga</h1><p>httpd is serving this drawer.</p></body></html>" > "$STAGE/Public/index.html"
echo "in a drawer" > "$STAGE/Public/Docs/notes.txt"

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-demo}"
EMU="$ROOT/build/amiberry-demo.log"

echo "==> booting $MODEL on '$BACKEND', httpd :$PORT, window ${WINDOW}s"

"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$BACKEND" -m "$MODEL" -t "$WINDOW" \
    -a "DH0:Public $PORT TERMINAL=DH0:terminal.html" \
    "$TOOLS/httpd" "$STAGE/devs" "$STAGE/libs" "$STAGE/Public" \
    "$STAGE/terminal.html" > "$ROOT/build/demo-run.log" 2>&1 &
RUNNER=$!

# The address, from the wire.  The guest announces itself by ARP as soon as it
# has a lease; the MAC to watch for is the one the emulator logged, not the one
# we asked for.  A release build says nothing on the serial line, so this is
# the only place the address appears.
MAC=""
for _ in $(seq 1 60); do
    sleep 2
    MAC=$(grep -oE "7990: '[^']*' ([0-9a-f]{2}:){5}[0-9a-f]{2}" "$EMU" 2>/dev/null |
          tail -1 | grep -oE "([0-9a-f]{2}:){5}[0-9a-f]{2}" || true)
    [ -n "$MAC" ] && break
done
[ -n "$MAC" ] || { echo "the emulator never reported a MAC; see $EMU" >&2; exit 1; }

echo "==> guest MAC $MAC, waiting for a lease"
ADDR=""
for _ in $(seq 1 40); do
    ADDR=$(timeout 10 tcpdump -i "$BACKEND" -n -c 1 "ether host $MAC and arp" 2>/dev/null |
           grep -oE "ARP, Reply [0-9.]+" | grep -oE "[0-9.]+$" || true)
    [ -n "$ADDR" ] && break
done

if [ -z "$ADDR" ]; then
    echo "no lease seen for $MAC on $BACKEND after 400s" >&2
    echo "the emulator is still running as pid $RUNNER; see $EMU" >&2
    exit 1
fi

# -p left the address right and the URL wrong until 2026-08-10: a demo on any
# port but 80 printed one nothing answers on.
HOSTPART="$ADDR"
[ "$PORT" = 80 ] || HOSTPART="$ADDR:$PORT"
NAMEPART="$NAME.local"
[ "$PORT" = 80 ] || NAMEPART="$NAME.local:$PORT"

cat <<EOF

  the drawer    http://$HOSTPART/
  the terminal  http://$HOSTPART/terminal      no password, anyone who can reach it

  by name       http://$NAMEPART/terminal      mDNS, once the responder has claimed it

  emulator pid $RUNNER, log $EMU
EOF
wait "$RUNNER"
