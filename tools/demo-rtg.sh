#!/usr/bin/env bash
#
# A LIVE AMIGA ON THE LAN, ON A GRAPHICS CARD, WITH ITS SCREEN IN A BROWSER.
#
#   tools/demo-rtg.sh [-b BUILDDIR] [-B BACKEND] [-m MODEL] [-n NAME]
#                     [-p PORT] [-t SECONDS]
#
#   http://<address>/           the Public drawer, WebDAV-writable
#   http://<address>/shell      an AmigaDOS Shell in a browser, NO PASSWORD
#   http://<address>/console    the Workbench screen, streamed off the card
#
# A board that fails to come up is invisible: Workbench falls back to the chipset
# and every other symptom is identical.  The geom word's format field is the one
# place it shows -- 1 is a card -- so this reads it back off a live session.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-$ROOT/build/cm}"
BACKEND="${AMINETXDUO_DEMO_BACKEND:-ens18}"
MODEL="${AMINETXDUO_EMU_MODEL:-A1200}"
PORT=80
WINDOW=32400
NAME="${AMINETXDUO_DEMO_NAME:-amiga-rtg}"
TAG="${AMINETXDUO_RUN_TAG:-demo-rtg}"
BOOT_MAX=300

RTG_BOARD=uaegfx
RTG_MODE_ID=0x50031000
RTG_W=640
RTG_H=480
RTG_DEPTH=8
P96DIR="${AMINETXDUO_P96_DIR:-$HOME/amiga-assets/p96}"

say() { printf '%s=%s\n' "$1" "$2"; }

while getopts "b:B:m:n:p:t:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        n) NAME="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        t) WINDOW="$OPTARG" ;;
        *) sed -n '3,6p' "$0" >&2; exit 2 ;;
    esac
done

[ -f "$HOME/amiga-assets/env.sh" ] && . "$HOME/amiga-assets/env.sh"

TOOLS="$BUILD/src/tools"
HTTPD="$TOOLS/httpd"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
SHELLPAGE="$ROOT/src/tools/web/shell.html"
CONSOLEPAGE="$ROOT/src/tools/web/console.html"
RTGSCREEN="${AMINETXDUO_RTGSCREEN:-$BUILD/tests/perf/rtgscreen}"
A2065="${AMINETXDUO_A2065:-$HOME/amiga-assets/devs/a2065.device}"

for f in "$HTTPD" "$BSD" "$SHELLPAGE" "$SHELLPAGE.gz" \
         "$CONSOLEPAGE" "$CONSOLEPAGE.gz" "$RTGSCREEN" "$A2065"; do
    [ -f "$f" ] || { say error "missing $f"; exit 2; }
done
for f in Libs/Picasso96API.library Libs/Picasso96/rtg.library \
         Libs/Picasso96/uaegfx.card Devs/Monitors/Picasso96; do
    [ -f "$P96DIR/$f" ] || { say error "no $P96DIR/$f"; exit 2; }
done

model_var=$(printf '%s' "$MODEL" | tr '[:lower:]' '[:upper:]' | tr -c 'A-Z0-9' '_')
model_var=${model_var%_}
eval "KICKSTART=\${AMINETXDUO_KICKSTART_$model_var:-}"
KICKSTART="${KICKSTART:-${AMINETXDUO_KICKSTART:-}}"
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    say error "no Kickstart for $MODEL"; exit 2; }

AMIBERRY="${AMIBERRY:-$(command -v amiberry || true)}"
[ -n "$AMIBERRY" ] && [ -x "$AMIBERRY" ] || { say error "no amiberry"; exit 2; }

# ------------------------------------------------------ Workbench 3.1 SYS: --

# shellcheck source=tests/tools/wb31-sys.sh
. "$ROOT/tests/tools/wb31-sys.sh"
wb31_assemble "$ROOT/build/wb31-sys" || exit 2
WB="$WB31_SYS"

