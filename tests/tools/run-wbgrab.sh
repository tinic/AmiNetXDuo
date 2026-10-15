#!/usr/bin/env bash
#
# wbgrab against a real Workbench 3.1, one boot per sequence.
#
#   tests/tools/run-wbgrab.sh [-b BUILDDIR] [-m MODEL] [-t SECONDS]
#                             [-o OUTDIR] [-s SEQUENCE]... [-d DEPTH]
#                             [-f FONT/SIZE] [-V 32] [-N BOARD]
#
# -f sets the Workbench screen font through ENVARC:Sys/font.prefs, e.g.
# helvetica.font/13.  -V 32 boots Workbench 3.2 on its own Kickstart.  -s netprefs grabs NetPrefs on each of its pages as
# netprefs.pfs, netprefs-1.pfs ... netprefs-4.pfs; netprefs_window is the
# window's box for cropping.  -s netswitch runs NetSwitch with the stack down
# and grabs netswitch.pfs: the legend, three not-attached rows, the "Network
# is not running." hint and the two buttons, all disabled.  -s netswitch-keys
# drives that window from the keyboard and the mouse -- a click that selects a
# row, then cursor keys past the last visible row so the list scrolls, Tab and
# Return against gadgets that are all disabled -- as netswitch-keys.pfs and
# netswitch-keys-1.pfs ... -4.pfs, with what WinKey sent and where it landed in
# netswitch-keys.txt.  -s netswitch-live is the same window against a running
# stack (needs -N BOARD, and a2065 is the board every other arm uses): one
# interface attached and up, the rest not attached, and the sequence toggles
# the selected interface off and on, opens the take-all requester, cancels it,
# opens it again and confirms, then waits past one 5-second poll, as
# netswitch-live.pfs and -1.pfs ... -9.pfs.  The requester's Cancel is gadget
# ID 0, which WinClick cannot name (its id is args[1] != 0 ? *(LONG *)args[1]
# : -1, so 0 means "no gadget given"), so that one click is by coordinates.
# -s netswitch-takeall is the take-all again with a subject it cannot miss:
# every definition is attached before the window opens, so every row that is
# up is one the list can show, and after the confirmed take-all each of them
# has to have changed inside its own glyph cell -- netswitch-takeall.pfs
# (before the click) and -1.pfs ... -5.pfs (-3 is the open requester, -4 and
# -5 are 4 s and 12 s after yes), with the stack's own INTERFACES listing
# before and after in netswitch-takeall.txt.
# -s netswitch-rows is the click-to-row mapping at its boundaries, with the
# stack down so nothing can be switched by a wrong row: the list's row bands
# are 13 px apart, and a click on the last pixel of one band, the first pixel
# of the next, the pixel above the first band and a point outside the list
# each grab a frame, so the FILLPEN band's own row can be read per click --
# netswitch-rows.pfs (the window's box) and -1.pfs ... -22.pfs.  The clicks are
# ordered so every probe is preceded by a click on the far row, which is what
# makes "the band did not move" distinguishable from "this frame is a stale
# repaint".  A click that lands on row N and selects row N+1 is how a user
# takes the wrong interface down, which is why this is worth a frame each.
# -s netwireless grabs NetWirelessPrefs against
# tests/tools/wifistub.c's unit with the NetWireless AppIcon beside it, then
# its join form and the Known networks window, as netwireless.pfs,
# netwireless-1.pfs and netwireless-2.pfs; then, with ENVARC: alone, moves
# Home below Cafe there and saves (known_envarc); then moves Cafe down and
# presses Use: the order stored, nothing connected (known_use); then
# selects the hidden Attic and presses Connect, with wmstub as
# C:WirelessManager: Attic on top in ENV: and ENVARC: (known_connect), the
# windows joining it grabbed as netwireless-3.pfs; after the 30 s the form
# is open, its Join starts a join (netwireless-4.pfs, "(connecting...)")
# and Cancel ends it (netwireless-5.pfs, the marker gone).  -s icons leaves the
# release tree's C drawer out on Workbench (DH0:.backdrop), opens it with a
# double-click and grabs its icons as icons.pfs; the tree is AMINETXDUO_DIST,
# build/dist/AmiNetXDuo by default (dist/make-dist.sh).
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-$ROOT/build/cm}"
MODEL="${AMINETXDUO_EMU_MODEL:-A1200}"
TIMEOUT=180
OUTDIR="$ROOT/build/wbgrab-out"
DEPTH=""
FONT=""
OSVER=31
NETBOARD=""
SEQUENCES=()

while getopts "b:m:t:o:s:d:f:V:N:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        d) DEPTH="$OPTARG" ;;
        s) SEQUENCES+=("$OPTARG") ;;
        f) FONT="$OPTARG" ;;
        V) OSVER="$OPTARG" ;;
        N) NETBOARD="$OPTARG" ;;
        *) sed -n '3,12p' "$0" >&2; exit 2 ;;
    esac
done

