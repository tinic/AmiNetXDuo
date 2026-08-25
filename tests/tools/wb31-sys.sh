# shellcheck shell=bash
# Five Workbench 3.1 floppies into one hard drive, for the harnesses that need
# a real Workbench SCREEN rather than a Shell.
# WHAT IT NEEDS
#   AMINETXDUO_ADF_DIR      the five amiga-wb31_*.adf files
#   AMINETXDUO_XDFTOOL      amitools' xdftool, or one on PATH
# SPDX-License-Identifier: MIT

command -v say >/dev/null 2>&1 || say() { printf '%s=%s\n' "$1" "$2"; }

WB31_LAYOUT=2

wb31_tool() {
    local candidate
    if [ -n "${AMINETXDUO_XDFTOOL:-}" ] && [ -x "$AMINETXDUO_XDFTOOL" ]; then
        printf '%s' "$AMINETXDUO_XDFTOOL"
        return 0
    fi
    for candidate in \
        "$(command -v xdftool || true)" \
        "$HOME/venv-amitools/bin/xdftool" \
        "$HOME/.venvs/amitools/bin/xdftool" \
        "$HOME/amigaos/tools/amitools/bin/xdftool"
    do
        [ -n "$candidate" ] && [ -x "$candidate" ] && {
            printf '%s' "$candidate"
            return 0
        }
    done
    return 1
}

wb31_unpack_adf() {
    local tool="$1" adf="$2" into="$3" marker="$4" inner
    rm -rf "$into"
    mkdir -p "$into"
    "$tool" "$adf" unpack "$into" >/dev/null
    [ -e "$into/$marker" ] && return 0
    inner=$(find "$into" -maxdepth 1 -mindepth 1 -type d | head -1)
    [ -n "$inner" ] && [ -e "$inner/$marker" ] || {
        say error "$(basename "$adf") did not unpack to something holding $marker"
        return 1
    }
    mv "$inner"/* "$into"/ 2>/dev/null || true
    rmdir "$inner" 2>/dev/null || true
}

wb31_assemble() {
    local sys="$1"
    local adfdir="${AMINETXDUO_ADF_DIR:-$HOME/amigaos/adf}"
    local stamp="$sys.stamp"
    local stale=0 disk adf tool scratch pair want

    [ -d "$sys" ] && [ -f "$stamp" ] || stale=1
    [ "$(cat "$stamp" 2>/dev/null)" = "layout $WB31_LAYOUT" ] || stale=1
    for disk in workbench fonts locale storage extras; do
        adf="$adfdir/amiga-wb31_$disk.adf"
        [ -f "$adf" ] || {
            say error "missing $adf"
            say hint "set AMINETXDUO_ADF_DIR to the directory holding the WB 3.1 set"
            return 2
        }
        [ "$adf" -nt "$stamp" ] && stale=1
    done

    if [ "$stale" = "1" ]; then
        tool=$(wb31_tool) || {
            say error "amitools' xdftool not found, needed to unpack the Workbench ADFs"
            return 2
        }

        scratch="$sys.unpack"
        rm -rf "$scratch" "$sys"
        mkdir -p "$scratch" "$sys"
        for pair in "workbench:S/Startup-Sequence" "fonts:topaz.font" \
                    "locale:Catalogs" "storage:DOSDrivers" "extras:Tools"; do
            wb31_unpack_adf "$tool" "$adfdir/amiga-wb31_${pair%%:*}.adf" \
                            "$scratch/${pair%%:*}" "${pair##*:}" || return 2
        done
        cp -R "$scratch/workbench/." "$sys/"
        for pair in "fonts:Fonts" "locale:Locale" "storage:Storage" \
                    "extras:Extras"; do
            mkdir -p "$sys/${pair##*:}"
            cp -R "$scratch/${pair%%:*}/." "$sys/${pair##*:}/"
        done
        for pair in "Prefs:Prefs" "Tools:Tools" "System:System"; do
            [ -d "$scratch/extras/${pair%%:*}" ] || continue
            mkdir -p "$sys/${pair##*:}"
            cp -R "$scratch/extras/${pair%%:*}/." "$sys/${pair##*:}/"
        done
        [ -f "$scratch/extras/Tools.info" ] &&
            cp "$scratch/extras/Tools.info" "$sys/Tools.info"

        rm -rf "$scratch"

        chmod -R a+rx "$sys"
        printf 'layout %s\n' "$WB31_LAYOUT" > "$stamp"
        for want in C/Assign C/LoadWB C/Wait C/Dir S/Startup-Sequence \
                    Fonts/topaz.font Devs/system-configuration \
                    Prefs/ScreenMode Prefs/Palette Tools/Lacer; do
            [ -e "$sys/$want" ] || {
                say error "assembled SYS: has no $want"
                return 2
            }
        done
    fi

    # shellcheck disable=SC2034  # the caller reads it
    WB31_SYS="$sys"
    say workbench "$sys"
    return 0
}

wb31_screenmode_prefs() {
    wb31_screenmode_prefs_id "$1" "$2" 0x00029000 640 256
}

wb31_screenmode_prefs_id() {
    local hd="$1" depth="$2" id="$3" w="$4" h="$5"

    mkdir -p "$hd/Prefs/Env-Archive/Sys"
    AMINETXDUO_SMP_DEPTH="$depth" AMINETXDUO_SMP_ID="$id" \
    AMINETXDUO_SMP_W="$w" AMINETXDUO_SMP_H="$h" \
        python3 - "$hd/Prefs/Env-Archive/Sys/screenmode.prefs" <<'EOF'
import os, struct, sys

# The default is PAL hires: PAL_MONITOR_ID | HIRES_KEY, 640x256, which is what
# a stock 3.1 Workbench is.  A caller with a graphics card passes the card's.
DISPLAY_ID = int(os.environ["AMINETXDUO_SMP_ID"], 0)
WIDTH = int(os.environ["AMINETXDUO_SMP_W"])
HEIGHT = int(os.environ["AMINETXDUO_SMP_H"])

depth = int(os.environ["AMINETXDUO_SMP_DEPTH"])

# struct FilePrefHeader: version, type, flags[4].
prhd = struct.pack(">BB4B", 0, 0, 0, 0, 0, 0)

# struct ScreenModePrefs: reserved[4], displayid, width, height, depth, control.
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
}
