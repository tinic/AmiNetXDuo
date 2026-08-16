#!/usr/bin/env bash
#
# A full Workbench on the LAN, in one command.
#
#   tools/classicwb.sh [-m MODEL] [-v VARIANT] [-b BUILDDIR] [-B BACKEND]
#                      [-n NAME] [-p PORT] [-t SECONDS] [-s SNAPSHOTS]
#
# tools/demo.sh boots the drive tools/amiberry-run.sh builds, which is httpd
# and whatever else the staging puts beside it.  This boots a ClassicWB
# snapshot instead: a Workbench somebody could actually use, with our httpd,
# bsdsocket.library and tools staged onto it at launch.
#
#   http://<address>/           the Public drawer, WebDAV-writable
#   http://<address>/shell      an AmigaDOS Shell in a browser, no password
#   http://<address>/console    the Workbench screen
#
# The snapshot is built once by ~/amiga-assets/classicwb/install.sh and carries
# no file of ours.  Everything of ours arrives here, from -b, on every launch,
# so pointing -b at a fresh build and booting is the whole update procedure.
#
# The run copy is a copy.  A guest that corrupts its drive costs the copy and
# not the install.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

MODEL="${AMINETXDUO_CWB_MODEL:-A1200}"
VARIANT="${AMINETXDUO_CWB_VARIANT:-plain}"
BUILD="${AMINETXDUO_BUILD:-$ROOT/build/cm}"
BACKEND="${AMINETXDUO_DEMO_BACKEND:-ens18}"
NAME=""
PORT=80
WINDOW=28800
SNAPROOT="${AMINETXDUO_CWB_SNAPSHOTS:-$HOME/amiga-assets/classicwb/snapshots}"
P96DIR="${AMINETXDUO_P96_DIR:-$HOME/amiga-assets/p96}"

usage() {
    cat <<'EOF'
usage: tools/classicwb.sh [-m A600|A1200|A3000] [-v plain|rtg] [-b builddir]
                          [-B backend] [-n name] [-p port] [-t seconds]
                          [-s snapshots]

  -m  model, picks the Kickstart          (default A1200)
  -v  plain boots ClassicWB, rtg adds a Picasso96 screen  (default plain)
  -b  build directory our binaries come from     (default build/cm)
  -B  bridge interface                             (default ens18)
  -n  hostname and mDNS name          (default amiga-<model>-<variant>)
  -p  httpd port                                      (default 80)
  -t  seconds before the guest is stopped          (default 28800)
  -s  snapshot store         (default ~/amiga-assets/classicwb/snapshots)
EOF
}

say() { printf '%s=%s\n' "$1" "$2"; }

case "${1:-}" in -h|--help) usage; exit 0 ;; esac

while getopts "m:v:b:B:n:p:t:s:h" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        v) VARIANT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        n) NAME="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        t) WINDOW="$OPTARG" ;;
        s) SNAPROOT="$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

MODEL=$(printf '%s' "$MODEL" | tr '[:lower:]' '[:upper:]')
VARIANT=$(printf '%s' "$VARIANT" | tr '[:upper:]' '[:lower:]')
case "$VARIANT" in
    rtg|p96) VARIANT=rtg ;;
    plain|nortg|full) VARIANT=plain ;;
    *) say error "unknown variant $VARIANT, want plain or rtg"; exit 2 ;;
esac

# Which ClassicWB a model gets.  FULL is a 68020 with 6 MB and an A600 is
# neither, so the 68000 arm takes the 68K edition; it is the distribution
# ClassicWB publishes for that machine, not a cut-down FULL.  P96 needs a
# graphics card and a 68020, so the A600 has no rtg row at all.
case "$MODEL:$VARIANT" in
    A600:plain)  DIST=68k ;;
    A600:rtg)    say error "no rtg on an A600: Picasso96 and ClassicWB P96 both\
 want a 68020, and a 68000 has none"; exit 2 ;;
    A1200:plain|A3000:plain) DIST=full ;;
    A1200:rtg|A3000:rtg)     DIST=p96 ;;
    *) say error "unknown model $MODEL, want A600, A1200 or A3000"; exit 2 ;;
