#!/usr/bin/env bash
#
# wbgrab against a real Workbench 3.1, one boot per sequence.
#
#   tests/tools/run-wbgrab.sh [-b BUILDDIR] [-m MODEL] [-t SECONDS]
#                             [-o OUTDIR] [-s SEQUENCE]... [-d DEPTH]
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

    [ -n "$DEPTH" ] && wb31_screenmode_prefs "$HD" "$DEPTH"
    return 0
}

startup_with() {
    sed -e '/^EndCLI/d' "$WB/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
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
    else
        say "${seq}_pfs" "MISSING"
        FAILED=1
    fi

    [ "$BOOT_STATUS" = "0" ] || FAILED=1
done

say outdir "$OUTDIR"
say RESULT "$([ "$FAILED" = 0 ] && echo PASS || echo FAIL)"
exit "$FAILED"
