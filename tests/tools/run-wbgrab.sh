#!/usr/bin/env bash
#
# wbgrab against a real Workbench 3.1, one boot per sequence.
#
#   tests/tools/run-wbgrab.sh [-b BUILDDIR] [-m MODEL] [-t SECONDS]
#                             [-o OUTDIR] [-s SEQUENCE]... [-d DEPTH]
#                             [-f FONT/SIZE] [-V 32]
#
# -f sets the Workbench screen font through ENVARC:Sys/font.prefs, e.g.
# helvetica.font/13.  -V 32 boots Workbench 3.2 on its own Kickstart.  -s netprefs grabs NetPrefs on each of its pages as
# netprefs.pfs, netprefs-1.pfs ... netprefs-4.pfs; netprefs_window is the
# window's box for cropping.  -s netwireless grabs NetWirelessPrefs against
# tests/tools/wifistub.c's unit with the NetWireless AppIcon beside it, then
# its join form, as netwireless.pfs and netwireless-1.pfs.
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
SEQUENCES=()

while getopts "b:m:t:o:s:d:f:V:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        d) DEPTH="$OPTARG" ;;
        s) SEQUENCES+=("$OPTARG") ;;
        f) FONT="$OPTARG" ;;
        V) OSVER="$OPTARG" ;;
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
NETWIRELESS="$BUILD/src/tools/NetWirelessPrefs"
NETWIRELESSCX="$BUILD/src/tools/NetWireless"
WIFISTUB="$BUILD/tests/tools/WifiStub"
WINCLICK="$BUILD/tests/tools/WinClick"
for seq in "${SEQUENCES[@]}"; do
    [ "$seq" = netwireless ] || continue
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

case "$FONT" in
    "") ;;
    */[0-9]*) ;;
    *) say error "-f wants FONT/SIZE, e.g. helvetica.font/13"; exit 2 ;;
esac

# ------------------------------------------------------------- the machine --

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

    if [ -x "$NETWIRELESS" ] && [ -x "$WIFISTUB" ] && [ -x "$WINCLICK" ]; then
        cp "$NETWIRELESS" "$HD/C/NetWirelessPrefs"
        cp "$NETWIRELESSCX" "$HD/C/NetWireless"
        cp "$WIFISTUB" "$HD/C/WifiStub"
        cp "$WINCLICK" "$HD/C/WinClick"
        chmod 755 "$HD/C/NetWirelessPrefs" "$HD/C/NetWireless" \
            "$HD/C/WifiStub" "$HD/C/WinClick"
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
Echo >DH0:.done "$RC"
EOF
        ;;
    *)
        say error "unknown sequence: $1"
        exit 2
        ;;
    esac
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

    [ "$BOOT_STATUS" = "0" ] || FAILED=1
done

say outdir "$OUTDIR"
say RESULT "$([ "$FAILED" = 0 ] && echo PASS || echo FAIL)"
exit "$FAILED"