esac

[ -n "$NAME" ] || NAME="amiga-$(printf '%s' "$MODEL" | tr '[:upper:]' '[:lower:]')-$VARIANT"

# The asset store carries the ROMs and exports the Kickstart each model needs.
# Skipping it boots a machine with no ROM, and the error names the ROM rather
# than the missing variable.
[ -f "$HOME/amiga-assets/env.sh" ] && . "$HOME/amiga-assets/env.sh"

case "$MODEL" in
    A600)  KICKSTART="${AMINETXDUO_KICKSTART_A600:-}" ;;
    A3000) KICKSTART="${AMINETXDUO_KICKSTART_A3000:-}" ;;
    *)     KICKSTART="${AMINETXDUO_KICKSTART:-}" ;;
esac
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    say error "no Kickstart for $MODEL in ~/amiga-assets/env.sh"; exit 2; }

SNAP="$SNAPROOT/$DIST/tree"
MANIFEST="$SNAPROOT/$DIST/manifest.json"
[ -d "$SNAP" ] || {
    say error "no snapshot at $SNAP -- build it with\
 ~/amiga-assets/classicwb/install.sh $DIST"; exit 2; }

TOOLS="$BUILD/src/tools"
HTTPD="$TOOLS/httpd"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
SHELLPAGE="$ROOT/src/tools/web/shell.html"
CONSOLEPAGE="$ROOT/src/tools/web/console.html"
A2065="${AMINETXDUO_A2065:-$HOME/amiga-assets/devs/a2065.device}"

for f in "$HTTPD" "$BSD" "$SHELLPAGE" "$SHELLPAGE.gz" \
         "$CONSOLEPAGE" "$CONSOLEPAGE.gz" "$A2065"; do
    [ -f "$f" ] || { say error "missing $f"; exit 2; }
done

AMIBERRY="${AMIBERRY:-$(command -v amiberry || true)}"
[ -n "$AMIBERRY" ] && [ -x "$AMIBERRY" ] || { say error "no amiberry"; exit 2; }

TAG="${AMINETXDUO_RUN_TAG:-cwb-$(printf '%s' "$MODEL" | tr '[:upper:]' '[:lower:]')-$VARIANT}"

say model "$MODEL"
say variant "$VARIANT"
say classicwb "$DIST"
say kickstart "$(basename "$KICKSTART")"
say snapshot "$SNAP"
[ -f "$MANIFEST" ] && say manifest "$MANIFEST"

# ------------------------------------------------------------- the drive ----

# A copy, every time.  The snapshot is the install and is never written to, so
# a run that wedges its drive is recovered by deleting the copy.
HD="$ROOT/build/classicwb-$TAG-dh0"
rm -rf "$HD"
mkdir -p "$HD"
cp -a "$SNAP/." "$HD/"

# Nothing of ours may already be here.  The snapshot is built without our
# files precisely so a stale binary cannot survive in it, and this is the
# assertion that says so rather than assuming it.
for stale in C/httpd Libs/bsdsocket.library Devs/Networks/a2065.device; do
    [ -e "$HD/$stale" ] && {
        say error "the snapshot carries $stale -- it must carry none of ours"
        exit 2; }
done

mkdir -p "$HD/Libs" "$HD/Devs/Networks" "$HD/Devs/NetInterfaces" \
         "$HD/Devs/Internet" "$HD/Public/Docs" "$HD/Console"

cp "$BSD"   "$HD/Libs/bsdsocket.library"
cp "$A2065" "$HD/Devs/Networks/a2065.device"
[ -f "$BUILD/src/tlslib/tls.library" ] &&
    cp "$BUILD/src/tlslib/tls.library" "$HD/Libs/"

