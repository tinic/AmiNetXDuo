#!/usr/bin/env bash
#
# THE WORKBENCH SCREEN, OFF THE WIRE, DECODED, AS PIXELS.
#
#   tests/tools/run-console.sh [-a ADDRESS] [-p PORT] [-b BUILDDIR] [-m MODEL]
#                              [-B BACKEND] [-d DEPTH]... [-t SECONDS]
#                              [-s SECONDS] [-H CONSOLEHTML] [-o OUTDIR]
#
# WHAT IT PROVES
#
#   An emulated Amiga boots Commodore's own Workbench, serves /console, upgrades
#   a real WebSocket, and streams its screen.  tests/tools/console-probe.py
#   carries the whole receiver -- the WebSocket framing, the geom and pal words,
#   COPY, RAW, PackBits and PackBits-over-XOR -- and writes a PNG, so what is
#   asserted is that the pixels are a screen and not that a socket stayed open.
#   A session that connects and streams zeroes passes every other check there
#   is.
#
#   One boot per depth, because the depth is a property of the screen Workbench
#   opens and not of anything this can change while it is running.  Two planes
#   is what a stock 3.1 comes up as; four is what a person sets and is twice the
#   chip RAM to read per grab.
#
#   -R serves a graphics card instead, and its depths are the card's: 8 for the
#   palette screen the console has always read, and 15, 16, 24 or 32 for a
#   truecolour one.  All four truecolour depths arrive as one thing on the
#   wire -- the Amiga downsamples to r5g6b5 before it sends -- so the geom word
#   says format 2 and depth 16 for every one of them, and what differs between
#   the arms is what the guest read from.  The bytes and the frame rate are
#   directly comparable with the 8-bit arm: the same Workbench, the same size,
#   two bytes a pixel instead of one.
#
# BRIDGED, AND NOT SLIRP
#
#   -B ens18 puts the guest on the host's own LAN at a static address, which is
#   the only configuration a client can connect to and the only one a
#   measurement means anything on.  tools/amiberry-run.sh is not used here: the
#   screen under test is the one LoadWB opens, and that script writes a
#   Startup-Sequence of its own with no Workbench in it.
#
#   THE HOST RUNNING THE EMULATOR MAY NOT BE ABLE TO REACH ITS OWN GUEST.  A
#   frame this host sends to the guest's MAC never comes back to that NIC's
#   pcap, which is the same thing tests/tools/run-httpd.sh says at its own
#   client.  -c <user@host> runs the probe on a THIRD machine over ssh; without
#   it the probe runs here and a run that cannot reach the guest says so as
#   infra rather than as a failure of the thing under test.
#
# A FRESH MAC EVERY RUN
#
#   Derived from the process id, so a rerun is never answered out of the
#   router's ARP cache -- a reused address is how a defect hides.  The a2065's
#   LANCE then reports a DIFFERENT address on the wire; that is the emulator's
#   business and not this script's, since the guest is found by its static IP.
#
# WHAT IT PRINTS
#
#   key=value and an exit code:
#
#     d<N>_boot_seconds     how long the guest took to answer at all
#     d<N>_page_bytes       the console page it served
#     d<N>_geom             what the guest said its screen was
#     d<N>_fps              frames a second the probe actually received
#     d<N>_bytes_per_second and what they cost
#
#   -A scroll runs `dir SYS: ALL` in the Shell on the screen while the probe
#   watches, which is the worst case for anything that diffs tiles: every row
#   of the window moves on every frame.  -A idle, the default, measures the
#   other end of the range, where the encoder sends five bytes a frame.
#
#   -T <word> types that word and Return at the guest, as Amiga rawkey codes
#   down the same socket, and the run fails unless the screen then changes.
#   That is the input half's whole assertion: on an idle machine nothing else
#   moves a pixel, so a change after the keys and none before them is the keys
#   having arrived.  Use it with -A idle; a screen that is already scrolling
#   can prove nothing about what made it scroll.
#     d<N>_guest_fbstat     the guest's own counters: frames, bytes, and the
#                           ticks it spent grabbing and encoding
#     d<N>_png              a decoded frame, to look at
#     RESULT                PASS, FAIL or INFRA
#
#   An -R arm prints three more:
#
#     d<N>_rtg_format       the geom word's format: 1 palette, 2 truecolour
#     d<N>_rtg_wire_depth   and its depth, which is 16 for every truecolour arm
#     d<N>_rtg_mode         the display mode the guest opened, learned from the
#                           board rather than written down here; relearned once
#                           and reported as d<N>_rtg_mode_relearned when the
#                           first boot asked for a mode the board does not hold
#
#   A depth the board publishes no mode for is reported as d<N>_skipped and
#   the arm does not run.  Which modes an emulated uaegfx offers is the
#   emulator's answer to give, not a defect in anything here.
#
#   0 pass, 1 a failed assertion, 2 infrastructure.
#
# WHAT IT NEEDS
#
#   The five Workbench 3.1 ADFs (AMINETXDUO_ADF_DIR), a Kickstart that matches
#   the model, a2065.device (AMINETXDUO_A2065), a 68020 Release build, and the
#   console page.  The page is the committed src/tools/web/console.html, so a
#   build host with no node has one; -H names another.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-$ROOT/build/cm}"
MODEL="${AMINETXDUO_EMU_MODEL:-A1200}"
BACKEND="${AMINETXDUO_CONSOLE_BACKEND:-ens18}"
ADDRESS=192.168.1.232
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
PORT=8080
BOOT_MAX=240
PROBE_SECONDS=10
OUTDIR="$ROOT/build/console-out"
PAGE="${AMINETXDUO_CONSOLE_PAGE:-$ROOT/src/tools/web/console.html}"
CLIENT="${AMINETXDUO_CONSOLE_CLIENT:-}"
ACTIVITY=idle
TYPE=""
DEPTHS=()
# -R serves a GRAPHICS CARD instead of the chipset: Amiberry's uaegfx board,
# Picasso96 staged onto the drive, and Workbench put on an RTG screen of the
# depth the arm asked for.  8 is the palette screen; 15, 16, 24 and 32 are the
# truecolour ones, and they all reach the wire as r5g6b5.
RTG=0
P96DIR="${AMINETXDUO_P96_DIR:-$HOME/amiga-assets/p96}"
RTG_BOARD=uaegfx
RTGSCREEN=""
# What Amiberry's uaegfx calls 640x480 at eight bits; see AssignModeID() in its
# picasso96.  There is no such constant for the truecolour depths and none is
# guessed at: rtg_mode_id() takes those off the board's own published list,
# which C:rtgscreen 0 writes into DH0:rtglist.txt on every RTG boot.
RTG_MODE_ID=0x50031000
RTG_W=640
RTG_H=480
RTG_DEPTHS="8 15 16 24 32"