[ ${#SEQUENCES[@]} -gt 0 ] || SEQUENCES=(idle windows scroll)

say() { printf '%s=%s\n' "$1" "$2"; }

WBGRAB="$BUILD/src/tools/wbgrab"
[ -x "$WBGRAB" ] || {
    say error "no wbgrab at $WBGRAB"
    say hint "cmake --build $BUILD --parallel --target tool_wbgrab"
    exit 2
}

NETPREFS="$BUILD/src/tools/NetPrefs"
NETSWITCH="$BUILD/src/tools/NetSwitch"
NETWIRELESS="$BUILD/src/tools/NetWirelessPrefs"
NETWIRELESSCX="$BUILD/src/tools/NetWireless"
WIFISTUB="$BUILD/tests/tools/WifiStub"
WINCLICK="$BUILD/tests/tools/WinClick"
WINKEY="$BUILD/tests/tools/WinKey"
WMSTUB="$BUILD/tests/tools/WmStub"
NC="$BUILD/src/tools/nc"
SHOWNETSTATUS="$BUILD/src/tools/ShowNetStatus"
ADDIF="$BUILD/src/tools/AddNetInterface"
LIBSBSD="$BUILD/src/bsdsocket/bsdsocket.library"
for seq in "${SEQUENCES[@]}"; do
    [ "$seq" = netwireless ] || [ "$seq" = screens ] || continue
    [ -x "$NETWIRELESS" ] && [ -x "$NETWIRELESSCX" ] && [ -x "$WIFISTUB" ] &&
        [ -x "$WINCLICK" ] || {
        say error "no NetWirelessPrefs, NetWireless, WifiStub or WinClick under $BUILD"
        say hint "cmake --build $BUILD --parallel --target tool_netwirelessprefs tool_netwireless test_wifistub test_winclick"
        exit 2
    }
done
for seq in "${SEQUENCES[@]}"; do
    [ "$seq" = netprefs ] || continue
    [ -x "$NETPREFS" ] && [ -x "$WINCLICK" ] || {
        say error "no NetPrefs or WinClick under $BUILD"
        say hint "cmake --build $BUILD --parallel --target tool_netprefs test_winclick"
        exit 2
    }
done
for seq in "${SEQUENCES[@]}"; do
    [ "$seq" = netswitch ] || [ "$seq" = netswitch-rows ] || continue
    [ -x "$NETSWITCH" ] && [ -x "$WINCLICK" ] || {
        say error "no NetSwitch or WinClick under $BUILD"
        say hint "cmake --build $BUILD --parallel --target tool_netswitch test_winclick"
        exit 2
    }
done
for seq in "${SEQUENCES[@]}"; do
    [ "$seq" = netswitch-keys ] || continue
    [ -x "$NETSWITCH" ] && [ -x "$WINCLICK" ] && [ -x "$WINKEY" ] || {
        say error "no NetSwitch, WinClick or WinKey under $BUILD"
        say hint "cmake --build $BUILD --parallel --target tool_netswitch test_winclick test_winkey"
        exit 2
    }
done
for seq in "${SEQUENCES[@]}"; do
    case "$seq" in netswitch-live | netswitch-takeall) ;; *) continue ;; esac
    [ -n "$NETBOARD" ] || {
        say error "$seq needs a board: -N a2065"
        say hint "the stack has to come up for an interface to attach, and"
        say hint "the same board runs every other emulator arm (tools/cards.sh)"
        exit 2
    }
    [ -x "$NETSWITCH" ] && [ -x "$WINCLICK" ] && [ -x "$WINKEY" ] &&
        [ -x "$NC" ] && [ -x "$SHOWNETSTATUS" ] && [ -x "$ADDIF" ] &&
        [ -f "$LIBSBSD" ] || {
        say error "no NetSwitch, WinClick, WinKey, nc, ShowNetStatus, AddNetInterface or bsdsocket.library under $BUILD"
        say hint "cmake --build $BUILD --parallel --target tool_netswitch test_winclick test_winkey tool_nc tool_shownetstatus tool_addnetinterface bsdsocket_library"
        exit 2
    }
done

case "$FONT" in
    "") ;;
    */[0-9]*) ;;
    *) say error "-f wants FONT/SIZE, e.g. helvetica.font/13"; exit 2 ;;
esac

# ------------------------------------------------------------- the machine --

# shellcheck source=tools/emu-board.sh
. "$ROOT/tools/emu-board.sh"
# shellcheck source=tools/emu-mac.sh
. "$ROOT/tools/emu-mac.sh"
# shellcheck source=tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"

model_var=$(printf '%s' "$MODEL" | tr '[:lower:]' '[:upper:]' | tr -c 'A-Z0-9' '_')
model_var=${model_var%_}
if [ "$OSVER" = 32 ]; then
    eval "KICKSTART=\${AMINETXDUO_KICKSTART_${model_var}_32:-}"
    [ -n "$KICKSTART" ] || [ "$model_var" != A1200 ] ||
        KICKSTART="$HOME/amiga-assets/os32/rom/kicka1200.rom"
else
    eval "KICKSTART=\${AMINETXDUO_KICKSTART_$model_var:-}"
    KICKSTART="${KICKSTART:-${AMINETXDUO_KICKSTART:-}}"
fi
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    say error "no Kickstart for $MODEL"
    say hint "set AMINETXDUO_KICKSTART_$model_var or AMINETXDUO_KICKSTART"
    exit 2
}

AMIBERRY="${AMIBERRY:-$(command -v amiberry || true)}"
if [ -z "$AMIBERRY" ]; then
    for candidate in "$HOME/amiberry/build/amiberry" "$HOME/amiberry/amiberry"; do
        [ -x "$candidate" ] && { AMIBERRY="$candidate"; break; }
    done
fi
[ -n "$AMIBERRY" ] || { say error "amiberry not found; set AMIBERRY=<path>"; exit 2; }

# The drive, before anything below assembles, wipes or restages it: one
# directory for every run in this tree.  No network, so no bridge.
HD="$ROOT/build/wbgrab-dh0"
# shellcheck source=tools/emu-rig-lock.sh
. "$ROOT/tools/emu-rig-lock.sh"
rig_claim_drive "$HD" "wbgrab in $ROOT" none || exit $?

# ------------------------------------------------------ Workbench 3.1 SYS: --

# shellcheck source=tests/tools/wb31-sys.sh
. "$ROOT/tests/tools/wb31-sys.sh"

if [ "$OSVER" = 32 ]; then
    # Workbench 3.2, as install/test/run-smbmount.sh -o 32 boots it: the
    # lab's unpacked tree, else tools/mkwb32.sh from the CD's ADFs.
    WB="$ROOT/build/wb32-sys"
    TARBALL="${AMINETXDUO_WB32_TARBALL:-$HOME/amiga-assets/os32/wb32-sys-A1200.tar.gz}"
    if [ ! -d "$WB" ]; then
        mkdir -p "$WB"
        if [ -f "$TARBALL" ]; then tar xzf "$TARBALL" -C "$WB"
        else "$ROOT/tools/mkwb32.sh" -o "$WB" || exit 2; fi
        chmod -R a+rx "$WB"
    fi
