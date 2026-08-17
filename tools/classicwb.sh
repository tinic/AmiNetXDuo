#!/usr/bin/env bash
#
# A full Workbench on the LAN, in one command, with AmiNetXDuo INSTALLED on it.
#
#   tools/classicwb.sh [-m MODEL] [-v VARIANT] [-b BUILDDIR] [-a ARCHIVE.lha]
#                      [-B BACKEND] [-n NAME] [-p PORT] [-t SECONDS]
#                      [-s SNAPSHOTS] [-c HOST] [-M SPEC]
#
# tools/demo.sh boots the drive tools/amiberry-run.sh builds, which is httpd
# and whatever else the staging puts beside it.  This boots a ClassicWB
# snapshot instead: a Workbench somebody could actually use.
#
#   http://<address>/           the Public drawer, WebDAV-writable
#   http://<address>/shell      an AmigaDOS Shell in a browser, no password
#   http://<address>/console    the Workbench screen
#
# THE INSTALLER PUTS OUR FILES THERE, not this script.  A launch is: copy the
# snapshot, unpack a release archive built from -b onto the copy the way a
# download would arrive, boot once and drive Commodore's Installer over
# Install-AmiNetXDuo, check what landed, then power cycle into the machine.
# install/test/run-workbench.sh has done exactly this against a stock
# Workbench 3.1 for months; the mechanism here is that one, and
# install/test/installdrive.c is the same program clicking Proceed.
#
# It used to be a hand-written list of `cp` lines, and a hand-written list
# drifts from install/Install-AmiNetXDuo, which is the only definition of a
# complete install there is.  It had drifted three ways at once: anxnet.device
# was built and never staged, so the rig tested Commodore's driver and never
# ours; usergroup.library was built and never staged; and ssh was looked for in
# three directories that a CMake build never writes, found nowhere, and skipped
# without a word.  Nothing failed.  A rig that cannot notice its own incomplete
# install is worse than one that installs nothing, so the install is now real
# and the check that it was complete is what decides the exit status.
#
# WHAT THIS SCRIPT STILL PUTS THERE, after the Installer has finished, and why
# each one is a step a user takes rather than a step the Installer skipped:
#
#   Devs/Networks/anxnet.device  Install-AmiNetXDuo deliberately does not
#                                install the driver -- see its note -- so a
#                                user copies it across and names it with a
#                                CARD= line.  This is that copy, through
#                                tools/sana2-stage.sh, which is the one place
#                                that maps a board to a driver.
#   MDNS=YES in the interface    one line in the file the Installer wrote, and
#                                its own help text says so.  Without it the
#                                .local name this script prints answers
#                                nothing.
#   hostname <name>              the same, in DEVS:Internet/name_resolution.
#                                The Installer writes "amiga" for everybody and
#                                five of these on one wire need five names.
#   the httpd line               in S:Startup-Sequence, not in the Installer's
#                                block: this rig serves a chosen drawer on a
#                                chosen port with the console page as well, and
#                                the Installer's own httpd question offers RAM:
#                                on port 80 with no console.  The Installer's
#                                block is left holding AddNetInterface alone,
#                                which is the line the network comes up from.
#
# The snapshot is built once by ~/amiga-assets/classicwb/install.sh and carries
# no file of ours.  That is asserted on every launch and is what makes the
# install honest: nothing of ours can survive in the snapshot to stand in for
# a file the install failed to write.
#
# The run copy is a copy.  A guest that corrupts its drive costs the copy and
# not the install.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

MODEL="${AMINETXDUO_CWB_MODEL:-A1200}"
VARIANT="${AMINETXDUO_CWB_VARIANT:-plain}"
BUILD="${AMINETXDUO_BUILD:-$ROOT/build/cm}"
ARCHIVE="${AMINETXDUO_CWB_ARCHIVE:-}"
BACKEND="${AMINETXDUO_DEMO_BACKEND:-ens18}"
NAME=""
PORT=80
WINDOW=28800
SNAPROOT="${AMINETXDUO_CWB_SNAPSHOTS:-$HOME/amiga-assets/classicwb/snapshots}"
P96DIR="${AMINETXDUO_P96_DIR:-$HOME/amiga-assets/p96}"
CHECKHOST="${AMINETXDUO_CWB_CHECKHOST:-}"

# The Installer needs a user level where the questions are drawn.  At NOVICE it
# draws none and aborts on a machine with no network driver in DEVS: -- which a
# ClassicWB snapshot is -- so the run would die before the first copy.
LEVEL="${AMINETXDUO_CWB_LEVEL:-AVERAGE}"

# How long the install boot gets.  It is a whole ClassicWB boot, then twelve
# Installer pages, then about a megabyte and a half of copying on an emulated
# 68020.  Measured at a little over two minutes; this is four times that, and
# it is a ceiling rather than a wait.
INSTALL_TIMEOUT="${AMINETXDUO_CWB_INSTALL_TIMEOUT:-600}"

usage() {
    cat <<'EOF'
usage: tools/classicwb.sh [-m A600|A1200|A3000] [-v plain|rtg] [-b builddir]
                          [-a archive.lha] [-B backend] [-n name] [-p port]
                          [-t seconds] [-s snapshots] [-c host] [-M spec]

  -m  model, picks the Kickstart          (default A1200)
  -v  plain boots ClassicWB, rtg adds a Picasso96 screen  (default plain)
  -b  build directory the archive is built from  (default build/cm)
  -a  install this archive instead of building one from -b
  -B  bridge interface                             (default ens18)
  -n  hostname and mDNS name          (default amiga-<model>-<variant>)
  -p  httpd port                                      (default 80)
  -t  seconds before the guest is stopped          (default 28800)
  -s  snapshot store         (default ~/amiga-assets/classicwb/snapshots)
  -c  ssh host that checks the served version; this host cannot reach
      its own guest, so leaving it unset skips that one check
  -M  run the TLS memory probes at boot, against a peer already serving.
      SPEC is host,port,address,truststore,cafile: the name the certificate
      carries, the port, the peer's dotted address, and the two host-side
      files tests/peer/mkpki.sh writes (teststore and testroots.pem)
EOF
}

say() { printf '%s=%s\n' "$1" "$2"; }

case "${1:-}" in -h|--help) usage; exit 0 ;; esac

MEMPROBE="${AMINETXDUO_CWB_MEMPROBE:-}"

while getopts "m:v:b:a:B:n:p:t:s:c:M:h" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        v) VARIANT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        a) ARCHIVE="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        n) NAME="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        t) WINDOW="$OPTARG" ;;
        s) SNAPROOT="$OPTARG" ;;
        c) CHECKHOST="$OPTARG" ;;
        M) MEMPROBE="$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

