#!/usr/bin/env bash
# THE WORKBENCH SCREEN, OFF THE WIRE, DECODED, AS PIXELS.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-$ROOT/build/cm}"
MODEL="${AMINETXDUO_EMU_MODEL:-A1200}"
BACKEND="${AMINETXDUO_CONSOLE_BACKEND:-ens18}"
ADDRESS=192.168.1.232
ADDRESS_SET=0
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
PORT=8080
BOOT_MAX=240
PROBE_SECONDS=10
PROBE_SECONDS_SET=0
OUTDIR="$ROOT/build/console-out"
PAGE="${AMINETXDUO_CONSOLE_PAGE:-$ROOT/src/tools/web/console.html}"
CLIENT="${AMINETXDUO_CONSOLE_CLIENT:-}"
ACTIVITY=idle
TYPE=""
LATENCY=""
DEPTHS=()
RTG=0
PATTERN=0
P96DIR="${AMINETXDUO_P96_DIR:-$HOME/amiga-assets/p96}"
RTG_BOARD=uaegfx
RTGMODES=""
RTGBARS=""
RTG_MODE_ID=0x50031000
RTG_W=640
RTG_H=480
RTG_DEPTHS="8 15 16 24 32"

# The screen the guest opens, when it is not the whole of the mode.  0 means
# the path's own default: 640x256 for a Workbench arm, 320x256 for a chipset
# one, and the mode's own size for an RTG one.
#
# WHY IT IS A KNOB AT ALL.  An Amiga screen and its bitmap are not the same
# size: a planar bitmap is allocated in whole 16-pixel words and an RTG board
# rounds to its own pitch, so a 312-wide screen sits in a 320-wide bitmap and a
# 632-wide card screen in a 640-wide one.  Serving the allocation instead of
# the screen is the defect 1ec557b9 fixed, and it survived a year because every
# size the lab ever asked for was a multiple of 16 with nothing to pad.
SCREEN_W=0
SCREEN_H=0

CHIPSET=()
CHIP_W=320
CHIP_H=256
CHIP_MODES="ham6 ham8 ehb"
CHIP=""

say() { printf '%s=%s\n' "$1" "$2"; }

while getopts "a:p:b:m:B:d:C:t:s:H:o:c:g:A:T:L:S:RP" opt; do
    case "$opt" in
        a) ADDRESS="$OPTARG"; ADDRESS_SET=1 ;;
        p) PORT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        d) DEPTHS+=("$OPTARG") ;;
        C) CHIPSET+=("$OPTARG") ;;
        t) BOOT_MAX="$OPTARG" ;;
        s) PROBE_SECONDS="$OPTARG"; PROBE_SECONDS_SET=1 ;;
        H) PAGE="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        c) CLIENT="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        A) ACTIVITY="$OPTARG" ;;
        L) LATENCY="$OPTARG" ;;
        T) TYPE="$OPTARG" ;;
        S) case "$OPTARG" in
               [0-9]*x[0-9]*) SCREEN_W=${OPTARG%%x*}; SCREEN_H=${OPTARG##*x} ;;
               *) say error "-S takes WxH, not $OPTARG"
                  say RESULT INFRA
                  exit 2 ;;
           esac ;;
        R) RTG=1 ;;
        P) RTG=1; PATTERN=1 ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

# Resolved here rather than beside -C, and staged on EVERY arm: at depth 0 it
# opens nothing and reports the front screen, which is the only way an arm
# serving the Workbench can say how wide that screen really is.  The size on the
# wire is measured against the screen and never against what the prefs asked
# for -- Workbench rounds a 632-pixel request up to 640, and an arm that
# compared the wire with the request would call that a console defect.
CHIPSCREEN="${AMINETXDUO_CHIPSCREEN:-$BUILD/tests/perf/chipscreen}"