say() { printf '%s=%s\n' "$1" "$2"; }

while getopts "a:p:b:m:B:d:t:s:H:o:c:g:A:T:R" opt; do
    case "$opt" in
        a) ADDRESS="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        d) DEPTHS+=("$OPTARG") ;;
        t) BOOT_MAX="$OPTARG" ;;
        s) PROBE_SECONDS="$OPTARG" ;;
        H) PAGE="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        c) CLIENT="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        A) ACTIVITY="$OPTARG" ;;
        T) TYPE="$OPTARG" ;;
        R) RTG=1 ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

if [ "$RTG" = 1 ]; then
    # -R alone is the 8-bit arm it has always been.  -d names the others, and
    # only the depths the card can hold: a chipset depth here would ask
    # Picasso96 for a screen it cannot open and the run would come up planar,
    # which is the one failure this file works hardest to make impossible.
    [ ${#DEPTHS[@]} -gt 0 ] || DEPTHS=(8)
    for d in "${DEPTHS[@]}"; do
        case " $RTG_DEPTHS " in
            *" $d "*) ;;
            *) say error "-R takes a card depth, one of $RTG_DEPTHS, not $d"
               say RESULT INFRA
               exit 2 ;;
        esac
    done
    RTGSCREEN="${AMINETXDUO_RTGSCREEN:-$BUILD/tests/perf/rtgscreen}"
    [ -f "$RTGSCREEN" ] || {
        say error "no $RTGSCREEN"
        say hint "cmake --build $BUILD --parallel --target rtgscreen"
        say RESULT INFRA
        exit 2
    }
    for f in Libs/Picasso96API.library Libs/Picasso96/rtg.library \
             Libs/Picasso96/uaegfx.card Devs/Monitors/Picasso96; do
        [ -f "$P96DIR/$f" ] || {
            say error "no $P96DIR/$f"
            say hint "AMINETXDUO_P96_DIR must hold a Picasso96 install tree"
            say RESULT INFRA
            exit 2
        }
    done
else
    [ ${#DEPTHS[@]} -gt 0 ] || DEPTHS=(2 4)
fi

# ------------------------------------------------------------ the card ------
#
# What the geom word has to say for a given guest depth.  Four truecolour
# depths land on one wire format because the Amiga downsamples before it
# sends, so the arm asserts the pair rather than the depth it asked for.
rtg_wire_format() {
    case "$1" in
        8)  printf 1 ;;
        15|16|24|32) printf 2 ;;
        *)  printf 0 ;;
    esac
}

rtg_wire_depth() {
    case "$1" in
        8) printf 8 ;;
        *) printf 16 ;;
    esac
}

# The RGBFF mask the emulated board publishes.  0x112 is what the 8-bit arm
# has always had -- RGBFF_CLUT | RGBFF_R5G6B5PC | RGBFF_R8G8B8A8 -- and it
# already carries a 16-bit and a 32-bit mode as well as the palette one.  The
# other two bits are added only for the arm that needs them, so no arm that
# passes today gets a different board.
rtg_modes_mask() {
    case "$1" in
        15) printf '0x132' ;;   # + RGBFF_R5G5B5PC
        24) printf '0x116' ;;   # + RGBFF_R8G8B8
        *)  printf '0x112' ;;
    esac
}

# Every mode the display database holds, as C:rtgscreen 0 printed it during a
# boot: `rtgmode=<id> <w>x<h> depth=<n>`.  Kept across arms and across runs so
# that only the first RTG boot of a machine has to discover anything.
RTG_MODES_SEEN=""

rtg_mode_from_list() {
    local f="$1" d="$2"
    [ -n "$f" ] && [ -f "$f" ] || return 0
    awk -v want="depth=$d" -v size="${RTG_W}x${RTG_H}" \
        '/^rtgmode=/ { if ($2 == size && $3 == want) { print substr($1, 9)
                                                       exit } }' "$f"
}

# The mode ID to write into screenmode.prefs for a depth, or nothing when the
# board has not been asked yet.  A named override first, then what the board
# itself published, then the one constant that is known from the emulator's
# source.  NOT a table of guesses: a wrong ID opens no screen, Workbench stays
# on the chipset, and every other check in this file passes on that.
rtg_mode_id() {
    local d="$1" id=""
    eval "id=\${AMINETXDUO_RTG_MODE_ID_$d:-}"
    if [ -n "$id" ]; then printf '%s' "$id"; return 0; fi
    for f in "${AMINETXDUO_RTG_MODE_LIST:-}" "$RTG_MODES_SEEN"; do
        id=$(rtg_mode_from_list "$f" "$d")
        if [ -n "$id" ]; then printf '0x%s' "$id"; return 0; fi
    done
    # The one constant, and it is not depth 8's alone.  Amiberry's uaegfx
    # derives a mode ID from the width and the height and from nothing else --
    # AssignModeID() in its picasso96.cpp looks up (w, h) in a table and
    # returns 0x50001000 | id << 16 -- so 640x480 is 0x50031000 at 8, 15, 16,
    # 24 and 32 bits alike, and the depth is chosen by the screenmode.prefs
    # field beside it.  A board that numbers its modes some other way needs
    # AMINETXDUO_RTG_MODE_ID_<depth>, which is why the override is first.
    printf '%s' "$RTG_MODE_ID"
    return 0
}

