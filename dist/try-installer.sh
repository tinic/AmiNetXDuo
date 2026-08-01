#!/usr/bin/env bash
#
# Boot a real Workbench with the archive on a hard drive, and stop.
#
#   dist/try-installer.sh [-a ARCHIVE_DIR] [-r ROM] [-w ADF] [-m MODEL]
#                         [-e fs-uae|amiberry]
#
# WHAT THIS IS FOR, AND WHY IT IS NOT LIKE THE OTHERS
#
#   Every other harness in this tree runs headless, asserts something and
#   exits.  This one does the opposite: it opens a window, boots a Workbench,
#   and leaves a person in front of it.  There is no timeout, no verdict and
#   nothing to grep -- the thing being checked is what the Installer LOOKS
#   like, which no assertion reaches.
#
#   It exists because the Installer script is the one part of this project a
#   user meets before anything works, and the only way to know that a question
#   reads well, that a default is the right one, or that a choice fits its
#   line, is to sit in front of it.  Changes to install/Install-AmiNetXDuo go
#   through here before they ship.
#
# WHAT IT NEEDS
#
#   An archive.  dist/make-dist.sh builds one; -a points at the unpacked
#   AmiNetXDuo/ tree it leaves in build/dist, which is the default.
#
#   A Kickstart 3.1 ROM and a Workbench 3.1 disk.  Neither is ours to ship.
#   AMINETXDUO_KICKSTART and AMINETXDUO_WB_ADF name them, or drop them beside
#   this script's defaults below.  The Workbench floppy is what boots; the
#   archive is on DH0:.
#
# HOW TO USE IT
#
#   The guest comes up on the Workbench desktop.  Open the DH0: icon, open
#   AmiNetXDuo, and double-click Install-AmiNetXDuo.  That is the same path a
#   user takes out of an .lha, including the icon's own tooltypes -- the
#   default user level among them, which is what decides how much the script
#   shows.
#
#   Close the emulator window to finish.  Nothing is written back to the
#   archive tree: the hard drive is a copy under build/.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

ARCHIVE="${AMINETXDUO_TRY_ARCHIVE:-$ROOT/build/dist/AmiNetXDuo}"
EMU="${AMINETXDUO_TRY_EMU:-fs-uae}"
MODEL="A1200"
ROM="${AMINETXDUO_KICKSTART:-}"
WB="${AMINETXDUO_WB_ADF:-}"

while getopts "a:r:w:m:e:" opt; do
    case "$opt" in
        a) ARCHIVE="$OPTARG" ;;
        r) ROM="$OPTARG" ;;
        w) WB="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        e) EMU="$OPTARG" ;;
        *) echo "usage: $0 [-a archive] [-r rom] [-w wb.adf] [-m model]" \
                "[-e fs-uae|amiberry]" >&2; exit 2 ;;
    esac
done

[ -d "$ARCHIVE" ] || {
    echo "no archive tree at $ARCHIVE" >&2
    echo "  dist/make-dist.sh builds one, or pass -a <dir>." >&2
    exit 2
}
[ -f "$ARCHIVE/Install-AmiNetXDuo" ] || {
    echo "$ARCHIVE has no Install-AmiNetXDuo -- is it the unpacked tree?" >&2
    exit 2
}

# The ROM and the Workbench, from the usual places if not named.
if [ -z "$ROM" ]; then
    for c in "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A1200)[!].rom" \
             "$HOME/Downloads/Kickstart v3.1 r40.63 (1993)(Commodore)(A500-A600-A2000)[!].rom" \
             "$ROOT/build/kick31.rom"
    do
        [ -f "$c" ] && { ROM="$c"; break; }
    done
fi
[ -n "$ROM" ] && [ -f "$ROM" ] || {
    echo "no Kickstart found. Set AMINETXDUO_KICKSTART=<rom> or pass -r." >&2
    exit 2
}

if [ -z "$WB" ]; then
    for c in "$HOME/anxd-try/wb31-workbench.adf" "$ROOT/build/wb31.adf"; do
        [ -f "$c" ] && { WB="$c"; break; }
    done
fi
[ -n "$WB" ] && [ -f "$WB" ] || {
    echo "no Workbench disk. Set AMINETXDUO_WB_ADF=<adf> or pass -w." >&2
    echo "  Disk 2 of the Workbench 3.1 set is the one that boots." >&2
    exit 2
}

# ------------------------------------------------------------- the drive ---
#
# A copy, so the Installer writing into it -- and it does, that is the point --
# leaves the archive tree alone and every run starts from the same place.
HD="$ROOT/build/try-installer-hd"
rm -rf "$HD"
mkdir -p "$HD"
cp -R "$ARCHIVE" "$HD/AmiNetXDuo"

# A card to find.
#
# The archive ships no SANA-II drivers -- none of them are ours -- and a bare
# Workbench floppy has an empty DEVS:Networks, so without this the installer is
# quite correctly unable to detect anything and the card question cannot be
# exercised at all. Anything named in AMINETXDUO_TRY_DEVS, or whatever is in
# ~/anxd-try, goes onto the drive; the guest picks them up with the ADD assign
# printed below, which puts DH0:Devs behind the floppy's own DEVS: rather than
# replacing it.
mkdir -p "$HD/Devs/Networks"
DEVS_FOUND=0
for d in ${AMINETXDUO_TRY_DEVS:-} "$HOME/anxd-try"/*.device; do
    [ -f "$d" ] || continue
    cp "$d" "$HD/Devs/Networks/"
    DEVS_FOUND=$((DEVS_FOUND + 1))
done

echo "==> archive:   $ARCHIVE"
echo "==> Kickstart: $(basename "$ROM")"
echo "==> Workbench: $(basename "$WB")"
echo "==> drive:     $HD  (a copy -- the archive is not written to)"
echo
if [ "$DEVS_FOUND" -gt 0 ]; then
    echo "==> drivers:   $DEVS_FOUND in DH0:Devs/Networks"
    echo
    echo "    For the installer to FIND a card, open a Shell first and type:"
    echo "        assign DEVS: DH0:Devs ADD"
    echo "    Without it there is no driver anywhere and the card question has"
    echo "    nothing to detect -- which is worth seeing too, but only on purpose."
else
    echo "==> drivers:   none found, so the installer will detect no card."
    echo "    Put .device files in ~/anxd-try or name them in"
    echo "    AMINETXDUO_TRY_DEVS to exercise the card question."
fi
echo
echo "    Open DH0:, then AmiNetXDuo, then double-click Install-AmiNetXDuo."
echo "    Close the window when finished."
echo

case "$EMU" in
    fs-uae)
        CFG="$ROOT/build/try-installer.fs-uae"
        cat > "$CFG" <<EOF
amiga_model = $MODEL
kickstart_file = $ROM
floppy_drive_0 = $WB
hard_drive_0 = $HD
hard_drive_0_label = DH0
fullscreen = 0
EOF
        echo "==> fs-uae $CFG"
        exec fs-uae "$CFG"
        ;;
    amiberry)
        CFG="$ROOT/build/try-installer.uae"
        cat > "$CFG" <<EOF
config_description=AmiNetXDuo installer
use_gui=no
quickstart=$MODEL,0
kickstart_rom_file=$ROM
floppy0=$WB
floppy0type=0
nr_floppies=1
uaehf0=dir,rw,DH0:DH0:$HD,0
fastmem_size=8
EOF
        echo "==> amiberry $CFG"
        exec amiberry -f "$CFG"
        ;;
    *)
        echo "unknown emulator '$EMU' (fs-uae or amiberry)" >&2
        exit 2
        ;;
esac