else
    wb31_assemble "$ROOT/build/wb31-sys" || exit 2
    WB="$WB31_SYS"
fi

[ -n "$DEPTH" ] && say screen_depth_asked "$DEPTH" || true

# ------------------------------------------------------------ the drive ----

stage() {
    rm -rf "$HD"
    mkdir -p "$HD"
    cp -R "$WB/." "$HD/"
    cp "$WBGRAB" "$HD/C/wbgrab"
    chmod 755 "$HD/C/wbgrab"

    cat > "$HD/S/scroller" <<'EOF'
Lab loop
Dir SYS: ALL
Skip loop BACK
EOF

    cat > "$HD/S/winclose" <<'EOF'
C:Wait 2
EndCLI
EOF

    cat > "$HD/S/windower" <<'EOF'
Lab loop
NewShell CON:20/30/300/120/One FROM S:winclose
C:Wait 1
NewShell CON:200/80/320/120/Two FROM S:winclose
C:Wait 1
NewShell CON:80/140/360/100/Three FROM S:winclose
C:Wait 2
Skip loop BACK
EOF

    chmod 644 "$HD/S/scroller" "$HD/S/winclose" "$HD/S/windower"

    if [ -x "$NETPREFS" ] && [ -x "$WINCLICK" ]; then
        cp "$NETPREFS" "$HD/C/NetPrefs"
        cp "$WINCLICK" "$HD/C/WinClick"
        chmod 755 "$HD/C/NetPrefs" "$HD/C/WinClick"
        # One definition, so the form has text in it.
        mkdir -p "$HD/Devs/NetInterfaces"
        printf '%s\n' "DEVICE=DEVS:Networks/a2065.device" "UNIT=0" \
            "CONFIGURE=DHCP" "MTU=1500" > "$HD/Devs/NetInterfaces/ETH0"
    fi

    if [ -x "$NETWIRELESS" ] && [ -x "$NETWIRELESSCX" ] && [ -x "$WIFISTUB" ] &&
       [ -x "$WINCLICK" ]; then
        cp "$NETWIRELESS" "$HD/C/NetWirelessPrefs"
        cp "$NETWIRELESSCX" "$HD/C/NetWireless"
        cp "$WIFISTUB" "$HD/C/WifiStub"
        # Connect restarts WirelessManager: the stand-in that logs it.
        [ -x "$WMSTUB" ] && cp "$WMSTUB" "$HD/C/WirelessManager" &&
            chmod 755 "$HD/C/WirelessManager"
        cp "$WINCLICK" "$HD/C/WinClick"
        chmod 755 "$HD/C/NetWirelessPrefs" "$HD/C/NetWireless" \
            "$HD/C/WifiStub" "$HD/C/WinClick"
        # Three known networks, for the Known networks window.
        mkdir -p "$HD/Prefs/Env-Archive/Sys"
        printf '%s\n' "# known networks" \
            'network={' '	ssid="Home"' '	psk=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef' \
            '	priority=3' '}' 'network={' '	ssid="Cafe"' '	key_mgmt=NONE' '	priority=2' '}' \
            'network={' '	ssid="Attic"' '	key_mgmt=NONE' '	scan_ssid=1' '	priority=1' '}' \
            > "$HD/Prefs/Env-Archive/Sys/Wireless.prefs"
    fi

    if { [ "$seq" = netswitch ] || [ "$seq" = netswitch-rows ]; } &&
       [ -x "$NETSWITCH" ] && [ -x "$WINCLICK" ]; then
        cp "$NETSWITCH" "$HD/C/NetSwitch"
        cp "$WINCLICK" "$HD/C/WinClick"
        chmod 755 "$HD/C/NetSwitch" "$HD/C/WinClick"
        # Three definitions, so the list has three rows.  The stack is not
        # running, so every row reads as "not attached" (hollow square).
        mkdir -p "$HD/Devs/NetInterfaces"
        printf '%s\n' "DEVICE=DEVS:Networks/a2065.device" "UNIT=0" \
            "CONFIGURE=DHCP" "MTU=1500" > "$HD/Devs/NetInterfaces/ETH0"
        printf '%s\n' "DEVICE=DEVS:Networks/a2065.device" "UNIT=1" \
            "CONFIGURE=DHCP" "MTU=1500" > "$HD/Devs/NetInterfaces/ETH1"
        printf '%s\n' "DEVICE=DEVS:Networks/a2065.device" "UNIT=2" \
            "CONFIGURE=STATIC" "ADDRESS=192.168.1.50" \
            "NETMASK=255.255.255.0" "MTU=1500" \
            > "$HD/Devs/NetInterfaces/ETH2"
    fi

    if [ "$seq" = netswitch-keys ] && [ -x "$NETSWITCH" ] &&
       [ -x "$WINCLICK" ] && [ -x "$WINKEY" ]; then
        cp "$NETSWITCH" "$HD/C/NetSwitch"
        cp "$WINCLICK" "$HD/C/WinClick"
        cp "$WINKEY" "$HD/C/WinKey"
        chmod 755 "$HD/C/NetSwitch" "$HD/C/WinClick" "$HD/C/WinKey"
        # Ten definitions, more than the list can show at once, so the list
        # scrolls and its corner arrows appear.  The stack is not running, so
        # every row reads as "not attached".
        mkdir -p "$HD/Devs/NetInterfaces"
        for i in 0 1 2 3 4 5 6 7 8 9; do
            printf '%s\n' "DEVICE=DEVS:Networks/a2065.device" "UNIT=$i" \
                "CONFIGURE=DHCP" "MTU=1500" \
                > "$HD/Devs/NetInterfaces/ETH$i"
        done
    fi

    if { [ "$seq" = netswitch-live ] || [ "$seq" = netswitch-takeall ]; } &&
       [ -n "$NETBOARD" ]; then
        cp "$NETSWITCH" "$HD/C/NetSwitch"
        cp "$WINCLICK" "$HD/C/WinClick"
        cp "$WINKEY" "$HD/C/WinKey"
        cp "$NC" "$HD/C/nc"
        cp "$SHOWNETSTATUS" "$HD/C/ShowNetStatus"
        cp "$ADDIF" "$HD/C/AddNetInterface"
        chmod 755 "$HD/C/NetSwitch" "$HD/C/WinClick" "$HD/C/WinKey" \
            "$HD/C/nc" "$HD/C/ShowNetStatus" "$HD/C/AddNetInterface"
        # The stack itself, and the driver for the card in the machine.  It
        # starts when a socket program opens bsdsocket.library and goes away
        # with the last one, so the sequence keeps one listening.
        mkdir -p "$HD/Libs"
        cp "$LIBSBSD" "$HD/Libs/bsdsocket.library"
        AMINETXDUO_SANA2_DRIVER="${AMINETXDUO_SANA2_DRIVER:-${AMINETXDUO_A2065:-}}" \
            sana2_stage_driver "$NETBOARD" "$HD/Devs"
        # Nine definitions.  The card has one unit, so the first is the
        # interface that comes up and the rest are skipped by the stack
        # (ami_ns_open_devices keeps what opened and drops the rest): the list
        # then has both shapes at once, an up row and not-attached ones.  The
        # take-all arm brings every one of them up instead, so that whatever
        # six rows the list is showing are all up whatever order the directory
        # hands them over in.
        mkdir -p "$HD/Devs/NetInterfaces"
        for i in 0 1 2 3 4 5 6 7 8; do
            printf '%s\n' "DEVICE=${SANA2_DEVICE:-a2065.device}" "UNIT=0" \
                "CONFIGURE=STATIC" "ADDRESS=192.168.1.$((50 + i))" \
                "NETMASK=255.255.255.0" "MTU=1500" \
                > "$HD/Devs/NetInterfaces/ETH$i"
        done
    fi


    [ -n "$DEPTH" ] && wb31_screenmode_prefs "$HD" "$DEPTH"
    [ -n "$FONT" ] && font_prefs "$HD" "${FONT%/*}" "${FONT##*/}"
    return 0
}

