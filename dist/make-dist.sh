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
#       Libs/                        the libraries: ONE build, every 68k
#       Libs/minimal/                the same stack with the big options out
#       Devs/Networks/               the SANA-II drivers, one build, and one
#                                    copy: no minimal drawer, see stage_build
#       Devs/Internet/               protocols, services, networks
#       Docs/  Docs.info             the manual, from docs/user/
#       Examples/  Examples.info     commented configuration files
#       Terminal/  Terminal.info     httpd's pages: the terminal's and the
#                                    console's, one HTML file each
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

# SANA-II drivers, DEVS:Networks/.  From the same build as the libraries and
# commands: this is interrupt-level packet handling, not a shell command whose
# hot work happens inside a library.
#
# Install-AmiNetXDuo installs it, into DEVS:Networks/.  It did not for a long
# time, and a machine therefore kept whatever driver it already had through
# every reinstall; install/test/run-workbench.sh now asserts the installed copy
# byte for byte against the one packed here, so the two cannot drift apart
# again in silence.
DEVICES=(netdev/anxnet)
CMDS=(AddNetInterface NetSetup Online Offline ShowNetStatus ShowNetServices
      ping netstat host hostname
      nslookup arp fetch nc telnet NetTrace NetCapture sntp traceroute tftp
      whois httpd
      iperf
      CheckNetConfig CheckNetDevice GetNetStatus NetShutdown RemoveNetInterface
      ConfigureNetInterface
      AddNetRoute DeleteNetRoute)

