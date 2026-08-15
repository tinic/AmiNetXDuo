#!/usr/bin/env bash
#
# wbgrab against a real Workbench 3.1, one boot per sequence.
#
#   tests/tools/run-wbgrab.sh [-b BUILDDIR] [-m MODEL] [-t SECONDS]
#                             [-o OUTDIR] [-s SEQUENCE]... [-d DEPTH]
#
# The screen has to be Workbench's own, so the guest boots Commodore's
# Startup-Sequence with LoadWB in it, exactly as install/test/run-workbench.sh
# does, and the Workbench 3.1 tree is assembled from the same five ADFs by the
# same code.  tools/amiberry-run.sh cannot drive this: it wipes the staging
# drive and writes a Startup-Sequence of its own, and the screen under test is
# the one that script does not create.
#
# THREE SEQUENCES, THREE BOOTS.  One boot doing all three would have each
# sequence capturing the tail of the one before it -- a shell that is still
# scrolling when the idle capture starts is the case the idle capture exists
# to disprove.
#
#   idle      LoadWB has settled and nothing is running.  50 frames.
#   windows   shells open and close over each other while the grab runs.
#             DRAGGING IS NOT DRIVEN: it needs mouse input, nothing here can
#             script it, and a window that moves itself is not the same event.
#   scroll    a shell running `dir SYS: ALL` on a loop.  100 frames.
#
# DH0: is a host directory, so the .pfs files the guest writes are on this
# filesystem when it stops; there is nothing to copy off.
#
# Output is key=value and the exit code is the verdict.
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
SEQUENCES=()

while getopts "b:m:t:o:s:d:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        d) DEPTH="$OPTARG" ;;
        s) SEQUENCES+=("$OPTARG") ;;
        *) sed -n '3,6p' "$0" >&2; exit 2 ;;
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

# ------------------------------------------------------------- the machine --

# A Kickstart must match the model: the A1200 40.68 image does not boot an
# A600 and the machine then never reaches a Shell, which reads as a crash in
# the thing under test.  Same rule and same variables as tools/amiberry-run.sh.
#
# So must the wbgrab in -b BUILDDIR.  It is copied, not rebuilt, so an A500 or
# an A600 wants a tree configured -DAMINETXDUO_CPU=68000; a 68020 binary takes
# an illegal instruction before it reaches the screen.
model_var=$(printf '%s' "$MODEL" | tr '[:lower:]' '[:upper:]' | tr -c 'A-Z0-9' '_')
model_var=${model_var%_}
eval "KICKSTART=\${AMINETXDUO_KICKSTART_$model_var:-}"
KICKSTART="${KICKSTART:-${AMINETXDUO_KICKSTART:-}}"
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

# ------------------------------------------------------ Workbench 3.1 SYS: --
#
# tests/tools/wb31-sys.sh: five floppies into one hard drive, Commodore's own
# layout, cached.  Shared with tests/tools/run-console.sh, which needs exactly
# the same tree for exactly the same reason -- a real Workbench SCREEN.

# shellcheck source=tests/tools/wb31-sys.sh
. "$ROOT/tests/tools/wb31-sys.sh"

wb31_assemble "$ROOT/build/wb31-sys" || exit 2
WB="$WB31_SYS"

[ -n "$DEPTH" ] && say screen_depth_asked "$DEPTH" || true

# ------------------------------------------------------------ the drive ----

HD="$ROOT/build/wbgrab-dh0"

stage() {
    rm -rf "$HD"
    mkdir -p "$HD"
    cp -R "$WB/." "$HD/"
    cp "$WBGRAB" "$HD/C/wbgrab"
    chmod 755 "$HD/C/wbgrab"

    # A shell whose FROM script never ends: `dir SYS: ALL` on a loop is the
    # worst case for anything that diffs tiles, because every row of the
    # window moves on every frame.
    cat > "$HD/S/scroller" <<'EOF'
Lab loop
Dir SYS: ALL
Skip loop BACK
EOF

    # Each window this opens lives two seconds and then takes itself away;
    # EndCLI is what closes a shell's window from inside its own script.
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

    [ -n "$DEPTH" ] && wb31_screenmode_prefs "$HD" "$DEPTH"
    return 0
}

# The stock 3.1 Startup-Sequence with the tail replaced.  LoadWB stays, since
# the screen under test is the one it opens; only EndCLI goes, because it would
# take the boot shell away before the capture line ran.
startup_with() {
    sed -e '/^EndCLI/d' "$WB/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
    printf '\n%s\n' "$1" >> "$HD/S/Startup-Sequence"
    chmod 755 "$HD/S/Startup-Sequence"
}

# `C:Wait 6` after LoadWB in every arm: Workbench draws its backdrop, its title
# bar and the disk icons after LoadWB returns, so a capture that started at
# once would record that as change and the idle arm would measure the boot.
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
    *)
        say error "unknown sequence: $1"
        exit 2
        ;;
    esac
}

# ------------------------------------------------------------- the emulator --

# Amiberry links SDL2 with no driver of its own, so without this it asks for a
# video device, finds a stale DISPLAY from a failed X11 forward, and aborts in
# about a second -- which reads as a guest that never booted.
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
    else
        say "${seq}_pfs" "MISSING"
        FAILED=1
    fi

    [ "$BOOT_STATUS" = "0" ] || FAILED=1
done

say outdir "$OUTDIR"
say RESULT "$([ "$FAILED" = 0 ] && echo PASS || echo FAIL)"
exit "$FAILED"
