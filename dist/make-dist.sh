#!/usr/bin/env bash
#
# Build the AmiNetXDuo distribution archive.
#
#   dist/make-dist.sh [-b BUILDDIR] [-v VERSION] [-o OUTDIR]
#
# Produces, in OUTDIR (default build/dist):
#
#   AmiNetXDuo-<version>.lha   the archive, in the conventional layout
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
# The convention is that an archive extracts into the directory it is
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
#       Docs/  Docs.info             the manual, from docs/user/
#       Examples/  Examples.info     commented configuration files
#       Developer/  Developer.info   headers and glue for the vectors past
#                                    the end of the NDK's SFD
#
# Compression: a real `lha a` is used when one is on the PATH.  Homebrew's
# `lha` is Lhasa, which can only extract, so the fallback is dist/lhapack.py
# correct, readable by every LhA, and uncompressed.  Build the upload on a
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
CMDS=(AddNetInterface NetSetup Online Offline ShowNetStatus ShowNetServices
      ping netstat host hostname
      nslookup arp fetch nc telnet NetTrace sntp traceroute tftp whois httpd
      CheckNetConfig GetNetStatus NetShutdown RemoveNetInterface
      ConfigureNetInterface
      AddNetRoute DeleteNetRoute)

# ---------------------------------------------------------- the CPU builds --
#
# ONE ARCHIVE, THREE LIBRARIES, and the installer picks.  The alternative --
# three archives, was rejected: the thing a user has to get right is the one
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
#             instruction set, so the 020 build is COMPLETE for it, there is
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
# build tree, install/test/run-installer-fsuae.sh uses it, because it boots
# one emulated machine and only ever installs one of the three.  A RELEASE
# must not set it: the installer aborts on a machine whose drawer is absent,
# which is the right behaviour for a damaged download and the wrong thing to
# discover about your own archive.
CPU_DIRS=(${AMINETXDUO_DIST_CPUS:-68000 68000-minimal 68020-40 68060})
declare -A CPU_BUILD=(
    [68000]="${AMINETXDUO_BUILD_68000:-$BUILD-68000}"
    [68000-minimal]="${AMINETXDUO_BUILD_68000_MINIMAL:-$BUILD-68000-minimal}"
    [68020-40]="$BUILD"
    [68060]="${AMINETXDUO_BUILD_68060:-$BUILD-68060}"
)

# 68000-minimal is the same stack with IPv6, mDNS, the packet filter, TLS and
# IPv4 multicast compiled out: 225 KB against the 68000 drawer's 308 KB, 83 KB
# of options.  Both figures are stripped, which they were not when this drawer
# was introduced -- it was the only one the release workflow stripped by hand,
# so 40 KB of the gap it used to claim was symbol table the other drawers were
# carrying and this one was not.  The free-memory figure §81 measured on the
# 1 MB machine predates the strip and is now conservative by that much.
#
# Multicast is the cheapest of the five at 3,888 bytes (3,696 of code and the
# 192-byte membership table) and is out for the same reason the others are:
# this drawer is what somebody installs having measured their machine.
#
# NOT what the installer picks.  It is here for somebody who has measured
# their machine and decided, which is why it is a drawer rather than a
# threshold: a stack that silently drops IPv6 and .local resolution on a
# machine that could have run them is a support question, not a saving.

# THE COMMANDS ARE BUILT ONCE, FOR THE 68000, and every machine runs that one
# set.  They are not where the work happens: each is a few hundred lines around
# bsdsocket.library calls, and the code whose instruction set actually matters
# the checksums, the copies, the bignums, is inside the libraries, which
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
# falls back to the primary build, which is right for what that option is
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
        echo "!! The build should have made one, see src/tlslib/CMakeLists.txt" >&2
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
        echo "!! Rebuild, or, if this build used -DAMINETXDUO_CA_BUNDLE --" >&2
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

