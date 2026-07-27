#!/usr/bin/env bash
#
# Build the AmiNetXDuo distribution archive.
#
#   dist/make-dist.sh [-b BUILDDIR] [-v VERSION] [-o OUTDIR]
#
# Produces, in OUTDIR (default build/dist):
#
#   AmiNetXDuo-<version>.lha   the archive, laid out the way Aminet expects
#   AmiNetXDuo/                the same tree unpacked, for inspection
#
# The version comes from tools/version.sh, i.e. from project(AmiNetXDuo
# VERSION ...) compounded with the NetX Duo version read out of the submodule.
# -v overrides the product part for a test build; there is deliberately no way
# to override the NetX Duo part, because it describes what is in the archive.
# The archive filename carries our version alone, because a filename is read
# by people.  The full compound is in the installer and in each binary's
# version string.
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
#       C/                           the commands
#       Libs/68000/                  one drawer per CPU; the installer picks
#       Libs/68020-40/               the one this machine can run
#       Libs/68060/
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

# tools/version.sh checks the recorded NetX Duo version against the submodule
# and exits nonzero if they have drifted, so a stale label cannot be packed.
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
CMDS=(AddNetInterface NetSetup Online Offline ShowNetStatus ping netstat host fetch
      nc telnet ftp NetTrace sntp traceroute tftp whois
      CheckNetConfig GetNetStatus NetShutdown AddNetRoute DeleteNetRoute)

# ---------------------------------------------------------- the CPU builds --
#
# ONE ARCHIVE, THREE LIBRARIES, and the installer picks.  The alternative --
# three archives -- was rejected: the thing a user has to get right is the one
# thing they cannot see from the outside, and an Amiga owner who downloads the
# wrong one gets either a machine that will not boot the stack or a library
# that quietly runs emulated instructions.  The installer already reads
# DEVS: to find the network card, so reading `database "cpu"` to pick a
# library is in keeping rather than a new kind of magic.
#
# THREE, NOT FOUR, and this is the part worth reading.  The toolchain has
# multilibs for `.` (68000), `libm020` and `libm060`, and those three cover
# every 68k Amiga:
#
#   68000     68000 and 68010.
#   68020-40  68020, 68030 and 68040.  A 68040 implements the whole 68020
#             instruction set, so the 020 build is COMPLETE for it -- there is
#             no 68040 multilib to link against and -m68040 would silently
#             select the 68000 C library.  AMINETXDUO_CPU=68040 exists for
#             someone building their own tuned copy; it produces the same
#             instructions and is not worth a fourth of everything in here.
#   68060     genuinely different: the 68060 dropped MULU.L's and DIVU.L's
#             64-bit-result forms, so it needs its own codegen.
#
# The naming is AmiSSL's, which ships exactly `68020-40` and `68060` drawers,
# because a user who has seen one will recognise the other.  docs/RESEARCH.md
# §45.
#
# The build directories are derived from -b rather than named separately, so
# `-b build/release` wants build/release-68000 and build/release-68060 beside
# it and cannot be pointed at a tree from a different commit by accident.
#
# AMINETXDUO_DIST_CPUS names a subset, for building a test archive from one
# build tree -- install/test/run-installer-fsuae.sh uses it, because it boots
# one emulated machine and only ever installs one of the three.  A RELEASE
# must not set it: the installer aborts on a machine whose drawer is absent,
# which is the right behaviour for a damaged download and the wrong thing to
# discover about your own archive.
CPU_DIRS=(${AMINETXDUO_DIST_CPUS:-68000 68020-40 68060})
declare -A CPU_BUILD=(
    [68000]="${AMINETXDUO_BUILD_68000:-$BUILD-68000}"
    [68020-40]="$BUILD"
    [68060]="${AMINETXDUO_BUILD_68060:-$BUILD-68060}"
)