if [ "$RTG" = 1 ] && [ ${#CHIPSET[@]} -gt 0 ]; then
    say error "-C is a chipset screen and -R and -P are a card's; one run serves one of them"
    say RESULT INFRA
    exit 2
fi

if [ ${#CHIPSET[@]} -gt 0 ]; then
    for m in "${CHIPSET[@]}"; do
        case " $CHIP_MODES " in
            *" $m "*) ;;
            *) say error "-C takes one of $CHIP_MODES, not $m"
               say RESULT INFRA
               exit 2 ;;
        esac
    done
    case " ${CHIPSET[*]} " in
        *" ham8 "*)
            case "$MODEL" in
                A1200|a1200|A4000|a4000|CD32|cd32) ;;
                *) say error "-C ham8 is an AGA mode and $MODEL is not an AGA machine"
                   say hint "-m A1200"
                   say RESULT INFRA
                   exit 2 ;;
            esac ;;
    esac
    [ -f "$CHIPSCREEN" ] || {
        say error "no $CHIPSCREEN"
        say hint "cmake --build $BUILD --parallel --target chipscreen"
        say RESULT INFRA
        exit 2
    }
    [ -f "$ROOT/tests/tools/chipscreen-check.py" ] || {
        say error "no $ROOT/tests/tools/chipscreen-check.py"
        say RESULT INFRA
        exit 2
    }
elif [ "$RTG" = 1 ]; then
    if [ ${#DEPTHS[@]} -eq 0 ]; then
        if [ "$PATTERN" = 1 ]; then DEPTHS=(15 16 24 32); else DEPTHS=(8); fi
    fi
    for d in "${DEPTHS[@]}"; do
        case " $RTG_DEPTHS " in
            *" $d "*) ;;
            *) say error "-R takes a card depth, one of $RTG_DEPTHS, not $d"
               say RESULT INFRA
               exit 2 ;;
        esac
    done
    if [ "$PATTERN" = 1 ] && [ "$PROBE_SECONDS_SET" = 0 ]; then
        PROBE_SECONDS=120
    fi

    if [ "$PATTERN" = 1 ]; then
        RTGMODES="${AMINETXDUO_RTGMODES:-$BUILD/tests/perf/rtgmodes}"
        RTGBARS="${AMINETXDUO_RTGBARS:-$BUILD/tests/perf/rtgbars}"
        for f in "$RTGMODES" "$RTGBARS"; do
            [ -f "$f" ] || {
                say error "no $f"
                say hint "cmake --build $BUILD --parallel --target rtgmodes rtgbars"
                say RESULT INFRA
                exit 2
            }
        done
        [ -f "$ROOT/tests/tools/rtgbars-check.py" ] || {
            say error "no $ROOT/tests/tools/rtgbars-check.py"
            say RESULT INFRA
            exit 2
        }
    fi
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

# The mode is chosen by RTG_W x RTG_H and the SCREEN is opened at these, so an
# RTG arm can ask for a screen narrower than the mode it runs on.  That is the
# shape of the reported defect: a 1368-wide screen in a 1600-wide bitmap.
WB_W=640
WB_H=256
RTG_SW=$RTG_W
RTG_SH=$RTG_H
if [ "$SCREEN_W" != 0 ]; then
    CHIP_W=$SCREEN_W; CHIP_H=$SCREEN_H
    WB_W=$SCREEN_W;   WB_H=$SCREEN_H
    RTG_SW=$SCREEN_W; RTG_SH=$SCREEN_H
fi

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

rtg_modes_mask() {
    case "$1" in
        15) printf '0x132' ;;   # + RGBFF_R5G5B5PC
        24) printf '0x116' ;;   # + RGBFF_R8G8B8
        *)  printf '0x112' ;;
    esac
}

rtg_db_depth() {
    case "$1" in
        8)     printf 8 ;;
        15)    printf 15 ;;
        16)    printf 16 ;;
        24|32) printf 24 ;;
        *)     printf 0 ;;
    esac
}

rtg_card_fmt() {
    case "$1" in
        8)  printf CLUT ;;
        15) printf R5G5B5PC ;;
        16) printf R5G6B5PC ;;
        24) printf R8G8B8 ;;
        32) printf R8G8B8A8 ;;
        *)  printf none ;;
    esac
}

rtg_pattern_mask() {
    case "$1" in
        8)  printf '0x002' ;;   # RGBFF_CLUT
        15) printf '0x022' ;;   # + RGBFF_R5G5B5PC
        16) printf '0x012' ;;   # + RGBFF_R5G6B5PC
        24) printf '0x006' ;;   # + RGBFF_R8G8B8
        32) printf '0x102' ;;   # + RGBFF_R8G8B8A8
        *)  printf '0x112' ;;
    esac
}

RTG_MODES_SEEN=""