# tests/tools/run-console.sh's icon, verbatim: devs/monitors/Picasso96 reads
# the board name out of the TOOLTYPES of the icon beside it, and with no icon
# it loads no card and Workbench comes up on the chipset without a word.
rtg_monitor_icon() {
    local out="$1" board="$2"

    AMINETXDUO_ICON_BOARD="$board" python3 - "$out" <<'EOF'
import os, struct, sys

board = os.environ["AMINETXDUO_ICON_BOARD"]
tools = ["IgnoreMask=Yes",
         "BoardType=" + board]

W, H, D = 8, 8, 1
rowbytes = ((W + 15) // 16) * 2

gadget = struct.pack(">LhhhhHHHLLLLLHL",
                     0,
                     0, 0, W, H,
                     0x0004,
                     0, 1,
                     1,
                     0, 0, 0, 0,
                     0, 0)
assert len(gadget) == 44, len(gadget)

obj = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
    ">BBLLLLLLL",
    3, 0,
    0,
    1,
    0x80000000, 0x80000000,
    0,
    0,
    4096)
assert len(obj) == 78, len(obj)

image = struct.pack(">hhhhhLBBL", 0, 0, W, H, D, 1, 0x1, 0x0, 0)
bits = bytes([0xFF] + [0x81] * (H - 2) + [0xFF]) if rowbytes == 1 else \
       b"".join(struct.pack(">H", v) for v in
                [0xFF00] + [0x8100] * (H - 2) + [0xFF00])

tt = struct.pack(">L", (len(tools) + 1) * 4)
for t in tools:
    b = t.encode("latin-1") + b"\0"
    tt += struct.pack(">L", len(b)) + b

with open(sys.argv[1], "wb") as fh:
    fh.write(obj + image + bits + tt)
EOF
}

# ------------------------------------------------------------- the drive ----

HD="$ROOT/build/demo-rtg-dh0"

rm -rf "$HD"
mkdir -p "$HD/Public/Docs" "$HD/Console"
cp -R "$WB/." "$HD/"

mkdir -p "$HD/Libs" "$HD/Devs/Networks" "$HD/Devs/NetInterfaces" \
         "$HD/Devs/Internet" "$HD/Devs/Monitors"
cp "$BSD" "$HD/Libs/bsdsocket.library"
cp "$A2065" "$HD/Devs/Networks/a2065.device"
[ -f "$BUILD/src/tlslib/tls.library" ] && cp "$BUILD/src/tlslib/tls.library" "$HD/Libs/"
for m in mathieeedoubbas mathieeedoubtrans; do
    for src in "$HOME/amiga-assets/nglibs/$m.library" "$HOME/amiga-assets/libs/$m.library"; do
        [ -f "$src" ] && { cp -f "$src" "$HD/Libs/"; break; }
    done
done

# The BUILT trust store, an 'ACS1' binary -- not the PEM it comes from, which
# tls_store.c rejects at the magic check while sitting there readable.
[ -f "$BUILD/certificates" ] && cp "$BUILD/certificates" "$HD/Devs/Internet/certificates"

