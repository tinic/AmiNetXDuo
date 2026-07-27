#!/usr/bin/env bash
#
# Build the AmiNetXDuo distribution archive.
#
#   dist/make-dist.sh [-b BUILDDIR] [-v VERSION] [-o OUTDIR]
#                     [-a "Name <address@example.com>"]
#
# Produces, in OUTDIR (default build/dist):
#
#   AmiNetXDuo-<version>.lha   the archive, laid out the way Aminet expects
#   AmiNetXDuo.readme          the upload description, with its header fields
#   AmiNetXDuo/                the same tree unpacked, for inspection
#
# The version comes from tools/version.sh, i.e. from project(AmiNetXDuo
# VERSION ...) compounded with the NetX Duo version read out of the submodule.
# -v overrides the product part for a test build; there is deliberately no way
# to override the NetX Duo part, because it describes what is in the archive.
# The archive filename carries our version alone -- an Aminet filename is read
# by people -- and the full compound goes in the .readme header, where the
# question "built on what?" is actually asked.
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
VERSION="$("$ROOT/tools/version.sh" --product)"
AUTHOR="${AMINETXDUO_AUTHOR:-UNSET -- pass -a \"Name <you@example.com>\"}"

while getopts "b:v:o:a:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        v) VERSION="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        a) AUTHOR="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-v version] [-o outdir]" \
                "[-a author]" >&2
           exit 2 ;;
    esac
done

case "$BUILD" in
    /*) ;;
    *) BUILD="$ROOT/$BUILD" ;;
esac

INSTALL="$ROOT/install"
TREE="$OUTDIR/AmiNetXDuo"

# What the .readme's Version: field says.  Aminet shows that field verbatim,
# so it is the one place a user sees the compound version without unpacking
# anything.  tools/version.sh checks the recorded NetX Duo version against the
# submodule and exits nonzero if they have drifted, so a stale label cannot be
# packed.
NETXDUO_VERSION=$("$ROOT/tools/version.sh" --netxduo)
THREADX_VERSION=$("$ROOT/tools/version.sh" --threadx)
VERSION_FIELD="$VERSION (NetX Duo $NETXDUO_VERSION, ThreadX $THREADX_VERSION)"

# ------------------------------------------------------------- ingredients --

need() {
    [ -f "$1" ] || {
        echo "missing: $1" >&2
        echo "  build it first:  cmake --build $BUILD --parallel" >&2
        exit 2
    }
}

LIBS=(bsdsocket usergroup)
CMDS=(AddNetInterface Online Offline ShowNetStatus ping netstat host fetch
      nc telnet ftp NetTrace sntp traceroute tftp whois
      CheckNetConfig GetNetStatus NetShutdown AddNetRoute DeleteNetRoute)

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

# tls.library and its trust store, when the build has them (AMINETXDUO_TLS=ON).
# They ship as a pair with the bsdsocket.library from the SAME build: the two
# share struct layouts and a private context ABI, so mixing versions is not
# supported (include/aminetxduo/nxcontext.h).
if [ -f "$BUILD/src/tlslib/tls.library" ]; then
    cp "$BUILD/src/tlslib/tls.library" "$TREE/Libs/"
    chmod 755 "$TREE/Libs/tls.library"
    echo "==> including tls.library"

    # A trust store is not optional when tls.library is packed: without one the
    # library refuses every connection with TLS_ERR_TRUSTSTORE, and a release
    # that ships that is worse than a release that does not build.  This used
    # to be a warning; it is a hard failure now.
    [ -f "$BUILD/certificates" ] || {
        echo "!! tls.library is packed and there is no $BUILD/certificates." >&2
        echo "!! The build should have made one -- see src/tlslib/CMakeLists.txt" >&2
        echo "!! and third_party/cacert/README.md." >&2
        exit 2
    }

    # And it has to be the store this source tree describes.  The build already
    # checks this; checking it again here is what stops a stale or hand-made
    # file in an old build directory reaching an archive.
    PIN=$(sed -n 's/^\([0-9a-fA-F]\{64\}\).*/\1/p' \
              "$ROOT/third_party/cacert/certificates.sha256")
    GOT=$( (shasum -a 256 "$BUILD/certificates" 2>/dev/null || \
            sha256sum "$BUILD/certificates") | cut -d" " -f1 )
    if [ -n "$PIN" ] && [ "$PIN" != "$GOT" ]; then
        echo "!! $BUILD/certificates is not the pinned trust store." >&2
        echo "!!   expected $PIN" >&2
        echo "!!   got      $GOT" >&2
        echo "!! Rebuild, or -- if this build used -DAMINETXDUO_CA_BUNDLE --" >&2
        echo "!! pack it from a build that did not." >&2
        exit 2
    fi

    cp "$BUILD/certificates" "$TREE/Devs/Internet/certificates"
    echo "==> including the trust store ($(wc -c < "$BUILD/certificates" | tr -d " ") bytes, $GOT)"
fi
for cmd in "${CMDS[@]}"; do
    cp "$BUILD/src/tools/$cmd" "$TREE/C/"