case "$BUILD"   in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
case "$ARCHIVE" in ""|/*) ;; *) ARCHIVE="$PWD/$ARCHIVE" ;; esac

MODEL=$(printf '%s' "$MODEL" | tr '[:lower:]' '[:upper:]')
VARIANT=$(printf '%s' "$VARIANT" | tr '[:upper:]' '[:lower:]')
case "$VARIANT" in
    rtg|p96) VARIANT=rtg ;;
    plain|nortg|full) VARIANT=plain ;;
    *) say error "unknown variant $VARIANT, want plain or rtg"; exit 2 ;;
esac

# Which ClassicWB a model gets.  FULL is a 68020 with 6 MB and an A600 is
# neither, so the 68000 arm takes the 68K edition; it is the distribution
# ClassicWB publishes for that machine, not a cut-down FULL.  P96 needs a
# graphics card and a 68020, so the A600 has no rtg row at all.
case "$MODEL:$VARIANT" in
    A600:plain)  DIST=68k ;;
    A600:rtg)    say error "no rtg on an A600: Picasso96 and ClassicWB P96 both\
 want a 68020, and a 68000 has none"; exit 2 ;;
    A1200:plain|A3000:plain) DIST=full ;;
    A1200:rtg|A3000:rtg)     DIST=p96 ;;
    *) say error "unknown model $MODEL, want A600, A1200 or A3000"; exit 2 ;;
esac

[ -n "$NAME" ] || NAME="amiga-$(printf '%s' "$MODEL" | tr '[:upper:]' '[:lower:]')-$VARIANT"

# -M is read here and used at the very end, three minutes of installing later.
# A typo in it that was only noticed then would cost the whole install.
MP_HOST=""; MP_PORT=""; MP_ADDR=""; MP_STORE=""; MP_CA=""
if [ -n "$MEMPROBE" ]; then
    IFS=, read -r MP_HOST MP_PORT MP_ADDR MP_STORE MP_CA <<< "$MEMPROBE"
    [ -n "$MP_HOST" ] && [ -n "$MP_PORT" ] && [ -n "$MP_ADDR" ] &&
    [ -f "${MP_STORE:-/nonexistent}" ] || {
        say error "-M wants host,port,address,truststore,cafile and a\
 readable truststore"; exit 2; }
fi

# The asset store carries the ROMs and exports the Kickstart each model needs.
# Skipping it boots a machine with no ROM, and the error names the ROM rather
# than the missing variable.
[ -f "$HOME/amiga-assets/env.sh" ] && . "$HOME/amiga-assets/env.sh"

case "$MODEL" in
    A600)  KICKSTART="${AMINETXDUO_KICKSTART_A600:-}" ;;
    A3000) KICKSTART="${AMINETXDUO_KICKSTART_A3000:-}" ;;
    *)     KICKSTART="${AMINETXDUO_KICKSTART:-}" ;;
esac
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    say error "no Kickstart for $MODEL in ~/amiga-assets/env.sh"; exit 2; }

SNAP="$SNAPROOT/$DIST/tree"
MANIFEST="$SNAPROOT/$DIST/manifest.json"
[ -d "$SNAP" ] || {
    say error "no snapshot at $SNAP -- build it with\
 ~/amiga-assets/classicwb/install.sh $DIST"; exit 2; }

AMIBERRY="${AMIBERRY:-$(command -v amiberry || true)}"
[ -n "$AMIBERRY" ] && [ -x "$AMIBERRY" ] || { say error "no amiberry"; exit 2; }

# installdrive is compiled here and runs in the guest, so it needs the cross
# compiler.  tools/amiga-toolchain.sh is the one resolver; a script with its
# own copy of the search order is how a machine ends up building under a
# compiler nobody chose.
AMIGA_TOOLCHAIN_QUIET=1 . "$ROOT/tools/amiga-toolchain.sh"
[ -x "${AMIGA_GCC:-}" ] || {
    say error "no m68k-amigaos-gcc: installdrive.c cannot be built"; exit 2; }

# The board -> driver -> CARD= table, shared with every card sweep.  Sourced
# rather than copied so this rig cannot disagree with them about what drives
# an A2065.
. "$ROOT/tools/sana2-stage.sh"

for need in lha sha256sum tcpdump; do
    command -v "$need" >/dev/null 2>&1 || { say error "no $need"; exit 2; }
done

TAG="${AMINETXDUO_RUN_TAG:-cwb-$(printf '%s' "$MODEL" | tr '[:upper:]' '[:lower:]')-$VARIANT}"

say model "$MODEL"
say variant "$VARIANT"
say classicwb "$DIST"
say kickstart "$(basename "$KICKSTART")"
say snapshot "$SNAP"
[ -f "$MANIFEST" ] && say manifest "$MANIFEST"

# ---------------------------------------------------------------- the archive --
#
# THE ARTIFACT, and not a directory that happens to hold the same files.  What
# a user has is an archive they downloaded, so that is what is installed here,
# and -a takes one as given -- which is the way to boot a real release.
#
# dbclient is built by clients/dropbear/build.sh and not by the CMake tree, so
# a plain `cmake --build` leaves it out and the archive would carry no ssh.
# It used to be looked for in three directories and skipped in silence when it
# was in none of them.  It is built here instead, once, and cached: the build
# is four minutes the first time and nothing on every launch after it.
SSHBIN="${AMINETXDUO_SSH:-}"
BUILT_HERE=0
if [ -z "$ARCHIVE" ]; then
    BUILT_HERE=1
    if [ -z "$SSHBIN" ]; then
        SSHBIN="$BUILD-dropbear/dbclient"
        if [ ! -f "$SSHBIN" ]; then
            say ssh_build "$BUILD-dropbear"
            AMINETXDUO_CLIENT_ANY=1 "$ROOT/clients/dropbear/build.sh" \
                -b "$BUILD-dropbear" >"$ROOT/build/cwb-ssh-$TAG.log" 2>&1 || {
                say error "clients/dropbear/build.sh failed, see\
 $ROOT/build/cwb-ssh-$TAG.log"
                exit 2; }
        fi
    fi
    [ -f "$SSHBIN" ] || {
        say error "no ssh at $SSHBIN: the archive would carry none"; exit 2; }

    # AMINETXDUO_DIST_NO_MINIMAL, because the minimal stack is a second build
    # tree and this rig has one.  The archive is otherwise the release layout.
    DISTOUT="$ROOT/build/cwb-dist-$TAG"
    rm -rf "$DISTOUT"
    mkdir -p "$DISTOUT"
    AMINETXDUO_DIST_NO_MINIMAL=1 AMINETXDUO_SSH="$SSHBIN" \
        "$ROOT/dist/make-dist.sh" -b "$BUILD" -o "$DISTOUT" \
        >"$ROOT/build/cwb-dist-$TAG.log" 2>&1 || {
        say error "dist/make-dist.sh failed, see $ROOT/build/cwb-dist-$TAG.log"
        tail -20 "$ROOT/build/cwb-dist-$TAG.log" >&2
        exit 2; }
    ARCHIVE=$(ls -1 "$DISTOUT"/AmiNetXDuo-*.lha 2>/dev/null | head -1)
fi
[ -n "$ARCHIVE" ] && [ -f "$ARCHIVE" ] || {
    say error "no archive to install"; exit 2; }
say archive "$ARCHIVE"
say archive_bytes "$(wc -c < "$ARCHIVE" | tr -d ' ')"

# ------------------------------------------------------------- the drive ----

# A copy, every time.  The snapshot is the install and is never written to, so
# a run that wedges its drive is recovered by deleting the copy.
HD="$ROOT/build/classicwb-$TAG-dh0"
rm -rf "$HD"
mkdir -p "$HD"
cp -a "$SNAP/." "$HD/"

# Nothing of ours may already be here, and this is the assertion the whole
# install rests on: a file that survived in the snapshot would stand in for one
# the Installer failed to write, and every check below would pass.
for stale in C/httpd C/ssh C/AddNetInterface Libs/bsdsocket.library \
             Libs/usergroup.library Libs/tls.library \
             Devs/Networks/anxnet.device Devs/Networks/a2065.device \
             Devs/NetInterfaces Devs/Internet AmiNetXDuo; do
    [ -e "$HD/$stale" ] && {
        say error "the snapshot carries $stale -- it must carry none of ours"
        exit 2; }
done
if [ -f "$HD/S/User-Startup" ] &&
   grep -qi 'AddNetInterface\|BEGIN AmiNetXDuo' "$HD/S/User-Startup"; then
    say error "the snapshot's S/User-Startup already names AmiNetXDuo"
    exit 2
fi

# AmigaDOS does not care about case and this host does, so a file the guest
# wrote as `libs/bsdsocket.library` is not `Libs/bsdsocket.library` to [ -f ].
# Every name checked below is resolved the way the guest would, one component
# at a time.  install/test/run-workbench.sh has the same helper and for the
# same reason: without it an installed file reads as a missing one.
amiga_path() {
    local cur="$HD" part next
    local -a parts
    IFS=/ read -ra parts <<< "$1"
    for part in "${parts[@]}"; do
        next=$(ls -1 "$cur" 2>/dev/null |
               awk -v p="$part" 'tolower($0) == tolower(p) { print; exit }')
        [ -n "$next" ] || return 1
        cur="$cur/$next"
    done
    printf '%s\n' "$cur"
}

# ------------------------------------------------------------------ AmiSSL --
#
# WHAT THE SNAPSHOT BROUGHT, as opposed to what the archive brings below.
#
# AmiSSL is third party and does not change when AmiNetXDuo is rebuilt, so it
# rides in the snapshot beside ClassicWB rather than being unpacked on every
# launch, and the rule asserted above -- that the snapshot carries no file of
# ours -- is about ours and is untouched by it.
#
# AmiSSL 5.27 wants AmigaOS 3.0+ and a 68020.  The A600 is a 68000, so the 68k
# snapshot has none and this says so with the reason attached: a caller that
# reads `amissl=absent` and nothing else has to go and find out why, and the
# answer is not a thing that will change.
#
# A launch that expected AmiSSL and did not find it stops here, the same way
# install_check and driver_match stop one.  A guest that came up without it
# would serve every page it serves now and fail the first HTTPS request, which
# is the shape of failure this rig exists to catch before a user does.
case "$DIST" in
    68k) AMISSL_WANT=no
         AMISSL_WHY="AmiSSL 5.27 needs a 68020 and the $MODEL is a 68000" ;;
    *)   AMISSL_WANT=yes
         AMISSL_WHY="" ;;
esac
say amissl_expected "$AMISSL_WANT"

# The version out of the binary's own VERSION tag.  A filename is what
# somebody typed; this is what the library will tell the guest it is.
amissl_verstag() {
    LC_ALL=C tr -c '[:print:]' '\n' < "$1" | grep -m1 '^\$VER: ' || true
}

AMISSL_MASTER=$(amiga_path Libs/amisslmaster.library || true)
AMISSL_LIBDIR=$(amiga_path Libs/AmiSSL || true)
AMISSL_LIB=""
[ -n "$AMISSL_LIBDIR" ] &&
    AMISSL_LIB=$(ls -1 "$AMISSL_LIBDIR"/amissl_v*.library 2>/dev/null | head -1)

if [ -n "$AMISSL_MASTER" ] && [ -n "$AMISSL_LIB" ]; then
    AMISSL_CERTS=$(amiga_path AmiSSL/Certs || true)
    say amissl "$(amissl_verstag "$AMISSL_LIB" | awk '{print $3}')"
    say amissl_library "$(basename "$AMISSL_LIB")"
    say amissl_library_bytes "$(wc -c < "$AMISSL_LIB" | tr -d ' ')"
    say amissl_master "$(amissl_verstag "$AMISSL_MASTER" | awk '{print $3}')"
    say amissl_certs \
        "$(ls -1 "${AMISSL_CERTS:-/nonexistent}" 2>/dev/null | wc -l | tr -d ' ')"
    if [ "$AMISSL_WANT" = no ]; then
        say error "the $DIST snapshot carries AmiSSL and $AMISSL_WHY -- a\
 library this machine cannot open is worse than none"
        exit 1
    fi
    say amissl_check ok
else
    say amissl absent
    if [ "$AMISSL_WANT" = yes ]; then
        say amissl_reason "the $DIST snapshot carries none"
        say error "$MODEL expects AmiSSL and the snapshot has no\
 Libs/AmiSSL/amissl_v#?.library -- rebuild it with\
 ~/amiga-assets/classicwb/install.sh $DIST"
        exit 1
    fi
    say amissl_reason "$AMISSL_WHY"
fi

# ------------------------------------------------------- the AmiSSL probe --
#
# install/test/amisslprobe.c, staged into C: so the standing guest can be
# asked whether AmiSSL WORKS rather than whether its file is on the drive.
# Everything above is this host reading bytes it copied itself; the probe
# opens the library on the Amiga, initialises it, and gets the version string
# back out of it by calling into it.
#
# It is not run here.  The open was expected to be minutes and measured 0.34 s
# on the A1200 arm -- AmigaDOS reads the 3.5 MB through the emulator's
# directory filesystem, which is a host memcpy, and only the relocation is
# paid at 68020 speed -- so the reason is no longer cost.  It is that the
# probe needs a stack a Shell does not give, so it cannot simply be appended
# to a boot script, and the answer does not change between launches:
#
#     Stack 65536
#     C:amisslprobe
#
# from the web shell or the guest's own Shell, then read DH0:amisslprobe.txt.
#
# The AmiSSL SDK supplies the headers.  It is not in the public tree and never
# will be, so this is skipped rather than fatal when the asset store is not
# on the machine: the probe is a diagnostic and a launch without one is still
# a launch.
AMISSL_SDK="${AMINETXDUO_AMISSL_SDK:-}"
if [ "$AMISSL_WANT" = yes ] && [ -z "$AMISSL_SDK" ]; then
    SDK_LHA=$(ls -1 "$HOME"/amiga-assets/amissl/archives/AmiSSL-*-SDK.lha \
              2>/dev/null | head -1)
    SDK_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/amissl-sdk"
    if [ -n "$SDK_LHA" ]; then
        if [ ! -d "$SDK_DIR/AmiSSL/Developer" ]; then
            rm -rf "$SDK_DIR"
            mkdir -p "$SDK_DIR"
            ( cd "$SDK_DIR" && lha xq "$SDK_LHA" ) >/dev/null 2>&1 || true
        fi
        [ -d "$SDK_DIR/AmiSSL/Developer" ] &&
            AMISSL_SDK="$SDK_DIR/AmiSSL/Developer"
    fi
fi
if [ "$AMISSL_WANT" = yes ] && [ -n "$AMISSL_SDK" ] &&
   [ -f "$AMISSL_SDK/include/proto/amissl.h" ]; then
    # -m68000 for the same reason installdrive.c is: nothing in it needs
    # 68020 codegen.  The library it opens does, and the machine it is staged
    # on has one, but that is the library's business and not this file's.
    PROBE="$ROOT/build/cwb-amisslprobe-$TAG"
    if "$AMIGA_GCC" -O2 -m68000 -Wall -Wextra -I"$AMIGA_NDK" \
        -I"$AMISSL_SDK/include" -D__amigaos3__=1 \
        -o "$PROBE" "$ROOT/install/test/amisslprobe.c" \
        > "$ROOT/build/cwb-amisslprobe-$TAG.log" 2>&1; then
        cp "$PROBE" "$HD/C/amisslprobe"
        chmod 755 "$HD/C/amisslprobe"
        say amissl_probe "C:amisslprobe"
        say amissl_probe_bytes "$(wc -c < "$PROBE" | tr -d ' ')"
    else
        say warning "install/test/amisslprobe.c did not build; see\
 $ROOT/build/cwb-amisslprobe-$TAG.log"
    fi
elif [ "$AMISSL_WANT" = yes ]; then
    say amissl_probe "absent: no AmiSSL SDK"
fi

# ---------------------------------------------------------- the TLS probe --
#
# install/test/tlsprobe.c, the same thing for tls.library, staged the same way
# into the same place.  It needs no third-party SDK -- our own header and the
# NDK are the whole of it -- so unlike the one above it is not conditional on
# an asset store, and it is staged on the A600 as well, which is the machine
# AmiSSL cannot serve at all.
#
# -m68000, for the reason installdrive.c is: nothing in the probe needs 68020
# codegen and it has to run on whatever MODEL says.
TLSPROBE="$ROOT/build/cwb-tlsprobe-$TAG"
if "$AMIGA_GCC" -O2 -m68000 -Wall -Wextra -I"$AMIGA_NDK" -I"$ROOT/include" \
    -o "$TLSPROBE" "$ROOT/install/test/tlsprobe.c" \
    > "$ROOT/build/cwb-tlsprobe-$TAG.log" 2>&1; then
    cp "$TLSPROBE" "$HD/C/tlsprobe"
    chmod 755 "$HD/C/tlsprobe"
    say tls_probe "C:tlsprobe"
    say tls_probe_bytes "$(wc -c < "$TLSPROBE" | tr -d ' ')"
else
    say warning "install/test/tlsprobe.c did not build; see\
 $ROOT/build/cwb-tlsprobe-$TAG.log"
fi

# The download, where a download would be: its own drawer, and not the one the
# Installer is going to create.  install/test/installdrive.c spells this path.
UNP="$HD/Unpacked/AmiNetXDuo"
mkdir -p "$HD/Unpacked"
( cd "$HD/Unpacked" && lha -xfq "$ARCHIVE" ) >/dev/null 2>&1 ||
( cd "$HD/Unpacked" && lha xf "$ARCHIVE" ) >/dev/null 2>&1 || {
    say error "could not unpack $ARCHIVE"; exit 2; }
[ -d "$UNP" ] || {
    say error "$ARCHIVE did not unpack to an AmiNetXDuo drawer"; exit 2; }
chmod -R a+rx "$HD/Unpacked"

# The version the archive says it is, read out of the Installer script the
# archive carries.  dist/make-dist.sh stamps that line from tools/version.sh
# and fails the build when it cannot, so it is the one label that is true of a
# downloaded release as well as of one built here a minute ago.
ARCH_VER=$(sed -n 's/^(set VERSION_STR "\(.*\)")$/\1/p' \
           "$UNP/Install-AmiNetXDuo" 2>/dev/null | head -1)
[ -n "$ARCH_VER" ] || {
    say error "$ARCHIVE has no stamped VERSION_STR in Install-AmiNetXDuo"
    exit 2; }
say archive_version "$ARCH_VER"

# WHAT THE ARCHIVE HOLDS, said out loud before anything is installed, because
# a gap here is the failure this rewrite exists to end.  All four were built
# and none of them reached the guest: bsdsocket.library was the only library
# staged, usergroup.library was not, anxnet.device was not, and ssh was looked
# for in directories a CMake build never writes.  A launch that cannot install
# them stops here rather than coming up looking correct.
ARCH_GAP=0
for f in Libs/bsdsocket.library Libs/usergroup.library C/ssh \
         Devs/Networks/anxnet.device; do
    if [ -f "$UNP/$f" ]; then
        say "archive_$(basename "$f" | tr . _)" "$(wc -c < "$UNP/$f" | tr -d ' ')"
    else
        say archive_absent "$f"
        ARCH_GAP=$((ARCH_GAP + 1))
    fi
done
[ -f "$UNP/Libs/tls.library" ] && say archive_tls_library \
    "$(wc -c < "$UNP/Libs/tls.library" | tr -d ' ')"
[ "$ARCH_GAP" = "0" ] || {
    say error "$ARCHIVE is missing $ARCH_GAP file(s) the machine needs"
    exit 2; }

# The Commodore Installer rides in the archive (dist/make-dist.sh puts it
# there so a double-click works), so there is nothing to find on this host.
[ -f "$UNP/Installer" ] || {
    say error "$ARCHIVE carries no Installer"; exit 2; }

# -m68000: this runs INSIDE the guest and the guest is whatever MODEL says.
# Nothing in it needs 68020 codegen, and built for one it takes an illegal
# instruction on the A600 before the Installer ever starts -- which presents as
# a boot that never finishes and leaves an empty drive, and an empty drive
# passes every "this file is absent" assertion there is.
DRIVER="$ROOT/build/cwb-installdrive-$TAG"
"$AMIGA_GCC" -O2 -m68000 -Wall -Wextra -DDRIVE_LEVEL="\"$LEVEL\"" \
    -DDRIVE_RUNS=1 -DDRIVE_YES_LABEL="\"\"" -I"$AMIGA_NDK" \
    -o "$DRIVER" "$ROOT/install/test/installdrive.c" || {
    say error "could not build install/test/installdrive.c"; exit 2; }
cp "$DRIVER" "$HD/C/installdrive"
chmod 755 "$HD/C/installdrive"

# ------------------------------------------------------------ the emulator --

export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"

# The emulator log is capped.  It used to be off here, because --log is
# unbounded -- 87 MB per 30 s on an RTG guest, and this guest stands for eight
# hours -- and off is how a wedge came to leave 94 bytes to read.  Through
# tools/logcap.sh it is bounded whatever happens: the head holds the boot,
# where the board and MAC lines and the first illegal instruction are printed,
# a run of identical lines collapses to one and a count, and the last lines are
# kept in a ring and written out when the emulator stops.  tools/demo.sh sizes
# a standing guest's log this way and this is that size.
export AMINETXDUO_LOG_HEAD="${AMINETXDUO_LOG_HEAD:-1048576}"
export AMINETXDUO_LOG_TAIL="${AMINETXDUO_LOG_TAIL:-200}"

EMU_PID=""
LOGCAP_PID=""
LOGPIPE=""

# Start amiberry on $1 with its output capped into $2.  THROUGH A FIFO rather
# than a pipeline, because $! after `a | b` is b and EMU_PID has to be the
# emulator: it is what gets killed.  Degrades to the plain redirect when the
# capper is missing, since opening a FIFO to write blocks until a reader
# appears and a missing one would hang the launch rather than lose a log.
start_emulator() {
    local cfg="$1" log="$2"

    LOGPIPE="$log.pipe"
    rm -f "$LOGPIPE"
    if [ -x "$ROOT/tools/logcap.sh" ] && mkfifo "$LOGPIPE" 2>/dev/null; then
        "$ROOT/tools/logcap.sh" < "$LOGPIPE" > "$log" &
        LOGCAP_PID=$!
    else
        say warning "no tools/logcap.sh; $log is UNCAPPED"
        rm -f "$LOGPIPE"
        LOGPIPE=""
    fi
    if [ -n "$LOGPIPE" ]; then
        ( trap '' PIPE; exec setsid "$AMIBERRY" --log -f "$cfg" ) \
            >"$LOGPIPE" 2>&1 &
    else
        ( trap '' PIPE; exec setsid "$AMIBERRY" --log -f "$cfg" ) \
            >"$log" 2>&1 &
    fi
    EMU_PID=$!
}

# The capper still holds the tail of this boot in its ring, so it has to be
# finished with before the next boot takes the same names.
stop_emulator() {
    [ -n "$EMU_PID" ] || return 0
    kill -TERM "$EMU_PID" 2>/dev/null || true
    sleep 2
    kill -KILL "$EMU_PID" 2>/dev/null || true
    wait "$EMU_PID" 2>/dev/null || true
    EMU_PID=""
    if [ -n "$LOGCAP_PID" ]; then
        wait "$LOGCAP_PID" 2>/dev/null || true
        LOGCAP_PID=""
    fi
    [ -z "$LOGPIPE" ] || { rm -f "$LOGPIPE"; LOGPIPE=""; }
}

# ----------------------------------------------------------- 1/2, the install --
#
# ClassicWB's own Startup-Sequence, with its final EndCLI taken off so a tail
# can replace it.  Anchored at column 0, which is the whole of why that is
# safe: ClassicWB's sequence has two more EndCLI lines indented inside the
# boot-menu branches, and those end the shell on purpose.
#
# LoadWB stays.  The Installer draws on the default public screen, and
# installdrive opens Workbench itself, but a machine that already has one is
# the machine a user is sitting at.
startup_with() {
    [ -f "$SNAP/S/Startup-Sequence" ] || {
        say error "the snapshot has no S/Startup-Sequence"; exit 2; }
    sed -e '/^EndCLI/d' "$SNAP/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
    printf '\n%s\n' "$1" >> "$HD/S/Startup-Sequence"
    chmod 755 "$HD/S/Startup-Sequence"
}

# No network on this boot.  The install needs none, a second MAC on the wire is
# a second machine to everything watching it, and the sniffer below would have
# two candidates for the same address.
INSTALL_CFG="$ROOT/build/$TAG-install.uae"
cat > "$INSTALL_CFG" <<EOF
config_description=AmiNetXDuo ClassicWB $DIST install on $MODEL
use_gui=no
headless=true
quickstart=$MODEL,0
kickstart_rom_file=$KICKSTART
fastmem_size=8
floppy0type=-1
nr_floppies=0
uaehf0=dir,rw,DH0:System:$HD,0
EOF

startup_with 'FailAt 9999
C:installdrive >DH0:install-console.txt
Echo >DH0:.done "$RC"'

rm -f "$HD/.done"
SDL_VIDEODRIVER=dummy
export SDL_VIDEODRIVER
unset DISPLAY WAYLAND_DISPLAY 2>/dev/null || true

say installer_level "$LEVEL"
# Only for this boot: a launch that dies in the wait below must not leave an
# emulator holding the drive.  The standing guest below wants no such trap.
trap 'stop_emulator' EXIT INT TERM
start_emulator "$INSTALL_CFG" "$ROOT/build/amiberry-$TAG-install.log"

INSTALL_RC=124
INSTALL_SECONDS=0
while [ "$INSTALL_SECONDS" -lt "$INSTALL_TIMEOUT" ]; do
    if [ -f "$HD/.done" ]; then
        INSTALL_RC=$(tr -dc '0-9' < "$HD/.done" | head -c 4)
        INSTALL_RC=${INSTALL_RC:-0}
        break
    fi
    kill -0 "$EMU_PID" 2>/dev/null || {
        say error "the emulator exited during the install"
        break; }
    sleep 2
    INSTALL_SECONDS=$((INSTALL_SECONDS + 2))
done
stop_emulator
trap - EXIT INT TERM

say install_seconds "$INSTALL_SECONDS"
say install_status "$INSTALL_RC"
if [ "$INSTALL_RC" != "0" ]; then
    say error "the Installer did not finish: status $INSTALL_RC"
    say installdrive "$HD/installdrive.txt"
    say installer_out "$HD/installer-out.txt"
    say drive "$HD"
    exit 1
fi

# ------------------------------------------- what the Installer actually did --
#
# WHAT THE INSTALLER WOULD INSTALL, derived from the archive rather than
# written down here, so this cannot drift from install/Install-AmiNetXDuo the
# way the `cp` list it replaces did.  Every file is compared by content: a name
# that exists proves the copy started and nothing else.
#
# The one thing kept in step by hand is which archive drawer goes where, and
# the Installer's own copy statements are the list:
#
#   Libs/*        -> LIBS:            copylib, tls.library only when packed
#   C/*           -> C:               copyfiles (all)
#   Docs/*        -> <docdir>         copyfiles (all) (infos) -- the drawer's
#                                     CONTENTS, and its (dest) is DOCDIR
#                                     itself, so the guide lands beside
#                                     Examples and not under a Docs drawer
#   Examples/*    -> <docdir>/Examples
#   Terminal/*    -> <docdir>/Terminal
#   AmiNetXDuo.info -> beside <docdir>
#   Devs/Internet/{protocols,services,networks}  only where there is none
#   Devs/Internet/certificates                   always, when packed
#
# Devs/Networks/anxnet.device is NOT in this list, and that is the Installer's
# decision, not an omission: it ships and a user copies it deliberately.  This
# script does that copy below, and the driver gate proves it is the one the
# stack opened.
#
# <docdir> is DH0:AmiNetXDuo -- @default-dest on a machine with one volume --
# and the askdir page is answered with its default.
DOCDIR_REL="AmiNetXDuo"

MISSING=0
CHECKED=0
sum_of() { sha256sum "$1" | cut -d' ' -f1; }

# $1 source file in the archive, $2 destination path on the drive.
check_copy() {
    local src="$1" dest="$2" real
    CHECKED=$((CHECKED + 1))
    if ! real=$(amiga_path "$dest") || [ ! -f "$real" ]; then
        say install_missing_file "$dest"
        MISSING=$((MISSING + 1))
        return
    fi
    if [ "$(sum_of "$src")" != "$(sum_of "$real")" ]; then
        say install_differs "$dest"
        MISSING=$((MISSING + 1))
    fi
}

# $1 a path the Installer WRITES rather than copies: it has to be there and
# have something in it, and its content is checked where it matters below.
check_written() {
    local real
    CHECKED=$((CHECKED + 1))
    if ! real=$(amiga_path "$1") || [ ! -s "$real" ]; then
        say install_missing_file "$1"
        MISSING=$((MISSING + 1))
    fi
}

check_copy "$UNP/Libs/bsdsocket.library" Libs/bsdsocket.library
check_copy "$UNP/Libs/usergroup.library" Libs/usergroup.library
[ -f "$UNP/Libs/tls.library" ] &&
    check_copy "$UNP/Libs/tls.library" Libs/tls.library

# Every command in the archive, because copyfiles (all) copies every one of
# them.  ssh is in here, which is the whole reason the archive is built with a
# dbclient in it.
while IFS= read -r rel; do
    check_copy "$UNP/C/$rel" "C/$rel"
done < <(cd "$UNP/C" && find . -type f | sed 's|^\./||' | sort)

for pair in "Docs:" "Examples:/Examples" "Terminal:/Terminal"; do
    drawer="${pair%%:*}"
    under="${pair#*:}"
    [ -d "$UNP/$drawer" ] || continue
    while IFS= read -r rel; do
        check_copy "$UNP/$drawer/$rel" "$DOCDIR_REL$under/$rel"
    done < <(cd "$UNP/$drawer" && find . -type f | sed 's|^\./||' | sort)
done

check_copy "$UNP/AmiNetXDuo.info" "$DOCDIR_REL.info"

[ -f "$UNP/Devs/Internet/certificates" ] &&
    check_copy "$UNP/Devs/Internet/certificates" Devs/Internet/certificates
for ndb in protocols services networks; do
    [ -f "$UNP/Devs/Internet/$ndb" ] &&
        check_copy "$UNP/Devs/Internet/$ndb" "Devs/Internet/$ndb"
done

check_written Devs/NetInterfaces/eth0
check_written Devs/Internet/routes
check_written Devs/Internet/name_resolution
check_written Devs/Internet/hosts
check_written S/User-Startup

# The managed block, once.  (startup) replaces what is between its markers, so
# two lines here would mean it appended instead -- and none would mean the
# network does not come up at all, which is the failure that reads as a driver
# fault three checks later.
IFACE_LINES=0
USTART=$(amiga_path S/User-Startup || true)
[ -n "$USTART" ] && [ -f "$USTART" ] &&
    IFACE_LINES=$(grep -c 'AddNetInterface' "$USTART" || true)
say startup_addnetinterface_lines "${IFACE_LINES:-0}"
[ "${IFACE_LINES:-0}" = "1" ] || {
    say install_missing_file "S/User-Startup: AddNetInterface"
    MISSING=$((MISSING + 1)); }

say install_files "$CHECKED"
say install_missing "$MISSING"
if [ "$MISSING" != "0" ]; then
    say error "the install is incomplete: $MISSING of $CHECKED files are\
 missing or differ from the archive"
    say installdrive "$HD/installdrive.txt"
    say drive "$HD"
    exit 1
fi
say install_check ok

# The harness's own binary goes away before the machine is handed over: a user
# who logs into that Shell should find C: holding what the Installer put there
# and nothing else.
rm -f "$HD/C/installdrive"

# ------------------------------------------------------- the deliberate steps --
#
# Install-AmiNetXDuo does not install anxnet.device, on purpose, so this is the
# copy its note describes: into DEVS:Networks, named in DEVS:NetInterfaces/eth0
# with a CARD= line.  tools/sana2-stage.sh owns the board-to-driver table, so
# the rig and the card sweeps cannot disagree about what drives an A2065.
#
# The emulated board is the A2065, which is what the .uae below enables.
BOARD=a2065
sana2_select "$BOARD" "$BUILD"
[ "$SANA2_SEL_SOURCE" = anxnet ] || {
    say error "tools/sana2-stage.sh does not map $BOARD onto anxnet.device"
    exit 2; }

# From the ARCHIVE, not from the build directory.  The archive is what a user
# has, and taking it from anywhere else would leave the one file on this drive
# that no download could have produced.
ANXNET="$UNP/Devs/Networks/anxnet.device"
[ -f "$ANXNET" ] || {
    say error "$ARCHIVE carries no Devs/Networks/anxnet.device"; exit 2; }

DEVSDIR=$(amiga_path Devs)
[ -f "$DEVSDIR/NetInterfaces/eth0" ] || {
    say error "the Installer wrote no $DEVSDIR/NetInterfaces/eth0"; exit 1; }
AMINETXDUO_SANA2_DRIVER="$ANXNET" \
AMINETXDUO_SANA2_DRIVER_NAME=anxnet.device \
AMINETXDUO_SANA2_DIR=Networks \
AMINETXDUO_SANA2_DEVICE=DEVS:Networks/anxnet.device \
AMINETXDUO_SANA2_CARD="$SANA2_SEL_CARD" \
    sana2_stage "$BOARD" "$DEVSDIR"

IFACE=$(amiga_path Devs/NetInterfaces/eth0)

# MDNS= is one line in the file the Installer wrote, and its own help text says
# so.  The Installer defaults it off -- the responder costs real time on a slow
# machine -- and this rig publishes a .local name, so it goes on here.
grep -v '^MDNS=' "$IFACE" > "$IFACE.new"
echo "MDNS=YES" >> "$IFACE.new"
mv "$IFACE.new" "$IFACE"

# The name, likewise: the Installer writes "amiga" for every machine and five
# of these on one wire need five names.
RESOLV=$(amiga_path Devs/Internet/name_resolution)
grep -vi '^hostname ' "$RESOLV" > "$RESOLV.new"
echo "hostname $NAME" >> "$RESOLV.new"
mv "$RESOLV.new" "$RESOLV"

# ------------------------------------------------------------------ rtg ----

RTG_BOARD=uaegfx
RTG_MODE_ID=0x50031000
RTG_W=640
RTG_H=480
RTG_DEPTH=8

if [ "$VARIANT" = rtg ]; then
    # ClassicWB P96 ships Picasso96 but is installed for whatever card the
    # person installing it had.  The board here is uaegfx, and the monitor
    # file is named after the BOARD rather than after Picasso96 -- one
    # executable drives every card P96 supports and the name is how it knows
    # which .card to open.  A monitor called Picasso96 loads no card and
    # Workbench comes up on the chipset without saying anything.
    if [ ! -f "$HD/Devs/Monitors/$RTG_BOARD" ]; then
        for f in Libs/Picasso96API.library Libs/Picasso96/rtg.library \
                 Libs/Picasso96/uaegfx.card Devs/Monitors/Picasso96; do
            [ -f "$P96DIR/$f" ] || { say error "no $P96DIR/$f"; exit 2; }
        done
        mkdir -p "$HD/Devs/Monitors"
        cp -Rn "$P96DIR/Libs/." "$HD/Libs/"
        cp "$P96DIR/Devs/Monitors/Picasso96" "$HD/Devs/Monitors/$RTG_BOARD"
    fi

    # devs/monitors reads the board name out of the tooltypes of the icon
    # beside it, so a monitor with no icon loads no card.  ClassicWB P96
    # brings its own, already set to uaegfx with the settings file off, and
    # that one is left alone; this builds one only where there is none.
    if [ ! -f "$HD/Devs/Monitors/$RTG_BOARD.info" ]; then
    AMINETXDUO_ICON_BOARD="$RTG_BOARD" python3 - \
        "$HD/Devs/Monitors/$RTG_BOARD.info" <<'EOF'
import os, struct, sys

board = os.environ["AMINETXDUO_ICON_BOARD"]
tools = ["IgnoreMask=Yes", "BoardType=" + board]

W, H, D = 8, 8, 1
rowbytes = ((W + 15) // 16) * 2

gadget = struct.pack(">LhhhhHHHLLLLLHL", 0, 0, 0, W, H, 0x0004, 0, 1, 1,
                     0, 0, 0, 0, 0, 0)
assert len(gadget) == 44, len(gadget)

obj = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
    ">BBLLLLLLL", 3, 0, 0, 1, 0, 0, 0x80000000, 0x80000000, 4096)

img = struct.pack(">hhhhhLBBL", 0, 0, W, H, D, 1, 0x1, 0x0, 0)
planes = b"\x00" * (rowbytes * H)

tt = struct.pack(">L", len(tools) + 1)
for t in tools:
    b = t.encode("latin-1") + b"\0"
    tt += struct.pack(">L", len(b)) + b

with open(sys.argv[1], "wb") as fh:
    fh.write(obj + img + planes + tt)
EOF
    fi

    # ENVARC:Sys/screenmode.prefs, which IPrefs reads through the ENVARC:
    # assign ClassicWB makes to SYS:Prefs/Env-Archive.  Without it Workbench
    # opens on whatever mode the snapshot was installed with, which is a
    # chipset mode, and the card sits there unused.
    mkdir -p "$HD/Prefs/Env-Archive/Sys"
    AMINETXDUO_SMP_DEPTH="$RTG_DEPTH" AMINETXDUO_SMP_ID="$RTG_MODE_ID" \
    AMINETXDUO_SMP_W="$RTG_W" AMINETXDUO_SMP_H="$RTG_H" \
        python3 - "$HD/Prefs/Env-Archive/Sys/screenmode.prefs" <<'EOF'
import os, struct, sys

DISPLAY_ID = int(os.environ["AMINETXDUO_SMP_ID"], 0)
WIDTH = int(os.environ["AMINETXDUO_SMP_W"])
HEIGHT = int(os.environ["AMINETXDUO_SMP_H"])
depth = int(os.environ["AMINETXDUO_SMP_DEPTH"])

prhd = struct.pack(">BB4B", 0, 0, 0, 0, 0, 0)
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
fi

# --------------------------------------------------------- the served drawer --

mkdir -p "$HD/Public/Docs"
echo "Hello from an Amiga." > "$HD/Public/readme.txt"
echo "<html><body><h1>Amiga</h1><p>httpd is serving this drawer.</p></body></html>" \
    > "$HD/Public/index.html"
echo "in a drawer" > "$HD/Public/Docs/notes.txt"

# ClassicWB has the IEEE maths pair, but a snapshot that turned out not to is a
# Shell where ssh dies saying mathieeedoubbas.library failed to load, which
# reads like a broken binary.  -n so ClassicWB's own are left alone.
for m in mathieeedoubbas mathieeedoubtrans; do
    for src in "$HOME/amiga-assets/nglibs/$m.library" \
               "$HOME/amiga-assets/libs/$m.library"; do
        [ -f "$src" ] && { cp -n "$src" "$HD/Libs/"; break; }
    done
done

# ------------------------------------------------------- 2/2, the machine ----
#
# A POWER CYCLE, not a continuation.  The network comes up from the line the
# Installer wrote in S:User-Startup, reached by ClassicWB's own
# `Execute S:User-Startup`, with nothing in the path this script put there.
#
# The tail is the rig's: the version of the binary the guest LOADED, written
# where the host can read it, then httpd, then the stack's own account of which
# driver it has open.
#
# The terminal and console pages are the INSTALLED ones, in the drawer the
# Installer created.  Naming them here is a fourth assertion that the install
# was complete, made by the thing that has to open them.
#
# THE SERVER'S OWN OUTPUT, WITHOUT THE BOOT WINDOW.  It used to go to NIL:,
# which is why a wedged guest left nothing to read; and `Run >file` does not
# move it, because Run keeps that stream for its own "[CLI n]" line and the
# process it starts does not get it -- measured here, the file held those eight
# bytes and nothing else after a request had been served.
#
# So the redirection is made by a Shell that is already detached.  `Run >NIL:
# <NIL:` is unchanged and is what lets EndCLI take the boot window with it:
# nothing is ever handed the console.  What it starts is Execute on the script
# below, and the Shell running THAT script opens the file and hands it to httpd
# as its output.
#
# The file grows with requests and not with time, so an idle guest writes its
# own banner and stops.
cat > "$HD/S/AmiNetXDuo-httpd" <<EOF
; Written by tools/classicwb.sh.
C:httpd DH0:Public $PORT -T PAGE=DH0:$DOCDIR_REL/Terminal/shell.html -C CONSOLEPAGE=DH0:$DOCDIR_REL/Terminal/console.html >DH0:httpd-log.txt
EOF
chmod 755 "$HD/S/AmiNetXDuo-httpd"

cat > "$HD/S/AmiNetXDuo-Serve" <<'EOF'
; Written by tools/classicwb.sh.  Nothing here is installed by AmiNetXDuo.
FailAt 9999
C:Wait 6
C:Version >DH0:httpd-ver.txt C:httpd FILE
C:ShowNetStatus >DH0:netstatus.txt
Run >NIL: <NIL: C:Execute S:AmiNetXDuo-httpd
C:Wait 30
C:ShowNetStatus >>DH0:netstatus.txt
EOF
chmod 755 "$HD/S/AmiNetXDuo-Serve"

# --------------------------------------------------------- the memory probes --
#
# WHAT THE TWO TLS LIBRARIES COST RESIDENT, measured in one boot with the arms
# interleaved.  Between-run variation on this tree is several times the
# within-run variation, so tls, amissl, tls, amissl is one measurement and four
# separate launches would be four.
#
# The second round of each library is also the expunge measurement: exec
# expunges a library only when it needs the memory, so a second opener may pay
# nothing, and each probe's own resident_before_* lines say which happened.
#
# `Stack 65536` applies to the Shell running this script and therefore to the
# commands it starts.  Both probes read the stack they were given and refuse on
# one too small, so a missing line here is a reported refusal rather than a
# hang.
if [ -n "$MEMPROBE" ]; then
    cp "$MP_STORE" "$HD/probestore"
    say memprobe_store "$MP_STORE"
    if [ -n "${MP_CA:-}" ] && [ -f "$MP_CA" ]; then
        cp "$MP_CA" "$HD/probe-ca.pem"
        say memprobe_cafile "$MP_CA"
    fi

    {
        echo "; Written by tools/classicwb.sh."
        echo "FailAt 9999"
        echo "Stack 65536"
        for round in 1 2; do
            echo "C:tlsprobe $MP_HOST $MP_PORT $MP_ADDR STORE DH0:probestore\
 REPORT DH0:tlsprobe-r$round.txt >DH0:tlsprobe-r$round-console.txt"
            if [ "$AMISSL_WANT" = yes ] && [ -f "$HD/C/amisslprobe" ] &&
               [ -f "$HD/probe-ca.pem" ]; then
                echo "C:amisslprobe HOST $MP_HOST PORT $MP_PORT ADDR $MP_ADDR\
 CAFILE DH0:probe-ca.pem REPORT DH0:amisslprobe-r$round.txt\
 >DH0:amisslprobe-r$round-console.txt"
            fi
        done
        echo 'Echo >DH0:.memprobe "done"'
    } > "$HD/S/AmiNetXDuo-MemProbe"
    chmod 755 "$HD/S/AmiNetXDuo-MemProbe"

    printf 'Execute S:AmiNetXDuo-MemProbe\n' >> "$HD/S/AmiNetXDuo-Serve"
    rm -f "$HD/.memprobe"
    say memprobe "$MP_HOST:$MP_PORT at $MP_ADDR"
fi

startup_with 'FailAt 9999
Execute S:AmiNetXDuo-Serve
EndCLI >NIL:'

XVFB_PID=""
if [ "$VARIANT" = rtg ]; then
    # An RTG board publishes the host's display modes, so a headless host has
    # none to publish: both headless SDL drivers report no display modes, the
    # board comes up with an empty resolution list and Workbench stays on the
    # chipset.  Xvfb is where that data comes from, not a way around a bug.
    XDISP=""
    for n in $(seq 90 99); do
        [ -e "/tmp/.X11-unix/X$n" ] || { XDISP=":$n"; break; }
    done
    [ -n "$XDISP" ] || { say error "no free X display in :90..:99"; exit 2; }
    Xvfb "$XDISP" -screen 0 1280x1024x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 2
    export DISPLAY="$XDISP"
    export SDL_VIDEODRIVER="${AMINETXDUO_CWB_SDL_VIDEO:-x11}"
    say xvfb "$XDISP pid $XVFB_PID"
fi

# One MAC per combination, so two of these on one wire are two machines.  The
# demo already on the segment keeps 02:41:4d:49:00:77 and its own name.
case "$MODEL:$VARIANT" in
    A600:plain)  MACTAIL=61 ;;
    A1200:plain) MACTAIL=62 ;;
    A1200:rtg)   MACTAIL=63 ;;
    A3000:plain) MACTAIL=64 ;;
    A3000:rtg)   MACTAIL=65 ;;
esac
MAC="${AMINETXDUO_CWB_MAC:-02:41:4d:49:c0:$MACTAIL}"

CFG="$ROOT/build/$TAG.uae"
cat > "$CFG" <<EOF
config_description=AmiNetXDuo ClassicWB $DIST on $MODEL
use_gui=no
headless=true
quickstart=$MODEL,0
kickstart_rom_file=$KICKSTART
fastmem_size=8
floppy0type=-1
nr_floppies=0
uaehf0=dir,rw,DH0:System:$HD,0
a2065_rom_file=:ENABLED
a2065_rom_options=mac=$MAC,$BACKEND
EOF

# The device is DH0, which is what the httpd arguments above spell, and the
# volume is System, which is what ClassicWB's backdrop names.  Get the volume
# wrong and the drive still works and its icon is missing from Workbench.

if [ "$VARIANT" = rtg ]; then
    # uaegfx is Zorro III, and a quickstart model comes up 24-bit: without
    # this the emulator drops the board into a log nothing reads and Workbench
    # comes up planar.  rtg_modes 0x112 has RGBFF_CLUT in it, which is the one
    # format the console serves.
    cat >> "$CFG" <<'EOF'
cpu_24bit_addressing=no
gfxcard_size=8
rtg_modes=0x112
EOF
fi

EMULOG="$ROOT/build/amiberry-$TAG.log"

# The wire, from before the guest is on it.  A booted guest talks unprompted
# exactly twice, the gratuitous ARP for its lease and the mDNS announcement
# for its name, and both are over within a second, so a sniffer started once
# the guest is up catches them by luck only.
WIRE="$ROOT/build/classicwb-wire-$TAG.txt"
: > "$WIRE"
tcpdump -i "$BACKEND" -n -l -e \
    "arp or (udp port 67 or udp port 68) or (udp port 5353)" \
    >> "$WIRE" 2>/dev/null &
SNIFFER=$!
trap 'kill "$SNIFFER" 2>/dev/null || true' INT TERM
sleep 1

start_emulator "$CFG" "$EMULOG"
say emulator_pid "$EMU_PID"

# A guest that is left up needs an end, and the emulator is the thing that has
# to stop: nothing else here holds the drive open.
setsid bash -c "sleep $WINDOW; kill -TERM $EMU_PID 2>/dev/null; sleep 10; \
    kill -KILL $EMU_PID 2>/dev/null; \
    [ -n '$XVFB_PID' ] && kill -TERM $XVFB_PID 2>/dev/null" \
    >/dev/null 2>&1 &
say window_seconds "$WINDOW"

# The MAC on the wire is not the one asked for.  The a2065's LANCE keeps the
# low three octets of the configured address and puts Commodore's OUI in front
# of them, so 02:41:4d:49:c0:61 answers as 00:80:10:49:c0:61.
#
# Matching the OUI alone is not enough here.  A demo left up is another
# Commodore OUI on the same wire, and the first frame from one of those is
# picked up as this guest: the launch then reports somebody else's address and
# the version gate fails against a machine it never booted.
EXPECT_MAC="00:80:10:${MAC#*:*:*:}"
say expect_mac "$EXPECT_MAC"
GUESTMAC=""
for _ in $(seq 1 300); do
    sleep 1
    GUESTMAC=$(grep -oiE "^[0-9:.]+ $EXPECT_MAC > " "$WIRE" 2>/dev/null |
               head -1 | grep -oiE "$EXPECT_MAC" || true)
    [ -n "$GUESTMAC" ] && break
done
[ -n "$GUESTMAC" ] || {
    kill "$SNIFFER" 2>/dev/null || true
    say error "no a2065 was heard on $BACKEND in 300s"
    say emulog "$EMULOG"; say drive "$HD"; exit 1; }
say guest_mac "$GUESTMAC"

ADDR=""
for _ in $(seq 1 200); do
    ADDR=$(grep -iE "^[0-9:.]+ $GUESTMAC > " "$WIRE" 2>/dev/null | awk '
        /ethertype IPv4/ {
            for (i = 1; i <= NF; i++)
                if ($i ~ /^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$/) {
                    split($i, a, ".")
                    ip = a[1] "." a[2] "." a[3] "." a[4]
                    if (ip != "0.0.0.0") { print ip; exit }
                }
        }
        /Reply [0-9.]+ is-at/ {
            for (i = 1; i <= NF; i++)
                if ($i == "Reply") { print $(i + 1); exit }
        }
        /who-has [0-9.]+ tell/ {
            for (i = 1; i <= NF; i++)
                if ($i == "tell") {
                    ip = $(i + 1); sub(/,$/, "", ip)
                    if (ip != "0.0.0.0") { print ip; exit }
                }
        }' | head -1 || true)
    [ -n "$ADDR" ] && break
    sleep 2
done
kill "$SNIFFER" 2>/dev/null || true
trap - INT TERM

[ -n "$ADDR" ] || {
    say error "no lease seen for $GUESTMAC on $BACKEND"
    say wire "$WIRE"; say emulog "$EMULOG"; exit 1; }
say address "$ADDR"

# --------------------------------------------------------- the version gate --

# Four questions, because a demo that quietly serves last week's binary is
# worse than one that does not start.  What the build directory holds, what the
# archive carries, what landed on the drive, and what the process that is
# answering says it is.
BUILD_VER=""
[ -f "$BUILD/include/aminetxduo/version.h" ] &&
    BUILD_VER=$(sed -n 's/.*AMINETXDUO_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' \
                "$BUILD/include/aminetxduo/version.h" | head -1)
say build_version "${BUILD_VER:-unknown}"
say build_verstag \
    "$(strings -a "$UNP/C/httpd" 2>/dev/null | grep -m1 '\$VER: httpd' || true)"

# What the guest wrote about the binary it loaded, read straight out of the
# host directory the drive is.
GUEST_VER=""
for _ in $(seq 1 60); do
    [ -s "$HD/httpd-ver.txt" ] && { GUEST_VER=$(tr -d '\r' < "$HD/httpd-ver.txt" | head -1); break; }
    sleep 2
done
say guest_httpd_version "${GUEST_VER:-unknown}"

if [ -z "$GUEST_VER" ]; then
    say error "the guest never wrote httpd-ver.txt: it did not get as far as\
 running the server"
    say emulog "$EMULOG"; say drive "$HD"; exit 1
fi
if [ "${GUEST_VER#*"$ARCH_VER"}" = "$GUEST_VER" ]; then
    say error "the guest loaded '$GUEST_VER' and the archive is $ARCH_VER --\
 the drive was installed from somewhere else"
    exit 1
fi

# The bytes on the drive against the bytes in the archive, and the bytes in the
# archive against the bytes in the build directory.  Version strings only catch
# a different release; these catch the same release built from a different
# commit, which is how a stale binary usually arrives.  The second one is
# skipped for an archive that was handed in with -a: it came from another
# machine's build and there is nothing here for it to match.
STAGED=$(sum_of "$(amiga_path C/httpd)")
[ "$STAGED" = "$(sum_of "$UNP/C/httpd")" ] || {
    say error "the httpd on the drive is not the one in the archive"; exit 1; }
if [ "$BUILT_HERE" = "1" ]; then
    [ "$STAGED" = "$(sum_of "$BUILD/src/tools/httpd")" ] || {
        say error "the httpd in the archive is not the one in $BUILD"; exit 1; }
    say build_match ok
fi
say staged_sha256 "$STAGED"
say version_match ok

# ------------------------------------------------------------- the driver gate --
#
# WHICH DRIVER THE STACK HAS OPEN, said by the stack.  ShowNetStatus prints the
# live driver and falls back to the configuration file only when it cannot read
# one (src/tools/shownetstatus.c:388), so this is the running interface and not
# a file somebody wrote.
#
# It exists because the rig ran on Commodore's a2065.device for its whole life
# and looked exactly like this from the outside: a lease, an address, a served
# page.  Ours was built every time and staged never.  A guest that comes up on
# any driver but the one that was installed fails the launch.
WANT_DRIVER="${SANA2_SEL_DRIVER}"
DRIVER_LINE=""
for _ in $(seq 1 60); do
    [ -s "$HD/netstatus.txt" ] && {
        DRIVER_LINE=$(grep -a '^Interface ' "$HD/netstatus.txt" | tail -1)
        [ -n "$DRIVER_LINE" ] && break; }
    sleep 2
done
if [ -z "$DRIVER_LINE" ]; then
    say error "the guest never reported an interface: ShowNetStatus wrote\
 nothing to DH0:netstatus.txt"
    say emulog "$EMULOG"; say drive "$HD"; exit 1
fi
DRIVER_IN_USE=$(printf '%s' "$DRIVER_LINE" | sed -n 's/.*(\(.*\) unit .*/\1/p')
say driver_installed "$WANT_DRIVER"
say driver_card "${SANA2_SEL_CARD:-none}"
say driver_in_use "${DRIVER_IN_USE:-unknown}"
case "$DRIVER_IN_USE" in
    *"$WANT_DRIVER") say driver_match ok ;;
    *) say error "the guest is running on '${DRIVER_IN_USE:-nothing}' and\
 $WANT_DRIVER was installed"
       say netstatus "$HD/netstatus.txt"
       exit 1 ;;
