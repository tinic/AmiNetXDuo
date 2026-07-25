#!/usr/bin/env bash
#
# Build the AmiNetXDuo distribution archive.
#
#   dist/make-dist.sh [-b BUILDDIR] [-v VERSION] [-o OUTDIR]
#
# Produces, in OUTDIR (default build/dist):
#
#   AmiNetXDuo.lha        the archive, laid out the way Aminet expects
#   AmiNetXDuo.readme     the upload description, with its header fields
#   AmiNetXDuo/           the same tree unpacked, for inspection
#
# The Aminet convention is that an archive extracts into the directory it is
# unpacked in as one drawer plus that drawer's icon, so that dropping it on a
# Workbench window leaves one recognisable object rather than a scattering of
# files:
#
#   AmiNetXDuo.info                  drawer icon
#   AmiNetXDuo/
#       Install-AmiNetXDuo           the Installer script
#       Install-AmiNetXDuo.info      its icon: default tool Installer
#       AmiNetXDuo.info              a spare drawer icon, for the drawer the
#                                    installer creates on the user's disk
#       ReadMe  ReadMe.info
#       AmiNetXDuo.readme            the Aminet description, for reference
#       C/                           the seven commands
#       Libs/                        bsdsocket.library, usergroup.library
#       Devs/Internet/               protocols, services, networks
#       Docs/  Docs.info             whatever docs/ holds
#       Examples/  Examples.info     commented configuration files
#
# Compression: a real `lha a` is used when one is on the PATH.  Homebrew's
# `lha` is Lhasa, which can only extract, so the fallback is dist/lhapack.py
# -- correct, readable by every LhA, and uncompressed.  Build the upload on a
# machine with a proper archiver.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD="${AMINETXDUO_BUILD:-build/cm}"
OUTDIR="$ROOT/build/dist"
VERSION="1.0"

while getopts "b:v:o:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        v) VERSION="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-v version] [-o outdir]" >&2
           exit 2 ;;
    esac
done

