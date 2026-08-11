#!/usr/bin/env bash
#
# A LIVE AMIGA ON THE LAN, IN ONE COMMAND.
#
#   tools/demo.sh [-b BUILDDIR] [-B BACKEND] [-C CMDDIR] [-m MODEL] [-n NAME]
#                 [-p PORT] [-t SECONDS]
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
# A SHELL WITH NO COMMANDS IN IT
#
#   The drive amiberry-run.sh builds carries httpd and nothing else, so the
#   Shell on the far end of the terminal answers `Dir` with "Unknown command"
#   and there is nothing to show anybody.  -C stages a commands drawer into
#   C:, and AMINETXDUO_DEMO_C is the same thing from the environment.  Where
#   they come from is a licensed Workbench and not ours to ship; the lab store
#   has the ADFs and amitools' xdftool unpacks one:
#
#     xdftool ~/amiga-assets/adf-wb31/amiga-wb31_workbench.adf unpack /tmp/wb
#     tools/demo.sh -C /tmp/wb/Workbench/C
#
#   Without it the demo still runs and the terminal still works.  It is a
#   Shell with 0 commands instead of 81.
#
# BRIDGED, OR SLIRP WITH A FORWARDED PORT
#
#   Bridged is the default and is what a demo is for: the machine appears on
#   the real network with a lease of its own and anyone can reach it.
#
#   `-B slirp` is the other one.  It needs no bridge, no root and no address
#   on the LAN -- the guest is behind NAT and one port is forwarded out to
#   127.0.0.1 -- which is what to use when the LAN is somebody else's, or when
#   a demo is ALREADY RUNNING on it.
#
#   Two bridged guests are not two machines.  The a2065's LANCE derives its
#   address from the unit, so both are 00:80:10:49:00:01 and the network sees
#   one host answering from two places: leases fight, ARP caches flap, and the
#   demo that was already up stops answering.  Second instance, -B slirp.
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
NAME="${AMINETXDUO_DEMO_NAME:-}"
CMDS="${AMINETXDUO_DEMO_C:-}"

while getopts "b:B:C:m:n:p:t:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        C) CMDS="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        n) NAME="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        t) WINDOW="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-B backend] [-C cmddir]" \
                "[-m model] [-n name] [-p port] [-t seconds]" >&2; exit 2 ;;
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

