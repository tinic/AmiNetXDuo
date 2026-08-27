#!/usr/bin/env bash
#
# A LIVE AMIGA ON THE LAN, IN ONE COMMAND.
#
#   tools/demo.sh [-b BUILDDIR] [-B BACKEND] [-C CMDDIR] [-m MODEL] [-n NAME]
#                 [-p PORT] [-t SECONDS]
#
#   http://<address>/           the Public drawer, WebDAV-writable
#   http://<address>/shell   an AmigaDOS Shell in a browser, NO PASSWORD
#   http://amiga-demo.local/    the same machine by name, -n renames it
#
# `amiga-demo` and 02:41:4d:49:00:77 do not change between runs; -n and
# AMINETXDUO_DEMO_MAC override for a second instance, which then needs BOTH.
#
# TWO BRIDGED GUESTS ARE NOT TWO MACHINES: the a2065's LANCE derives its address
# from the unit, so both are 00:80:10:49:00:01 -- and that derivation is why the
# MAC on the wire is not the one you set.  Second instance, -B slirp.
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
NAME="${AMINETXDUO_DEMO_NAME:-amiga-demo}"
CMDS="${AMINETXDUO_DEMO_C:-}"
WBLIBS="${AMINETXDUO_DEMO_WBLIBS:-}"
# Workbench's own commands, unpacked once from the ADF in the asset store and
# kept in ~/wbc.  Without them the Shell answers "Unknown command" to Dir and
# List, which is not a demo.  /tmp is swept, so it does not live there.
if [ -z "$CMDS" ]; then
    _wb="$HOME/wbc/Workbench3.1/C"
    _wblibs="$HOME/wbc/Workbench3.1/Libs"
    _adf="$HOME/amiga-assets/adf-wb31/amiga-wb31_workbench.adf"
    _xdf="$HOME/venv-amitools/bin/xdftool"
    if [ ! -d "$_wb" ] && [ -f "$_adf" ] && [ -x "$_xdf" ]; then
        mkdir -p "$HOME/wbc" && "$_xdf" "$_adf" unpack "$HOME/wbc" >/dev/null 2>&1 || true
    fi
    [ -d "$_wb" ] && CMDS="$_wb"
    [ -d "$_wblibs" ] && WBLIBS="$_wblibs"
fi

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
PAGE="$ROOT/src/tools/web/shell.html"
# The compressed copy httpd hands to a browser that asked for gzip.  Beside
# the page, found by spelling, and committed with it.
PAGEGZ="$PAGE.gz"
A2065="${AMINETXDUO_A2065:-$HOME/amiga-assets/devs/a2065.device}"

for f in "$TOOLS/httpd" "$BSD" "$PAGE" "$PAGEGZ" "$A2065"; do
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

# The trust store is the BUILT one, "$BUILD/certificates" -- an 'ACS1'
# binary with a magic header, not the PEM it is generated from.  Staging
# third_party/cacert/cacert.pem gives a file of the right name that
# tls_store.c:262 rejects at the magic check, and every https fetch then
# says "cannot read DEVS:Internet/certificates" about a file sitting there
# readable at 186 KB.  That cost an evening.
mkdir -p "$STAGE/devs/Internet"
[ -f "$ROOT/$BUILD/certificates" ] &&
    cp "$ROOT/$BUILD/certificates" "$STAGE/devs/Internet/certificates"
cp "$PAGE"   "$STAGE/shell.html"
cp "$PAGEGZ" "$STAGE/shell.html.gz"

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

# Our own name_resolution, never the one under tests/netstack: that file
# carries "domain localdomain", which exists on no real network.  A demo that
# inherits it calls itself amiga.localdomain while answering to amiga.local.
#
# No name servers and no domain: DHCP supplies both.  The hostname is the one
# line we do write, because this machine has a fixed identity people bookmark.
# Unset the name and it falls back to naming itself after its card.
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
# ssh.  dbclient is built by clients/dropbear/build.sh, NOT by the CMake tree,
# so a plain `cmake --build` does not produce it and it lands under build/
# rather than anywhere under clients/.  Staging it from the wrong path is a
# silent omission -- the Shell simply has no ssh -- so the places it is
# actually built into are named here and the one taken is printed.
#
#   build/ssh          what .github/workflows/release.yml builds and packs
#   build/dropbear     the script's own default -b
#   build/dropbear-any an explicitly named one-binary build
#
# AMINETXDUO_DEMO_SSH overrides all three.
DEMO_SSH="${AMINETXDUO_DEMO_SSH:-}"
if [ -z "$DEMO_SSH" ]; then
    for _c in build/ssh build/dropbear build/dropbear-any; do
        [ -f "$ROOT/$_c/dbclient" ] || continue
        DEMO_SSH="$ROOT/$_c/dbclient"
        break
    done