# The screen font alone: GadTools windows take theirs from the screen.
font_prefs() {
    mkdir -p "$1/Prefs/Env-Archive/Sys"
    python3 - "$1/Prefs/Env-Archive/Sys/font.prefs" "$2" "$3" <<'EOF'
import struct, sys

path, name, size = sys.argv[1], sys.argv[2], int(sys.argv[3])
FP_SCREENFONT = 2
# FPF_ROMFONT for topaz 8, else FPF_DISKFONT | FPF_DESIGNED.
flags = 0x01 if (name, size) == ("topaz.font", 8) else 0x42

def chunk(tag, payload):
    out = tag + struct.pack(">L", len(payload)) + payload
    return out + (b"\0" if len(payload) & 1 else b"")

# struct FontPrefs: reserved[3], reserved2, type, front, back, drawmode, pad,
# TextAttr (name pointer, ysize, style, flags), name[128].
font = struct.pack(">3L HH BBBx L HBB", 0, 0, 0, 0, FP_SCREENFONT,
                   1, 0, 1, 0, size, 0, flags)
font += name.encode("latin-1").ljust(128, b"\0")
body = b"PREF" + chunk(b"PRHD", bytes(6)) + chunk(b"FONT", font)
with open(path, "wb") as fh:
    fh.write(b"FORM" + struct.pack(">L", len(body)) + body)
EOF
}

startup_with() {
    # 3.2 spells it Startup-sequence; AmigaDOS does not mind, Linux does.
    local ss
    ss=$(find "$WB/S" -maxdepth 1 -iname 'startup-sequence' | head -1)
    rm -f "$HD/S/$(basename "$ss")"
    sed -e '/^EndCLI/d' "$ss" > "$HD/S/Startup-Sequence"
    printf '\n%s\n' "$1" >> "$HD/S/Startup-Sequence"
    chmod 755 "$HD/S/Startup-Sequence"
}

tail_for() {
    case "$1" in
    idle)
        cat <<'EOF'
FailAt 9999
C:Wait 6
C:wbgrab FRAMES 50 DELAY 2 TO DH0:idle.pfs >DH0:idle.txt
Echo >DH0:.done "$RC"
EOF
        ;;
    windows)
        cat <<'EOF'
FailAt 9999
C:Wait 6
NewShell CON:0/11/240/60/Driver FROM S:windower
C:Wait 2
C:wbgrab FRAMES 100 DELAY 2 TO DH0:windows.pfs >DH0:windows.txt
Echo >DH0:.done "$RC"
EOF
        ;;
    scroll)
        cat <<'EOF'
FailAt 9999
C:Wait 6
NewShell CON:0/11/640/190/Scroll FROM S:scroller
C:Wait 3
C:wbgrab FRAMES 100 DELAY 2 TO DH0:scroll.pfs >DH0:scroll.txt
Echo >DH0:.done "$RC"
EOF
        ;;
    netprefs)
        # TIMES 0 only reports the window's box; each later call cycles
        # the page gadget (GID_PANEL) once.
        cat <<'EOF'
