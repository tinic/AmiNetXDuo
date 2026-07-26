#!/usr/bin/env bash
#
# Fetch the AROS m68k boot ROM pair -- a free, redistributable Kickstart
# replacement, and the reason the emulator tests can run in public CI at all.
#
#   tools/fetch-aros-rom.sh                     # fetch (or reuse), print paths
#   eval "$(tools/fetch-aros-rom.sh --export)"  # ... and export them
#   tools/fetch-aros-rom.sh --print-dir         # just say where they would be
#
# WHY THIS EXISTS
#
#   Everything under tools/fsuae-run.sh needs a boot ROM.  Kickstart 3.1 is
#   Commodore's and cannot be redistributed or downloaded, so any CI that
#   depends on it can only ever run on a machine that already has a copy.
#
#   AROS (aros.org, APL 1.1) is an open-source AmigaOS reimplementation, and
#   its m68k build ships as a Kickstart-replacement ROM that FS-UAE boots.
#   Measured 2026-07-25 against Kickstart 3.1 40.68 on the same binaries: the
#   smoke probe, the ThreadX task-lifecycle probe and the kernel-stop probe all
#   pass with IDENTICAL check counts (5/5, 18/18, 41+8/0-fail), +1-2 s wall.
#   RemTask()/tc_MemEntry reaping, Forbid() nesting across Wait(),
#   timer.device, RawPutChar()/RawDoFmt() serial output and the Alert() hook
#   all behave as the code expects.
#
#   TWO ROMs, NOT ONE.  AROS m68k is a 512 KB base ROM plus a 512 KB extended
#   ROM.  Booting the base alone dies in Exec Bootstrap with "graphics.library
#   could not open library hidd" and never reaches DOS -- so both are exported
#   and fsuae-run.sh passes the second as kickstart_ext_file.
#
# ON PINNING, HONESTLY
#
#   AROS publishes m68k ROMs only in its NIGHTLY builds, and SourceForge keeps
#   about two days of them.  The stable "snapshots" line has had no m68k build
#   since 2017.  So the pin below WILL rot, and when it does this script falls
#   back to discovering the newest nightly and says so loudly -- an unpinned
#   ROM is still infinitely better than no emulator tier, but you should know
#   which one you got.
#
#   The durable fix is to mirror the two 512 KB files yourself -- a GitHub
#   release asset on this repo is ideal, and is now what happens by default:
#   AMINETXDUO_AROS_MIRROR holds the ROM pair of the pinned build, and the
#   SourceForge path below is only reached if the mirror is unreachable or its
#   checksums do not match. To refresh the pin: take a new nightly, update the
#   two checksums, upload the pair, and move the mirror URL.
#   You can also mirror the ROM pair yourself -- point AMINETXDUO_AROS_ROM_URL
#   (a .zip of the boot ISO) or AMINETXDUO_AROS_ROM_DIR (a directory holding
#   aros-rom.bin and aros-ext.bin) at it.  APL 1.1 permits redistribution
#   provided the licence text travels with the files.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

# ------------------------------------------------------------------- pin ----

AROS_PIN_DATE="20260725"
AROS_SHA_ROM="b6ba153943490f813b9ff5705b54c943431d2d8ec584d4cba6f5311b95e0898e"
AROS_SHA_EXT="45e69d52597623f014598f8fc44b94493fab4fd015a1898bd447348a676621cb"

AROS_SF="https://sourceforge.net/projects/aros/files/nightly2"
AROS_ISO_MEMBER_ROM="boot/amiga/aros-rom.bin"
AROS_ISO_MEMBER_EXT="boot/amiga/aros-ext.bin"

# ---------------------------------------------------------------- options ----

MODE="fetch"
FORCE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --export)    MODE="export" ;;
        --print-dir) MODE="print" ;;
        --force)     FORCE=1 ;;
        -h|--help)   sed -n '2,45p' "$0"; exit 0 ;;
        *) echo "usage: $0 [--export|--print-dir] [--force]" >&2; exit 2 ;;
    esac
    shift
done

say() { [ "$MODE" = "export" ] && echo "$*" >&2 || echo "$*"; }

CACHE="${AMINETXDUO_AROS_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/aminetxduo/aros-rom}"
DIR="$CACHE/$(printf '%s' "$AROS_SHA_ROM" | cut -c1-12)"

if [ "$MODE" = "print" ]; then
    printf '%s\n' "$DIR"
    exit 0
fi

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

emit() {
    local d="$1"
    if [ "$MODE" = "export" ]; then
        printf 'export AMINETXDUO_KICKSTART=%s/aros-rom.bin\n' "$d"
        printf 'export AMINETXDUO_KICKSTART_EXT=%s/aros-ext.bin\n' "$d"
    else
        printf '%s/aros-rom.bin\n%s/aros-ext.bin\n' "$d" "$d"
    fi
}