fi
if [ -n "$DEMO_SSH" ] && [ -f "$DEMO_SSH" ]; then
    cp -f "$DEMO_SSH" "$STAGE/c/ssh"
    echo "==> ssh from $DEMO_SSH"
else
    echo "==> no ssh: run clients/dropbear/build.sh to get one" >&2
fi

# vim, from the asset store.  It is the demanding console test this demo
# exists to make possible: 2.2 MB, full-screen redraw, cursor addressing,
# raw keys and a resize, all of it through the WebSocket.
#
# $VIM has to be set.  The port's own notes record why: with it unset, vim
# derives the path from the binary name, "vim" is read by AmigaDOS as a
# volume reference, and the machine asks you to insert volume vim: in any
# drive.  S:vim/ is the path its pathdef.c was generated with.
#
# The runtime files are not in the package, so syntax highlighting will not
# work; plain editing does.
EXTRA_DRAWERS=()
# Workbench's own libraries, all of them, beside ours.  Picking them out one
# at a time is how an evening goes: vim wants locale.library, something
# else will want diskfont or iffparse, and each one is discovered by a
# program failing to load in front of somebody.  They are small and they
# are on the disk we already unpack.  -n so ours are never overwritten.
if [ -d "$WBLIBS" ]; then
    cp -n "$WBLIBS"/*.library "$STAGE/libs/" 2>/dev/null || true
fi

VIM="${AMINETXDUO_DEMO_VIM:-$HOME/amiga-assets/apps/vim-9.1/vim}"
if [ -f "$VIM" ]; then
    cp -f "$VIM" "$STAGE/c/vim"
    mkdir -p "$STAGE/s/vim" "$STAGE/env"
    # ENV:VIM is what GetVar() reads.  amiberry-run.sh makes the drive's
    # env/ drawer itself, and an extra drawer whose name already exists is
    # merged rather than nested, so this lands beside whatever is there.
    printf 'S:vim' > "$STAGE/env/VIM"
    EXTRA_DRAWERS=("$STAGE/s" "$STAGE/env")
    echo "==> vim staged, ENV:VIM is S:vim"
else
    echo "==> no vim at $VIM: the console has no full-screen editor to try" >&2
fi

# LhA 2.15, from the asset store.  Workbench 3.1 does not ship one, and
# "lha x" of something fetched over WebDAV is what this machine is mostly
# for.  The package carries a build per CPU the same way we do; lha_68020
# traps on a 68000, so the model picks.
LHADIR="${AMINETXDUO_DEMO_LHA:-$HOME/amiga-assets/apps/lha-2.15}"
case "$MODEL" in
    A4000*) LHABIN=lha_68040 ;;
    A500*|A600*|A1000*|A2000*) LHABIN=lha_68k ;;
    *) LHABIN=lha_68020 ;;
esac
if [ -f "$LHADIR/$LHABIN" ]; then
    cp -f "$LHADIR/$LHABIN" "$STAGE/c/lha"
    echo "==> lha staged, $LHABIN for $MODEL"
else
    echo "==> no lha at $LHADIR/$LHABIN: nothing on the drive unpacks archives" >&2
fi

echo "==> C: has $(ls "$STAGE/c" | wc -l | tr -d ' ') commands in it"

echo "Hello from an Amiga." > "$STAGE/Public/readme.txt"
echo "<html><body><h1>Amiga</h1><p>httpd is serving this drawer.</p></body></html>" > "$STAGE/Public/index.html"
echo "in a drawer" > "$STAGE/Public/Docs/notes.txt"

# ------------------------------------------------------------------ run ---

# A MAC of our own, set here because ~/amiga-assets/env.sh may export the lab's
# AMINETXDUO_AMIBERRY_MAC and sourcing it above would otherwise win.  It is
# also PINNED rather than derived, unlike amiberry-run.sh's per-tag default:
# the demo is meant to keep one address across restarts so a link handed out
# stays good, and 0x77 is a fifth byte of 0x00, which that default never emits.
#
# A demo and a benchmark sharing one address on one wire collide and the demo
# never reaches the network -- httpd binds 0.0.0.0 and reports itself happily,
# which is why that reads as stuck rather than as unplugged.  Cost three
# restarts on 2026-08-10.
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

# THIS ONE STANDS FOR DAYS, so its log gets the smallest cap in the tree.
# amiberry-run.sh puts every emulator log through tools/logcap.sh; the default
# 4 MB head is sized for a harness that runs for minutes, and a demo instance
# that is left up is exactly the case that filled playhouse3 -- 27.6 GB in one
# file.  A standing guest needs its boot header, which is where the board line
# and the MAC this script greps for are printed, and a short recent tail.
export AMINETXDUO_LOG_HEAD="${AMINETXDUO_LOG_HEAD:-1048576}"
export AMINETXDUO_LOG_TAIL="${AMINETXDUO_LOG_TAIL:-100}"

# One demo at a time.  Two guests on one wire derive the same MAC from the
# a2065 unit and neither reaches the network -- httpd serves on 0.0.0.0 and
# reports itself happily, so it reads as stuck rather than as unplugged.
# Stacked instances cost four restarts before anyone noticed tonight.
pkill -f "amiberry --log -f $ROOT/build/amiberry-$AMINETXDUO_RUN_TAG.uae" 2>/dev/null || true
# Tag-scoped, both patterns.  Without the tag this matched every demo on the
# box, so starting a second one killed the first -- which is not "one demo
# at a time", it is "only ever the newest".
pkill -f "amiberry-run.sh .*amiberry-testhd-$AMINETXDUO_RUN_TAG" 2>/dev/null || true
for _ in $(seq 1 15); do
    pgrep -f "amiberry --log -f $ROOT/build/amiberry-$AMINETXDUO_RUN_TAG.uae" >/dev/null || break
    sleep 1
done
sleep 2

echo "==> booting $MODEL on '$BACKEND', httpd :$PORT, window ${WINDOW}s"

# Everything the demo needs, asserted on the STAGE before the drive is built.
# A file that is staged late, or under a name amiberry-run.sh does not copy,
# turns into an error the person at the terminal hits half an hour later --
# "cannot read DEVS:Internet/certificates" for a trust store that was sitting
# in the staging directory the whole time.  Fail here instead.
missing=""
for f in "$STAGE/libs/bsdsocket.library" \
         "$STAGE/devs/Internet/certificates" \
         "$STAGE/devs/NetInterfaces/eth0" \
         "$STAGE/shell.html" \
         "$STAGE/shell.html.gz" \
         "$STAGE/c/httpd"; do
    [ -f "$f" ] || missing="$missing ${f#"$STAGE/"}"
done
# These three are wanted but not fatal: tls.library and the maths pair only
# matter for https and ssh, and a tree that has not built them is a legitimate
# state to run a demo from.  Say so rather than leaving it to be discovered.
for f in "$STAGE/libs/tls.library" \
         "$STAGE/libs/mathieeedoubbas.library" \
         "$STAGE/c/ssh"; do
    [ -f "$f" ] || echo "==> without ${f#"$STAGE/"}: expect that half not to work" >&2
done
if [ -n "$missing" ]; then
    echo "==> refusing to boot, these are missing from the staging:$missing" >&2
    exit 1
fi

# The wire, from before the guest is on it.
#
# A booted guest talks unprompted exactly twice: the gratuitous ARP that
# claims its lease and the mDNS announcement that claims its name.  Both are
# over within a second of the stack coming up and neither is a reply, so a
# sniffer started afterwards -- once the emulator has logged its MAC, which is
# the same moment -- catches them only by luck.  Start it first and read it
# later; the filter is by protocol here and by MAC below, because the MAC is
# not known yet.
#
# Not `ether host $MAC and arp`, which was the whole defect: that is an ARP
# REPLY, and nothing replies unless something asked.  A healthy guest nobody
# happened to talk to sat undetected for the full 400 s while it was serving.
if [ "$BACKEND" != slirp ]; then
    WIRE="$ROOT/build/demo-wire-$AMINETXDUO_RUN_TAG.txt"
    : > "$WIRE"
    tcpdump -i "$BACKEND" -n -l -e \
        "arp or (udp port 67 or udp port 68) or (udp port 5353)" \
        >> "$WIRE" 2>/dev/null &
    SNIFFER=$!
    # INT and TERM, never EXIT: bash runs an EXIT trap in the subshell a
    # command substitution forks, and the first `MAC=$(grep ...)` below then
    # killed the sniffer one second after it started.
    trap 'kill "$SNIFFER" 2>/dev/null || true' INT TERM
    # tcpdump needs a moment to attach before the guest's first frame.
    sleep 1
fi

"$ROOT/tools/amiberry-run.sh" -N a2065 -B "$BACKEND" -m "$MODEL" -t "$WINDOW" \
    -a "DH0:Public $PORT -T PAGE=DH0:shell.html" \
    "$TOOLS/httpd" "$STAGE/devs" "$STAGE/libs" "$STAGE/Public" \
    "$STAGE/shell.html" "$STAGE/shell.html.gz" "${EXTRA_C[@]}" \
    "${EXTRA_DRAWERS[@]}" \
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
        # Case-insensitive: the emulator logs the address in UPPER case, and
        # a lowercase-only class matched it anyway for as long as every guest
        # here happened to have a MAC of nothing but digits.  The first one
        # with a letter in it was never found and the run died saying the
        # emulator had reported no MAC, with the MAC in the log.
        MAC=$(grep -oEi "7990: '[^']*' ([0-9a-f]{2}:){5}[0-9a-f]{2}" "$EMU" \
              2>/dev/null | tail -1 |
              grep -oEi "([0-9a-f]{2}:){5}[0-9a-f]{2}" || true)
        [ -n "$MAC" ] && break
    done
    [ -n "$MAC" ] || {
        echo "the emulator never reported a MAC; see $EMU" >&2
        exit 1
    }

    echo "==> guest MAC $MAC, waiting for a lease"
    # Three things in the capture carry the address, and the guest sends all
    # three on its own: the mDNS announcement (an IPv4 source address next to
    # its MAC), the gratuitous ARP (who-has X tell X, same X), and an ARP
    # reply if anything did ask.  0.0.0.0 is skipped, which is what the guest
    # sources its DHCP discover and request from.
    #
    # Only lines the guest SENT, so the address taken off one is its own: the
    # capture also holds the DHCP server answering it, and reading the first
    # address on one of those lines names the server.
    ADDR=""
    for _ in $(seq 1 200); do
        ADDR=$(grep -iE "^[0-9:.]+ $MAC > " "$WIRE" 2>/dev/null | awk '
            /ethertype IPv4/ {
                for (i = 1; i <= NF; i++)
                    if ($i ~ /^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$/) {
                        split($i, a, ".")
                        ip = a[1] "." a[2] "." a[3] "." a[4]
                        if (ip != "0.0.0.0") { print ip; exit }
                    }
            }
            /Reply [0-9.]+ is-at/ {
                for (i = 1; i <= NF; i++)
                    if ($i == "Reply") { print $(i + 1); exit }
            }
            /who-has [0-9.]+ tell/ {
                for (i = 1; i <= NF; i++)
                    if ($i == "tell") {
                        ip = $(i + 1); sub(/,$/, "", ip)
                        if (ip != "0.0.0.0") { print ip; exit }
                    }
            }' | head -1 || true)
        [ -n "$ADDR" ] && break
        sleep 2
    done

    [ -n "$ADDR" ] || {
        kill "$SNIFFER" 2>/dev/null || true
        echo "no lease seen for $MAC on $BACKEND after 400s" >&2
        echo "the emulator is still running as pid $RUNNER; see $EMU" >&2
        echo "what was on the wire is in $WIRE" >&2
        exit 1
    }
    kill "$SNIFFER" 2>/dev/null || true
    trap - INT TERM
fi


# -p left the address right and the URL wrong until 2026-08-10: a demo on any
# port but 80 printed one nothing answers on.
HOSTPART="$ADDR"
[ "$PORT" = 80 ] || HOSTPART="$ADDR:$PORT"
cat <<EOF

  the drawer    http://$HOSTPART/
  the terminal  http://$HOSTPART/shell      no password, anyone who can reach it
EOF

# Only when there is a network for a name to mean anything on.  Behind NAT the
# responder is answering a network of one.
if [ -n "$NAME" ]; then
    NAMEPART="$NAME.local"
    [ "$PORT" = 80 ] || NAMEPART="$NAME.local:$PORT"
    echo
    echo "  by name       http://$NAMEPART/shell      mDNS, once the responder has claimed it"
fi

cat <<EOF

  emulator pid $RUNNER, log $EMU
EOF
wait "$RUNNER"