rtg_mode_from_list() {
    local f="$1" d="$2"
    [ -n "$f" ] && [ -f "$f" ] || return 0
    awk -v want="depth=$d" -v size="${RTG_W}x${RTG_H}" \
        '/^rtgmode=/ { if ($2 == size && $3 == want) { print substr($1, 9)
                                                       exit } }' "$f"
}

rtg_mode_id() {
    local d="$1" id=""
    eval "id=\${AMINETXDUO_RTG_MODE_ID_$d:-}"
    if [ -n "$id" ]; then printf '%s' "$id"; return 0; fi
    for f in "${AMINETXDUO_RTG_MODE_LIST:-}" "$RTG_MODES_SEEN"; do
        id=$(rtg_mode_from_list "$f" "$d")
        if [ -n "$id" ]; then printf '0x%s' "$id"; return 0; fi
    done
    printf '%s' "$RTG_MODE_ID"
    return 0
}

arm_tag() {
    case "$1" in
        ham6|ham8|ehb) printf '%s' "$1" ;;
        *)             printf 'd%s' "$1" ;;
    esac
}

arm_depth() {
    case "$1" in
        ham6|ehb) printf 6 ;;
        ham8)     printf 8 ;;
        *)        printf '%s' "$1" ;;
    esac
}

arm_mode_id() {
    case "$1" in
        ham6|ham8) printf '0x00021800' ;;
        ehb)       printf '0x00021080' ;;
        *)         printf '' ;;
    esac
}

arm_wire_format() {
    case "$1" in
        ham6) printf 3 ;;
        ham8) printf 4 ;;
        ehb)  printf 5 ;;
        *)    printf 0 ;;
    esac
}

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

# THE RIG IS SHARED.  Several agents boot guests on this host at once, and this
# harness used to take a fixed LAN address and no lock at all: two runs then
# answered for one address and the slower one read as a console that had
# stopped serving.  The address is claimed against every other run AND pinged
# against the LAN, so it may safely overlap real hosts.
# shellcheck source=tools/emu-rig-lock.sh
. "$ROOT/tools/emu-rig-lock.sh"

rig_claim_name_shared bridged-rig "console ($BACKEND) in $ROOT" || {
    say RESULT INFRA
    exit 2
}

if [ "$ADDRESS_SET" = 0 ]; then
    rig_claim_address 192.168.1 200 239 "console in $ROOT" >/dev/null || {
        say error "no free address in 192.168.1.200-239"
        say RESULT INFRA
        exit 2
    }
    ADDRESS="$RIG_ADDRESS"
fi

# THE SERIAL PORT, so a machine that never reaches a Shell can be told from one
# whose httpd is broken.  Both look identical from the network side -- nothing
# answers -- and that is exactly how a Kickstart that does not match the model
# reads: an arm that times out with nothing to show.
rig_claim_port "console in $ROOT" || {
    say RESULT INFRA
    exit 2
}
SERIAL_PORT="$RIG_PORT"

# shellcheck source=tests/tools/wb31-sys.sh
. "$ROOT/tests/tools/wb31-sys.sh"

wb31_assemble "$ROOT/build/wb31-sys" || { say RESULT INFRA; exit 2; }
WB="$WB31_SYS"

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

    if [ -f "$CHIPSCREEN" ]; then
        cp "$CHIPSCREEN" "$HD/C/chipscreen"
        chmod 755 "$HD/C/chipscreen"
    fi

    cat > "$HD/Devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
EOF

    echo "Hello from an Amiga." > "$HD/Public/readme.txt"

    cat > "$HD/S/scroller" <<'EOF'
Lab loop
Dir SYS: ALL
Skip loop BACK
EOF
    chmod 644 "$HD/S/scroller"

    if [ "$RTG" = 1 ]; then
        cp -R "$P96DIR/Libs/." "$HD/Libs/"
        mkdir -p "$HD/Devs/Monitors"
        cp "$P96DIR/Devs/Monitors/Picasso96" "$HD/Devs/Monitors/$RTG_BOARD"
        rtg_monitor_icon "$HD/Devs/Monitors/$RTG_BOARD.info" "$RTG_BOARD"
        if [ "$PATTERN" = 1 ]; then
            wb31_screenmode_prefs_id "$HD" 8 "$RTG_MODE_ID" "$RTG_SW" "$RTG_SH"
        else
            wb31_screenmode_prefs_id "$HD" "$depth" "${mode_id:-$RTG_MODE_ID}" \
                                     "$RTG_SW" "$RTG_SH"
        fi
        if [ "$PATTERN" = 1 ]; then
            cp "$RTGMODES" "$HD/C/rtgmodes"; chmod 755 "$HD/C/rtgmodes"
            cp "$RTGBARS"  "$HD/C/rtgbars";  chmod 755 "$HD/C/rtgbars"
        fi
    elif [ -n "$CHIP" ]; then
        wb31_screenmode_prefs_id "$HD" "$depth" "$(arm_mode_id "$CHIP")" \
                                 "$CHIP_W" "$CHIP_H"
    else
        wb31_screenmode_prefs_id "$HD" "$depth" 0x00029000 "$WB_W" "$WB_H"
    fi
}