done
chmod 755 "$TREE"/C/* "$TREE"/Libs/*

# The ported Unix clients, when they have been built. Optional, because they
# come from clients/ rather than the CMake tree and a plain `cmake --build`
# produces neither. They go in their own drawer rather than into C:, because
# each needs something a user has to provide -- mathieeedoubbas.library, which
# is Commodore's and not ours to ship, and a far larger stack than a Shell
# gives a command -- and a `curl` in C: that fails on both would be worse than
# one that is clearly a separate thing.
#
# The build directory is NOT fixed: clients/curl/build.sh defaults to
# build/curl and puts a TLS build wherever -b says, so hardcoding one path
# means packing whichever tree the author happened to have. AMINETXDUO_CURL
# and AMINETXDUO_SSH let the caller state it outright -- the release workflow
# does, so the path it builds into and the path packed here cannot drift --
# and the fallback prefers a TLS build over a plain one, because a curl that
# cannot open an https:// URL is not the curl anybody wants shipped.
CLIENT_CURL="${AMINETXDUO_CURL:-}"
if [ -z "$CLIENT_CURL" ]; then
    for c in "$ROOT/build/curl-tls/src/curl" "$ROOT/build/curl/src/curl"; do
        [ -x "$c" ] && { CLIENT_CURL="$c"; break; }
    done
fi
CLIENT_SSH="${AMINETXDUO_SSH:-$ROOT/build/dropbear/dbclient}"
if [ -x "$CLIENT_CURL" ] || [ -x "$CLIENT_SSH" ]; then
    mkdir -p "$TREE/Clients"
    [ -x "$CLIENT_CURL" ] && { cp "$CLIENT_CURL" "$TREE/Clients/curl"; \
        echo "==> including curl ($(wc -c < "$CLIENT_CURL" | tr -d ' ') bytes)"; }
    [ -x "$CLIENT_SSH" ]  && { cp "$CLIENT_SSH" "$TREE/Clients/ssh"; \
        echo "==> including ssh ($(wc -c < "$CLIENT_SSH" | tr -d ' ') bytes)"; }
    chmod 755 "$TREE"/Clients/*
    cat > "$TREE/Clients/ReadMe" <<'CLIENTEOF'
These are ports of ordinary Unix programs, not commands we wrote. They are
here rather than in C: because each needs something this archive cannot
provide for you.

  curl   fetches http:// and https:// URLs.
  ssh    connects to an SSH server. It is Dropbear's dbclient under another
         name. Use a key: -i <keyfile>. A connection takes roughly ten seconds
         while the machine does the cryptography. Password logins are built in
         but have not been tried against a real server.

Both need:

  * mathieeedoubbas.library in LIBS:. It is Commodore's, so it is not in this
    archive, but every Workbench installation has it.

  * a much bigger stack than a Shell gives a command. Type

        stack 200000

    once in the Shell you are going to run them from.

Copy them into C: yourself if you would like them on your path.
CLIENTEOF
fi

# The installer prints its version in the about text; stamp it from the same
# source as everything else rather than shipping whatever the source tree last
# had in it.
sed -e "s/^(set VERSION_STR \".*\")/(set VERSION_STR \"$VERSION\")/" \
    "$INSTALL/Install-AmiNetXDuo" > "$TREE/Install-AmiNetXDuo"
grep -q "(set VERSION_STR \"$VERSION\")" "$TREE/Install-AmiNetXDuo" || {
    echo "the Installer script has no VERSION_STR line to stamp" >&2
    exit 2
}

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

# Lines beginning with ';' are notes to whoever edits the template -- how the
# header works, how Replaces: works. They are not for somebody reading the
# description on Aminet, so they are stripped from the generated file rather
# than shipped as clutter under the header.
sed -e "s|@VERSION@|$VERSION_FIELD|g" -e "s|@AUTHOR@|$AUTHOR|g" -e "/^;/d" \
    "$ROOT/dist/AmiNetXDuo.readme" > "$OUTDIR/AmiNetXDuo.readme"
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
if grep -q "@VERSION@\|@AUTHOR@" "$OUTDIR/AmiNetXDuo.readme"; then
    echo "AmiNetXDuo.readme still has a placeholder in it" >&2
    exit 2
fi
if grep -q "^Author: *UNSET" "$OUTDIR/AmiNetXDuo.readme"; then
    echo "!! Author/Uploader are unset.  Aminet wants a real name and" >&2
    echo "!! address there; re-run with -a \"Name <you@example.com>\"." >&2
fi

# ------------------------------------------------------------ the archive --

# Versioned filename, unversioned drawer: the .lha says which release it is,
# and unpacking it still leaves one drawer called AmiNetXDuo the way Aminet
# and Workbench expect.
ARCHIVE_NAME="AmiNetXDuo-$VERSION.lha"
ARCHIVE="$OUTDIR/$ARCHIVE_NAME"
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
    (cd "$OUTDIR" && "$ARCHIVER" a "$ARCHIVE_NAME" AmiNetXDuo.info AmiNetXDuo)
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
echo "==> version $VERSION_FIELD"
echo "==> $ARCHIVE"
echo "==> $OUTDIR/AmiNetXDuo.readme"
du -sh "$ARCHIVE" | sed 's/^/    /'