# THE COMMANDS ARE BUILT ONCE, FOR THE 68000, and every machine runs that one
# set.  They are not where the work happens: each is a few hundred lines around
# bsdsocket.library calls, and the code whose instruction set actually matters
# -- the checksums, the copies, the bignums -- is inside the libraries, which
# ARE per-CPU.  Twenty-one commands times three would add roughly 9 MB to the
# archive to make `ping` parse its arguments faster.
#
# It costs nothing in features: src/tools/CMakeLists.txt notes that one binary
# serves both -DAMINETXDUO_TLS=ON and OFF, because fetch opens LIBS:tls.library
# at run time rather than linking it.  So the 68000-built fetch still does
# https on a machine whose installed library has TLS.

for cpu in "${CPU_DIRS[@]}"; do
    b="${CPU_BUILD[$cpu]}"
    case "$b" in /*) ;; *) b="$ROOT/$b"; CPU_BUILD[$cpu]="$b" ;; esac
    [ -d "$b" ] || {
        echo "missing the $cpu build: $b" >&2
        echo "  cmake -S . -B $b -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \\" >&2
        echo "        -DCMAKE_BUILD_TYPE=Release -DAMINETXDUO_CPU=${cpu%%-*}" >&2
        echo "  cmake --build $b --parallel" >&2
        exit 2
    }
    for lib in "${LIBS[@]}"; do need "$b/src/$lib/$lib.library"; done
done

# Normally the 68000 build, per the note above.  When AMINETXDUO_DIST_CPUS has
# excluded it there is no 68000 tree to take them from, so a subset archive
# falls back to the primary build -- which is right for what that option is
# for, and never happens in a release.
CMD_BUILD="${CPU_BUILD[68000]:-$BUILD}"
[ -d "$CMD_BUILD" ] || CMD_BUILD="$BUILD"

for cmd in "${CMDS[@]}"; do need "$CMD_BUILD/src/tools/$cmd"; done

# -------------------------------------------------------------- the icons --
#
# Regenerated every time so that a change to makeicon.py cannot leave a stale
# icon in an archive.

python3 "$INSTALL/tools/makeicon.py" "$INSTALL" >/dev/null

# -------------------------------------------------------------- the layout --

rm -rf "$TREE" "$OUTDIR/AmiNetXDuo.info"
mkdir -p "$TREE/C" "$TREE/Libs" "$TREE/Devs/Internet" "$TREE/Docs" \
         "$TREE/Examples"

for cpu in "${CPU_DIRS[@]}"; do
    mkdir -p "$TREE/Libs/$cpu"
    for lib in "${LIBS[@]}"; do
        cp "${CPU_BUILD[$cpu]}/src/$lib/$lib.library" "$TREE/Libs/$cpu/"
    done

    # tls.library ships with the bsdsocket.library FROM THE SAME BUILD: the two
    # share struct layouts and a private context ABI, so a 68060 tls.library
    # beside a 68020 bsdsocket.library is not a supported combination even
    # though both would load (include/aminetxduo/nxcontext.h).  Per-CPU
    # drawers make that impossible to get wrong.
    #
    # There is deliberately no 68000 tls.library.  §9's M9 gate measured the
    # handshake on the 68020 floor and rejected it there; a 7 MHz 68000 with
    # no 32-bit multiply is not the machine that changes that answer.  The
    # stack ships for the 68000, the encryption does not, and the installer
    # says so rather than leaving a drawer mysteriously short of a file.
    if [ -f "${CPU_BUILD[$cpu]}/src/tlslib/tls.library" ]; then
        cp "${CPU_BUILD[$cpu]}/src/tlslib/tls.library" "$TREE/Libs/$cpu/"
        echo "==> $cpu: bsdsocket, usergroup, tls"
    else
        echo "==> $cpu: bsdsocket, usergroup (no tls.library in this build)"
    fi
    chmod 755 "$TREE/Libs/$cpu"/*
done

# The trust store comes from the primary build, and is packed whenever any
# CPU build produced a tls.library.
if [ -f "$BUILD/src/tlslib/tls.library" ]; then

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
    cp "$CMD_BUILD/src/tools/$cmd" "$TREE/C/"
done
chmod 755 "$TREE"/C/*

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
du -sh "$ARCHIVE" | sed 's/^/    /'