FailAt 9999
C:Wait 6
Run >NIL: C:NetPrefs
C:Wait 4
C:WinClick "AmiNetXDuo Network" 2 TIMES 0 >DH0:netprefs.txt
C:wbgrab TO DH0:netprefs.pfs >>DH0:netprefs.txt
C:WinClick "AmiNetXDuo Network" 2 >NIL:
C:Wait 1
C:wbgrab TO DH0:netprefs-1.pfs >NIL:
C:WinClick "AmiNetXDuo Network" 2 >NIL:
C:Wait 1
C:wbgrab TO DH0:netprefs-2.pfs >NIL:
C:WinClick "AmiNetXDuo Network" 2 >NIL:
C:Wait 1
C:wbgrab TO DH0:netprefs-3.pfs >NIL:
C:WinClick "AmiNetXDuo Network" 2 >NIL:
C:Wait 1
C:wbgrab TO DH0:netprefs-4.pfs >NIL:
Echo >DH0:.done "$RC"
EOF
        ;;
    netswitch)
        # The stack is not running: the window shows the legend, three
        # not-attached rows, the "Network is not running." hint and the two
        # buttons, all disabled.  The X/Y TIMES 0 click only reports the
        # window's box.
        cat <<'EOF'
FailAt 9999
C:Wait 6
Run >NIL: C:NetSwitch
C:Wait 4
C:WinClick "AmiNetXDuo NetSwitch" X 5 Y 5 TIMES 0 >DH0:netswitch.txt
C:wbgrab TO DH0:netswitch.pfs >>DH0:netswitch.txt
Echo >DH0:.done "$RC"
EOF
        ;;
    netswitch-rows)
        # Where the three row bands begin and end, and what a click inside the
        # list but off the rows does.  WinClick's X and Y are the window's
        # content, the way netswitch_layout.c lays it out: three 13 px bands at
        # y 22, 35 and 48 (netswitch_layout.c: row_h = fh + ROW_LEAD = 13,
        # first row at list_y + CELL_PAD = 22 in the window's content).  The
        # probes are the last pixel of a band against the first pixel of the
        # next (34/35 and 47/48), the first pixel of each band (22/35/48), one
        # pixel above the first band (21), 5 (the legend, above the list) and
        # 61 (the list's own bottom padding, past the last row), then 19 and 17
        # (the list's top padding, down to one pixel above the list) and 5
        # again (the legend).  The stack is down, so nothing can be switched by a click
        # that lands wrong, and every probe is preceded by a click on a band
        # the probe could not land on, so that a probe that selects something
        # is visible as a band that moved instead of a frame that did not
        # change.
        cat <<'EOF'
FailAt 9999
C:Wait 6
Run >NIL: C:NetSwitch
C:Wait 4
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 60 TIMES 0 >DH0:netswitch-rows.txt
C:wbgrab TO DH0:netswitch-rows.pfs >>DH0:netswitch-rows.txt
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 60 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-1.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 34 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-2.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 35 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-3.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 60 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-4.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 47 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-5.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 60 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-6.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 48 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-7.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 22 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-8.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 60 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-9.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 21 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-10.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 60 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-11.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 5 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-12.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 60 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-13.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 61 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-14.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 22 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-15.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 61 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-16.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 60 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-17.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 19 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-18.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 60 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-19.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 17 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-20.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 60 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-21.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 5 >>DH0:netswitch-rows.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-rows-22.pfs >NIL:
Echo >DH0:.done "$RC"
EOF
        ;;
    netswitch-keys)
        # The same window driven from the keyboard and the mouse.  The click
        # activates the window and selects row 0; the cursor keys move the
        # selection, past the last row the list can show, which scrolls it.
        # Nothing is attached, so both buttons are disabled: Tab has nowhere
        # to go and Return is the disabled gadget's no-op, and the two grabs
        # are what that looks like.  WinKey is the keyboard half of WinClick.
        cat <<'EOF'
FailAt 9999
C:Wait 6
Run >NIL: C:NetSwitch
C:Wait 4
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 40 TIMES 0 >DH0:netswitch-keys.txt
C:wbgrab TO DH0:netswitch-keys.pfs >>DH0:netswitch-keys.txt
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 40 >>DH0:netswitch-keys.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-keys-1.pfs >NIL:
C:WinKey DOWN TIMES 8 >>DH0:netswitch-keys.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-keys-2.pfs >NIL:
C:WinKey TAB >>DH0:netswitch-keys.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-keys-3.pfs >NIL:
C:WinKey RETURN >>DH0:netswitch-keys.txt
C:Wait 2
C:wbgrab TO DH0:netswitch-keys-4.pfs >NIL:
Echo >DH0:.done "$RC"
EOF
        ;;
    netswitch-live)
        # The stack is up: nc listens on a port and holds bsdsocket.library
        # open for the length of the arm.  Opening the library starts the
        # stack loopback-only -- no definition is attached by starting it, so
        # AddNetInterface attaches ETH0 and brings it up, and the other eight
        # stay definitions.  One Tab puts the focus on the action button,
        # which is enabled because the selected row is up; Return takes it
        # down and Return again brings it back.  Two more Tabs reach Take all
        # offline, enabled because one interface is online, and Return opens
        # the requester: Cancel first, then yes.  ShowNetStatus writes the
        # stack's own answer down as text at the start and at the end, so the
        # pictures can be read against it.
        cat <<'EOF'