STAGE="$ROOT/build/demo-stage-${AMINETXDUO_RUN_TAG:-demo}"
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
# Every tool we build, so the terminal can drive the stack it is running on.
# The -C drawer is Workbench's commands; these are ours, and without them a
# Shell can list a directory and do nothing else with the network.
for t in "$TOOLS"/*; do
    [ -f "$t" ] && [ -x "$t" ] || continue
    case "$(basename "$t")" in
        *.map|*.cmake|Makefile|ToolsSmoke|*Probe) continue ;;
    esac
    cp -f "$t" "$STAGE/c/" 2>/dev/null || true
done

# dbclient needs the IEEE double libraries, which a real Workbench has in LIBS:
# and a staged drive does not.  Without them ssh loads and dies with
# "mathieeedoubbas.library failed to load", which reads like a broken binary.
for m in mathieeedoubbas mathieeedoubtrans; do
    for src in "$HOME/amiga-assets/nglibs/$m.library" "$HOME/amiga-assets/libs/$m.library"; do
        [ -f "$src" ] && { cp -f "$src" "$STAGE/libs/"; break; }
    done
done

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

# Our own name_resolution, never the one under tests/netstack: that file is
# written for a SLIRP guest and carries "nameserver 10.0.2.3" and "domain
# localdomain", neither of which exists on a real network.  A demo that
# inherits it reports a name server it cannot reach and calls itself
# amiga.localdomain while answering to amiga.local.
#
# Empty by default, deliberately.  DHCP supplies the name servers and the
# domain, and an unnamed machine names itself after its card -- which is the
# behaviour worth showing.  -n forces a name and outranks that.
: > "$STAGE/devs/Internet/name_resolution"
[ -z "$NAME" ] || echo "hostname $NAME" >> "$STAGE/devs/Internet/name_resolution"

# Workbench's own commands, ADDED to the drawer our tools are already in.
#
# Not replacing it.  This block used to `rm -rf "$STAGE/c"` before copying,
# from when the drawer held nothing else; the loop above now puts every tool
# we build there, and deleting it took all of them with it.  Ours are copied
# again afterwards so that a name in both drawers resolves to ours -- Version
# and Which exist on both sides and the interesting one is not Commodore's.
#
# amiberry-run.sh merges an extra drawer into one of that name on the drive,
# so the whole thing arrives as C:.
if [ -n "$CMDS" ]; then
    [ -d "$CMDS" ] || { echo "no such commands drawer: $CMDS" >&2; exit 2; }
    cp -R "$CMDS"/. "$STAGE/c/"
    chmod -R u+rw "$STAGE/c"
    for t in "$TOOLS"/*; do
        [ -f "$t" ] && [ -x "$t" ] || continue
        case "$(basename "$t")" in
            *.map|*.cmake|Makefile|ToolsSmoke|*Probe) continue ;;
        esac
        cp -f "$t" "$STAGE/c/" 2>/dev/null || true
    done
else
    echo "==> no Workbench commands (-C); C: has our tools and nothing else" >&2
fi

EXTRA_C=("$STAGE/c")
echo "==> C: has $(ls "$STAGE/c" | wc -l | tr -d ' ') commands in it"

echo "Hello from an Amiga." > "$STAGE/Public/readme.txt"
echo "<html><body><h1>Amiga</h1><p>httpd is serving this drawer.</p></body></html>" > "$STAGE/Public/index.html"
echo "in a drawer" > "$STAGE/Public/Docs/notes.txt"

# ------------------------------------------------------------------ run ---

# A MAC of our own, set here because ~/amiga-assets/env.sh exports the lab's
# AMINETXDUO_AMIBERRY_MAC and sourcing it above would otherwise win.  Every
# default-configured a2065 on the segment comes up as 00:80:10:49:00:01, so a
# demo and a benchmark bridged on one wire collide and the demo never reaches
# it -- httpd binds 0.0.0.0 and reports itself happily, which is why that reads
# as stuck rather than as unplugged.  Cost three restarts on 2026-08-10.
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_DEMO_MAC:-02:41:4d:49:00:77}"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-demo}"

# Behind NAT the guest is always 10.0.2.15 and the port has to be forwarded
# out; the same line tests/tools/run-wsterm.sh uses.  HOSTPORT is the port on
# THIS machine, and defaults to the guest's so the printed URL is the one
# asked for whenever it is free to be.
if [ "$BACKEND" = slirp ]; then
    HOSTPORT="${AMINETXDUO_DEMO_HOSTPORT:-$PORT}"
    export AMINETXDUO_AMIBERRY_EXTRA="slirp_redir=tcp:${HOSTPORT}:${PORT}:10.0.2.15"
fi

# Named after the tag, because amiberry-run.sh names its log after the tag.
# This said build/amiberry-demo.log outright, so a second demo started with
# AMINETXDUO_RUN_TAG set watched a file its own emulator was not writing and
# sat in the MAC loop until the timeout with nothing wrong anywhere else.
EMU="$ROOT/build/amiberry-$AMINETXDUO_RUN_TAG.log"

echo "==> booting $MODEL on '$BACKEND', httpd :$PORT, window ${WINDOW}s"

"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$BACKEND" -m "$MODEL" -t "$WINDOW" \
    -a "DH0:Public $PORT TERMINAL=DH0:terminal.html" \
    "$TOOLS/httpd" "$STAGE/devs" "$STAGE/libs" "$STAGE/Public" \
    "$STAGE/terminal.html" "${EXTRA_C[@]}" \
    > "$ROOT/build/demo-run-$AMINETXDUO_RUN_TAG.log" 2>&1 &
RUNNER=$!

# WHERE IT ENDED UP.  Two backends, two ways of finding out.

if [ "$BACKEND" = slirp ]; then
    # Behind NAT there is nothing on the wire to sniff and no lease to wait
    # for.  Poll the forwarded port, which is the same question asked where
    # the answer is.
    ADDR="127.0.0.1"
    PORT="$HOSTPORT"
    NAME=""

    UP=no
    for _ in $(seq 1 90); do
        sleep 2
        if curl -s -o /dev/null --max-time 2 "http://127.0.0.1:${HOSTPORT}/"
        then
            UP=yes
            break
        fi
    done
    [ "$UP" = yes ] || {
        echo "nothing answered on 127.0.0.1:${HOSTPORT} after 180s; see $EMU" >&2
        exit 1
    }
else
    # Bridged: the address comes off the wire.  The guest announces itself by
    # ARP as soon as it has a lease, and the MAC to watch for is the one the
    # emulator logged, not the one we asked for.  A release build says nothing
    # on the serial line, so this is the only place the address appears.
    MAC=""
    for _ in $(seq 1 60); do
        sleep 2
        MAC=$(grep -oE "7990: '[^']*' ([0-9a-f]{2}:){5}[0-9a-f]{2}" "$EMU" \
              2>/dev/null | tail -1 |
              grep -oE "([0-9a-f]{2}:){5}[0-9a-f]{2}" || true)
        [ -n "$MAC" ] && break
    done
    [ -n "$MAC" ] || {
        echo "the emulator never reported a MAC; see $EMU" >&2
        exit 1
    }

    echo "==> guest MAC $MAC, waiting for a lease"
    ADDR=""
    for _ in $(seq 1 40); do
        ADDR=$(timeout 10 tcpdump -i "$BACKEND" -n -c 1 \
                   "ether host $MAC and arp" 2>/dev/null |
               grep -oE "ARP, Reply [0-9.]+" | grep -oE "[0-9.]+$" || true)
        [ -n "$ADDR" ] && break
    done

    [ -n "$ADDR" ] || {
        echo "no lease seen for $MAC on $BACKEND after 400s" >&2
        echo "the emulator is still running as pid $RUNNER; see $EMU" >&2
        exit 1
    }
fi


# -p left the address right and the URL wrong until 2026-08-10: a demo on any
# port but 80 printed one nothing answers on.
HOSTPART="$ADDR"
[ "$PORT" = 80 ] || HOSTPART="$ADDR:$PORT"
cat <<EOF

  the drawer    http://$HOSTPART/
  the terminal  http://$HOSTPART/terminal      no password, anyone who can reach it
EOF

# Only when there is a network for a name to mean anything on.  Behind NAT the
# responder is answering a network of one.
if [ -n "$NAME" ]; then
    NAMEPART="$NAME.local"
    [ "$PORT" = 80 ] || NAMEPART="$NAME.local:$PORT"
    echo
    echo "  by name       http://$NAMEPART/terminal      mDNS, once the responder has claimed it"
fi

cat <<EOF

  emulator pid $RUNNER, log $EMU
EOF
wait "$RUNNER"