startup_for() {
    local depth="${1:-}"
    local db_depth
    db_depth=$(rtg_db_depth "${depth:-0}")

    sed -e '/^EndCLI/d' "$WB/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
    cat >> "$HD/S/Startup-Sequence" <<EOF

FailAt 9999
C:Wait 6
EOF
    if [ -n "${AMINETXDUO_CONSOLE_PROBE_CMD:-}" ]; then
        cp "$AMINETXDUO_CONSOLE_PROBE_BIN" "$HD/C/$(basename "$AMINETXDUO_CONSOLE_PROBE_BIN")"
        cat >> "$HD/S/Startup-Sequence" <<EOF
$AMINETXDUO_CONSOLE_PROBE_CMD
EOF
    fi

    [ "$RTG" = 1 ] && cat >> "$HD/S/Startup-Sequence" <<'EOF'
C:Version >DH0:rtg-ver.txt LIBS:Picasso96/rtg.library FILE
C:Version >>DH0:rtg-ver.txt LIBS:Picasso96API.library FILE
C:Version >>DH0:rtg-ver.txt Picasso96API.library
C:Version >>DH0:rtg-ver.txt cybergraphics.library
C:List >DH0:rtg-libs.txt LIBS:Picasso96
C:List >>DH0:rtg-libs.txt DEVS:Monitors
C:Avail >DH0:rtg-avail.txt
EOF

    if [ "$PATTERN" = 1 ]; then
        cat >> "$HD/S/Startup-Sequence" <<EOF
C:rtgmodes >DH0:rtglist.txt
Run >NIL: <NIL: C:rtgbars $db_depth $RTG_SW $RTG_SH
C:Wait 5
EOF
    fi

    if [ -z "$CHIP" ] && [ -f "$HD/C/chipscreen" ]; then
        cat >> "$HD/S/Startup-Sequence" <<'EOF'
C:chipscreen 0 0 0 0 DH0:frontscreen.txt
EOF
    fi

    if [ -n "$CHIP" ]; then
        cat >> "$HD/S/Startup-Sequence" <<EOF
C:chipscreen $(arm_mode_id "$CHIP") $CHIP_W $CHIP_H 0 DH0:chipscreen-wb.txt
Run >DH0:chipscreen-run.txt <NIL: C:chipscreen $(arm_mode_id "$CHIP") $CHIP_W $CHIP_H $depth DH0:chipscreen.txt
C:Wait 15
EOF
    fi

    cat >> "$HD/S/Startup-Sequence" <<EOF
Run >DH0:httpd.txt <NIL: C:httpd DH0:Public $PORT -C CONSOLEPAGE DH0:Console/console.html -v
EOF
    [ "$ACTIVITY" = "scroll" ] && cat >> "$HD/S/Startup-Sequence" <<'EOF'
Execute S:scroller
EOF
    chmod 755 "$HD/S/Startup-Sequence"
}