case "$BUILD" in
    /*) ;;
    *) BUILD="$ROOT/$BUILD" ;;
esac

INSTALL="$ROOT/install"
TREE="$OUTDIR/AmiNetXDuo"

# ------------------------------------------------------------- ingredients --

need() {
    [ -f "$1" ] || {
        echo "missing: $1" >&2
        echo "  build it first:  cmake --build $BUILD --parallel" >&2
        exit 2
    }
}

LIBS=(bsdsocket usergroup)
CMDS=(AddNetInterface Online Offline ShowNetStatus ping netstat host)

for lib in "${LIBS[@]}"; do need "$BUILD/src/$lib/$lib.library"; done
for cmd in "${CMDS[@]}"; do need "$BUILD/src/tools/$cmd"; done

# -------------------------------------------------------------- the icons --
#
# Regenerated every time so that a change to makeicon.py cannot leave a stale
# icon in an archive.

python3 "$INSTALL/tools/makeicon.py" "$INSTALL" >/dev/null

# -------------------------------------------------------------- the layout --

rm -rf "$TREE" "$OUTDIR/AmiNetXDuo.info"
mkdir -p "$TREE/C" "$TREE/Libs" "$TREE/Devs/Internet" "$TREE/Docs" \
         "$TREE/Examples"

for lib in "${LIBS[@]}"; do
    cp "$BUILD/src/$lib/$lib.library" "$TREE/Libs/"
done
for cmd in "${CMDS[@]}"; do
    cp "$BUILD/src/tools/$cmd" "$TREE/C/"
done
chmod 755 "$TREE"/C/* "$TREE"/Libs/*

cp "$INSTALL/Install-AmiNetXDuo"       "$TREE/"
cp "$INSTALL/Install-AmiNetXDuo.info"  "$TREE/"
cp "$INSTALL/AmiNetXDuo.info"          "$TREE/"
cp "$INSTALL/AmiNetXDuo.info"          "$OUTDIR/"

cp -R "$INSTALL/devs/Internet/." "$TREE/Devs/Internet/"
cp -R "$INSTALL/examples/."      "$TREE/Examples/"

cp "$ROOT/dist/ReadMe" "$TREE/ReadMe"
cp "$INSTALL/Document.info" "$TREE/ReadMe.info"
cp "$INSTALL/Drawer.info"   "$TREE/Docs.info"
cp "$INSTALL/Drawer.info"   "$TREE/Examples.info"

# ---------------------------------------------------------------- the docs --
#
# docs/ belongs to whoever is writing the documentation; take whatever is
# there and do not fail if it is not there yet.

shopt -s nullglob
for doc in "$ROOT"/docs/*.guide "$ROOT"/docs/*.txt "$ROOT"/docs/*.doc; do
    cp "$doc" "$TREE/Docs/"
done
for doc in "$ROOT"/docs/*.guide.info "$ROOT"/docs/*.txt.info; do
    cp "$doc" "$TREE/Docs/"
done
shopt -u nullglob

# Give every doc that has no icon of its own the generic document icon, so a
# Workbench user can see and open all of them.
for doc in "$TREE"/Docs/*; do
    [ -f "$doc" ] || continue
    case "$doc" in *.info) continue ;; esac
    [ -f "$doc.info" ] || cp "$INSTALL/Document.info" "$doc.info"
done

if [ -z "$(ls -A "$TREE/Docs" 2>/dev/null)" ]; then
    echo "note: docs/ had nothing to ship; Docs/ will contain only the ReadMe"
    cp "$ROOT/dist/ReadMe" "$TREE/Docs/ReadMe"
    cp "$INSTALL/Document.info" "$TREE/Docs/ReadMe.info"
fi

# ------------------------------------------------------------- the .readme --

sed "s/@VERSION@/$VERSION/g" "$ROOT/dist/AmiNetXDuo.readme" \
    > "$OUTDIR/AmiNetXDuo.readme"
cp "$OUTDIR/AmiNetXDuo.readme" "$TREE/AmiNetXDuo.readme"
cp "$INSTALL/Document.info" "$TREE/AmiNetXDuo.readme.info"

# Aminet rejects a .readme whose header fields are not all present and in
# order, so check rather than find out after uploading.
missing=""
for field in Short Author Uploader Type Version Architecture; do
    grep -q "^$field: " "$OUTDIR/AmiNetXDuo.readme" || missing="$missing $field"
done
[ -z "$missing" ] || {
    echo "AmiNetXDuo.readme is missing header fields:$missing" >&2
    exit 2
}
grep -q "^Version: *@VERSION@" "$OUTDIR/AmiNetXDuo.readme" && {
    echo "AmiNetXDuo.readme still has @VERSION@ in it" >&2
    exit 2
}

# ------------------------------------------------------------ the archive --

ARCHIVE="$OUTDIR/AmiNetXDuo.lha"
rm -f "$ARCHIVE"

# Lhasa answers to `lha` and cannot create archives; detect that rather than
# producing a zero byte file and calling it a release.
CAN_CREATE=0
for tool in lha jlha lharc; do
    command -v "$tool" >/dev/null 2>&1 || continue
    if (cd "$OUTDIR" && "$tool" a /dev/null /dev/null) >/dev/null 2>&1; then
        CAN_CREATE=1
        ARCHIVER="$tool"
        break
    fi
done

if [ "$CAN_CREATE" = "1" ]; then
    echo "==> packing with $ARCHIVER (compressed)"
    (cd "$OUTDIR" && "$ARCHIVER" a AmiNetXDuo.lha AmiNetXDuo.info AmiNetXDuo)
else
    echo "==> no archiver that can create found; using dist/lhapack.py"
    echo "    (valid LHA, but stored rather than compressed)"
    python3 "$ROOT/dist/lhapack.py" "$ARCHIVE" "$OUTDIR" \
            AmiNetXDuo.info AmiNetXDuo
fi

# --------------------------------------------------------------- checking --

echo
if command -v lha >/dev/null 2>&1; then
    lha l "$ARCHIVE" || true
    echo
    # Extract into a scratch directory and diff it against what we laid out.
    # An archive nobody has unpacked is not known to be an archive.
    VERIFY="$OUTDIR/.verify"
    rm -rf "$VERIFY"
    mkdir -p "$VERIFY"
    (cd "$VERIFY" && lha xfq2 "$ARCHIVE") >/dev/null 2>&1 || \
        (cd "$VERIFY" && lha xf "$ARCHIVE") >/dev/null 2>&1
    if diff -r "$TREE" "$VERIFY/AmiNetXDuo" >/dev/null 2>&1 && \
       cmp -s "$OUTDIR/AmiNetXDuo.info" "$VERIFY/AmiNetXDuo.info"; then
        echo "==> archive round-trips byte for byte"
        rm -rf "$VERIFY"
    else
        echo "!! the extracted archive does not match $TREE" >&2
        diff -r "$TREE" "$VERIFY/AmiNetXDuo" >&2 || true
        exit 1
    fi
fi

python3 "$INSTALL/tools/checkscript.py" "$TREE/Install-AmiNetXDuo"
python3 "$INSTALL/tools/showicon.py" "$TREE"/*.info >/dev/null

echo
echo "==> $ARCHIVE"
echo "==> $OUTDIR/AmiNetXDuo.readme"
du -sh "$ARCHIVE" | sed 's/^/    /'