FailAt 9999
C:Wait 6
Run >NIL: C:nc -4 -l 4321
C:Wait 2
C:AddNetInterface DEVS:NetInterfaces/ETH0 >DH0:netswitch-live.txt
C:Wait 3
C:ShowNetStatus INTERFACES >>DH0:netswitch-live.txt
Run >NIL: C:NetSwitch
C:Wait 4
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 40 TIMES 0 >>DH0:netswitch-live.txt
C:wbgrab TO DH0:netswitch-live.pfs >>DH0:netswitch-live.txt
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 40 >>DH0:netswitch-live.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-live-1.pfs >NIL:
C:WinKey TAB >>DH0:netswitch-live.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-live-2.pfs >NIL:
C:WinKey RETURN >>DH0:netswitch-live.txt
C:Wait 3
C:wbgrab TO DH0:netswitch-live-3.pfs >NIL:
C:WinKey RETURN >>DH0:netswitch-live.txt
C:Wait 3
C:wbgrab TO DH0:netswitch-live-4.pfs >NIL:
C:WinKey TAB >>DH0:netswitch-live.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-live-5.pfs >NIL:
C:WinKey RETURN >>DH0:netswitch-live.txt
C:Wait 2
C:WinClick "AmiNetXDuo NetSwitch" X 5 Y 5 TIMES 0 >>DH0:netswitch-live.txt
C:wbgrab TO DH0:netswitch-live-6.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" X 294 Y 44 >>DH0:netswitch-live.txt
C:Wait 2
C:wbgrab TO DH0:netswitch-live-7.pfs >NIL:
C:WinKey RETURN >>DH0:netswitch-live.txt
C:Wait 2
C:WinClick "AmiNetXDuo NetSwitch" X 5 Y 5 TIMES 0 >>DH0:netswitch-live.txt
C:WinClick "AmiNetXDuo NetSwitch" 1 >>DH0:netswitch-live.txt
C:Wait 3
C:wbgrab TO DH0:netswitch-live-8.pfs >NIL:
C:Wait 8
C:wbgrab TO DH0:netswitch-live-9.pfs >NIL:
C:ShowNetStatus INTERFACES >>DH0:netswitch-live.txt
Echo >DH0:.done "$RC"
EOF
        ;;
    netswitch-takeall)
        # The take-all against a subject the list cannot hide.  The live arm
        # attaches one interface and lets the click land wherever the
        # directory's order puts it, so if that one row is one of the three
        # the six-row list is not showing, the confirmed take-all has nothing
        # to do on screen and the arm passes while testing nothing.  Here
        # every definition is attached first, so every row that is up is one
        # of the six the list is showing whichever six they are, whatever
        # order the directory hands them over in.  The stack has four
        # interface slots and AddNetInterface says so (ENOSPC, naming the four
        # that hold them), so nine definitions leave the first four up; the
        # click then selects one of them and the others stay unselected
        # subjects.  After the confirmed take-all every visible row that was
        # an online disc has to have changed inside its own glyph cell.  Two
        # Tabs from the selected row reach Take all offline, Return opens its
        # requester (Cancel is gadget 0, which WinClick cannot name), and
        # gadget 1 is yes.
        cat <<'EOF'
FailAt 9999
C:Wait 6
Run >NIL: C:nc -4 -l 4321
C:Wait 2
C:AddNetInterface DEVS:NetInterfaces/ETH0 >DH0:netswitch-takeall.txt
C:Wait 2
C:AddNetInterface DEVS:NetInterfaces/ETH1 >>DH0:netswitch-takeall.txt
C:Wait 2
C:AddNetInterface DEVS:NetInterfaces/ETH2 >>DH0:netswitch-takeall.txt
C:Wait 2
C:AddNetInterface DEVS:NetInterfaces/ETH3 >>DH0:netswitch-takeall.txt
C:Wait 2
C:AddNetInterface DEVS:NetInterfaces/ETH4 >>DH0:netswitch-takeall.txt
C:Wait 2
C:AddNetInterface DEVS:NetInterfaces/ETH5 >>DH0:netswitch-takeall.txt
C:Wait 2
C:AddNetInterface DEVS:NetInterfaces/ETH6 >>DH0:netswitch-takeall.txt
C:Wait 2
C:AddNetInterface DEVS:NetInterfaces/ETH7 >>DH0:netswitch-takeall.txt
C:Wait 2
C:AddNetInterface DEVS:NetInterfaces/ETH8 >>DH0:netswitch-takeall.txt
C:Wait 12
C:ShowNetStatus INTERFACES >>DH0:netswitch-takeall.txt
Run >NIL: C:NetSwitch
C:Wait 4
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 40 TIMES 0 >>DH0:netswitch-takeall.txt
C:wbgrab TO DH0:netswitch-takeall.pfs >>DH0:netswitch-takeall.txt
C:WinClick "AmiNetXDuo NetSwitch" X 12 Y 40 >>DH0:netswitch-takeall.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-takeall-1.pfs >NIL:
C:WinKey TAB >>DH0:netswitch-takeall.txt
C:WinKey TAB >>DH0:netswitch-takeall.txt
C:Wait 1
C:wbgrab TO DH0:netswitch-takeall-2.pfs >NIL:
C:WinKey RETURN >>DH0:netswitch-takeall.txt
C:Wait 2
C:WinClick "AmiNetXDuo NetSwitch" X 5 Y 5 TIMES 0 >>DH0:netswitch-takeall.txt
C:wbgrab TO DH0:netswitch-takeall-3.pfs >NIL:
C:WinClick "AmiNetXDuo NetSwitch" 1 >>DH0:netswitch-takeall.txt
C:Wait 4
C:wbgrab TO DH0:netswitch-takeall-4.pfs >NIL:
C:Wait 8
C:wbgrab TO DH0:netswitch-takeall-5.pfs >NIL:
C:ShowNetStatus INTERFACES >>DH0:netswitch-takeall.txt
Echo >DH0:.done "$RC"
EOF
        ;;
    screens)
        # The Wi-Fi window open beside the AppIcon, folded to its title bar
        # by the zoom gadget, and the AppIcon alone once it has quit.
        cat <<'EOF'
FailAt 9999
C:Wait 6
Run >NIL: C:WifiStub SERVE
C:Wait 2
Run >NIL: C:NetWireless DEVICE=wifistub.device
C:Wait 2
Run >NIL: C:NetWirelessPrefs DEVICE=wifistub.device
C:Wait 8
C:wbgrab TO DH0:screens.pfs >DH0:screens.txt
C:WinClick "Wi-Fi" ZIP >DH0:zip.txt
C:Wait 2
C:WinClick "Wi-Fi" 4 TIMES 0 >>DH0:zip.txt
C:wbgrab TO DH0:screens-1.pfs >NIL:
C:Break `C:Status COMMAND=C:NetWirelessPrefs` C >NIL:
C:Wait 3
C:wbgrab TO DH0:screens-2.pfs >NIL:
Echo >DH0:.done "$RC"
EOF
        ;;
    netwireless)
        # The window once its first scan is in, then Join... (gadget 4)
        # with nothing selected: the form for a hidden network.
        cat <<'EOF'