# ------------------------------------------------------ already have them ----

# A directory you supplied yourself always wins, and is never checksummed
# against the pin -- that is the whole point of the escape hatch.
if [ -n "${AMINETXDUO_AROS_ROM_DIR:-}" ]; then
    [ -f "$AMINETXDUO_AROS_ROM_DIR/aros-rom.bin" ] &&
    [ -f "$AMINETXDUO_AROS_ROM_DIR/aros-ext.bin" ] || {
        echo "AMINETXDUO_AROS_ROM_DIR=$AMINETXDUO_AROS_ROM_DIR must hold" >&2
        echo "aros-rom.bin and aros-ext.bin" >&2
        exit 2
    }
    say "==> AROS ROM from AMINETXDUO_AROS_ROM_DIR"
    emit "$AMINETXDUO_AROS_ROM_DIR"
    exit 0
fi

if [ "$FORCE" = "0" ] && [ -f "$DIR/aros-rom.bin" ] && [ -f "$DIR/aros-ext.bin" ]; then
    say "==> AROS ROM already at $DIR"
    emit "$DIR"
    exit 0
fi

command -v curl >/dev/null || { echo "curl is required" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 2; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/aminetxduo-aros.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

# ---------------------------------------------------------------- source ----

# The mirror comes first, and it is the reason CI is not at the mercy of a
# nightly's two-day lifetime. It holds the two 512 KB ROMs of the pinned build
# -- not the 108 MB ISO -- so it is also a much smaller fetch. SourceForge stays
# as the fallback for anyone who would rather take it from upstream, and for
# refreshing the pin.
AROS_MIRROR="${AMINETXDUO_AROS_MIRROR:-https://github.com/tinic/AmiNetXDuo/releases/download/aros-m68k-rom-20260725/aros-m68k-20260725.tar.gz}"

try_mirror() {
    [ -n "$AROS_MIRROR" ] || return 1
    say "==> AROS ROM pair from the mirror"
    curl -fsSL --max-time 300 -o "$TMP/mirror.tar.gz" "$AROS_MIRROR" 2>/dev/null || return 1
    tar xzf "$TMP/mirror.tar.gz" -C "$TMP" 2>/dev/null || return 1
    [ -f "$TMP/aros-rom.bin" ] && [ -f "$TMP/aros-ext.bin" ] || return 1
    got_rom=$(sha256_of "$TMP/aros-rom.bin"); got_ext=$(sha256_of "$TMP/aros-ext.bin")
    if [ "$got_rom" != "$AROS_SHA_ROM" ] || [ "$got_ext" != "$AROS_SHA_EXT" ]; then
        say "!! the mirror does not match the pinned checksums; ignoring it"
        return 1
    fi
    mkdir -p "$DIR"
    cp "$TMP/aros-rom.bin" "$TMP/aros-ext.bin" "$DIR/"
    return 0
}

if [ -z "${AMINETXDUO_AROS_ROM_URL:-}" ] && try_mirror; then
    emit "$DIR"
    exit 0
fi

PINNED=1
if [ -n "${AMINETXDUO_AROS_ROM_URL:-}" ]; then
    URL="$AMINETXDUO_AROS_ROM_URL"
    PINNED=0
    say "==> AROS boot ISO from AMINETXDUO_AROS_ROM_URL"
else
    URL="$AROS_SF/$AROS_PIN_DATE/Binaries/AROS-$AROS_PIN_DATE-amiga-m68k-boot-iso.zip/download"
    if ! curl -fsIL --max-time 60 "$URL" >/dev/null 2>&1; then
        say "!! the pinned AROS nightly $AROS_PIN_DATE is gone from SourceForge"
        say "   (nightlies are kept for about two days); looking for the newest"
        # The project RSS lists every file in nightly2/, newest first.
        NEWURL=$(curl -fsSL --max-time 60 "https://sourceforge.net/projects/aros/rss?path=/nightly2" \
                 | grep -oE 'https://[^<]*amiga-m68k-boot-iso\.zip/download' \
                 | head -1 || true)
        [ -n "$NEWURL" ] || {
            echo "!! could not find any AROS m68k boot ISO on SourceForge." >&2
            echo "   Mirror the ROM pair yourself and set" >&2
            echo "   AMINETXDUO_AROS_ROM_URL or AMINETXDUO_AROS_ROM_DIR." >&2
            exit 1
        }
        URL="$NEWURL"
        PINNED=0
        say "!! falling back to $(printf '%s' "$URL" | sed 's#.*/\(AROS-[^/]*\)/download#\1#')"
        say "!! THIS IS NOT THE PINNED BUILD. Checksums are reported below;"
        say "!! update the pin in this script, or mirror the files."
    fi
fi

say "==> downloading the AROS m68k boot ISO (~108 MB for 1 MB of ROM --"
say "    the ROMs are published nowhere smaller)"
curl -fsSL --retry 3 --max-time 900 -o "$TMP/aros.zip" "$URL"

# ISO 9660 inside a zip, and neither GNU tar nor macOS unzip alone can reach
# through both.  A directory-record walk is ~60 lines of python and has no
# dependency beyond the interpreter, which every runner already has.
python3 - "$TMP/aros.zip" "$TMP" "$AROS_ISO_MEMBER_ROM" "$AROS_ISO_MEMBER_EXT" <<'PYEOF'
import sys, zipfile, io, os

SECTOR = 2048

def read_dir(f, extent, length):
    f.seek(extent * SECTOR)
    data = f.read(length)
    off = 0
    while off < len(data):
        rec_len = data[off]
        if rec_len == 0:                       # rest of the sector is padding
            off = (off // SECTOR + 1) * SECTOR
            if off >= len(data):
                break
            continue
        rec = data[off:off + rec_len]
        ext = int.from_bytes(rec[2:6], 'little')
        size = int.from_bytes(rec[10:14], 'little')
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        if nlen == 1 and name in (b'\x00', b'\x01'):
            nm = '.' if name == b'\x00' else '..'
        else:
            nm = name.decode('latin-1').split(';')[0]
        yield nm, ext, size, bool(flags & 0x02)
        off += rec_len

def find(f, path):
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    if pvd[1:6] != b'CD001':
        raise SystemExit('not an ISO 9660 image')
    root = pvd[156:156 + 34]
    extent = int.from_bytes(root[2:6], 'little')
    size = int.from_bytes(root[10:14], 'little')
    for part in path.split('/'):
        for nm, ext, sz, isdir in read_dir(f, extent, size):
            if nm.lower() == part.lower():
                extent, size = ext, sz
                break
        else:
            raise SystemExit('%s not found in the ISO' % path)
    f.seek(extent * SECTOR)
    return f.read(size)

zippath, outdir = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zippath) as z:
    isos = [n for n in z.namelist() if n.lower().endswith('.iso')]
    if not isos:
        raise SystemExit('no .iso inside the download')
    with z.open(isos[0]) as raw:
        iso = io.BytesIO(raw.read())
    # The licence travels with the ROMs: APL 1.1 requires it.
    for n in z.namelist():
        if os.path.basename(n).upper() in ('LICENSE', 'LICENSE.TXT', 'COPYING'):
            with z.open(n) as lf, open(os.path.join(outdir, 'AROS-LICENSE'), 'wb') as o:
                o.write(lf.read())
            break

for member in sys.argv[3:]:
    open(os.path.join(outdir, os.path.basename(member)), 'wb').write(find(iso, member))
PYEOF

[ -f "$TMP/aros-rom.bin" ] && [ -f "$TMP/aros-ext.bin" ] || {
    echo "!! the ISO did not contain the expected ROM pair" >&2
    exit 1
}

GOT_ROM=$(sha256_of "$TMP/aros-rom.bin")
GOT_EXT=$(sha256_of "$TMP/aros-ext.bin")

if [ "$PINNED" = "1" ]; then
    if [ "$GOT_ROM" != "$AROS_SHA_ROM" ] || [ "$GOT_EXT" != "$AROS_SHA_EXT" ]; then
        echo "!! checksum mismatch on the PINNED AROS nightly $AROS_PIN_DATE" >&2
        echo "   aros-rom.bin want $AROS_SHA_ROM" >&2
        echo "                 got  $GOT_ROM" >&2
        echo "   aros-ext.bin want $AROS_SHA_EXT" >&2
        echo "                 got  $GOT_EXT" >&2
        exit 1
    fi
    say "    checksums match the pin ($AROS_PIN_DATE)"
    OUT="$DIR"
else
    say "    aros-rom.bin sha256 $GOT_ROM"
    say "    aros-ext.bin sha256 $GOT_EXT"
    OUT="$CACHE/$(printf '%s' "$GOT_ROM" | cut -c1-12)"
fi

rm -rf "$OUT"
mkdir -p "$OUT"
cp "$TMP/aros-rom.bin" "$TMP/aros-ext.bin" "$OUT/"
[ -f "$TMP/AROS-LICENSE" ] && cp "$TMP/AROS-LICENSE" "$OUT/" || true

say "==> $OUT"
emit "$OUT"