# Every tool we build, after Workbench's own commands, so a name in both
# drawers resolves to ours: Version and Which exist on both sides.
for t in "$TOOLS"/*; do
    [ -f "$t" ] && [ -x "$t" ] || continue
    case "$(basename "$t")" in
        *.map|*.cmake|Makefile|ToolsSmoke|*Probe) continue ;;
    esac
    cp -f "$t" "$HD/C/" 2>/dev/null || true
done
cp -f "$RTGSCREEN" "$HD/C/rtgscreen"

DEMO_SSH="${AMINETXDUO_DEMO_SSH:-}"
if [ -z "$DEMO_SSH" ]; then
    for _c in build/ssh build/dropbear build/dropbear-any; do
        [ -f "$ROOT/$_c/dbclient" ] && { DEMO_SSH="$ROOT/$_c/dbclient"; break; }
    done
fi
[ -n "$DEMO_SSH" ] && [ -f "$DEMO_SSH" ] && cp -f "$DEMO_SSH" "$HD/C/ssh"

case "$MODEL" in
    A4000*) LHABIN=lha_68040 ;;
    A500*|A600*|A1000*|A2000*) LHABIN=lha_68k ;;
    *) LHABIN=lha_68020 ;;
esac
LHADIR="${AMINETXDUO_DEMO_LHA:-$HOME/amiga-assets/apps/lha-2.15}"
[ -f "$LHADIR/$LHABIN" ] && cp -f "$LHADIR/$LHABIN" "$HD/C/lha"

# vim needs $VIM set or AmigaDOS reads "vim" as a volume name and asks for a
# disk; S:vim is the path its pathdef.c was generated with.
VIM="${AMINETXDUO_DEMO_VIM:-$HOME/amiga-assets/apps/vim-9.1/vim}"
if [ -f "$VIM" ]; then
    cp -f "$VIM" "$HD/C/vim"
    mkdir -p "$HD/S/vim" "$HD/Prefs/Env-Archive"
    printf 'S:vim' > "$HD/Prefs/Env-Archive/VIM"
fi

chmod -R a+rx "$HD/C"

cp "$SHELLPAGE"      "$HD/shell.html"
cp "$SHELLPAGE.gz"   "$HD/shell.html.gz"
cp "$CONSOLEPAGE"    "$HD/Console/console.html"
cp "$CONSOLEPAGE.gz" "$HD/Console/console.html.gz"

# MDNS= is per interface and defaults to off, so this line is the whole of
# turning the name on.
cat > "$HD/Devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
MDNS=YES
EOF

# No name servers and no domain: DHCP supplies both.  Never the file under
# tests/netstack, whose "domain localdomain" exists on no real network.
: > "$HD/Devs/Internet/name_resolution"
[ -z "$NAME" ] || echo "hostname $NAME" >> "$HD/Devs/Internet/name_resolution"

echo "Hello from an Amiga." > "$HD/Public/readme.txt"
echo "<html><body><h1>Amiga</h1><p>httpd is serving this drawer.</p></body></html>" > "$HD/Public/index.html"
echo "in a drawer" > "$HD/Public/Docs/notes.txt"

# The card.  The monitor file is named after the BOARD and not after
# Picasso96: the one executable drives every card P96 supports and the name is
# how it knows which .card to load.
cp -R "$P96DIR/Libs/." "$HD/Libs/"
cp "$P96DIR/Devs/Monitors/Picasso96" "$HD/Devs/Monitors/$RTG_BOARD"
rtg_monitor_icon "$HD/Devs/Monitors/$RTG_BOARD.info" "$RTG_BOARD"
wb31_screenmode_prefs_id "$HD" "$RTG_DEPTH" "$RTG_MODE_ID" "$RTG_W" "$RTG_H"

# The stock Startup-Sequence with the tail replaced: LoadWB stays, since the
# screen served is the one it opens; EndCLI goes, because it would take the
# boot shell away before the server started.
sed -e '/^EndCLI/d' "$WB/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
#
# THE SERVER IS THE FIRST THING AFTER LoadWB, and nothing diagnostic runs before
# it: a probe that never returns leaves the guest booted, idle and answering
# nothing.  DEVS:Monitors is already run by the stock sequence above.
#
# `Run >NIL: <NIL:`, not a file redirect.  Run hands the started process the
# output stream it was given, and NIL: on both sides is what makes the final
# EndCLI take the boot shell's window with it.
cat >> "$HD/S/Startup-Sequence" <<EOF

FailAt 9999
C:Wait 6
Run >NIL: <NIL: C:httpd DH0:Public $PORT -T PAGE=DH0:shell.html -C CONSOLEPAGE=DH0:Console/console.html
C:Wait 3
EndCLI >NIL:
EOF
chmod 755 "$HD/S/Startup-Sequence"

# ------------------------------------------------------------ the emulator --

export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"

# AN RTG BOARD PUBLISHES THE HOST'S DISPLAY MODES, so a headless host has none:
# both headless SDL drivers report "0 display modes.", the board comes up with
# an empty resolution list and Workbench stays on the chipset.  Xvfb is the
# SOURCE OF THE DATA the board reports here, not a workaround.
XDISP=""
for n in $(seq 90 99); do
    [ -e "/tmp/.X11-unix/X$n" ] || { XDISP=":$n"; break; }
done
[ -n "$XDISP" ] || { say error "no free X display in :90..:99"; exit 2; }
Xvfb "$XDISP" -screen 0 1280x1024x24 >/dev/null 2>&1 &
XVFB_PID=$!
sleep 2
export DISPLAY="$XDISP"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
say xvfb "$XDISP pid $XVFB_PID"

MAC="${AMINETXDUO_DEMO_MAC:-02:41:4d:49:52:47}"

CFG="$ROOT/build/$TAG.uae"
cat > "$CFG" <<EOF
config_description=AmiNetXDuo RTG demo
use_gui=no
headless=true
quickstart=$MODEL,0
kickstart_rom_file=$KICKSTART
fastmem_size=8
floppy0type=-1
nr_floppies=0
uaehf0=dir,rw,DH0:DH0:$HD,0
a2065_rom_file=:ENABLED
a2065_rom_options=mac=$MAC,$BACKEND
cpu_24bit_addressing=no
gfxcard_size=8
rtg_modes=0x112
EOF

# cpu_24bit_addressing=no IS NOT OPTIONAL: uaegfx is Zorro III, a quickstart
# model comes up 24-bit, and the emulator then drops the board into a log
# nothing reads and Workbench comes up planar.
# rtg_modes 0x112 has RGBFF_CLUT in it, which is the only format the console
# serves; a mask without it is a board with no palette mode.

EMULOG="$ROOT/build/amiberry-$TAG.log"

# The wire, from before the guest is on it.  A booted guest talks unprompted
# exactly twice -- the gratuitous ARP for its lease and the mDNS announcement
# for its name -- and both are over within a second, so a sniffer started once
# the MAC is known catches them by luck only.
WIRE="$ROOT/build/demo-rtg-wire-$TAG.txt"
: > "$WIRE"
tcpdump -i "$BACKEND" -n -l -e \
    "arp or (udp port 67 or udp port 68) or (udp port 5353)" \
    >> "$WIRE" 2>/dev/null &
SNIFFER=$!
trap 'kill "$SNIFFER" 2>/dev/null || true' INT TERM
sleep 1

setsid "$AMIBERRY" -f "$CFG" >"$EMULOG" 2>&1 &
EMU_PID=$!
say emulator_pid "$EMU_PID"

# The window.  A demo that is left up needs an end, and the emulator is the
# thing that has to stop: nothing else here holds the drive open.
setsid bash -c "sleep $WINDOW; kill -TERM $EMU_PID 2>/dev/null; sleep 10; \
    kill -KILL $EMU_PID 2>/dev/null; kill -TERM $XVFB_PID 2>/dev/null" \
    >/dev/null 2>&1 &
say window_seconds "$WINDOW"
say window_expires "$(date -u -d "+$WINDOW seconds" '+%Y-%m-%dT%H:%M:%SZ')"

# THE MAC ON THE WIRE IS NOT THE ONE ASKED FOR.  The a2065's LANCE derives its
# address from the unit and reports 00:80:10:49:xx:xx, so looking for the one
# configured above finds nothing.
#
# tools/demo.sh reads it out of the emulator's own log, which this cannot do:
# that line is printed under `amiberry --log`, and --log writes about a
# megabyte a second.  A harness that runs for minutes can afford it; a guest
# left up for nine hours writes tens of gigabytes and fills the disk, which is
# how playhouse3 filled once already.  So the MAC is read off the WIRE instead,
# by its Commodore OUI, which is the same fact from the other end.
#
# There is nothing to see until httpd runs: bsdsocket.library configures the
# interface when the first program opens it, so a guest that is booting is a
# guest that is silent.
GUESTMAC=""
for _ in $(seq 1 "$BOOT_MAX"); do
    sleep 1
    GUESTMAC=$(grep -oEi "^[0-9:.]+ (00:80:10(:[0-9a-f]{2}){3}) > " "$WIRE" \
               2>/dev/null | head -1 |
               grep -oEi "00:80:10(:[0-9a-f]{2}){3}" || true)
    [ -n "$GUESTMAC" ] && break
done
[ -n "$GUESTMAC" ] || { say error "no a2065 was heard on $BACKEND in ${BOOT_MAX}s"; \
                        say wire "$WIRE"; say drive "$HD"; exit 1; }
say guest_mac "$GUESTMAC"

ADDR=""
for _ in $(seq 1 200); do
    ADDR=$(grep -iE "^[0-9:.]+ $GUESTMAC > " "$WIRE" 2>/dev/null | awk '
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
kill "$SNIFFER" 2>/dev/null || true
trap - INT TERM

[ -n "$ADDR" ] || { say error "no lease seen for $GUESTMAC on $BACKEND"; \
                    say wire "$WIRE"; say emulog "$EMULOG"; exit 1; }

HOSTPART="$ADDR"
[ "$PORT" = 80 ] || HOSTPART="$ADDR:$PORT"
say address "$ADDR"
say drawer "http://$HOSTPART/"
say shell "http://$HOSTPART/shell"
say console "http://$HOSTPART/console"
[ -z "$NAME" ] || say name "http://$NAME.local/console"
say emulog "$EMULOG"
say drive "$HD"
say stop "pkill -f 'amiberry -f $CFG'"
say RESULT UP