esac

# What the running server says it is, asked from somewhere that can hear it.
#
# Not from here.  A frame this host sends to a guest of its own never reaches
# it, so curl on the emulator host times out on a guest that is serving
# perfectly well and the timeout says nothing about the guest.  -c names a
# machine on the same segment that is not this one.
if [ -n "$CHECKHOST" ]; then
    SERVED=""
    for _ in $(seq 1 30); do
        SERVED=$(ssh -o BatchMode=yes -o ConnectTimeout=5 "$CHECKHOST" \
                 "curl -s -m 5 -D - -o /dev/null http://$ADDR:$PORT/" 2>/dev/null |
                 sed -n 's/^[Ss]erver:[[:space:]]*//p' | tr -d '\r' | head -1 || true)
        [ -n "$SERVED" ] && break
        sleep 2
    done
    say served_by "${SERVED:-none}"
    [ -n "$SERVED" ] || {
        say error "nothing answered on http://$ADDR:$PORT/ from $CHECKHOST"
        exit 1; }
    [ "$SERVED" = "AmiNetXDuo-httpd/$ARCH_VER" ] || {
        say error "the guest is serving '$SERVED', not $ARCH_VER"; exit 1; }
    say served_check ok
fi

# --------------------------------------------------------------------------

HOSTPART="$ADDR"
[ "$PORT" = 80 ] || HOSTPART="$ADDR:$PORT"
say drawer "http://$HOSTPART/"
say shell "http://$HOSTPART/shell"
[ "$VARIANT" = rtg ] && say console "http://$HOSTPART/console"
NAMEPART="$NAME.local"
[ "$PORT" = 80 ] || NAMEPART="$NAME.local:$PORT"
say name "http://$NAMEPART/"
say emulog "$EMULOG"
say emulog_bytes "$(wc -c < "$EMULOG" | tr -d ' ')"
say httpd_log "$HD/httpd-log.txt"
say httpd_log_bytes "$(wc -c < "$HD/httpd-log.txt" 2>/dev/null | tr -d ' ' || echo 0)"
say netstatus "$HD/netstatus.txt"
say installdrive "$HD/installdrive.txt"
say drive "$HD"
say stop "pkill -f 'amiberry --log -f $CFG'"
say RESULT UP