FailAt 9999
C:Wait 6
Run >NIL: C:WifiStub SERVE
C:Wait 2
Run >NIL: C:NetWireless DEVICE=wifistub.device
C:Wait 2
Run >NIL: C:NetWirelessPrefs DEVICE=wifistub.device
C:Wait 20
C:NetWirelessPrefs DEVICE=wifistub.device STATUS >DH0:nwstatus.txt
GetEnv WiFi >>DH0:nwstatus.txt
C:WinClick "Wi-Fi" 4 TIMES 0 >DH0:netwireless.txt
C:wbgrab TO DH0:netwireless.pfs >>DH0:netwireless.txt
C:WinClick "Wi-Fi" 4 >NIL:
C:Wait 2
C:WinClick "Join a" 7 TIMES 0 >DH0:netwireless-join.txt
C:wbgrab TO DH0:netwireless-1.pfs >NIL:
C:WinClick "Join a" 14 >NIL:
C:Wait 1
C:WinClick "Wi-Fi" 5 >NIL:
C:Wait 2
C:wbgrab TO DH0:netwireless-2.pfs >NIL:
C:WinClick "Known Wi-Fi" 21 >NIL:
C:Wait 1
Delete >NIL: ENV:Sys/Wireless.prefs
C:WinClick "Wi-Fi" 5 >NIL:
C:Wait 2
C:WinClick "Known Wi-Fi" 15 AT 6 >NIL:
C:WinClick "Known Wi-Fi" 17 >NIL:
C:WinClick "Known Wi-Fi" 19 >NIL:
C:Wait 2
C:WinClick "Known Wi-Fi" 21 TIMES 0 >DH0:known-envarc.txt
Type ENV:Sys/Wireless.prefs >>DH0:known-envarc.txt
C:WinClick "Wi-Fi" 5 >NIL:
C:Wait 2
C:WinClick "Known Wi-Fi" 15 AT 6 >NIL:
C:WinClick "Known Wi-Fi" 17 >NIL:
C:WinClick "Known Wi-Fi" 20 >NIL:
C:Wait 2
Type ENV:Sys/Wireless.prefs >DH0:known-use.txt
If EXISTS T:wmstub.log
  Echo "wm=started" >>DH0:known-use.txt
Else
  Echo "wm=none" >>DH0:known-use.txt
EndIf
C:WinClick "Wi-Fi" 5 >NIL:
C:Wait 2
C:WinClick "Known Wi-Fi" 15 AT 22 >NIL:
C:WinClick "Known Wi-Fi" 22 >NIL:
C:Wait 3
C:wbgrab TO DH0:netwireless-3.pfs >NIL:
Type ENV:Sys/Wireless.prefs >DH0:known-connect.txt
Type ENVARC:Sys/Wireless.prefs >DH0:known-connect-arc.txt
C:Wait 32
C:WinClick "Join a" 13 >NIL:
C:Wait 3
C:wbgrab TO DH0:netwireless-4.pfs >NIL:
C:WinClick "Join a" 14 >NIL:
C:Wait 2
C:wbgrab TO DH0:netwireless-5.pfs >NIL:
Echo >DH0:.done "$RC"
EOF
        ;;
    icons)
        # 3.1 puts the left-out C.info at its snapshot, 62,146
        # (layout-icons.py); 3.2 lines left-out icons up at the left.
        local xy="X 90 Y 153"
        [ "$OSVER" = 32 ] && xy="X 38 Y 22"
        cat <<EOF
FailAt 9999
C:Wait 6
C:WinClick "Workbench" $xy TIMES 2 >DH0:icons.txt
C:Wait 4
C:wbgrab TO DH0:icons.pfs >>DH0:icons.txt
Echo >DH0:.done "\$RC"
EOF
        ;;
    *)
        say error "unknown sequence: $1"
        exit 2
        ;;
    esac
}

# The release tree's four tools with their Icons/ icons, in a drawer left out
# on the Workbench window.  The release has no C.info: the drawer's icon is
# Examples.info at 62,146.
stage_icons() {
    local dist="${AMINETXDUO_DIST:-$ROOT/build/dist/AmiNetXDuo}" t
    [ -d "$dist/Icons" ] || { say error "no $dist/Icons (dist/make-dist.sh)"; exit 2; }
    mkdir -p "$HD/AmiNetXDuo/C"
    for t in NetPrefs NetMeter NetWireless NetWirelessPrefs; do
        cp "$dist/C/$t" "$dist/Icons/$t.info" "$HD/AmiNetXDuo/C/"
    done
    cp "$dist/Examples.info" "$HD/AmiNetXDuo/C.info"
    python3 - "$ROOT/install/tools/layout-icons.py" "$HD/AmiNetXDuo/C.info" <<'PY'
import importlib.util, pathlib, sys
spec = importlib.util.spec_from_file_location("layout", sys.argv[1])
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)
layout.edit(pathlib.Path(sys.argv[2]), (62, 146), (280, 12, 340, 176))
PY
    printf ':AmiNetXDuo/C\n' > "$HD/.backdrop"
}

# ------------------------------------------------------------- the emulator --

export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
[ "${SDL_VIDEODRIVER}" = "dummy" ] && unset DISPLAY WAYLAND_DISPLAY || true

EMU_PID=""
SERIAL_PID=""
cleanup() {
    [ -n "$EMU_PID" ] && { kill -TERM "$EMU_PID" 2>/dev/null || true; }
    [ -n "$SERIAL_PID" ] && { kill -TERM "$SERIAL_PID" 2>/dev/null || true; }
    EMU_PID=""; SERIAL_PID=""
}
trap cleanup EXIT INT TERM HUP