# --------------------------------------------------------------- the parts --

HTTPD="$BUILD/src/tools/httpd"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$HTTPD" "$BSD"; do
    [ -f "$f" ] || {
        say error "no $f"
        say hint "cmake --build $BUILD --parallel"
        say RESULT INFRA
        exit 2
    }
done

# The page is a committed artifact, src/tools/web/console.html, the same as the
# Shell's: what a released archive carries is what this serves.  Named as the
# one missing thing when it is not there rather than built -- never faked,
# because a placeholder would make the one check that the page is served pass
# against something that is not the page.
[ -f "$PAGE" ] || {
    say error "no console page at $PAGE"
    say hint "node tools/web/build-console.mjs, or -H <console.html>"
    say RESULT INFRA
    exit 2
}

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    say error "no a2065.device; set AMINETXDUO_A2065=<path>"
    say RESULT INFRA
    exit 2
}

# A Kickstart must match the model: the A1200 40.68 image does not boot an A600
# and the machine then never reaches a Shell, which reads as a crash in the
# thing under test.  Same rule and same variables as tools/amiberry-run.sh.
model_var=$(printf '%s' "$MODEL" | tr '[:lower:]' '[:upper:]' | tr -c 'A-Z0-9' '_')
model_var=${model_var%_}
eval "KICKSTART=\${AMINETXDUO_KICKSTART_$model_var:-}"
KICKSTART="${KICKSTART:-${AMINETXDUO_KICKSTART:-}}"
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    say error "no Kickstart for $MODEL"
    say hint "set AMINETXDUO_KICKSTART_$model_var or AMINETXDUO_KICKSTART"
    say RESULT INFRA
    exit 2
}

AMIBERRY="${AMIBERRY:-$(command -v amiberry || true)}"
if [ -z "$AMIBERRY" ]; then
    for candidate in "$HOME/amiberry/build/amiberry" "$HOME/amiberry/amiberry"; do
        [ -x "$candidate" ] && { AMIBERRY="$candidate"; break; }
    done
fi
[ -n "$AMIBERRY" ] || {
    say error "amiberry not found; set AMIBERRY=<path>"
    say RESULT INFRA
    exit 2
}

# ------------------------------------------------------ Workbench 3.1 SYS: --

# shellcheck source=tests/tools/wb31-sys.sh
. "$ROOT/tests/tools/wb31-sys.sh"

wb31_assemble "$ROOT/build/wb31-sys" || { say RESULT INFRA; exit 2; }
WB="$WB31_SYS"