# The ssh client, when it has been built. Optional, because it comes from
# clients/ rather than the CMake tree and a plain `cmake --build` does not
# produce it. It goes into C: with the other commands: the argv/stack shim
# (clients/compat/amiga_argv.c) now gives it a real POSIX argv[] and its own
# 256 KB stack, so the two reasons the clients used to be kept out of C:, no
# tokenised arguments, and a stack far bigger than a Shell hands a command --
# are both gone. It still wants mathieeedoubbas.library in LIBS:, which every
# Workbench installation has; the installer's help text says so.
#
# AMINETXDUO_SSH names the built binary outright, the release workflow sets
# it, so the path it builds into and the path packed here cannot drift.
# ONE PER CPU, all in C:.  dbclient is the only thing in the archive that
# comes from clients/ rather than the CMake tree, so it is built once per
# architecture rather than inheriting the CMake target's.  Copying the drawer
# wholesale would put a 68020 binary on a 68000 machine: the installer offered
# it, the user ran it, and the machine took an illegal instruction.
#
# So the archive carries ssh.000, ssh.020 and ssh.060 and the installer copies
# the one this machine can run to C:ssh.  The suffixed set exists only inside
# the archive; what lands on the disk is C:ssh, where it has always been.
#
# AMINETXDUO_SSH_68000, _68020 and _68060 name the three builds; the release
# workflow runs clients/dropbear/build.sh once per architecture to make them.
declare -A SSH_BUILD=(
    [000]="${AMINETXDUO_SSH_68000:-$ROOT/build/ssh00/dbclient}"
    [020]="${AMINETXDUO_SSH_68020:-$ROOT/build/ssh20/dbclient}"
    [060]="${AMINETXDUO_SSH_68060:-$ROOT/build/ssh60/dbclient}"
)
for sshcpu in "${!SSH_BUILD[@]}"; do
    src="${SSH_BUILD[$sshcpu]}"
    [ -x "$src" ] || continue
    cp "$src" "$TREE/C/ssh.$sshcpu"
    chmod 755 "$TREE/C/ssh.$sshcpu"
    echo "==> including ssh.$sshcpu ($(wc -c < "$src" | tr -d ' ') bytes)"
done

CLIENT_SSH="${AMINETXDUO_SSH:-}"
if [ -n "$CLIENT_SSH" ] && [ -x "$CLIENT_SSH" ]; then
    cp "$CLIENT_SSH" "$TREE/C/ssh"
    chmod 755 "$TREE/C/ssh"
    echo "==> including ssh ($(wc -c < "$CLIENT_SSH" | tr -d ' ') bytes)"
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

# The Commodore Installer, bundled in the same drawer.  Install-AmiNetXDuo.info's
# default tool is "Installer", so Workbench finds it here and a double-click just
# works, nothing to fetch or install first.
cp "$INSTALL/Installer" "$TREE/Installer"
chmod 755 "$TREE/Installer"

cp -R "$INSTALL/devs/Internet/." "$TREE/Devs/Internet/"
cp -R "$INSTALL/examples/."      "$TREE/Examples/"

cp "$ROOT/dist/ReadMe" "$TREE/ReadMe"
cp "$INSTALL/Document.info" "$TREE/ReadMe.info"
cp "$INSTALL/Drawer.info"   "$TREE/Docs.info"
cp "$INSTALL/Drawer.info"   "$TREE/Examples.info"

# ----------------------------------------------------------- the Developer --
#
# The headers and compiler glue for what bsdsocket.library has past the end of
# the NDK's SFD.  Staged by the same script the build uses, so what a
# downloader gets is what tests/tools' IfNames was compiled against.
#
# NOT installed by Install-AmiNetXDuo: where headers belong is a property of
# the compiler someone has, not of this machine, so copying them to a fixed
# place would be a guess.  They are in the archive, to be put wherever the
# NDK is.
"$ROOT/tools/stage-developer.sh" "$TREE/Developer"
cp "$INSTALL/Drawer.info"   "$TREE/Developer.info"
cp "$INSTALL/Document.info" "$TREE/Developer/ReadMe.info"