BOOT_STATUS=0
BOOT_SECONDS=0
boot() {
    local name="$1"
    local cfg="$ROOT/build/wbgrab-$name.uae"
    local serial="$ROOT/build/serial-wbgrab-$name.log"
    local elapsed=0 port

    port=$((12000 + $(printf '%s' "wbgrab-$name-$$" | cksum | cut -d' ' -f1) % 900))

    : > "$serial"
    rm -f "$HD/.done"

    cat > "$cfg" <<EOF
config_description=AmiNetXDuo wbgrab $name
use_gui=no
headless=true
quickstart=$MODEL,0
kickstart_rom_file=$KICKSTART
fastmem_size=8
floppy0type=-1
nr_floppies=0
uaehf0=dir,rw,DH0:DH0:$HD,0
serial_port=tcp://127.0.0.1:$port/wait
EOF

    # The one sequence that needs the stack has a card in the machine.  slirp
    # is user mode: it takes no host interface, so the drive claim above stays
    # "none" and nothing here touches the bench's LAN.  The MAC is the run's
    # own, derived from the sequence name, the way tools/amiberry-run.sh does
    # it -- one default on two guests is one address in two ARP caches.
    if [ -n "$NETBOARD" ]; then
        emu_board_lines "$NETBOARD" "$(emu_mac_default "wbgrab-$name")" \
            slirp >> "$cfg" || exit 2
    fi

    ( trap '' PIPE; exec "$AMIBERRY" --log -f "$cfg" ) \
        >"$ROOT/build/amiberry-wbgrab-$name.log" 2>&1 &
    EMU_PID=$!

    # serial_port=.../wait blocks the emulator until something connects, so
    # retry until it is listening or the emulator is gone.
    (
        for _ in $(seq 1 60); do
            kill -0 "$EMU_PID" 2>/dev/null || exit 0
            nc 127.0.0.1 "$port" >> "$serial" 2>/dev/null && exit 0
            sleep 0.5
        done
    ) &
    SERIAL_PID=$!

    BOOT_STATUS=124
    while [ "$elapsed" -lt "$TIMEOUT" ]; do
        if [ -f "$HD/.done" ]; then
            BOOT_STATUS=$(tr -dc '0-9' < "$HD/.done" | head -c 4)
            BOOT_STATUS=${BOOT_STATUS:-0}
            break
        fi
        kill -0 "$EMU_PID" 2>/dev/null || { BOOT_STATUS=125; break; }
        sleep 1
        elapsed=$((elapsed + 1))
    done
    BOOT_SECONDS=$elapsed

    kill -TERM "$EMU_PID" 2>/dev/null || true
    wait "$EMU_PID" 2>/dev/null || true
    EMU_PID=""
    kill -TERM "$SERIAL_PID" 2>/dev/null || true
    SERIAL_PID=""
}

# ------------------------------------------------------------------ run ----

mkdir -p "$OUTDIR"
FAILED=0

for seq in "${SEQUENCES[@]}"; do
    stage
    [ "$seq" = icons ] && stage_icons
    startup_with "$(tail_for "$seq")"
    boot "$seq"

    say "${seq}_boot_seconds" "$BOOT_SECONDS"
    say "${seq}_guest_rc"     "$BOOT_STATUS"

    if [ -f "$HD/$seq.txt" ]; then
        # wbgrab's own key=value, prefixed with the sequence.
        while IFS='=' read -r k v; do
            [ -n "$k" ] || continue
            say "${seq}_$k" "$v"
        done < <(tr -d '\r' < "$HD/$seq.txt" | grep '=')
    else
        say "${seq}_output" "MISSING"
    fi

    if [ -f "$HD/$seq.pfs" ]; then
        cp "$HD/$seq.pfs" "$OUTDIR/$seq.pfs"
        say "${seq}_pfs_bytes" "$(wc -c < "$OUTDIR/$seq.pfs" | tr -d ' ')"
        for extra in "$HD/$seq"-*.pfs; do
            [ -f "$extra" ] || continue
            cp "$extra" "$OUTDIR/"
            say "$(basename "$extra" .pfs | tr '-' '_')_pfs_bytes" \
                "$(wc -c < "$extra" | tr -d ' ')"
        done
    else
        say "${seq}_pfs" "MISSING"
        FAILED=1
    fi

    # Known networks with ENVARC: alone: Home moved below Cafe and saved,
    # the window closed.
    if [ "$seq" = netwireless ]; then
        k="$HD/known-envarc.txt"
        if [ -f "$k" ] && tr -d '\r\t' < "$k" |
               awk '/ssid="Home"/{h=1} /ssid="Cafe"/{h=0} h&&/^priority=2$/{hp=1}
                    !h&&/^priority=3$/{cp=1} /^error=no window$/{w=1}
                    END{exit !(hp&&cp&&w)}'; then
            say known_envarc PASS
        else
            say known_envarc FAIL
            FAILED=1
        fi
        # Down then Use: the order, no connect (the selection was moved).
        k="$HD/known-use.txt"
        if [ -f "$k" ] && tr -d '\r\t' < "$k" |
               awk '/ssid="Home"/{h=1} /ssid="(Cafe|Attic)"/{h=0} h&&/^priority=3$/{ok=1}
                    /^wm=none$/{w=1} END{exit !(ok&&w)}'; then
            say known_use PASS
        else
            say known_use FAIL
            FAILED=1
        fi
        # Connect on the hidden Attic: on top in ENV: and ENVARC:.
        _kc=PASS
        for k in "$HD/known-connect.txt" "$HD/known-connect-arc.txt"; do
            [ -f "$k" ] && tr -d '\r\t' < "$k" |
                awk '/ssid="Attic"/{a=1} /ssid="(Home|Cafe)"/{a=0} a&&/^priority=3$/{ok=1}
                     END{exit !ok}' || _kc=FAIL
        done
        say known_connect "$_kc"
        [ "$_kc" = PASS ] || FAILED=1
    fi

    [ "$BOOT_STATUS" = "0" ] || FAILED=1
done

say outdir "$OUTDIR"
say RESULT "$([ "$FAILED" = 0 ] && echo PASS || echo FAIL)"
exit "$FAILED"