# ------------------------------------------------------- the monitor icon ----
#
# WHAT THE MONITOR READS BEFORE IT LOADS ANYTHING, and it is not in the file.
#
# devs/monitors/Picasso96 is one executable that drives every card Picasso96
# supports; which .card it opens comes from the TOOLTYPES of the icon beside
# it, which InstallPicasso96 writes in P_InstallCard.  Without an icon it
# loads no board driver, publishes no resolutions, and Workbench comes up on
# the chipset -- silently, because nothing on an Amiga complains about a
# missing .info.
#
# So the icon is generated rather than hand-copied: the archive ships
# Picasso96.info, which carries a PicassoIV's tooltypes and would be wrong
# here in exactly the way that is hardest to notice.  A minimal WBTOOL icon
# with a 8x8 one-plane image and the three tooltypes the installer sets that
# are not disabled -- the parenthesised ones it writes are off by convention.
rtg_monitor_icon() {
    local out="$1" board="$2"

    AMINETXDUO_ICON_BOARD="$board" python3 - "$out" <<'EOF'
import os, struct, sys

board = os.environ["AMINETXDUO_ICON_BOARD"]
# NO SettingsFile.  The one the archive ships is configured for a PicassoIV --
# its own installer says so and tells you to re-attach it with Picasso96Mode --
# and a settings file naming another board is worse than none: uaegfx reports
# its own resolutions and wants no timing list at all.
tools = ["IgnoreMask=Yes",
         "BoardType=" + board]

W, H, D = 8, 8, 1
rowbytes = ((W + 15) // 16) * 2

# struct Gadget, 44 bytes, inside the DiskObject at offset 4.  GadgetRender is
# non-NULL so the Image below it is read; SelectRender and GadgetText are not.
gadget = struct.pack(">LhhhhHHHLLLLLHL",
                     0,            # NextGadget
                     0, 0, W, H,   # LeftEdge, TopEdge, Width, Height
                     0x0004,       # Flags: GADGIMAGE
                     0, 1,         # Activation, GadgetType (BOOLGADGET)
                     1,            # GadgetRender, any non-zero
                     0, 0, 0, 0,   # SelectRender, GadgetText, Mutual, Special
                     0, 0)         # GadgetID, UserData
assert len(gadget) == 44, len(gadget)

# struct DiskObject: magic, version, the gadget, then the pointers that say
# which of the sections after it are present.
obj = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
    ">BBLLLLLLL",
    3, 0,       # do_Type = WBTOOL, pad
    0,          # DefaultTool: none
    1,          # ToolTypes: present
    0x80000000, 0x80000000,   # CurrentX, CurrentY = NO_ICON_POSITION
    0,          # DrawerData
    0,          # ToolWindow
    4096)       # StackSize
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

HD="$ROOT/build/console-dh0"

stage() {
    local depth="$1" mode_id="${2:-}"

    rm -rf "$HD"
    mkdir -p "$HD/Public" "$HD/Console"
    cp -R "$WB/." "$HD/"

    cp "$HTTPD" "$HD/C/httpd"
    chmod 755 "$HD/C/httpd"

    mkdir -p "$HD/Libs" "$HD/Devs/Networks" "$HD/Devs/NetInterfaces"
    cp "$BSD" "$HD/Libs/bsdsocket.library"
    cp "$A2065" "$HD/Devs/Networks/a2065.device"
    cp "$PAGE" "$HD/Console/console.html"

    cat > "$HD/Devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
EOF

    echo "Hello from an Amiga." > "$HD/Public/readme.txt"

    # A shell whose FROM script never ends: `dir SYS: ALL` on a loop is the
    # worst case for anything that diffs tiles, because every row of the window
    # moves on every frame.  run-wbgrab.sh drives its capture the same way.
    cat > "$HD/S/scroller" <<'EOF'
Lab loop
Dir SYS: ALL
Skip loop BACK
EOF
    chmod 644 "$HD/S/scroller"

    if [ "$RTG" = 1 ]; then
        # Picasso96 as its Installer would leave it, minus everything a
        # headless uaegfx does not touch.  rtg.library loads uaegfx.card out
        # of LIBS:Picasso96/ and emulation.library is what answers
        # cybergraphics.library, so both of the console's readback families
        # are on the machine.
        #
        # THE ARCHIVE'S OWN uaegfx.card IS THE RIGHT ONE, and no other copy is
        # needed from anywhere.  This used to delete it and demand a "modern
        # stub from WinUAE" in its place, on the reading that Picasso96 2.0's
        # 1998 card drives the obsolete uaelib trap.  It does not: with the
        # archive's card staged unchanged, the emulator logs
        #
        #   uaegfx.card 3.4 init @0020E2DC
        #   P96 RESINFO: 0020E380-0020EB60 (42,2016)
        #   uaegfx.card magic code: 00F04800-00F0494A BI=00263BC4
        #   SetSwitch() - Picasso96 640x480x8 - immediate
        #
        # -- the card finds the library uaegfx_card_install() builds in the
        # boot ROM through the magic-code handshake, and never touches the
        # trap.  Not one run has printed the obsolete-hook line.
        #
        # `Picasso96: Could not create graphics board context for 'uaegfx'` is
        # NOT the symptom it was read as.  DEVS:Monitors/<board> is already run
        # by the stock Startup-Sequence, so the harness's own second run of it
        # is a duplicate init, and that message is what a duplicate init says
        # on a board that came up perfectly.  It is in every green run here.
        cp -R "$P96DIR/Libs/." "$HD/Libs/"
        mkdir -p "$HD/Devs/Monitors"
        # THE MONITOR IS NAMED AFTER THE BOARD, and that is not cosmetic.
        # InstallPicasso96's P_InstallCard copies devs/monitors/Picasso96 with
        # (newname #_boardname) and writes BoardType=<boardname> into the icon
        # it drops beside it; the one file drives every card P96 supports and
        # the name is how it knows which .card to load.  Staged as `Picasso96`
        # it loads nothing, publishes no resolutions, and Workbench comes up on
        # the chipset with every check here still passing.
        cp "$P96DIR/Devs/Monitors/Picasso96" "$HD/Devs/Monitors/$RTG_BOARD"
        rtg_monitor_icon "$HD/Devs/Monitors/$RTG_BOARD.info" "$RTG_BOARD"
        # No mode ID for this depth yet means the board has not been asked what
        # it publishes.  The prefs file is written with the 8-bit one so the
        # boot is a normal RTG boot and its listing can be read; the arm then
        # restages with the answer.  Writing nothing instead would leave
        # Workbench on the chipset and cost the same boot.
        wb31_screenmode_prefs_id "$HD" "$depth" "${mode_id:-$RTG_MODE_ID}" \
                                 "$RTG_W" "$RTG_H"
        # And the prober, which is what actually puts an RTG screen in front:
        # screenmode.prefs moves WORKBENCH, and whether that lands depends on a
        # mode ID this harness worked out from the emulator's source.  The
        # prober asks the display database instead, prints every mode it holds,
        # and opens a public screen on the one the machine picked.
        [ -f "$RTGSCREEN" ] && { cp "$RTGSCREEN" "$HD/C/rtgscreen"; \
                                 chmod 755 "$HD/C/rtgscreen"; }
    else
        wb31_screenmode_prefs "$HD" "$depth"
    fi
}

# The stock 3.1 Startup-Sequence with the tail replaced.  LoadWB stays, since
# the screen under test is the one it opens; only EndCLI goes, because it would
# take the boot shell away before the server was started.
#
# `C:Wait 6` after LoadWB: Workbench draws its backdrop, its title bar and the
# disk icons after LoadWB returns, and a server started at once would be
# serving a screen that is still being drawn.
startup_for() {
    sed -e '/^EndCLI/d' "$WB/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
    cat >> "$HD/S/Startup-Sequence" <<EOF

FailAt 9999
C:Wait 6
EOF
    # AMINETXDUO_CONSOLE_PROBE_CMD runs on the guest, after Workbench is up and
    # before httpd, with its output on the serial port.  It exists so a question
    # about the SCREEN the server is about to serve -- what depth it really
    # opened, which BitMap layout it really got -- can be answered by the guest
    # rather than assumed by the harness.  Unset by default and nothing is
    # written when it is.
    if [ -n "${AMINETXDUO_CONSOLE_PROBE_CMD:-}" ]; then
        cp "$AMINETXDUO_CONSOLE_PROBE_BIN" "$HD/C/$(basename "$AMINETXDUO_CONSOLE_PROBE_BIN")"
        cat >> "$HD/S/Startup-Sequence" <<EOF
$AMINETXDUO_CONSOLE_PROBE_CMD
EOF
    fi

    # DH0 is a host directory, so anything the guest writes there is readable
    # from the outside at once.  This is the only way to ask an RTG boot what
    # it actually found: whether rtg.library loaded, whether it saw a board,
    # and what Intuition ended up opening.
    #
    # AND NO SCREEN OF ITS OWN.  This ran `rtgscreen 8 640 480` as well, which
    # put a PUBLIC, QUIET, EMPTY screen in front of Workbench -- and the front
    # screen is the one the console serves, so the session read a card screen
    # correctly and streamed 640x480 of colour zero.  Every RTG assertion
    # passed on it: format 1, a palette, a decoded frame, one pixel value.
    # screenmode.prefs puts WORKBENCH on the card and the geom word's format
    # says so per run, so there is nothing for a second screen to insure
    # against -- an -R run that comes up planar is meant to fail, not to be
    # rescued by a blank screen that hides what Workbench did.
    # AND IT NO LONGER RUNS THE MONITOR OR THE MODE LISTER, because both took
    # the guest down before it ever reached the httpd line below.
    #
    # `DEVS:Monitors/uaegfx` was here to prove the board had been found.  By
    # the time S:Startup-Sequence reaches it, LoadWB has already brought the
    # board up -- the emulator log shows SetSwitch() on a Picasso96 640x480
    # screen about fifteen seconds in -- so running the monitor a SECOND time
    # meets a board context that already exists.  It printed "Could not create
    # graphics board context for 'uaegfx'" and then took the machine with it:
    # a Gary timeout and a B-Trap at PC=ffffffff, with everything after it in
    # the sequence, httpd included, never reached.  From outside that is
    # d8_up=no and RESULT=INFRA, which reads as a network or staging fault
    # rather than as the guest having crashed on a diagnostic.  The message
    # was doubly misleading: it is a symptom of the board WORKING.
    #
    # `C:rtgscreen 0` went with it.  Amiberry's uaegfx assigns a mode ID from
    # the width and height alone -- AssignModeID() in its picasso96.cpp keys
    # on nothing else -- so 640x480 is one ID at every depth and there is no
    # per-depth list to learn.  What the lister cost was a whole boot and a
    # second chance to wedge; what it bought was a number already known.
    #
    # The remaining lines are all reads.  None of them opens a board, a screen
    # or a card, so none of them can do this again.
    [ "$RTG" = 1 ] && cat >> "$HD/S/Startup-Sequence" <<'EOF'
C:Version >DH0:rtg-ver.txt LIBS:Picasso96/rtg.library FILE
C:Version >>DH0:rtg-ver.txt LIBS:Picasso96API.library FILE
C:Version >>DH0:rtg-ver.txt Picasso96API.library
C:Version >>DH0:rtg-ver.txt cybergraphics.library
C:List >DH0:rtg-libs.txt LIBS:Picasso96
C:List >>DH0:rtg-libs.txt DEVS:Monitors
C:Avail >DH0:rtg-avail.txt
EOF

    cat >> "$HD/S/Startup-Sequence" <<EOF
Run >DH0:httpd.txt <NIL: C:httpd DH0:Public $PORT -C CONSOLEPAGE DH0:Console/console.html -v
EOF
    # The BOOT shell does the scrolling, in the window that is already on the
    # screen.  `NewShell CON:...` was tried first and opened nothing that ever
    # appeared in a grab -- no window, no changed tile, over twenty seconds --
    # so this drives the one window that is demonstrably there.  Last, because
    # it never returns.
    [ "$ACTIVITY" = "scroll" ] && cat >> "$HD/S/Startup-Sequence" <<'EOF'
Execute S:scroller
EOF
    chmod 755 "$HD/S/Startup-Sequence"
}

# ------------------------------------------------------------ the emulator --

# Amiberry links SDL2 with no driver of its own, so without this it asks for a
# video device, finds a stale DISPLAY from a failed X11 forward, and aborts in
# about a second -- which reads as a guest that never booted.
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"

#
# AN RTG BOARD PUBLISHES THE HOST'S DISPLAY MODES, SO A HEADLESS HOST HAS NONE.
#
# Amiberry's addresolutions() builds the whole Picasso96 resolution list out of
# the modes SDL enumerates for the host, and BOTH headless SDL drivers report
# none: `dummy` and `offscreen` each log "0 display modes.", the board then
# calls InitCard with an empty list -- "P96 RESINFO: 00000000-00000000 (0,0)"
# -- and the Amiga side has a graphics card with no resolutions on it.  Every
# library loads, the board is mapped, and Workbench comes up on the chipset.
#
# So the RTG arm gets a real X server with a real mode on it.  One host mode is
# enough: the FAKE-mode substitution in the same loop fills in every standard
# resolution smaller than it, which is where 640x480 comes from.  Xvfb is not
# a workaround for a missing display here, it is the source of the data the
# board reports.
XVFB_PID=""
if [ "$RTG" = 1 ] && [ -z "${AMINETXDUO_CONSOLE_NO_XVFB:-}" ]; then
    command -v Xvfb >/dev/null 2>&1 || {
        say error "no Xvfb, and an RTG board has no modes without one"
        say hint "apt install xvfb, or set AMINETXDUO_CONSOLE_NO_XVFB=1 and \
supply a DISPLAY that enumerates modes"
        say RESULT INFRA
        exit 2
    }
    XDISP=""
    for n in $(seq 90 99); do
        [ -e "/tmp/.X11-unix/X$n" ] || { XDISP=":$n"; break; }
    done
    [ -n "$XDISP" ] || { say error "no free X display in :90..:99"; \
                         say RESULT INFRA; exit 2; }
    Xvfb "$XDISP" -screen 0 1280x1024x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 2
    export DISPLAY="$XDISP"
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
    say xvfb "$XDISP 1280x1024x24 pid $XVFB_PID"
else
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
    [ "${SDL_VIDEODRIVER}" = "dummy" ] && unset DISPLAY WAYLAND_DISPLAY || true
fi

# Fresh per run: a reused MAC lets the router's cache answer for a guest that
# never came up, and the defect then looks like a pass.
MAC=$(printf '02:41:4d:49:%02x:%02x' $(( ($$ >> 8) & 0xff )) $(( $$ & 0xff )))

# TERM, and then KILL.  A headless Amiberry does not always go on TERM, and one
# that survives the harness leaves a guest ANSWERING AT THIS ADDRESS -- which
# the next run then measures instead of its own.  That happened: a run reported
# a one-second boot because the previous run's guest was still there.
EMU_PID=""
cleanup() {
    local n=0
    [ -n "$EMU_PID" ] || return 0
    kill -TERM "$EMU_PID" 2>/dev/null || true
    while [ "$n" -lt 10 ] && kill -0 "$EMU_PID" 2>/dev/null; do
        sleep 1
        n=$((n + 1))
    done
    kill -KILL "$EMU_PID" 2>/dev/null || true
    wait "$EMU_PID" 2>/dev/null || true
    EMU_PID=""
    return 0
}

reap_xvfb() {
    [ -n "$XVFB_PID" ] || return 0
    kill -TERM "$XVFB_PID" 2>/dev/null || true
    wait "$XVFB_PID" 2>/dev/null || true
    XVFB_PID=""
    return 0
}
trap 'cleanup; reap_xvfb' EXIT INT TERM HUP

EMULOG=""

boot() {
    local depth="$1"
    local cfg="$ROOT/build/console-d$depth.uae"

    EMULOG="$ROOT/build/amiberry-console-d$depth.log"

    cat > "$cfg" <<EOF
config_description=AmiNetXDuo console depth $depth
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

    # The graphics card: uaegfx, which is board type 0 and therefore what a
    # gfxcard_size on its own asks for.  8 MB is a good deal more than a
    # 640x480x8 screen needs and leaves Picasso96 room for its own buffers.
    # rtg_modes is the emulator's RGBFF_ mask and it is written out because a
    # minimal config does not go through the path that fills in the default.
    # rtg_modes_mask() says what each depth needs; a mask without RGBFF_CLUT is
    # a board that offers no palette mode and a Workbench that quietly stays on
    # the chipset, and the same is true of a truecolour arm whose bit is
    # missing.
    #
    # cpu_24bit_addressing=no IS NOT OPTIONAL AND IT FAILS SILENTLY WITHOUT IT.
    # uaegfx is a Zorro III board and a quickstart model comes up with a 24-bit
    # address space; the emulator then says "Z3 RTG and 24bit address space are
    # not compatible" into a log nothing reads, drops the board, and Workbench
    # comes up on the chipset.  Every assertion in this harness still passes on
    # that -- it is a screen, it has colours, it changes -- and the only thing
    # that says the card was never there is the depth in the geom word.
    [ "$RTG" = 1 ] && cat >> "$cfg" <<EOF
cpu_24bit_addressing=no
gfxcard_size=8
rtg_modes=$(rtg_modes_mask "$depth")
EOF

    # NOT --log.  It writes about a megabyte a second, playhouse3 is shared,
    # and a five-minute run left 427 MB of it here; nothing in this harness
    # reads it, because the backend assertion below is a better one.
    ( trap '' PIPE; exec "$AMIBERRY" -f "$cfg" ) >"$EMULOG" 2>&1 &
    EMU_PID=$!
}

# ---------------------------------------------------------------- the probe --
#
# On this host, or on a third machine over ssh.  The script and its output both
# travel, so a remote probe is the same file producing the same key=value.
probe() {
    local out="$1"
    shift
    if [ -z "$CLIENT" ]; then
        python3 "$ROOT/tests/tools/console-probe.py" "$@" > "$out" 2>&1
        return $?
    fi
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT" \
        "python3 - $*" < "$ROOT/tests/tools/console-probe.py" > "$out" 2>&1
}

# ONE HTTP CLIENT, AND IT IS THE ONE THE CLIENT MACHINE ALREADY HAS TO HAVE.
#
# These fetches used curl, and the probe travels as python3 -- so a client with
# python3 and no curl (playhouse4 carries wget instead) answered every probe and
# no fetch.  What that looks like from here is not "curl is missing": alive()
# returns the empty string on every attempt, the boot loop runs to BOOT_MAX, and
# the run reports d8_up=no on a guest that was serving the whole time.  Two
# 240-second RTG runs were spent on it.  python3 is already a hard requirement
# for the client, so the fetches use it too and the dependency list is one line
# shorter.
HTTP_GET_PY='
import sys, urllib.request, urllib.error
url, t = sys.argv[1], float(sys.argv[2])
try:
    with urllib.request.urlopen(url, timeout=t) as r:
        print(r.status, len(r.read()))
except urllib.error.HTTPError as e:
    print(e.code, 0)
except Exception:
    print(0, 0)
'

# url seconds -> "<status> <bytes>", "0 0" when nothing answered.
http_get() {
    local out
    if [ -z "$CLIENT" ]; then
        out=$(python3 -c "$HTTP_GET_PY" "$1" "$2" 2>/dev/null || true)
    else
        out=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT" \
              "python3 - '$1' '$2'" <<<"$HTTP_GET_PY" 2>/dev/null || true)
    fi
    printf '%s' "${out:-0 0}"
}

fetch_page() {
    http_get "http://$ADDRESS:$PORT/console" 8
}

alive() {
    local s
    s=$(http_get "http://$ADDRESS:$PORT/" 4)
    printf '%s' "${s%% *}"
}

# The board's own mode list, off the guest, while it is still booting.
#
# NOTHING ASKS FOR ONE ANY MORE and this waits for nothing.  The list came
# from `C:rtgscreen 0` in the Startup-Sequence, which is gone: see the staging
# above for why, and rtg_mode_id() for why the answer was not needed.  On this
# board a mode ID follows the width and the height alone, so there is one ID
# for the shape and the depth is the field beside it.
#
# Kept as a function, and kept returning failure, so that a board which does
# number its modes some other way has an obvious place to start: fill this in,
# and the caller already does the right thing with what it returns.  The
# escape hatch until then is AMINETXDUO_RTG_MODE_ID_<depth>.
rtg_wait_modes() {
    : "${1:-}"
    return 1
}

# ------------------------------------------------------------------- run ----

mkdir -p "$OUTDIR"
RTG_MODES_SEEN="$OUTDIR/rtgmodes.txt"
say address "$ADDRESS:$PORT"
say backend "$BACKEND"
say mac "$MAC"
say model "$MODEL"
say client "${CLIENT:-this host}"
say page "$PAGE"
say activity "$ACTIVITY"

# NOTHING MAY ALREADY BE AT THIS ADDRESS.
#
# The MAC is fresh every run but the address is not, and a guest left over from
# an earlier run answers on it exactly as this one's would.  A run that measures
# the previous run's machine is worse than a run that does not start.
if [ "$(alive)" = "200" ]; then
    say error "something already answers on http://$ADDRESS:$PORT/"
    say hint "an earlier guest is still up, or -a names an address in use"
    say RESULT INFRA
    exit 2
fi

VERDICT=pass
ran=0

for depth in "${DEPTHS[@]}"; do
    tag="d$depth"
    skipped=no

    mode_id=$(rtg_mode_id "$depth")
    stage "$depth" "$mode_id"
    startup_for
    started=$(date +%s)
    boot "$depth"

    # The mode ID comes off the board rather than out of this file.
    #
    # 640x480x8 is 0x50031000 because somebody read the emulator's source; the
    # truecolour ones have no such constant here, and a screenmode.prefs naming
    # a mode the board does not publish opens nothing at all.  Workbench then
    # comes up on the chipset and every check except the format word passes on
    # it.  So the first boot of a depth is allowed to be a boot that learns:
    # the listing arrives before the server does, and the arm restages on it.
    # Nothing is measured from this -- the boot clock above is what
    # d<N>_boot_seconds has always been, and the wait happens inside it.
    if [ "$RTG" = 1 ]; then
        if rtg_wait_modes 120; then
            say "${tag}_rtg_modes_seen" \
                "$(grep -c '^rtgmode=' "$RTG_MODES_SEEN" 2>/dev/null || true)"
            learned=$(rtg_mode_from_list "$RTG_MODES_SEEN" "$depth")
            if [ -z "$learned" ]; then
                # Not a failure of anything under test: Amiberry's uaegfx
                # publishes what the RGBFF mask and the host's own display
                # modes give it, and whether that includes a 15 or a 24-bit
                # mode is the board's answer to give.  The arm says so and
                # does not run.
                say "${tag}_skipped" "the board publishes no\
 ${RTG_W}x${RTG_H} mode at depth $depth; see $RTG_MODES_SEEN"
                cleanup
                skipped=yes
            elif [ "0x$learned" != "$mode_id" ]; then
                say "${tag}_rtg_mode_relearned" "0x$learned, not\
 ${mode_id:-none}"
                cleanup
                mode_id="0x$learned"
                stage "$depth" "$mode_id"
                startup_for
                started=$(date +%s)
                boot "$depth"
            fi
            [ "$skipped" = yes ] || say "${tag}_rtg_mode" "$mode_id"
        else
            say "${tag}_rtg_mode" "${mode_id:-none}"
        fi
    fi
    if [ "$skipped" = yes ]; then
        continue
    fi
    ran=$(( ran + 1 ))

    up=no
    # BOOT_MAX IS SECONDS, so the loop is bounded by the clock and not by a
    # count of attempts.  Each attempt costs a curl, and a curl at an address
    # nothing answers for costs its whole -m: counting attempts made the
    # ceiling 240 on a reachable guest and half an hour on an unreachable one,
    # which is the case the ceiling exists for.
    deadline=$(( started + BOOT_MAX ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        sleep 1
        kill -0 "$EMU_PID" 2>/dev/null || break
        [ "$(alive)" = "200" ] && { up=yes; break; }
    done
    say "${tag}_boot_seconds" "$(( $(date +%s) - started ))"

    if [ "$up" != yes ]; then
        say "${tag}_up" no
        say "${tag}_emulog" "$EMULOG"
        cleanup
        VERDICT=infra
        continue
    fi

    # BRIDGED IS PROVEN BY THE ADDRESS, not by a line in a log.  The interface
    # is configured STATIC on the host's own LAN and the client is another
    # machine on it: a guest that quietly came up on SLIRP is behind NAT at
    # 10.0.2.15 and cannot be reached from there at all, so an answer from
    # $ADDRESS is the assertion.  (Amiberry logs no UAENET line for the a2065,
    # so the string tools/amiberry-run.sh greps for is not available here.)
    say "${tag}_bridged" "confirmed by an answer at $ADDRESS from ${CLIENT:-this host}"

    # The page, from disk, on the same address the socket upgrades on.
    read -r code bytes <<<"$(fetch_page)"
    say "${tag}_page_status" "${code:-none}"
    say "${tag}_page_bytes" "${bytes:-0}"
    [ "${code:-0}" = "200" ] && [ "${bytes:-0}" -gt 1000 ] || VERDICT=fail

    typing=()
    [ -z "$TYPE" ] || typing=(--type "$TYPE")

    # A frozen picture passes everything else.
    #
    # A readback route that reads nothing wins the speed probe, and the console
    # then serves one grab for the whole session: frames arrive, the sequence
    # is whole, the picture has colours in it, and every check that only looks
    # at the socket says PASS.  --min-changed is the one that asks the guest --
    # it fails a session in which no frame differed from the one before it --
    # and it can only be asked when something on the screen is moving, so it
    # goes on when the run is scrolling or typing and not on an idle one.
    #
    # It is a truecolour thing here only because the arms it is new to are the
    # truecolour ones; the hole it closes is the same on all three formats, and
    # the 8-bit and planar arms are left exactly as they were.
    moving=()
    if [ "$RTG" = 1 ] && [ "$(rtg_wire_format "$depth")" = 2 ] &&
       { [ "$ACTIVITY" = "scroll" ] || [ -n "$TYPE" ]; }; then
        moving=(--min-changed 2)
    fi

    set +e
    probe "$OUTDIR/$tag-probe.txt" \
        "$ADDRESS" "$PORT" --seconds "$PROBE_SECONDS" \
        --png "$OUTDIR/$tag.png" --pfs "$OUTDIR/$tag.pfs" \
        ${typing[@]+"${typing[@]}"} ${moving[@]+"${moving[@]}"}
    rc=$?
    set -e

    while IFS='=' read -r k v; do
        [ -n "$k" ] || continue
        say "${tag}_$k" "$v"
    done < <(grep '=' "$OUTDIR/$tag-probe.txt" || true)

    #
    # -R MUST FAIL IF THE SESSION DID NOT COME UP ON A CARD.
    #
    # Every other check in this file passes on the chipset screen Workbench
    # falls back to when the board is missing: it is a screen, it has more
    # than one colour, its palette is not black, and it changes.  A run that
    # tested the planar path while reporting on the RTG one is worse than a
    # run that fails, and it happened three times here -- once on a 24-bit
    # address space, once on an RGBFF mask without CLUT, once on a monitor
    # file staged under the wrong name.  The seventh number of the geom word
    # is rfb_geom.format, and 0 is the only value that says the chipset: 1 is
    # the card's palette screen and 2 is a truecolour one downsampled to
    # r5g6b5.  Which of the two an arm must see is its depth's business, so the
    # third number is checked as well -- a 16-bit arm that came up on the 8-bit
    # Workbench screen would otherwise be a card session and pass.
    #
    # EVERY geom, not the geom.  A session gets a fresh geometry whenever the
    # screen it serves changes, and this read all of them into one variable:
    # the first run in which the front screen changed resolution mid-session
    # put two lines in `fmt`, and the assertion then failed a perfectly good
    # card session with "says format 1<newline>1, not 1".  sort -u collapses a
    # session that stayed on the card to the one value, and a session that
    # dropped to the chipset half way through still fails -- which is the case
    # this check exists for and the one a last-line-wins fix would have lost.
    if [ "$RTG" = 1 ]; then
        want_fmt=$(rtg_wire_format "$depth")
        want_depth=$(rtg_wire_depth "$depth")
        fmt=$(awk '/^geom=/ { print $NF }' "$OUTDIR/$tag-probe.txt" 2>/dev/null \
              | sort -u | tr '\n' ' ' || true)
        fmt=${fmt% }
        wire_depth=$(awk '/^geom=/ { print $3 }' "$OUTDIR/$tag-probe.txt" \
                     2>/dev/null | sort -u | tr '\n' ' ' || true)
        wire_depth=${wire_depth% }
        say "${tag}_rtg_format" "${fmt:-none}"
        say "${tag}_rtg_wire_depth" "${wire_depth:-none}"
        if [ "${fmt:-0}" != "$want_fmt" ] ||
           [ "${wire_depth:-0}" != "$want_depth" ]; then
            say "${tag}_error" "-R asked for a $depth-bit card screen, which is\
 format $want_fmt depth $want_depth on the wire, and the geom word says format\
 ${fmt:-none} depth ${wire_depth:-none}"
            VERDICT=fail
        fi
    fi

    case "$rc" in
        0) ;;
        2) VERDICT=infra ;;
        *) VERDICT=fail ;;
    esac

    # A PNG the probe wrote here rather than on the client machine is not one
    # this host can show, so it is copied back.
    if [ -n "$CLIENT" ] && [ "$rc" = "0" ]; then
        say "${tag}_png_on" "$CLIENT"
    fi

    cleanup
done

say outdir "$OUTDIR"
say arms_run "$ran"

# A run where every arm was skipped is not a pass.  The board publishing no
# mode at a depth is a fair reason for one arm not to run and no reason at all
# to report that the thing under test is good, which is what PASS would say.
if [ "$ran" = 0 ]; then
    say error "no arm ran: every depth asked for was skipped"
    VERDICT=infra
fi

say RESULT "$(printf '%s' "$VERDICT" | tr '[:lower:]' '[:upper:]')"

case "$VERDICT" in
    pass)  exit 0 ;;
    fail)  exit 1 ;;
    *)     exit 2 ;;
esac