# ------------------------------------------------------------- the builds --
#
# ONE ARCHIVE, ONE SET OF BINARIES.  There is no CPU in here any more and the
# installer does not ask: every library, device and command is built -m68000 and
# picks its own inner loops from SysBase->AttnFlags at init
# (src/net68k/n68k_cpu.c, src/crypto68k/c68k_cpu.c).  What used to be four
# drawers keyed on `database "cpu"` -- 68000, 68000-minimal, 68020-40, 68060 --
# is now one, plus `minimal`, which is a FEATURE choice and not a CPU one.
#
# That removes the failure this archive was shaped around.  A user who picked
# the low drawer to be safe got a library with none of the 68020 assembly, and
# on crypto that is not a few percent: a 68060 running the portable bignums
# spends 64% of a TLS 1.3 transfer in one function.  Nobody can pick wrong now
# because there is nothing to pick.
#
# minimal is the same stack with IPv6, mDNS, the packet filter, TLS, IPv4
# multicast, the ARexx host and the TCP: handler compiled out: 243,332 bytes
# against the full build's 369,820, 124 KB of options, both stripped.  It is for
# somebody who has measured their 1 MB machine and decided, which is why it is
# a drawer and not a threshold: a stack that silently drops IPv6 and .local
# resolution on a machine that could have run them is a support question, not a
# saving.
#
# The build directory is derived from -b, so `-b build/release` wants
# build/release-minimal beside it and cannot be pointed at a tree from a
# different commit by accident.
MINIMAL_BUILD="${AMINETXDUO_BUILD_MINIMAL:-$BUILD-minimal}"
case "$MINIMAL_BUILD" in /*) ;; *) MINIMAL_BUILD="$ROOT/$MINIMAL_BUILD" ;; esac

# AMINETXDUO_DIST_NO_MINIMAL=1 packs the full build alone, for a test archive
# from one build tree.  A RELEASE must not set it.
WANT_MINIMAL=1
[ -z "${AMINETXDUO_DIST_NO_MINIMAL:-}" ] || WANT_MINIMAL=0

# The seven the release workflow turns off for the floor drawer.  Kept here so
# a hand-run of this script produces the same minimal library CI does; the
# workflow's own copy at .github/workflows/release.yml is the other one, and
# they have to agree.
MINIMAL_OPTIONS="-DAMINETXDUO_IPV6=OFF -DAMINETXDUO_MDNS=OFF \
-DAMINETXDUO_BPF=OFF -DAMINETXDUO_TLS=OFF -DAMINETXDUO_MULTICAST=OFF \
-DAMINETXDUO_AREXX=OFF -DAMINETXDUO_TCPDEVICE=OFF"

BUILDS=("$BUILD")
[ "$WANT_MINIMAL" = "0" ] || BUILDS+=("$MINIMAL_BUILD")

# WHAT IS IN THE TREE IS WHAT GETS PACKED, so check that it is the build that
# runs everywhere.  A build directory configured before the default changed --
# or by hand for a measurement -- still has AMINETXDUO_CPU=68020 in its cache,
# CMake keeps a cached value across a reconfigure, and the result is an archive
# that promises every 68k and contains a library that stops a 68000 on an
# illegal instruction.  That happened here once, and nothing said a word: the
# libraries were the right size for what they were and the wrong thing entirely.
cpu_of() {
    sed -n 's/^AMINETXDUO_CPU:STRING=//p' "$1/CMakeCache.txt" 2>/dev/null
}

lto_of() {
    sed -n 's/^AMINETXDUO_LTO:BOOL=//p' "$1/CMakeCache.txt" 2>/dev/null
}

# The Release C flags, not a grep of the whole cache: the option's own help
# text contains the string "-flto" whether it is ON or OFF, so a bare grep
# says yes on a build that has none.
flto_in() {
    sed -n 's/^CMAKE_C_FLAGS_RELEASE:STRING=//p' "$1/CMakeCache.txt" 2>/dev/null \
        | grep -q -- '-flto' && echo yes || echo no
}

# A missing build is built, not reported.  This script's job is to produce the
# archive, and printing the two commands that would produce it and exiting 2 is
# the same work with a person in the middle of it.  AMINETXDUO_DIST_NO_BUILD=1
# restores the old behaviour for a caller that wants to be told rather than
# served -- CI sets it, because there a missing tree means the build stage did
# not run and building it here would hide that.
#
# AN EXISTING TREE IS BUILT TOO, and that is the whole of the second cmake call
# below rather than a nicety.  Until 2026-08-20 the build ran only when the
# directory was absent, so a checkout that had ever been built packed whatever
# was in build/ regardless of what the source now said.  It shipped:
# AmiNetXDuo-rel.lha, packed 20:11 UTC from a HEAD containing acd18077, carried
# the tls.library linked at 07:59 UTC that morning -- twelve hours and one
# crypto fix behind its own source tree, md5 identical to the stale object
# still sitting in that build directory.  Nothing said a word, because the
# `need` checks below assert that a library EXISTS and its CPU cache entry
# matches, and both were true of the old one.
#
# It cost a release gate: install/test/run-workbench.sh went red four times
# against a stack whose defect had already been fixed, and read as "the fix did
# not take" rather than "the fix was not in the archive".  `cmake --build` on an
# up-to-date tree is a few seconds; an archive that does not correspond to the
# source it was built from is not a thing this script may produce.
for b in "${BUILDS[@]}"; do
    if [ ! -d "$b" ]; then
        if [ -n "${AMINETXDUO_DIST_NO_BUILD:-}" ]; then
            echo "missing build: $b" >&2
            echo "  cmake -S . -B $b -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \\" >&2
            echo "        -DCMAKE_BUILD_TYPE=Release" >&2
            echo "  cmake --build $b --parallel" >&2
            exit 2
        fi

        cpu_flag=-DAMINETXDUO_CPU=any
        min_flags=
        [ "$b" != "$MINIMAL_BUILD" ] || min_flags="$MINIMAL_OPTIONS"

        echo "==> configuring $b" >&2
        # shellcheck disable=SC2086
        cmake -S "$ROOT" -B "$b" \
              -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake \
              -DCMAKE_BUILD_TYPE=Release $cpu_flag $min_flags >&2 || exit 2
    fi

    # NO_BUILD means "do not build", not "do not build a tree that is missing":
    # CI has already run its build stage and a second one here would hide a
    # stage that did not.
    if [ -z "${AMINETXDUO_DIST_NO_BUILD:-}" ]; then
        echo "==> building $b" >&2
        cmake --build "$b" --parallel >&2 || exit 2
    fi

    # -flto IS MANDATORY IN EVERY SHIPPED DRAWER.  Not a default, not a
    # recommendation, and not a decision this script or anything calling it may
    # take: 0.25.3 was cut without it, the archive was larger and slower than
    # the release before it, and it was pulled.  Both halves are checked --
    # the option, and the flag actually reaching the Release flags -- because
    # a cached value survives a reconfigure and the option alone has been
    # true while the flag was absent.
    lto=$(lto_of "$b")
    [ "$lto" = "ON" ] || {
        echo "!! $b was configured with AMINETXDUO_LTO=${lto:-unset}." >&2
        echo "!! Every shipped drawer is built with -flto.  Reconfigure with" >&2
        echo "!! -DAMINETXDUO_LTO=ON, in a fresh build directory: a cached" >&2
        echo "!! OFF survives a reconfigure." >&2
        exit 2
    }
    [ "$(flto_in "$b")" = "yes" ] || {
        echo "!! $b has AMINETXDUO_LTO=ON but no -flto in its cache." >&2
        echo "!! The option is set and the flag never reached the compiler," >&2
        echo "!! which is the shape that ships a non-LTO archive while every" >&2
        echo "!! check that reads the option says it is fine.  Configure it" >&2
        echo "!! fresh." >&2
        exit 2
    }

    got=$(cpu_of "$b")
    [ "$got" = "any" ] || {
        echo "!! $b was configured with AMINETXDUO_CPU=${got:-unset}." >&2
        echo "!! The archive has no CPU drawers: one build serves every 68k," >&2
        echo "!! and a per-CPU one packed into it would stop a machine it was" >&2
        echo "!! not built for.  Configure it fresh -- a cached value survives" >&2
        echo "!! a reconfigure -- or pass -DAMINETXDUO_CPU=any." >&2
        exit 2
    }

    for lib in "${LIBS[@]}"; do need "$b/src/$lib/$lib.library"; done
    for dev in "${DEVICES[@]}"; do need "$b/src/$dev.device"; done
done

# The commands come from the full build, which is the only build there is for
# them: src/tools/CMakeLists.txt notes that one binary serves both
# -DAMINETXDUO_TLS=ON and OFF, because fetch opens LIBS:tls.library at run time
# rather than linking it, so even a minimal installation runs these.
CMD_BUILD="$BUILD"

for cmd in "${CMDS[@]}"; do need "$CMD_BUILD/src/tools/$cmd"; done

# A command that is BUILT and not listed above ships nowhere, and nothing
# notices: `need` above catches a name in the list with no binary, which is the
# easy direction.  iperf was merged and left out of CMDS, and the archive would
# have gone out without it.
#
# Anything deliberately not shipped goes here with its reason.  ToolsSmoke is a
# test driver -- it runs the real commands through SystemTagList() and records
# rc, milliseconds and free memory per command, which is what the leak sweep
# and the tool tests read -- and has no use on a user's machine.
# wbgrab writes the Workbench screen's bitplanes to a file and is the harness
# half of the console: httpd -C is what a user runs, and this is what
# tests/tools/run-wbgrab.sh runs to check the grab against a decoder on
# another machine.  A test driver, like ToolsSmoke, and no use on a user's
# machine for the same reason.
# El3Diag reads one card's registers at one machine's address through one
# measured byte-order quirk, and its capture-watch phases read 0/100 on a
# HEALTHY machine (the driver drains frames faster than the tool samples).
# A specialist's instrument for a bench, not a command for a drawer.
# paysum is the guest half of the payload-integrity harness: it moves a
# seeded pattern and prints content CRCs for tests/tools/paypeer.py to be
# compared against, and without that peer and run-payverify.sh's join it
# proves nothing a user can read.  A harness half, like ToolsSmoke.
NOT_SHIPPED=(ToolsSmoke CensusProbe UafProbe HangProbe wbgrab El3Diag paysum)
missing_from_cmds=()
for path in "$CMD_BUILD"/src/tools/*; do
    [ -f "$path" ] && [ -x "$path" ] || continue
    name=$(basename "$path")
    case "$name" in *.map|*.cmake|Makefile|*.o) continue ;; esac
    for known in "${CMDS[@]}" "${NOT_SHIPPED[@]}"; do
        [ "$name" = "$known" ] && continue 2
    done
    missing_from_cmds+=("$name")
done
if [ ${#missing_from_cmds[@]} -ne 0 ]; then
    echo "these commands are built but ship nowhere: ${missing_from_cmds[*]}" >&2
    echo "add them to CMDS, or to NOT_SHIPPED with the reason." >&2
    exit 1
fi

# -------------------------------------------------------------- the icons --
#
# Regenerated every time so that a change to makeicon.py cannot leave a stale
# icon in an archive.

python3 "$INSTALL/tools/makeicon.py" "$INSTALL" >/dev/null

# -------------------------------------------------------------- the layout --

rm -rf "$TREE" "$OUTDIR/AmiNetXDuo.info"
mkdir -p "$TREE/C" "$TREE/Libs" "$TREE/Devs/Internet" "$TREE/Devs/Networks" \
         "$TREE/Docs" "$TREE/Examples" "$TREE/Terminal"

# The full build at the top of Libs: and Devs/Networks:, minimal one drawer
# down.  tls.library ships with the bsdsocket.library FROM THE SAME BUILD --
# the two share struct layouts and a private context ABI
# (include/aminetxduo/nxcontext.h) -- which one directory per build keeps
# impossible to get wrong.
#
# THE 68000 GETS ENCRYPTION NOW.  There used to be no 68000 tls.library at all:
# §9's M9 gate measured the handshake on the 68020 floor and rejected it below.
# One binary settles that differently, because it is the same binary -- measured
# on an A600 under Amiberry, TLS 1.3 completes in 58.4 s against an RSA leaf and
# 97.7 s against an EC one, and session resumption takes a repeat connection to
# well under a second (13).  Slow is a thing a user can decide about; absent is
# not.
stage_build() {          # $1 = build dir, $2 = "" for the top or a drawer name
    local b="$1" sub="$2"
    local libs="$TREE/Libs${sub:+/$sub}"
    local devs="$TREE/Devs/Networks${sub:+/$sub}"
    local lib dev

    mkdir -p "$libs" "$devs"
    for lib in "${LIBS[@]}"; do
        cp "$b/src/$lib/$lib.library" "$libs/"
    done
    if [ -f "$b/src/tlslib/tls.library" ]; then
        cp "$b/src/tlslib/tls.library" "$libs/"
        echo "==> ${sub:-full}: bsdsocket, usergroup, tls"
    else
        echo "==> ${sub:-full}: bsdsocket, usergroup (no tls.library in this build)"
    fi
    chmod 755 "$libs"/*

    # THE DRIVER IS STAGED ONCE, at the top, and not again in the minimal
    # drawer.  anxnet.device is standalone -- it opens exec and expansion and
    # nothing else, and no AMINETXDUO_* feature flag reaches src/netdev -- so
    # the minimal build compiles the same code.  Checked rather than assumed:
    # the two binaries are the same 25084 bytes and differ in FIVE, which are
    # the CPU label inside the $VER: string ("any" against "68000").  Shipping
    # a second copy of one driver is a second thing to keep in step for no
    # difference a machine can observe.
    if [ -z "$sub" ]; then
        for dev in "${DEVICES[@]}"; do
            cp "$b/src/$dev.device" "$devs/"
        done
        chmod 755 "$devs"/*
    else
        rmdir "$devs" 2>/dev/null || true
    fi
}

stage_build "$BUILD" ""
[ "$WANT_MINIMAL" = "0" ] || stage_build "$MINIMAL_BUILD" "minimal"

# The trust store comes from the primary build, and is packed whenever any
# build produced a tls.library.
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
# ONE ssh, in C:, like everything else.  It used to be three -- ssh.000,
# ssh.020 and ssh.060, with the installer copying the one this machine could run
# -- because dbclient comes from clients/ rather than the CMake tree and was
# built once per architecture, and copying the drawer wholesale had put a 68020
# binary on a 68000 machine: the installer offered it, the user ran it, and the
# machine took an illegal instruction.
#
# clients/dropbear/build.sh with AMINETXDUO_CLIENT_ANY=1 builds one instead: the
# code is -m68000 and both X25519 field multiplies are in it, the MULU.L one and
# the four-MULU.W one, picked from AttnFlags at the first handshake
# (clients/dropbear/amiga_25519.c).  That matters more here than anywhere else
# in the archive -- an ssh handshake is 97% public-key arithmetic (35), so the
# wrong binary was never a few percent.
#
# AMINETXDUO_SSH names the built binary outright; the release workflow sets it,
# so the path it builds into and the path packed here cannot drift.
#
# AND WHEN NOBODY NAMES ONE, BUILD IT. A release archive without C:ssh is not
# the release: the same reasoning that makes this script build a missing CMake
# tree rather than print the command applies here, and the half that was left
# out is the one that silently produces a SHORTER archive rather than an error.
# An e2e run against a hand-built archive staged `C/ssh ABSENT` and nothing
# said so. AMINETXDUO_DIST_NO_BUILD=1 still reports instead of building, which
# is what the release workflow wants -- there the client is built in its own
# step and AMINETXDUO_SSH names it, so building here would be the second copy.
#
# UNCONDITIONALLY, for the reason the CMake loop above is unconditional: a
# dbclient left in build/ssh from an earlier checkout is packed as readily as a
# current one and looks the same in the archive listing.  clients/dropbear/
# build.sh reconfigures only when the flags change and otherwise runs `make`,
# so an up-to-date tree costs one make invocation.
CLIENT_SSH="${AMINETXDUO_SSH:-}"
if [ -z "$CLIENT_SSH" ] && [ -z "${AMINETXDUO_DIST_NO_BUILD:-}" ]; then
    CLIENT_SSH="$ROOT/build/ssh/dbclient"
    echo "==> building the ssh client (no AMINETXDUO_SSH given)" >&2
    # The same two settings the release workflow uses: ONE binary for the
    # whole 68k family, picking its X25519 field multiply from AttnFlags at
    # the first handshake rather than being built per CPU.
    AMINETXDUO_CLIENT_ANY=1 AMIGA_CLIENT_ARCH=-m68000 \
        "$ROOT/clients/dropbear/build.sh" -b "$ROOT/build/ssh" >&2 || {
        echo "ssh client build failed; the archive would carry none" >&2
        exit 2; }
fi
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

# httpd's terminal page.  It ships from src/tools/web because it is the CLIENT
# half of httpd's own protocol and has to change in the same commit the server
# does; an archive that carried a page from a different version would fail in
# the browser with nothing on the Amiga to see.  `need` it rather than glob it:
# a release that quietly shipped no page would leave -T naming a file that is
# not there.
#
# shell.html.gz goes with it, and is `need`ed for the same reason: httpd
# picks it up by spelling, so an archive that shipped only the plain page
# would work and would spend three seconds handing 400 KB to every browser
# instead of a fraction of a second handing over 120 KB.  A missing one is
# silent, which is exactly why it is checked here.
need "$ROOT/src/tools/web/shell.html"
need "$ROOT/src/tools/web/shell.html.gz"
cp "$ROOT/src/tools/web/shell.html"    "$TREE/Terminal/shell.html"
cp "$ROOT/src/tools/web/shell.html.gz" "$TREE/Terminal/shell.html.gz"

# httpd's console page, into the same drawer, `need`ed for the same reasons.
#
# The SAME drawer and not one of its own, which is what src/tools/httpd.c's
# httpd_console_places[] says too: the installer copies this drawer whole, so
# a page in it is a page that gets installed and a page anywhere else needs an
# installer that knows about it.  What the drawer holds is httpd's pages; it
# is named after the first one.
need "$ROOT/src/tools/web/console.html"
need "$ROOT/src/tools/web/console.html.gz"
cp "$ROOT/src/tools/web/console.html"    "$TREE/Terminal/console.html"
cp "$ROOT/src/tools/web/console.html.gz" "$TREE/Terminal/console.html.gz"

cp "$ROOT/dist/ReadMe" "$TREE/ReadMe"
cp "$INSTALL/Document.info" "$TREE/ReadMe.info"
cp "$INSTALL/Drawer.info"   "$TREE/Docs.info"
cp "$INSTALL/Drawer.info"   "$TREE/Examples.info"
cp "$INSTALL/Drawer.info"   "$TREE/Terminal.info"

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
# The one build, like the commands and for the same reason: the sampling
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