# --------------------------------------------------------------- Profile ---
#
# The sampling profiler, in Developer/ rather than in C:.
#
# It is not part of the stack and the installer does not copy it anywhere: it
# profiles ANY AmigaOS program, it depends on nothing in this archive, and a
# network stack has no business putting a developer tool in somebody's C:.
# It is HERE because this is the only way it reaches a real machine, and a
# real machine is the only place the answer can come from, no emulator above
# a 68020 has a usable cycle model, so what this stack costs on a 68060 with a
# real card is not currently knowable any other way.
#
# The 68000 build, like the commands and for the same reason: the sampling
# handler is assembly and identical on every 68k, and nothing else in the tool
# is on a hot path.  One binary runs on every machine.
#
# profspin goes with it.  It is the self-test, an ordinary program that
# links nothing from the profiler and writes the exact byte ranges of its own
# assembly kernels, so that "is this profiler telling me the truth on MY
# machine" has an answer that does not require taking our word for it.
PROFILE_BUILD="$CMD_BUILD/tools/profiler"
if [ -x "$PROFILE_BUILD/Profile" ]; then
    mkdir -p "$TREE/Developer/Profile"
    cp "$PROFILE_BUILD/Profile"  "$TREE/Developer/Profile/"
    cp "$PROFILE_BUILD/profspin" "$TREE/Developer/Profile/"
    chmod 755 "$TREE/Developer/Profile"/Profile "$TREE/Developer/Profile"/profspin
    cp "$ROOT/tools/profiler/ReadMe"       "$TREE/Developer/Profile/ReadMe"
    cp "$ROOT/tools/profiler/profreport.py" "$TREE/Developer/Profile/"
    cp "$INSTALL/Drawer.info"   "$TREE/Developer/Profile.info"
    cp "$INSTALL/Document.info" "$TREE/Developer/Profile/ReadMe.info"
    echo "==> including Profile ($(cat "$PROFILE_BUILD/Profile" "$PROFILE_BUILD/profspin" | wc -c | tr -d " ") bytes of binaries)"
else
    echo "note: no Profile in $PROFILE_BUILD; the archive will not carry the profiler"
fi

# ---------------------------------------------------------------- the docs --
#
# docs/user/ is the manual and nothing else: docs/ itself is developer notes
# and RESEARCH.md, none of which belongs in an archive a user unpacks.
#
# The glob used to read docs/*.guide.  docs/ holds only Markdown, so it matched
# nothing, the empty-Docs fallback fired, and every release shipped a Docs
# drawer holding a second copy of the ReadMe and no manual, for a month,
# because the fallback printed a note and carried on.  It is fatal now.

DOCSRC="$ROOT/docs/user"

shopt -s nullglob
for doc in "$DOCSRC"/*.guide "$DOCSRC"/*.txt "$DOCSRC"/*.doc; do
    cp "$doc" "$TREE/Docs/"
done
for doc in "$DOCSRC"/*.info; do
    cp "$doc" "$TREE/Docs/"
done
shopt -u nullglob

# Give every doc that has no icon of its own an icon, so a Workbench user can
# see and open all of them.  A .guide gets the one whose default tool is
# MultiView; More would show it its own markup.
for doc in "$TREE"/Docs/*; do
    [ -f "$doc" ] || continue
    case "$doc" in *.info) continue ;; esac
    [ -f "$doc.info" ] && continue
    case "$doc" in
        *.guide) cp "$INSTALL/Guide.info"    "$doc.info" ;;
        *)       cp "$INSTALL/Document.info" "$doc.info" ;;
    esac
done

# The manual is not optional.  An archive without it is the defect above, and
# the only way to be sure it never recurs is to refuse to build one.
shopt -s nullglob
packed_guides=("$TREE"/Docs/*.guide)
shopt -u nullglob

if [ ${#packed_guides[@]} -eq 0 ]; then
    echo "ERROR: no .guide reached $TREE/Docs" >&2
    echo "  looked in: $DOCSRC" >&2
    echo "  the manual ships in every archive; there is no build without it" >&2
    exit 2
fi

echo "==> including $(basename "$DOCSRC")/: $(cd "$TREE/Docs" && echo *)"

# ------------------------------------------------------------ the archive --

# Versioned filename, unversioned drawer: the .lha says which release it is,
# and unpacking it still leaves one drawer called AmiNetXDuo the way the
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
elif [ -n "${AMINETXDUO_ALLOW_STORED:-}" ]; then
    echo "==> no archiver that can create found; using dist/lhapack.py"
    echo "    (valid LHA, but stored rather than compressed)"
    python3 "$ROOT/dist/lhapack.py" "$ARCHIVE" "$OUTDIR" \
            AmiNetXDuo.info AmiNetXDuo
else
    # A note here is what let every release up to 0.17.0 ship uncompressed:
    # the fallback is fine for a local trial and is not fine for the file
    # people download, and nothing told the difference between the two.
    echo "!! no archiver that can create was found." >&2
    echo "   dist/lhapack.py writes a valid archive with NOTHING compressed" >&2
    echo "   in it, which is not what a release should be.  Install one of" >&2
    echo "   lha, jlha or lharc -- on Debian and Ubuntu that is jlha-utils." >&2
    echo "   AMINETXDUO_ALLOW_STORED=1 packs it stored anyway." >&2
    exit 1
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