# ClassicWB has the IEEE maths pair, but a snapshot that turned out not to is
# a Shell where ssh dies saying mathieeedoubbas.library failed to load, which
# reads like a broken binary.  -n so ClassicWB's own are left alone.
for m in mathieeedoubbas mathieeedoubtrans; do
    for src in "$HOME/amiga-assets/nglibs/$m.library" \
               "$HOME/amiga-assets/libs/$m.library"; do
        [ -f "$src" ] && { cp -n "$src" "$HD/Libs/"; break; }
    done
done

# The built trust store, an 'ACS1' binary.  The PEM it is generated from has
# the right name and is rejected at the magic check, and every https fetch
# then complains about a file sitting there readable.
[ -f "$BUILD/certificates" ] && cp "$BUILD/certificates" "$HD/Devs/Internet/certificates"

# Our tools last, so a name that exists in both drawers resolves to ours:
# ClassicWB has a Version and a Which of its own and the interesting one here
# is not theirs.
for t in "$TOOLS"/*; do
    [ -f "$t" ] && [ -x "$t" ] || continue
    case "$(basename "$t")" in
        *.map|*.cmake|Makefile|ToolsSmoke|*Probe) continue ;;
    esac
    cp -f "$t" "$HD/C/" 2>/dev/null || true
done

# dbclient is built by clients/dropbear/build.sh and not by the CMake tree, so
# a plain cmake --build leaves it out and the Shell simply has no ssh.
SSHBIN="${AMINETXDUO_DEMO_SSH:-}"
if [ -z "$SSHBIN" ]; then
    for c in build/ssh build/dropbear build/dropbear-any; do
        [ -f "$ROOT/$c/dbclient" ] && { SSHBIN="$ROOT/$c/dbclient"; break; }
    done
fi
[ -n "$SSHBIN" ] && [ -f "$SSHBIN" ] && cp -f "$SSHBIN" "$HD/C/ssh"

chmod -R u+rw "$HD/C"
chmod -R a+rx "$HD/C"

cp "$SHELLPAGE"      "$HD/shell.html"
cp "$SHELLPAGE.gz"   "$HD/shell.html.gz"
cp "$CONSOLEPAGE"    "$HD/Console/console.html"
cp "$CONSOLEPAGE.gz" "$HD/Console/console.html.gz"

# MDNS= is per interface and off by default, so this line is the whole of
# turning the name on.
cat > "$HD/Devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
MDNS=YES
EOF

# DHCP supplies the servers and the domain.  Never the file under
# tests/netstack, which names a SLIRP guest's 10.0.2.3.
: > "$HD/Devs/Internet/name_resolution"
echo "hostname $NAME" >> "$HD/Devs/Internet/name_resolution"

echo "Hello from an Amiga." > "$HD/Public/readme.txt"
echo "<html><body><h1>Amiga</h1><p>httpd is serving this drawer.</p></body></html>" \
    > "$HD/Public/index.html"
echo "in a drawer" > "$HD/Public/Docs/notes.txt"

# ------------------------------------------------------------------ rtg ----

RTG_BOARD=uaegfx
RTG_MODE_ID=0x50031000
RTG_W=640
RTG_H=480
RTG_DEPTH=8

if [ "$VARIANT" = rtg ]; then
    # ClassicWB P96 ships Picasso96 but is installed for whatever card the
    # person installing it had.  The board here is uaegfx, and the monitor
    # file is named after the BOARD rather than after Picasso96 -- one
    # executable drives every card P96 supports and the name is how it knows
    # which .card to open.  A monitor called Picasso96 loads no card and
    # Workbench comes up on the chipset without saying anything.
    if [ ! -f "$HD/Devs/Monitors/$RTG_BOARD" ]; then
        for f in Libs/Picasso96API.library Libs/Picasso96/rtg.library \
                 Libs/Picasso96/uaegfx.card Devs/Monitors/Picasso96; do
            [ -f "$P96DIR/$f" ] || { say error "no $P96DIR/$f"; exit 2; }
        done
        mkdir -p "$HD/Devs/Monitors"
        cp -Rn "$P96DIR/Libs/." "$HD/Libs/"
        cp "$P96DIR/Devs/Monitors/Picasso96" "$HD/Devs/Monitors/$RTG_BOARD"
    fi

    # devs/monitors reads the board name out of the TOOLTYPES of the icon
    # beside it, so a monitor with no icon loads no card.
    AMINETXDUO_ICON_BOARD="$RTG_BOARD" python3 - \
        "$HD/Devs/Monitors/$RTG_BOARD.info" <<'EOF'
import os, struct, sys

board = os.environ["AMINETXDUO_ICON_BOARD"]
tools = ["IgnoreMask=Yes", "BoardType=" + board]

W, H, D = 8, 8, 1
rowbytes = ((W + 15) // 16) * 2

gadget = struct.pack(">LhhhhHHHLLLLLHL", 0, 0, 0, W, H, 0x0004, 0, 1, 1,
                     0, 0, 0, 0, 0, 0)
assert len(gadget) == 44, len(gadget)

obj = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
    ">BBLLLLLLL", 3, 0, 0, 1, 0, 0, 0x80000000, 0x80000000, 4096)

img = struct.pack(">hhhhhLBBL", 0, 0, W, H, D, 1, 0x1, 0x0, 0)
planes = b"\x00" * (rowbytes * H)

tt = struct.pack(">L", len(tools) + 1)
for t in tools:
    b = t.encode("latin-1") + b"\0"
    tt += struct.pack(">L", len(b)) + b

with open(sys.argv[1], "wb") as fh:
    fh.write(obj + img + planes + tt)
EOF

    # ENVARC:Sys/screenmode.prefs, which IPrefs reads through the ENVARC:
    # assign ClassicWB makes to SYS:Prefs/Env-Archive.  Without it Workbench
    # opens on whatever mode the snapshot was installed with, which is a
    # chipset mode, and the card sits there unused.
    mkdir -p "$HD/Prefs/Env-Archive/Sys"
    AMINETXDUO_SMP_DEPTH="$RTG_DEPTH" AMINETXDUO_SMP_ID="$RTG_MODE_ID" \
    AMINETXDUO_SMP_W="$RTG_W" AMINETXDUO_SMP_H="$RTG_H" \
        python3 - "$HD/Prefs/Env-Archive/Sys/screenmode.prefs" <<'EOF'
import os, struct, sys

DISPLAY_ID = int(os.environ["AMINETXDUO_SMP_ID"], 0)
WIDTH = int(os.environ["AMINETXDUO_SMP_W"])
HEIGHT = int(os.environ["AMINETXDUO_SMP_H"])
depth = int(os.environ["AMINETXDUO_SMP_DEPTH"])

prhd = struct.pack(">BB4B", 0, 0, 0, 0, 0, 0)
scrm = struct.pack(">4L L HHHH", 0, 0, 0, 0, DISPLAY_ID, WIDTH, HEIGHT,
                   depth, 0)

def chunk(tag, payload):
    out = tag + struct.pack(">L", len(payload)) + payload
    if len(payload) & 1:
        out += b"\0"
    return out

body = b"PREF" + chunk(b"PRHD", prhd) + chunk(b"SCRM", scrm)
with open(sys.argv[1], "wb") as fh:
    fh.write(b"FORM" + struct.pack(">L", len(body)) + body)
EOF
fi

# ------------------------------------------------------- startup-sequence ---

# ClassicWB's own sequence, with its EndCLI taken off so the tail below can
# replace it.  LoadWB stays: on the rtg arm the screen it opens is the screen
# being served.
[ -f "$HD/S/Startup-Sequence" ] || {
    say error "the snapshot has no S/Startup-Sequence"; exit 2; }
sed -e '/^[[:space:]]*EndCLI/Id' "$SNAP/S/Startup-Sequence" \
    > "$HD/S/Startup-Sequence"

# Version of the binary the guest actually loads, written where the host can
# read it: DH0 is a host directory, so this file appears beside the drive.  It
# is how a launch proves it is serving the build it staged and not one left
# over from an earlier run.
#
# Run >NIL: <NIL:, not a file redirect.  Run hands the started process the
# stream it was given, so redirecting to a file leaves httpd holding the boot
# shell's console: the shell ends, the window stays, and nothing can close it
# because the process still owning it is the web server.  NIL: on both sides
# is what lets EndCLI take the window with it.
cat >> "$HD/S/Startup-Sequence" <<EOF

FailAt 9999
C:Wait 6
C:Version >DH0:httpd-ver.txt DH0:C/httpd FILE
Run >NIL: <NIL: C:httpd DH0:Public $PORT -T PAGE=DH0:shell.html -C CONSOLEPAGE=DH0:Console/console.html
C:Wait 3
EndCLI >NIL:
EOF
chmod 755 "$HD/S/Startup-Sequence"

# ------------------------------------------------------------ the emulator --

export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"

XVFB_PID=""
if [ "$VARIANT" = rtg ]; then
    # An RTG board publishes the host's display modes, so a headless host has
    # none to publish: both headless SDL drivers report no display modes, the
    # board comes up with an empty resolution list and Workbench stays on the
    # chipset.  Xvfb is where that data comes from, not a way around a bug.
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
else
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
    unset DISPLAY WAYLAND_DISPLAY 2>/dev/null || true
fi

# One MAC per combination, so two of these on one wire are two machines.  The
# demo already on the segment keeps 02:41:4d:49:00:77 and its own name.
case "$MODEL:$VARIANT" in
    A600:plain)  MACTAIL=61 ;;
    A1200:plain) MACTAIL=62 ;;
    A1200:rtg)   MACTAIL=63 ;;
    A3000:plain) MACTAIL=64 ;;
    A3000:rtg)   MACTAIL=65 ;;
esac
MAC="${AMINETXDUO_CWB_MAC:-02:41:4d:49:c0:$MACTAIL}"

CFG="$ROOT/build/$TAG.uae"
cat > "$CFG" <<EOF
config_description=AmiNetXDuo ClassicWB $DIST on $MODEL
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
EOF

if [ "$VARIANT" = rtg ]; then
    # uaegfx is Zorro III, and a quickstart model comes up 24-bit: without
    # this the emulator drops the board into a log nothing reads and Workbench
    # comes up planar.  rtg_modes 0x112 has RGBFF_CLUT in it, which is the one
    # format the console serves.
    cat >> "$CFG" <<'EOF'
cpu_24bit_addressing=no
gfxcard_size=8
rtg_modes=0x112
EOF
fi

EMULOG="$ROOT/build/amiberry-$TAG.log"

# The wire, from before the guest is on it.  A booted guest talks unprompted
# exactly twice, the gratuitous ARP for its lease and the mDNS announcement
# for its name, and both are over within a second, so a sniffer started once
# the guest is up catches them by luck only.
WIRE="$ROOT/build/classicwb-wire-$TAG.txt"
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

# A guest that is left up needs an end, and the emulator is the thing that has
# to stop: nothing else here holds the drive open.
setsid bash -c "sleep $WINDOW; kill -TERM $EMU_PID 2>/dev/null; sleep 10; \
    kill -KILL $EMU_PID 2>/dev/null; \
    [ -n '$XVFB_PID' ] && kill -TERM $XVFB_PID 2>/dev/null" \
    >/dev/null 2>&1 &
say window_seconds "$WINDOW"

# The MAC on the wire is not the one asked for.  The a2065's LANCE keeps the
# low three octets of the configured address and puts Commodore's OUI in front
# of them, so 02:41:4d:49:c0:61 answers as 00:80:10:49:c0:61.
#
# Matching the OUI alone is not enough here.  A demo left up is another
# Commodore OUI on the same wire, and the first frame from one of those is
# picked up as this guest: the launch then reports somebody else's address and
# the version gate fails against a machine it never booted.
EXPECT_MAC="00:80:10:${MAC#*:*:*:}"
say expect_mac "$EXPECT_MAC"
GUESTMAC=""
for _ in $(seq 1 300); do
    sleep 1
    GUESTMAC=$(grep -oiE "^[0-9:.]+ $EXPECT_MAC > " "$WIRE" 2>/dev/null |
               head -1 | grep -oiE "$EXPECT_MAC" || true)
    [ -n "$GUESTMAC" ] && break
done
[ -n "$GUESTMAC" ] || {
    kill "$SNIFFER" 2>/dev/null || true
    say error "no a2065 was heard on $BACKEND in 300s"
    say emulog "$EMULOG"; say drive "$HD"; exit 1; }
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

[ -n "$ADDR" ] || {
    say error "no lease seen for $GUESTMAC on $BACKEND"
    say wire "$WIRE"; say emulog "$EMULOG"; exit 1; }
say address "$ADDR"

# --------------------------------------------------------- the version gate --

# Three questions, because a demo that quietly serves last week's binary is
# worse than one that does not start.  What the build directory holds, what
# landed on the drive, and what the process that is answering says it is.
BUILD_VER=""
[ -f "$BUILD/include/aminetxduo/version.h" ] &&
    BUILD_VER=$(sed -n 's/.*AMINETXDUO_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' \
                "$BUILD/include/aminetxduo/version.h" | head -1)
BUILD_VER_TAG=$(strings -a "$HTTPD" 2>/dev/null | grep -m1 '\$VER: httpd' || true)
say build_version "${BUILD_VER:-unknown}"
say build_verstag "${BUILD_VER_TAG:-unknown}"

# What the guest wrote about the binary it loaded, read straight out of the
# host directory the drive is.
GUEST_VER=""
for _ in $(seq 1 60); do
    [ -s "$HD/httpd-ver.txt" ] && { GUEST_VER=$(tr -d '\r' < "$HD/httpd-ver.txt" | head -1); break; }
    sleep 2
done
say guest_httpd_version "${GUEST_VER:-unknown}"

SERVED=""
for _ in $(seq 1 60); do
    SERVED=$(curl -s -m 4 -D - -o /dev/null "http://$ADDR:$PORT/" 2>/dev/null |
             sed -n 's/^[Ss]erver:[[:space:]]*//p' | tr -d '\r' | head -1 || true)
    [ -n "$SERVED" ] && break
    sleep 2
done
say served_by "${SERVED:-none}"

if [ -z "$SERVED" ]; then
    say error "nothing answered on http://$ADDR:$PORT/"
    say emulog "$EMULOG"; say drive "$HD"; exit 1
fi
if [ -n "$BUILD_VER" ] && [ "$SERVED" != "AmiNetXDuo-httpd/$BUILD_VER" ]; then
    say error "the guest is serving '$SERVED' and $BUILD built $BUILD_VER --\
 the drive was staged from somewhere else"
    exit 1
fi
say version_match ok

# --------------------------------------------------------------------------

HOSTPART="$ADDR"
[ "$PORT" = 80 ] || HOSTPART="$ADDR:$PORT"
say drawer "http://$HOSTPART/"
say shell "http://$HOSTPART/shell"
[ "$VARIANT" = rtg ] && say console "http://$HOSTPART/console"
NAMEPART="$NAME.local"
[ "$PORT" = 80 ] || NAMEPART="$NAME.local:$PORT"
say name "http://$NAMEPART/"
say emulog "$EMULOG"
say drive "$HD"
say stop "pkill -f 'amiberry -f $CFG'"
say RESULT UP
