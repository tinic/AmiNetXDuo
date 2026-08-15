# shellcheck shell=bash
# Five Workbench 3.1 floppies into one hard drive, for the harnesses that need
# a real Workbench SCREEN rather than a Shell.
#
#   . tests/tools/wb31-sys.sh
#   wb31_assemble "$ROOT/build/wb31-sys"      # exports WB31_SYS
#   wb31_screenmode_prefs "$HD" 4             # ask for a screen of that depth
#
# Sourced, not run.  It is the block tests/tools/run-wbgrab.sh had first and
# tests/tools/run-console.sh needs identically -- the same five ADFs, the same
# Commodore layout, the same cache -- and two copies of it would be two things
# to keep in step for no reason.
#
# WHAT IT NEEDS
#
#   AMINETXDUO_ADF_DIR      the five amiga-wb31_*.adf files
#   AMINETXDUO_XDFTOOL      amitools' xdftool, or one on PATH
#
# WHAT IT DOES NOT DO
#
#   Nothing here writes a Startup-Sequence or stages a binary: the assembled
#   tree is SYS: as Commodore shipped it, and what a harness wants on top of
#   that is the harness's business.
#
# `say` is the caller's key=value printer; a caller that has none gets one.
#
# SPDX-License-Identifier: MIT

command -v say >/dev/null 2>&1 || say() { printf '%s=%s\n' "$1" "$2"; }

# The SHAPE of the assembled tree, not the contents.  The cache is keyed on the
# ADFs' timestamps, which do not move when this file changes what it does with
# them, so a tree built by an older version has to be recognised and rebuilt.
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

# One ADF into a directory, tolerating xdftool's habit of putting the volume in
# a subdirectory of its own.
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

# The Workbench disk IS the root and the other four are the drawers the HD
# installer would have made.  Cached: unpacking five ADFs every run is a minute
# of nothing.  Sets WB31_SYS to the tree.
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
        # A HARD DRIVE, NOT FIVE FLOPPIES.
        #
        # Commodore's installer merges the Extras disk INTO SYS:, which is why
        # a real 3.1 hard drive has SYS:Prefs/ScreenMode and SYS:Tools.  The
        # floppy layout leaves them under Extras/, so the Prefs drawer holds
        # nothing but Env-Archive and Presets and there is no ScreenMode
        # anywhere a person would look -- which is exactly what a viewer of
        # this screen goes looking for first.
        #
        # Copied and not moved: Extras/ stays as it was, so anything that
        # already knew where these lived still finds them there.
        for pair in "Prefs:Prefs" "Tools:Tools" "System:System"; do
            [ -d "$scratch/extras/${pair%%:*}" ] || continue
            mkdir -p "$sys/${pair##*:}"
            cp -R "$scratch/extras/${pair%%:*}/." "$sys/${pair##*:}/"
        done
        # The drawer icon too, or Workbench shows SYS:Tools as a plain file.
        [ -f "$scratch/extras/Tools.info" ] &&
            cp "$scratch/extras/Tools.info" "$sys/Tools.info"

        rm -rf "$scratch"

        # A directory hard drive takes its Amiga protection bits from the
        # host's mode bits, and xdftool unpacks everything 0644, which would
        # leave every command in C: without its E bit.
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

# A Workbench screen of a given depth, and the way to ask for one is the file
# the ScreenMode editor writes: an IFF PREF with one SCRM chunk, dropped in
# ENVARC: before the boot copies it to ENV: and IPrefs reads it.  Without one a
# 3.1 Workbench comes up two planes deep whatever the machine.
wb31_screenmode_prefs() {
    local hd="$1" depth="$2"

    mkdir -p "$hd/Prefs/Env-Archive/Sys"
    AMINETXDUO_SMP_DEPTH="$depth" \
        python3 - "$hd/Prefs/Env-Archive/Sys/screenmode.prefs" <<'EOF'
import os, struct, sys

# PAL hires: PAL_MONITOR_ID | HIRES_KEY.  640x256 is what the stock screen is,
# so the depth is the only thing this changes.
DISPLAY_ID = 0x00021000 | 0x00008000

depth = int(os.environ["AMINETXDUO_SMP_DEPTH"])

# struct FilePrefHeader: version, type, flags[4].
prhd = struct.pack(">BB4B", 0, 0, 0, 0, 0, 0)

# struct ScreenModePrefs: reserved[4], displayid, width, height, depth, control.
scrm = struct.pack(">4L L HHHH", 0, 0, 0, 0, DISPLAY_ID, 640, 256, depth, 0)

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