export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"

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
    for n in $(seq 90 149); do
        [ -e "/tmp/.X11-unix/X$n" ] || { XDISP=":$n"; break; }
    done
    [ -n "$XDISP" ] || { say error "no free X display in :90..:149"; \
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

MAC=$(printf '02:41:4d:49:%02x:%02x' $(( ($$ >> 8) & 0xff )) $(( $$ & 0xff )))

EMU_PID=""
READER_PID=""
cleanup() {
    local n=0
    if [ -n "$READER_PID" ]; then
        kill -TERM "$READER_PID" 2>/dev/null || true
        wait "$READER_PID" 2>/dev/null || true
        READER_PID=""
    fi
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
SERIALLOG=""

boot() {
    local tag="$1"
    local depth="$2"
    local mask=""
    local cfg="$ROOT/build/console-$tag.uae"

    EMULOG="$ROOT/build/amiberry-console-$tag.log"
    SERIALLOG="$ROOT/build/console-serial-$tag.log"
    : > "$SERIALLOG"

    cat > "$cfg" <<EOF
config_description=AmiNetXDuo console $tag
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
serial_port=tcp://127.0.0.1:$SERIAL_PORT/wait
uaeserial=false
EOF

    if [ "$RTG" = 1 ]; then
        if [ "$PATTERN" = 1 ]; then
            mask=$(rtg_pattern_mask "$depth")
        else
            mask=$(rtg_modes_mask "$depth")
        fi
        cat >> "$cfg" <<EOF
cpu_24bit_addressing=no
gfxcard_size=8
rtg_modes=$mask
EOF
    fi

    ( trap '' PIPE; exec "$AMIBERRY" --log -f "$cfg" ) >"$EMULOG" 2>&1 &
    EMU_PID=$!

    # `/wait` holds the emulator until this connects, and the emulator has to
    # have opened the listener first, so the connect is retried.  The reader is
    # nc's own pid and not this subshell's: killing the subshell alone leaves
    # the reader on the port, and the next run then refuses to boot.
    (
        reader=""
        trap '[ -z "$reader" ] || kill -TERM "$reader" 2>/dev/null; exit 0' \
             TERM INT
        n=0
        while [ "$n" -lt 90 ]; do
            kill -0 "$EMU_PID" 2>/dev/null || exit 0
            nc 127.0.0.1 "$SERIAL_PORT" >> "$SERIALLOG" 2>/dev/null &
            reader=$!
            wait "$reader" && exit 0
            reader=""
            sleep 1
            n=$((n + 1))
        done
    ) &
    READER_PID=$!
}

probe() {
    local out="$1"
    shift
    if [ -z "$CLIENT" ]; then
        python3 "$ROOT/tests/tools/console-probe.py" "$@" > "$out" 2>&1
        return $?
    fi
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT" \
        "python3 - $(printf '%q ' "$@")" \
        < "$ROOT/tests/tools/console-probe.py" > "$out" 2>&1
}

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

rtg_wait_modes() {
    local limit="${1:-120}" n=0

    [ "$PATTERN" = 1 ] || return 1

    while [ "$n" -lt "$limit" ]; do
        if [ -f "$HD/rtglist.txt" ] &&
           grep -q '^result=listed' "$HD/rtglist.txt" 2>/dev/null; then
            cp "$HD/rtglist.txt" "$RTG_MODES_SEEN"
            return 0
        fi
        kill -0 "$EMU_PID" 2>/dev/null || return 1
        sleep 1
        n=$((n + 1))
    done
    return 1
}

mkdir -p "$OUTDIR"
RTG_MODES_SEEN="$OUTDIR/rtgmodes.txt"
say address "$ADDRESS:$PORT"
say backend "$BACKEND"
say mac "$MAC"
say model "$MODEL"
say client "${CLIENT:-this host}"
say page "$PAGE"
say activity "$ACTIVITY"
if [ "$SCREEN_W" != 0 ]; then
    say screen "${SCREEN_W}x${SCREEN_H} asked for"
fi
if [ "$PATTERN" = 1 ]; then
    say pattern "rtgbars colour bars, checked with tests/tools/rtgbars-check.py"
fi

if [ "$(alive)" = "200" ]; then
    say error "something already answers on http://$ADDRESS:$PORT/"
    say hint "an earlier guest is still up, or -a names an address in use"
    say RESULT INFRA
    exit 2
fi

VERDICT=pass
ran=0

if [ ${#CHIPSET[@]} -gt 0 ]; then
    ARMS=("${CHIPSET[@]}")
else
    ARMS=("${DEPTHS[@]}")
fi

for arm in "${ARMS[@]}"; do
    tag=$(arm_tag "$arm")
    depth=$(arm_depth "$arm")
    CHIP=""
    [ ${#CHIPSET[@]} -gt 0 ] && CHIP="$arm"
    skipped=no

    mode_id=$(rtg_mode_id "$depth")
    stage "$depth" "$mode_id"
    startup_for "$depth"
    started=$(date +%s)
    boot "$tag" "$depth"

    if [ "$RTG" = 1 ]; then
        if rtg_wait_modes 120; then
            say "${tag}_rtg_modes_seen" \
                "$(grep -c '^rtgmode=' "$RTG_MODES_SEEN" 2>/dev/null || true)"
            learned=$(rtg_mode_from_list "$RTG_MODES_SEEN" \
                                        "$(rtg_db_depth "$depth")")
            if [ -z "$learned" ]; then
                say "${tag}_skipped" "the board publishes no\
 ${RTG_W}x${RTG_H} mode at depth $(rtg_db_depth "$depth"); see\
 $RTG_MODES_SEEN"
                cleanup
                skipped=yes
            elif [ "$PATTERN" = 1 ]; then
                mode_id="0x$learned"
            elif [ "0x$learned" != "$mode_id" ]; then
                say "${tag}_rtg_mode_relearned" "0x$learned, not\
 ${mode_id:-none}"
                cleanup
                mode_id="0x$learned"
                stage "$depth" "$mode_id"
                startup_for "$depth"
                started=$(date +%s)
                boot "$tag" "$depth"
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
    deadline=$(( started + BOOT_MAX ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        sleep 1
        kill -0 "$EMU_PID" 2>/dev/null || break
        [ "$(alive)" = "200" ] && { up=yes; break; }
    done
    say "${tag}_boot_seconds" "$(( $(date +%s) - started ))"
    say "${tag}_serial_log" "$SERIALLOG"

    if [ "$up" != yes ]; then
        say "${tag}_up" no
        say "${tag}_emulog" "$EMULOG"
        say "${tag}_serial" "$SERIALLOG"
        say "${tag}_serial_bytes" \
            "$( [ -f "$SERIALLOG" ] && wc -c < "$SERIALLOG" | tr -d ' ' \
                || echo 0)"
        say "${tag}_serial_tail" \
            "$(tail -3 "$SERIALLOG" 2>/dev/null | tr '\n' ' ' || true)"
        cleanup
        VERDICT=infra
        continue
    fi

    say "${tag}_bridged" "confirmed by an answer at $ADDRESS from ${CLIENT:-this host}"

    read -r code bytes <<<"$(fetch_page)"
    say "${tag}_page_status" "${code:-none}"
    say "${tag}_page_bytes" "${bytes:-0}"
    [ "${code:-0}" = "200" ] && [ "${bytes:-0}" -gt 1000 ] || VERDICT=fail

    typing=()
    [ -z "$TYPE" ] || typing=(--type "$TYPE")

    latency=()
    [ -z "$LATENCY" ] || latency=(--latency "$LATENCY")

    moving=()
    if [ "$RTG" = 1 ] && [ "$(rtg_wire_format "$depth")" = 2 ] &&
       { [ "$PATTERN" = 1 ] || [ "$ACTIVITY" = "scroll" ] ||
         [ -n "$TYPE" ]; }; then
        moving=(--min-changed 2)
    fi

    set +e
    probe "$OUTDIR/$tag-probe.txt" \
        "$ADDRESS" "$PORT" --seconds "$PROBE_SECONDS" \
        --png "$OUTDIR/$tag.png" --pfs "$OUTDIR/$tag.pfs" \
        ${latency[@]+"${latency[@]}"} \
        ${typing[@]+"${typing[@]}"} ${moving[@]+"${moving[@]}"}
    rc=$?
    set -e

    while IFS='=' read -r k v; do
        [ -n "$k" ] || continue
        say "${tag}_$k" "$v"
    done < <(grep '=' "$OUTDIR/$tag-probe.txt" || true)

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

    if [ "$PATTERN" = 1 ]; then
        cp "$HD/rtgbars.txt" "$OUTDIR/$tag-rtgbars.txt" 2>/dev/null || true
        cp "$HD/rtglist.txt" "$OUTDIR/$tag-rtglist.txt" 2>/dev/null || true

        word=$(sed -n 's/^word_unknown=\(rtg .*\)$/\1/p' \
               "$OUTDIR/$tag-probe.txt" 2>/dev/null | tail -1 || true)
        card_depth=$(printf '%s' "$word" | tr ' ' '\n' \
                     | sed -n 's/^depth=//p' | tail -1 || true)
        card_fmt=$(printf '%s' "$word" | tr ' ' '\n' \
                   | sed -n 's/^fmt=//p' | tail -1 || true)
        want_db_depth=$(rtg_db_depth "$depth")
        want_card_fmt=$(rtg_card_fmt "$depth")
        say "${tag}_rtg_card_depth" "${card_depth:-none}"
        say "${tag}_rtg_card_fmt" "${card_fmt:-none}"

        if [ "${card_fmt:-none}" != "$want_card_fmt" ] ||
           [ "${card_depth:-0}" != "$want_db_depth" ]; then
            say "${tag}_error" "-P asked for the $want_card_fmt path, which is\
 depth $want_db_depth on this board, and the readback word says depth\
 ${card_depth:-none} format ${card_fmt:-none}"
            VERDICT=fail
        fi

        if [ -n "$CLIENT" ]; then
            scp -q -o BatchMode=yes "$CLIENT:$OUTDIR/$tag.png" \
                "$OUTDIR/$tag.png" 2>/dev/null || true
        fi

        if [ -f "$OUTDIR/$tag.png" ]; then
            set +e
            python3 "$ROOT/tests/tools/rtgbars-check.py" "$OUTDIR/$tag.png" \
                > "$OUTDIR/$tag-bars.txt" 2>&1
            brc=$?
            set -e
            say "${tag}_bands_ok" \
                "$(sed -n 's/^bands_ok=//p' "$OUTDIR/$tag-bars.txt" \
                   | tail -1 || true)"
            swapped=$(sed -n 's/^red_blue_exchanged=//p' \
                      "$OUTDIR/$tag-bars.txt" | tail -1 || true)
            [ -z "$swapped" ] || say "${tag}_red_blue_exchanged" "$swapped"
            say "${tag}_bars_report" "$OUTDIR/$tag-bars.txt"
            case "$brc" in
                0) ;;
                2) VERDICT=infra ;;
                *) say "${tag}_error" "the bands rtgbars drew did not come\
 back: see $OUTDIR/$tag-bars.txt"
                   VERDICT=fail ;;
            esac
        else
            say "${tag}_error" "no decoded frame at $OUTDIR/$tag.png to check"
            VERDICT=fail
        fi
    fi

    # THE SIZE ON THE WIRE, ON EVERY ARM.  A screen and its bitmap are not the
    # same size: a 632-pixel planar screen is allocated 640 wide and a 633-pixel
    # card screen gets a 636-byte row, and the geom word must carry the screen.
    # That is the defect 1ec557b9 fixed, and the reason it survived a year is
    # that nothing in the lab ever asked for a size with anything to pad.
    #
    # MEASURED AGAINST THE SCREEN, NOT THE REQUEST.  `C:chipscreen ... 0` opens
    # nothing and reports the front screen, and that report is the reference:
    # Workbench rounds a 632-pixel request up to 640 and the console is right to
    # serve 640 when it does.
    front_size=""
    if [ -n "$CHIP" ]; then
        front_size=$(awk -F= '/^screen_size=/ { print $2 }' \
                     "$HD/chipscreen.txt" 2>/dev/null | tail -1 || true)
    else
        front_size=$(awk -F= '/^wb_size=/ { print $2 }' \
                     "$HD/frontscreen.txt" 2>/dev/null | tail -1 || true)
    fi

    got_w=$(awk '/^geom=/ { sub(/^geom=/, "", $1); print $1 }' \
            "$OUTDIR/$tag-probe.txt" 2>/dev/null | sort -u | head -1 || true)
    got_h=$(awk '/^geom=/ { print $2 }' "$OUTDIR/$tag-probe.txt" \
            2>/dev/null | sort -u | head -1 || true)

    [ "$SCREEN_W" = 0 ] || say "${tag}_screen_asked" "${SCREEN_W}x${SCREEN_H}"
    say "${tag}_screen_open" "${front_size:-unknown}"
    say "${tag}_screen_served" "${got_w:-none}x${got_h:-none}"

    if [ -z "$front_size" ]; then
        say "${tag}_error" "no front-screen report, so the size on the wire is\
 measured against nothing"
        VERDICT=fail
    elif [ "${got_w:-none}x${got_h:-none}" != "$front_size" ]; then
        say "${tag}_error" "the screen the guest opened is $front_size and the\
 geom word says ${got_w:-none}x${got_h:-none}: the console is serving the\
 bitmap's allocation, not the screen"
        say "${tag}_hint" "the DWidth clamp in fb_geometry_of(),\
 src/tools/httpfb.c, and http_rtg_describe(), src/tools/httprtg.c"
        VERDICT=fail
    fi

    if [ -n "$CHIP" ]; then
        want_fmt=$(arm_wire_format "$CHIP")
        want_depth="$depth"
        fmt=$(awk '/^geom=/ { print $NF }' "$OUTDIR/$tag-probe.txt" 2>/dev/null \
              | sort -u | tr '\n' ' ' || true)
        fmt=${fmt% }
        wire_depth=$(awk '/^geom=/ { print $3 }' "$OUTDIR/$tag-probe.txt" \
                     2>/dev/null | sort -u | tr '\n' ' ' || true)
        wire_depth=${wire_depth% }
        say "${tag}_chipset_format" "${fmt:-none}"
        say "${tag}_chipset_wire_depth" "${wire_depth:-none}"

        if [ -f "$HD/chipscreen-wb.txt" ]; then
            cp "$HD/chipscreen-wb.txt" "$OUTDIR/$tag-chipscreen-wb.txt"
            while IFS='=' read -r k v; do
                case "$k" in wb_*) say "${tag}_$k" "$v" ;; esac
            done < <(grep '=' "$HD/chipscreen-wb.txt" || true)
        else
            say "${tag}_chip_loaded" "no; C:chipscreen wrote no report at depth 0"
        fi

        guest="$HD/chipscreen.txt"
        if [ -f "$guest" ]; then
            cp "$guest" "$OUTDIR/$tag-chipscreen.txt"
            while IFS='=' read -r k v; do
                [ -n "$k" ] || continue
                say "${tag}_chip_$k" "$v"
            done < <(grep '=' "$guest" || true)
        else
            say "${tag}_chip_result" "C:chipscreen wrote no report to DH0:"
            say "${tag}_chip_run" \
                "$(tr '\n' ' ' < "$HD/chipscreen-run.txt" 2>/dev/null || true)"
            VERDICT=fail
        fi

        drew=$(awk -F= '/^pattern=/ { print $2 }' "$guest" 2>/dev/null || true)
        if [ "$drew" != "$CHIP" ]; then
            say "${tag}_error" "C:chipscreen drew ${drew:-nothing}, not $CHIP,\
 so the picture on the wire is not the one this arm asserts against"
            VERDICT=fail
        fi

        if [ "${fmt:-0}" != "$want_fmt" ] ||
           [ "${wire_depth:-0}" != "$want_depth" ]; then
            say "${tag}_error" "-C asked for a $CHIP screen, which is format\
 $want_fmt depth $want_depth on the wire, and the geom word says format\
 ${fmt:-none} depth ${wire_depth:-none}"
            VERDICT=fail
        fi

        want_size="$front_size"

        if [ -n "$CLIENT" ]; then
            scp -q -o BatchMode=yes "$CLIENT:$OUTDIR/$tag.png" \
                "$OUTDIR/$tag.png" 2>/dev/null || true
        fi

        # EVERY PIXEL, against the picture chipscreen drew.  A geom word and an
        # exit code are both right on a frame that is one column out.
        if [ -f "$OUTDIR/$tag.png" ] && [ -n "$want_size" ]; then
            set +e
            python3 "$ROOT/tests/tools/chipscreen-check.py" \
                "$OUTDIR/$tag.png" --pattern "$CHIP" --size "$want_size" \
                --depth "$depth" > "$OUTDIR/$tag-pixels.txt" 2>&1
            crc=$?
            set -e
            say "${tag}_pixels_mismatched" \
                "$(sed -n 's/^mismatched=//p' "$OUTDIR/$tag-pixels.txt" \
                   | tail -1 || true)"
            say "${tag}_pixels_report" "$OUTDIR/$tag-pixels.txt"
            case "$crc" in
                0) ;;
                2) VERDICT=infra ;;
                *) say "${tag}_error" "the picture chipscreen drew did not come\
 back: see $OUTDIR/$tag-pixels.txt"
                   VERDICT=fail ;;
            esac
        else
            say "${tag}_error" "no decoded frame at $OUTDIR/$tag.png to check"
            VERDICT=fail
        fi
    fi

    case "$rc" in
        0) ;;
        2) VERDICT=infra ;;
        *) VERDICT=fail ;;
    esac

    if [ -n "$CLIENT" ] && [ "$rc" = "0" ]; then
        say "${tag}_png_on" "$CLIENT"
    fi

    cleanup
done

say outdir "$OUTDIR"
say arms_run "$ran"

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
