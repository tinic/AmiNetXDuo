#!/usr/bin/env bash
#
# Build a Workbench 3.2 SYS: tree from the AmigaOS 3.2 floppy set.
#
#   tools/mkwb32.sh [-a ADFDIR] [-o OUTDIR]
#
# install/test/run-workbench.sh does this for 3.1 from five ADFs and boots the
# result as a directory hard drive.  This is the same idea for 3.2, from the
# richer set the 3.2 CD carries in its ADF/ directory: Workbench is the base
# and the rest are overlaid onto it in the order the Installer would.
#
# WHY A TREE AND NOT A DISK IMAGE
#
#   Amiberry mounts a host directory as a volume (uaehf0=dir), so a tree is
#   directly bootable and can be inspected, diffed and patched from the host
#   without loopback mounts or an Amiga-side filesystem.  That is what the 3.1
#   harness has always used.
#
# WHY NOT RUN THE REAL INSTALLER
#
#   The 3.2 Installer is interactive and its script asks about the target
#   machine, the languages and the printer.  Assembling the floppies directly
#   gives a system that boots and runs commands, which is what a test harness
#   needs.  It is NOT a substitute for a real install when the question is
#   about the installer itself.
#
# The ADFs are not ours to ship: they come from a purchased AmigaOS 3.2
# licence.  See the lab asset store, os32/README.md.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ADFDIR="${AMINETXDUO_ADF32_DIR:-$HOME/amiga-assets/os32/adf-cd}"
OUT="${AMINETXDUO_WB32_DIR:-$ROOT/build/wb32-sys}"

while getopts "a:o:" opt; do
    case "$opt" in
        a) ADFDIR="$OPTARG" ;;
        o) OUT="$OPTARG" ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

XDFTOOL="${AMINETXDUO_XDFTOOL:-}"
if [ -z "$XDFTOOL" ]; then
    for c in "$(command -v xdftool || true)" \
             "$HOME/venv-amitools/bin/xdftool" \
             "$HOME/.venvs/amitools/bin/xdftool"; do
        [ -n "$c" ] && [ -x "$c" ] && { XDFTOOL="$c"; break; }
    done
fi
[ -n "$XDFTOOL" ] && [ -x "$XDFTOOL" ] || {
    echo "amitools' xdftool not found; pip install amitools" >&2
    exit 2
}

# Workbench first because it carries S/Startup-Sequence and the C: commands
# everything else assumes; the rest are overlays and their order among
# themselves does not matter, none of them replaces a file another supplies.
BASE=Workbench3.2
OVERLAY=(Extras3.2 Fonts Locale Storage3.2 Classes3.2 Backdrops3.2 GlowIcons3.2 MMULibs)

for d in "$BASE" "${OVERLAY[@]}"; do
    [ -f "$ADFDIR/$d.adf" ] || { echo "missing $ADFDIR/$d.adf" >&2; exit 2; }
done

# amitools writes the volume contents into the directory it is given, but some
# versions make a subdirectory named after the volume.  Cope with both rather
# than depending on which one is installed -- the same dance
# install/test/run-workbench.sh does.
unpack_adf() {
    local adf="$1" into="$2" inner
    rm -rf "$into"; mkdir -p "$into"
    "$XDFTOOL" "$adf" unpack "$into" >/dev/null 2>&1

    # amitools drops the volume in a subdirectory named after it, beside
    # .blkdev/.bootcode/.xdfmeta sidecars.  Take the one directory and discard
    # the sidecars; they are the image's metadata, not files on the disk.
    inner=$(find "$into" -maxdepth 1 -mindepth 1 -type d | head -1)
    if [ -n "$inner" ]; then
        mv "$inner"/* "$into"/ 2>/dev/null || true
        mv "$inner"/.[!.]* "$into"/ 2>/dev/null || true
        rmdir "$inner" 2>/dev/null || true
    fi
    rm -f "$into"/*.blkdev "$into"/*.bootcode "$into"/*.xdfmeta
}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "==> base: $BASE"
unpack_adf "$ADFDIR/$BASE.adf" "$WORK/base"

rm -rf "$OUT"; mkdir -p "$OUT"
cp -a "$WORK/base/." "$OUT/"

for d in "${OVERLAY[@]}"; do
    echo "==> overlay: $d"
    unpack_adf "$ADFDIR/$d.adf" "$WORK/ov"
    cp -a "$WORK/ov/." "$OUT/"
done

# The one thing that decides whether this is a Workbench or a pile of files.
# Case-insensitively: AmigaOS does not care and the 3.2 disk ships
# "Startup-sequence", where 3.1 ships "Startup-Sequence".  A case-sensitive
# host filesystem is the only reason this is worth a comment.
SS=$(find "$OUT/S" -maxdepth 1 -iname "startup-sequence" 2>/dev/null | head -1)
[ -n "$SS" ] || {
    echo "!! no S/Startup-Sequence in the result; this will not boot" >&2
    exit 1
}

echo
echo "wb32 tree: $OUT"
echo "  $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1)"
echo "  C: $(ls "$OUT/C" 2>/dev/null | wc -l) commands, Libs: $(ls "$OUT/Libs" 2>/dev/null | wc -l)"
