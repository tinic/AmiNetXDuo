#!/usr/bin/env bash
#
# Install the release archive on a REAL Workbench 3.1, reboot, and then use
# the machine the way its owner would.
#
#   install/test/run-workbench.sh [-b BUILDDIR] [-a ARCHIVE.lha]
#                                       [-l NOVICE|AVERAGE|EXPERT]
#                                       [-t SECONDS] [-T SECONDS] [-k] [-H]
#
# -H IS THE TERMINAL ARM, and it makes the run three installs instead of one:
#
#   1. a pre-existing S:User-Startup with another application's lines in it is
#      written BEFORE anything is installed, then the Installer runs twice,
#      both times answering "Yes, serve them" to the question about serving a
#      drawer and a Shell at boot.  Two runs, one httpd line and one assign
#      line, or the managed block was appended to rather than replaced;
#   2. the power cycle, and while the machine is up this host fetches
#      http://<the Amiga>/terminal over the bridge, which is what "reachable"
#      means;
#   3. a third install answering the question the other way, which has to take
#      both lines away again and leave AddNetInterface and the other
#      application's lines exactly where they were.
#
# It needs -l AVERAGE or -l EXPERT: at NOVICE the Installer draws no questions
# at all and there is nothing to answer.
#
# WHY THIS EXISTS, given that install/test/run-installer.sh already installs and
# boots.  That harness stages the machine itself: it makes an empty LIBS:, a
# DEVS: holding one driver and an S: with nothing in it, boots a bare
# directory hard drive with no Workbench on it at all, and drives its own
# boot script.  Every failure a user has reported so far, `fetch https://`
# not working, a command not seeing its arguments, happened on a real
# Workbench and was invisible here, because "works on the staging tree" and
# "works on a Workbench" are different claims and only the second one is the
# product.
#
# So this one does the whole thing end to end:
#
#   0. assembles a genuine Workbench 3.1 SYS: out of Commodore's five
#      floppies (workbench, fonts, locale, storage, extras) and caches it;
#   1. boots it, SetPatch, IPrefs, LoadWB, the real Startup-Sequence;
#   2. unpacks the release .lha into DH0:Unpacked/ the way a download
#      would arrive, and runs Install-AmiNetXDuo through Commodore's
#      Installer with installdrive clicking Proceed;
#   3. POWER CYCLES the machine.  The second boot runs the stock 3.1
#      Startup-Sequence untouched, which reaches `Execute S:User-Startup`
#      and starts the network from the line the installer wrote, nothing
#      the harness knows is in the path;
#   4. runs, from an ordinary Shell script, the four things a user does
#      first: look at the network, fetch an http: URL, fetch an https: URL,
#      and run the shipped commands WITH ARGUMENTS.
#
# Each of those four reports its own return code, and this script prints them
# as a table.  IT DOES NOT ADJUST THEM UNTIL THEY PASS: a failure here is the
# finding.
#
# The bare-hardware harness is untouched and still the one to run for
# installer-script coverage (five user levels, the static-address branch, the
# reinstall path).  This one covers the other axis: one install, on a machine
# that looks like the user's.
#
# -a TAKES THE ARCHIVE AS GIVEN, and that is the strongest form of this test:
#
#     gh release download v0.8.1 -D /tmp/rel
#     install/test/run-workbench.sh -a /tmp/rel/AmiNetXDuo-0.8.1.lha
#
# Without it the archive is built here, from this machine's toolchain, and a
# defect that lives in the RELEASE build, a different compiler, a different
# crt0, cannot show up.  Run it both ways before believing a release.
#
# WHY ftp.gnu.org AND NOT www.iana.org, which this used to fetch.
#
# A server puts a clock on the TLS handshake, started at accept() and stopped
# when the client's Finished arrives, and a 68020 does not finish inside every
# one of them.  Measured on this machine, seconds a server will hold a
# connection with no ClientHello on it:
#
#   www.iana.org / www.rfc-editor.org (Cloudflare)   15.0
#   www.google.com / dns.google                      10.0
#   aminet.net, www.python.org, archive.org          ~60
#   ftp.gnu.org, www.gnu.org, www.debian.org,
#   www.openbsd.org, ftp.funet.fi                    no close at 240
#
# Against www.iana.org the A1200 in this harness took 20.1 s of handshake, so
# Cloudflare sent FIN 15.0 s in, five seconds before the client Finished went
# out, and the RST arrived before the request could be answered.  That is not
# a defect in the stack, and no amount of work on our side fits a
# three-certificate chain into 15 s at 14 MHz.
#
# Handshake cost measured here, A1200/68020, whole chain verified:
#
#   ftp.gnu.org        18.4 s   3 certificates, all RSA
#   aminet.net         18.5 s   3 certificates, all RSA
#   www.openbsd.org    25.2 s   3 certificates, RSA-4096 leaf
#   www.debian.org     25.4 s   3 certificates, RSA-4096 leaf
#
# ftp.gnu.org is the cheapest of the hosts that impose no budget at all, its
# index page is 9 kB, and gnu.org has been at that name longer than this
# hardware has been out of production.
#
# INGREDIENTS, none of which are ours to ship:
#
#   Workbench 3.1 ADFs   ~/amigaos/adf/amiga-wb31_{workbench,extras,fonts,
#                        locale,storage}.adf, or AMINETXDUO_ADF_DIR
#   Kickstart 3.1        AMINETXDUO_KICKSTART, else the A1200 40.68 image
#   Commodore Installer  build/Installer, or AMINETXDUO_INSTALLER
#   a2065.device         build/a2065.device, or AMINETXDUO_A2065
#   amitools' xdftool    AMINETXDUO_XDFTOOL, or on PATH (pip install amitools)
#   lha                  to unpack the archive on the host; Lhasa will do
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="${AMINETXDUO_BUILD:-build/cm}"
ARCHIVE=""
LEVEL=NOVICE
INSTALL_TIMEOUT=420
BOOT_TIMEOUT=720
KEEP=0
TERMINAL=0

while getopts "b:a:l:t:T:kH" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        a) ARCHIVE="$OPTARG" ;;
        l) LEVEL="$OPTARG" ;;
        t) INSTALL_TIMEOUT="$OPTARG" ;;
        T) BOOT_TIMEOUT="$OPTARG" ;;
        k) KEEP=1 ;;
        H) TERMINAL=1 ;;
        *) echo "usage: $0 [-b builddir] [-a archive.lha]" \
                "[-l NOVICE|AVERAGE|EXPERT] [-t seconds] [-T seconds] [-k] [-H]" >&2
           exit 2 ;;
    esac
done

# -H needs a level where the questions are drawn at all.  At NOVICE every
# ask... returns its default without showing anything, so a run that asked for
# the terminal and got NOVICE would install without it and pass every check
# below that does not look for it -- which is the vacuous pass this whole
# script exists not to produce.
if [ "$TERMINAL" = "1" ] && [ "$LEVEL" = "NOVICE" ]; then
    echo "-H needs -l AVERAGE or -l EXPERT: at NOVICE the Installer draws no" >&2
    echo "questions and the terminal one cannot be answered." >&2
    exit 2
fi

case "$LEVEL" in
    NOVICE|AVERAGE|EXPERT) ;;
    *) echo "unknown user level: $LEVEL" >&2; exit 2 ;;
esac

# The unpack below runs from inside DH0:Unpacked, so a relative -a would be
# resolved against the wrong directory and read as a missing archive.
case "$ARCHIVE" in
    ""|/*) ;;
    *)     ARCHIVE="$PWD/$ARCHIVE" ;;
esac
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

GCC="${AMIGA_GCC:-$HOME/amigaos/tools/m68k-amigaos-gcc/bin/m68k-amigaos-gcc}"
NDK="${AMIGA_NDK:-$HOME/amigaos/tools/m68k-amigaos-gcc/m68k-amigaos/ndk-include}"

TAG="${AMINETXDUO_RUN_TAG:-wb31}"
HD="$ROOT/build/testhd-$TAG"

# ------------------------------------------------------------ ingredients --

INSTALLER="${AMINETXDUO_INSTALLER:-}"
if [ -z "$INSTALLER" ]; then
    for candidate in "$ROOT/build/Installer" "$HOME/amigaos/tools/Installer"; do
        [ -f "$candidate" ] && { INSTALLER="$candidate"; break; }
    done
fi
[ -n "$INSTALLER" ] && [ -f "$INSTALLER" ] || {
    echo "No Commodore Installer found." >&2
    echo "Set AMINETXDUO_INSTALLER=<path>, or put one in build/Installer." >&2
    exit 2
}

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# THE ROM AND THE MODEL ARE A PAIR, and the wrong half of it is a silent
# failure.  tls.library needs a 68020, so a 68000 machine cannot run the part
# of this test that matters most; and an A1200 booted on a CD32 ROM is not an
# A1200.  So the candidates here are A1200-or-better 3.1 images ONLY, named
# explicitly, and the one chosen is printed.  A CD32 image is never picked by
# accident because it is not in the list.
MODEL="${AMINETXDUO_MODEL:-A1200}"
# The pairing above is enforced, not just described.  An A1200 40.68 ROM is
# AGA and expects an A1200; booting it on quickstart=A600 crashes the guest
# before Workbench comes up, and the crash presents as a boot that never
# finishes, so the run dies on INSTALL_TIMEOUT and leaves an empty drive.  An
# empty drive passes every "file is absent" assertion in this script.  Three
# runs of the 68000 arm were read as green that way.  So a model outside the
# AGA set must name its own ROM, and not naming one is fatal here rather than
# 420 seconds later.
#
# AMINETXDUO_KICKSTART_<MODEL> WINS OVER AMINETXDUO_KICKSTART.  The generic one
# is an A1200 AGA image in every environment that sets it, including the lab's
# own env.sh, which sets the A600 ROM two lines below it and annotates it "the
# 68000 machines need a 68000 ROM".  Reading the generic one first threw that
# away and put the AGA ROM back on the A600.
eval "KICKSTART=\${AMINETXDUO_KICKSTART_$MODEL:-}"
if [ -z "$KICKSTART" ]; then
    case "$MODEL" in
    A1200|A3000|A4000) KICKSTART="${AMINETXDUO_KICKSTART:-}" ;;
    esac
fi
if [ -z "$KICKSTART" ]; then
    case "$MODEL" in
    A1200|A3000|A4000)
        for candidate in \
            "$HOME/Downloads/Kickstart v3.1 rev 40.68 (1993)(Commodore)(A1200)[!].rom" \
            "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A1200)[!].rom" \
            "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A4000).rom" \
            "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A3000).rom" \
            "$HOME/png2amiga_testing/kick31.rom"
        do
            [ -f "$candidate" ] && { KICKSTART="$candidate"; break; }
        done
        ;;
    *)
        echo "MODEL=$MODEL needs its own ROM: the default list is AGA and" >&2
        echo "an AGA ROM on a 68000 machine crashes before Workbench loads." >&2
        echo "Set AMINETXDUO_KICKSTART_$MODEL=<path> or AMINETXDUO_KICKSTART." >&2
        exit 2
        ;;
    esac
fi
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    echo "No Kickstart 3.1 ROM; set AMINETXDUO_KICKSTART=<path>." >&2
    exit 2
}

# Amiberry, not fs-uae.  fs-uae opens an SDL window even with nothing to open
# it on, so on a headless machine it dies in seconds and the failure reads as
# a guest that never booted; and the Debian build's SLIRP is a stub that logs
# `stub, uae_slirp_start` and carries on, so the network half of this test
# would measure nothing.  tools/amiberry-run.sh's header has the long version.
AMIBERRY="${AMIBERRY:-$(command -v amiberry || true)}"
if [ -z "$AMIBERRY" ]; then
    for candidate in "$HOME/amiberry/build/amiberry" "$HOME/amiberry/amiberry"; do
        [ -x "$candidate" ] && { AMIBERRY="$candidate"; break; }
    done
fi
[ -n "$AMIBERRY" ] || { echo "amiberry not found; set AMIBERRY=<path>" >&2; exit 2; }

# The network backend the guest's A2065 is wired to.  A bare interface name
# (`ens18`) puts it on the host's own LAN with its own MAC, which is what
# makes the DHCP, DNS and http checks below real rather than NAT-shaped.
BACKEND="${AMINETXDUO_EMU_BACKEND:-slirp}"
MAC="${AMINETXDUO_EMU_MAC:-52:54:00:c0:ff:ee}"

XDFTOOL="${AMINETXDUO_XDFTOOL:-}"
if [ -z "$XDFTOOL" ]; then
    for candidate in \
        "$(command -v xdftool || true)" \
        "$HOME/.venvs/amitools/bin/xdftool" \
        "$HOME/amigaos/tools/amitools/bin/xdftool"
    do
        [ -n "$candidate" ] && [ -x "$candidate" ] && { XDFTOOL="$candidate"; break; }
    done
fi

command -v lha >/dev/null 2>&1 || {
    echo "lha not found, needed to unpack the release archive on the host." >&2
    exit 2
}

ADFDIR="${AMINETXDUO_ADF_DIR:-$HOME/amigaos/adf}"

echo "==> model $MODEL on $(basename "$KICKSTART")"

# ------------------------------------------------------ Workbench 3.1 SYS: --
#
# Five floppies make one hard drive.  The layout is Commodore's own: the
# Workbench disk IS the root, and the other four are the drawers the HD
# installer would have made for them.  Extras3.1 goes in Extras/ rather than
# being merged into the root, which is what the stock Startup-Sequence expects
# when it says `Assign L: Extras3.1:L DEFER`.
#
# Cached, because unpacking five ADFs on every run is a minute of nothing.
# The stamp is outside the tree so it cannot end up on the emulated disk.

WB="$ROOT/build/wb31-sys"
WBSTAMP="$ROOT/build/.wb31-sys.stamp"

wb_adfs=()
for disk in workbench fonts locale storage extras; do
    wb_adfs+=("$ADFDIR/amiga-wb31_$disk.adf")
done

wb_stale=0
[ -d "$WB" ] && [ -f "$WBSTAMP" ] || wb_stale=1
for adf in "${wb_adfs[@]}"; do
    [ -f "$adf" ] || {
        echo "missing $adf" >&2
        echo "Set AMINETXDUO_ADF_DIR to the directory holding the WB 3.1 set." >&2
        exit 2
    }
    [ "$adf" -nt "$WBSTAMP" ] && wb_stale=1
done

# amitools writes the volume's contents into the directory it is given, but
# older versions make a subdirectory named after the volume; cope with both
# rather than depending on which one is installed.
unpack_adf() {
    local adf="$1" into="$2" marker="$3" inner
    rm -rf "$into"
    mkdir -p "$into"
    "$XDFTOOL" "$adf" unpack "$into" >/dev/null
    [ -e "$into/$marker" ] && return 0
    inner=$(find "$into" -maxdepth 1 -mindepth 1 -type d | head -1)
    [ -n "$inner" ] && [ -e "$inner/$marker" ] || {
        echo "!! $(basename "$adf") did not unpack to something holding $marker" >&2
        exit 2
    }
    mv "$inner"/* "$into"/ 2>/dev/null || true
    mv "$inner"/.[!.]* "$into"/ 2>/dev/null || true
    rmdir "$inner" 2>/dev/null || true
}

if [ "$wb_stale" = "1" ]; then
    [ -n "$XDFTOOL" ] && [ -x "$XDFTOOL" ] || {
        echo "amitools' xdftool not found, needed to unpack the Workbench ADFs." >&2
        echo "  pip install amitools, or set AMINETXDUO_XDFTOOL=<path>" >&2
        exit 2
    }
    echo "==> assembling Workbench 3.1 into $WB (five ADFs)"
    SCRATCH="$ROOT/build/.wb31-unpack"
    rm -rf "$SCRATCH" "$WB"
    mkdir -p "$SCRATCH" "$WB"
    for pair in "workbench:S/Startup-Sequence" "fonts:topaz.font" \
                "locale:Catalogs" "storage:DOSDrivers" "extras:Tools"; do
        unpack_adf "$ADFDIR/amiga-wb31_${pair%%:*}.adf" \
                   "$SCRATCH/${pair%%:*}" "${pair##*:}"
    done
    cp -R "$SCRATCH/workbench/." "$WB/"
    for pair in "fonts:Fonts" "locale:Locale" "storage:Storage" "extras:Extras"; do
        src="${pair%%:*}"; dst="${pair##*:}"
        mkdir -p "$WB/$dst"
        cp -R "$SCRATCH/$src/." "$WB/$dst/"
    done
    rm -rf "$SCRATCH"

    # A directory hard drive takes its Amiga protection bits from the host's
    # mode bits, and xdftool unpacks everything 0644, which would leave
    # every command in C: without its E bit.  LhA on a real Amiga restores
    # the bits from the archive; here the host has to.
    chmod -R a+rx "$WB"

    : > "$WBSTAMP"
    for want in C/Assign C/LoadWB Libs/version.library S/Startup-Sequence \
                Fonts/topaz.font Locale/Catalogs Storage/DOSDrivers \
                Devs/system-configuration Extras/Tools; do
        [ -e "$WB/$want" ] || { echo "!! assembled SYS: has no $want" >&2; exit 2; }
    done
else
    echo "==> Workbench 3.1 tree is current ($WB)"
fi

# --------------------------------------------------------- the release .lha --
#
# THE ARTIFACT, not a directory that happens to hold the same files.  What a
# user has is an archive they downloaded, so that is what gets unpacked here.

if [ -z "$ARCHIVE" ]; then
    echo "==> building the distribution archive"
    AMINETXDUO_DIST_CPUS="68020-40" \
        "$ROOT/dist/make-dist.sh" -b "$BUILD" >"$ROOT/build/wb31-make-dist.log" 2>&1 || {
        echo "dist/make-dist.sh failed, see build/wb31-make-dist.log" >&2
        tail -20 "$ROOT/build/wb31-make-dist.log" >&2
        exit 2
    }
    VERSION=$("$ROOT/tools/version.sh" --product)
    ARCHIVE="$ROOT/build/dist/AmiNetXDuo-$VERSION.lha"
fi
[ -f "$ARCHIVE" ] || { echo "no such archive: $ARCHIVE" >&2; exit 2; }
echo "==> archive $(basename "$ARCHIVE") ($(wc -c < "$ARCHIVE" | tr -d ' ') bytes)"

# ------------------------------------------------------------ the machine --

# -m68000, not -m68020: this runs INSIDE the guest, and the guest is whatever
# MODEL says.  Built for a 68020 it executed TST.L A0 on the A600 and took an
# illegal instruction before the Installer ever started, so the boot never
# finished, INSTALL_TIMEOUT fired, and the drive was left empty -- which reads
# as a clean pass to every "this file is absent" check below.  Nothing here
# needs 68020 codegen.
#
# $1 names the binary, $2 is how many times it runs the Installer, $3 is the
# label of the yes/no button to press instead of the first one (empty: press
# the first, which is every question's default).
build_driver() {
    local out="$1" runs="$2" label="$3"
    "$GCC" -O2 -m68000 -Wall -Wextra -DDRIVE_LEVEL="\"$LEVEL\"" \
           -DDRIVE_RUNS="$runs" -DDRIVE_YES_LABEL="\"$label\"" -I"$NDK" \
           -o "$out" "$ROOT/install/test/installdrive.c" || exit 2
}

# -H installs TWICE in the first boot, both times saying yes to the terminal.
# One run proves the lines get written; two prove the block is rewritten and
# not appended to, which is the property a user with an existing S:User-Startup
# is relying on.
YES_LABEL=""
DRIVE_RUNS=1
if [ "$TERMINAL" = "1" ]; then
    YES_LABEL="Yes, serve them"
    DRIVE_RUNS=2
fi

echo "==> building installdrive ($LEVEL, $DRIVE_RUNS run(s)${YES_LABEL:+, \"$YES_LABEL\"})"
DRIVER="$ROOT/build/installdrive-wb-$LEVEL"
build_driver "$DRIVER" "$DRIVE_RUNS" "$YES_LABEL"

rm -rf "$HD"
mkdir -p "$HD"
cp -R "$WB/." "$HD/"
cp "$A2065" "$HD/Devs/a2065.device"
cp "$DRIVER" "$HD/C/installdrive"
chmod 755 "$HD/Devs/a2065.device" "$HD/C/installdrive"

# SOMEBODY ELSE'S S:User-Startup, written before the installer ever runs.
#
# The installer's stated contract is that it replaces its own marked block and
# leaves every other application's lines alone, and that is the thing most
# worth not breaking: this file is where every Amiga application on the machine
# has put its line.  So the file exists, with lines in it that are nothing to
# do with us, and each phase below checks they are all still there, in order,
# byte for byte.
FOREIGN_LINES=(
    "; -- SomeOtherApp 2.4 --"
    "Assign OTHERAPP: DH0:OtherApp"
    "C:SetPatch QUIET"
    "; -- end SomeOtherApp --"
)
mkdir -p "$HD/S"
printf '%s\n' "${FOREIGN_LINES[@]}" > "$HD/S/User-Startup"
chmod 644 "$HD/S/User-Startup"

# AmigaDOS does not care about case and this host does, so a file the guest
# wrote as `s/user-startup` is not `S/User-Startup` to `[ -f ]`.  Every name
# below is resolved the way the guest would, one component at a time; without
# this the installer looks like it skipped the one file it did write.
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

user_startup() { amiga_path S/User-Startup 2>/dev/null || true; }

foreign_intact() {
    local f line
    f=$(user_startup)
    [ -n "$f" ] && [ -f "$f" ] || return 1
    for line in "${FOREIGN_LINES[@]}"; do
        grep -Fqx -- "$line" "$f" || return 1
    done
    return 0
}

# How many times a pattern appears in S:User-Startup.  Duplicates are the
# failure mode a second install has, so this counts rather than tests.
startup_count() {
    local f n
    f=$(user_startup)
    [ -n "$f" ] && [ -f "$f" ] || { echo 0; return; }
    # grep -c prints 0 AND exits 1 when it matches nothing, so `|| echo 0`
    # would print it twice and every arithmetic test downstream reads "0\n0".
    n=$(grep -c -- "$1" "$f" 2>/dev/null) || n=0
    echo "${n:-0}"
}

# The download, where a download would be: its own drawer, not the one the
# installer is going to create.
mkdir -p "$HD/Unpacked"
( cd "$HD/Unpacked" && lha -xfq "$ARCHIVE" ) >/dev/null 2>&1 || \
( cd "$HD/Unpacked" && lha xf "$ARCHIVE" ) >/dev/null 2>&1 || {
    echo "could not unpack $ARCHIVE" >&2; exit 2; }
[ -d "$HD/Unpacked/AmiNetXDuo" ] || {
    echo "the archive did not unpack to an AmiNetXDuo drawer" >&2
    ls -la "$HD/Unpacked" >&2
    exit 2
}
cp "$INSTALLER" "$HD/Unpacked/AmiNetXDuo/Installer"
chmod -R a+rx "$HD/Unpacked"

# WHAT IS IN THE ARCHIVE, said out loud BEFORE anything is installed.  The
# https: check below cannot pass if the archive has no tls.library, and an
# archive built from a tree configured without TLS is an ordinary thing to
# have lying around, so the difference between "the product is broken" and
# "you packed a build that never had it" is stated here rather than left for
# somebody to work out from a return code.
echo "==> the archive holds:"
for f in Libs/68020-40/bsdsocket.library Libs/68020-40/usergroup.library \
         Libs/68020-40/tls.library Devs/Internet/certificates \
         C/fetch C/ssh; do
    if [ -f "$HD/Unpacked/AmiNetXDuo/$f" ]; then
        printf '      %-36s %s bytes\n' "$f" \
               "$(wc -c < "$HD/Unpacked/AmiNetXDuo/$f" | tr -d ' ')"
    else
        printf '      %-36s ABSENT\n' "$f"
    fi
done
if [ ! -f "$HD/Unpacked/AmiNetXDuo/Libs/68020-40/tls.library" ]; then
    echo "!! This archive has NO tls.library, so the https: check cannot pass"
    echo "!! and its failure will say nothing about the product.  Build the"
    echo "!! archive from a tree configured with -DAMINETXDUO_TLS=ON."
fi

# ------------------------------------------------------------- the emulator --
#
# tools/amiberry-run.sh cannot drive these runs: it wipes the staging drive and
# writes its own Startup-Sequence, and the whole point here is a machine that
# boots Commodore's.  So the emulator is started directly, with the same
# config that harness generates, and with its lock, in its measurement lane,
# so a run here does not share the host with anything else.

LOCKDIR="$ROOT/build/.emu.lock"
SLOTDIR="$ROOT/build/.emu.slots"
PERFWAIT="$ROOT/build/.emu.perfwait"
LOCK_HELD=0

slots_busy() { ls -d "$SLOTDIR"/*/ 2>/dev/null | wc -l | tr -d ' '; }

reap_stale() {
    local d owner
    for d in "$LOCKDIR" "$PERFWAIT" "$SLOTDIR"/*; do
        [ -d "$d" ] || continue
        owner=$(cat "$d/pid" 2>/dev/null || echo "")
        if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then
            rm -rf "$d" 2>/dev/null || true
        fi
    done
}

take_lock() {
    [ "${AMINETXDUO_NO_LOCK:-0}" = "1" ] && return 0
    local waited=0 limit="${AMINETXDUO_LOCK_WAIT:-2400}"
    mkdir -p "$SLOTDIR"
    while ! mkdir "$PERFWAIT" 2>/dev/null; do
        reap_stale
        [ "$waited" -ge "$limit" ] && break
        [ "$waited" = 0 ] && echo "==> another exclusive run is queued; waiting"
        sleep 5; waited=$((waited + 5))
    done
    echo $$ > "$PERFWAIT/pid" 2>/dev/null || true
    while [ "$(slots_busy)" -gt 0 ] || ! mkdir "$LOCKDIR" 2>/dev/null; do
        reap_stale
        if [ "$waited" -ge "$limit" ]; then
            mkdir -p "$LOCKDIR"; break
        fi
        [ "$waited" = 0 ] && echo "==> waiting for the emulator to go quiet"
        sleep 5; waited=$((waited + 5))
    done
    echo $$ > "$LOCKDIR/pid" 2>/dev/null || true
    LOCK_HELD=1
}

release_lock() {
    [ "$LOCK_HELD" = "1" ] || return 0
    rm -rf "$LOCKDIR" "$PERFWAIT" 2>/dev/null || true
    LOCK_HELD=0
}

EMU_PID=""
SERIAL_PID=""
cleanup() {
    if [ -n "$EMU_PID" ]; then
        kill -TERM "$EMU_PID" 2>/dev/null || true
        sleep 1
        kill -KILL "$EMU_PID" 2>/dev/null || true
        EMU_PID=""
    fi
    [ -n "$SERIAL_PID" ] && kill -TERM "$SERIAL_PID" 2>/dev/null || true
    SERIAL_PID=""
    release_lock
}
trap cleanup EXIT INT TERM HUP

# The headless switch.  Amiberry links SDL2 with no driver of its own, so
# without this it asks for a video device, finds a stale DISPLAY from an ssh
# X11 forward that failed, and aborts in about a second -- which reads as a
# guest that never booted.  DISPLAY is cleared for the same reason: a stale one
# is worse than none.  tools/amiberry-run.sh does exactly this.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
[ "${SDL_VIDEODRIVER}" = "dummy" ] && unset DISPLAY WAYLAND_DISPLAY || true

# One boot of the machine as it stands.  $1 names the run, $2 is the timeout,
# $3 is "net" to attach the A2065 to SLIRP.  Returns the guest's own exit
# status out of DH0:.done, or 124.
BOOT_STATUS=0
boot() {
    local name="$1" timeout="$2" net="${3:-}"
    local cfg="$ROOT/build/wb31-$name.uae"
    local serial="$ROOT/build/serial-wb31-$name.log"
    local elapsed=0
    local port

    # One listening port per run name, so two runs never collide.  Same
    # hashing as tools/amiberry-run.sh.
    port=$((12000 + $(printf '%s' "wb31-$name" | cksum | cut -d' ' -f1) % 900))

    : > "$serial"
    rm -f "$HD/.done"

    cat > "$cfg" <<EOF
config_description=AmiNetXDuo wb31 $name
use_gui=no
headless=true
quickstart=$MODEL,0
kickstart_rom_file=$KICKSTART
fastmem_size=8
floppy0type=-1
nr_floppies=0
uaehf0=dir,rw,DH0:DH0:$HD,0
serial_port=tcp://127.0.0.1:$port/wait
EOF
    if [ "$net" = "net" ]; then
        cat >> "$cfg" <<EOF
a2065_rom_file=:ENABLED
a2065_rom_options=mac=$MAC,$BACKEND
EOF
    fi

    echo "==> booting ($name, timeout ${timeout}s, network $([ "$net" = net ] && echo "$BACKEND" || echo off))"
    # SIGPIPE ignored for the reason tools/amiberry-run.sh ignores it: SLIRP
    # writes guest payload to host sockets without MSG_NOSIGNAL, so a peer that
    # hangs up first otherwise kills the emulator and it looks like a guru.
    ( trap '' PIPE; exec "$AMIBERRY" --log -f "$cfg" ) \
        >"$ROOT/build/amiberry-wb31-$name.log" 2>&1 &
    EMU_PID=$!

    # serial_port=.../wait blocks the emulator until something connects, so
    # retry until it is listening or the emulator is gone.
    (
        for _ in $(seq 1 60); do
            kill -0 "$EMU_PID" 2>/dev/null || exit 0
            nc 127.0.0.1 "$port" >> "$serial" 2>/dev/null && exit 0
            sleep 0.5
        done
    ) &
    SERIAL_PID=$!

    BOOT_STATUS=124
    while [ "$elapsed" -lt "$timeout" ]; do
        if [ -f "$HD/.done" ]; then
            BOOT_STATUS=$(tr -dc '0-9' < "$HD/.done" | head -c 4)
            BOOT_STATUS=${BOOT_STATUS:-0}
            break
        fi
        kill -0 "$EMU_PID" 2>/dev/null || {
            echo "!! amiberry exited early after ${elapsed}s" >&2
            break
        }
        sleep 1
        elapsed=$((elapsed + 1))
    done

    kill -TERM "$EMU_PID" 2>/dev/null || true
    wait "$EMU_PID" 2>/dev/null || true
    EMU_PID=""
    kill -TERM "$SERIAL_PID" 2>/dev/null || true
    SERIAL_PID=""

    echo "    ($name finished after ${elapsed}s, status $BOOT_STATUS)"
    if [ -s "$serial" ]; then
        echo "---- serial ----"
        tail -40 "$serial"
    fi
}

# The stock 3.1 Startup-Sequence, with the tail replaced.  LoadWB stays --
# the Installer draws on the Workbench screen and a user has one, and only
# `EndCLI`, which would take the boot shell away before our line runs, goes.
#
# $2 = "nouserstartup" also drops `Execute S:User-Startup`, which is what a
# boot into the Installer has to be once the machine has already been
# installed once.  A running stack holds LIBS:bsdsocket.library open and the
# copy then fails -- the script says so itself, in @special-msg: "the network
# is already running.  Reboot and run this installer again before doing
# anything else."  This is that reboot.
STARTUP_SUM=""
startup_with() {
    local tail_cmds="$1" mode="${2:-}"
    if [ "$mode" = "nouserstartup" ]; then
        sed -e '/^EndCLI/d' -e '/Execute S:User-Startup/d' \
            "$WB/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
    else
        sed -e '/^EndCLI/d' "$WB/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
    fi
    printf '\n%s\n' "$tail_cmds" >> "$HD/S/Startup-Sequence"
    chmod 755 "$HD/S/Startup-Sequence"
    STARTUP_SUM=$(shasum "$HD/S/Startup-Sequence" | cut -d' ' -f1)
}

take_lock

# ------------------------------------------------------------------ run 1 ---

echo
echo "============================================================"
echo "  1/2  Workbench 3.1 boots, then installs the archive ($LEVEL)"
echo "============================================================"

startup_with 'FailAt 9999
C:installdrive >DH0:install-console.txt
Echo >DH0:.done "$RC"'

boot install "$INSTALL_TIMEOUT"
INSTALL_STATUS=$BOOT_STATUS

echo
echo "---- installdrive report ----"
cat "$HD/installdrive.txt" 2>/dev/null || echo "(none)"
echo
echo "---- Installer log ----"
cat "$HD/install-log.txt" 2>/dev/null || echo "(none written)"

# ------------------------------------------- what a real Workbench now has --

echo
echo "============================================================"
echo "  what the installer put on a real Workbench"
echo "============================================================"

fail=0

check_file() {
    local real
    if real=$(amiga_path "$1") && [ -f "$real" ]; then
        printf '  ok      %s\n' "$1"
    else
        printf '  MISSING %s\n' "$1"
        fail=1
    fi
}

check_file Libs/bsdsocket.library
check_file Libs/usergroup.library
for cmd in AddNetInterface Online Offline ShowNetStatus ping netstat host fetch; do
    check_file "C/$cmd"
done
check_file Devs/NetInterfaces/eth0
check_file Devs/Internet/name_resolution
check_file S/User-Startup

# tls.library and the trust store are what https: needs, and their absence is
# the first thing to know if the https: check fails.
for f in Libs/tls.library Devs/Internet/certificates; do
    real=$(amiga_path "$f" || true)
    if [ -n "$real" ] && [ -f "$real" ]; then
        printf '  ok      %-32s %s bytes\n' "$f" "$(wc -c < "$real" | tr -d ' ')"
    else
        printf '  ABSENT  %s\n' "$f"
    fi
done

echo
echo "---- S:User-Startup ----"
cat "$(amiga_path S/User-Startup 2>/dev/null)" 2>/dev/null || echo "(none)"
echo "---- DEVS:NetInterfaces/eth0 ----"
cat "$(amiga_path Devs/NetInterfaces/eth0 2>/dev/null)" 2>/dev/null || echo "(none)"

# Machine-readable, one key per line, so nothing downstream has to read prose.
TERM_LINES=0
TERM_ASSIGNS=0
FOREIGN=no
foreign_intact && FOREIGN=yes
TERM_LINES=$(startup_count 'httpd')
TERM_ASSIGNS=$(startup_count 'Assign AmiNetXDuo:')

echo
echo "startup_foreign_lines_intact=$FOREIGN"
echo "startup_httpd_lines=$TERM_LINES"
echo "startup_assign_lines=$TERM_ASSIGNS"
echo "startup_installer_runs=$DRIVE_RUNS"

if [ "$FOREIGN" != "yes" ]; then
    echo
    echo "!! S:User-Startup lost lines that were not ours.  The installer's"
    echo "   contract is that it touches only its own marked block."
    fail=1
fi

if [ "$TERMINAL" = "1" ]; then
    # Two installs, both answering yes: exactly one of each line, or the block
    # was appended to rather than replaced.
    if [ "$TERM_LINES" != "1" ] || [ "$TERM_ASSIGNS" != "1" ]; then
        echo
        echo "!! after $DRIVE_RUNS installs answering yes, S:User-Startup has"
        echo "   $TERM_LINES httpd line(s) and $TERM_ASSIGNS assign line(s); one of each"
        echo "   is the only right answer."
        fail=1
    fi
elif [ "$TERM_LINES" != "0" ]; then
    echo
    echo "!! nobody asked for httpd and S:User-Startup has $TERM_LINES httpd line(s)"
    fail=1
fi

if [ "$INSTALL_STATUS" != "0" ] || [ "$fail" != "0" ]; then
    echo
    echo "!! the install run did not complete cleanly (status $INSTALL_STATUS)"
    echo "   the drive is left at $HD"
    exit 1
fi

# ------------------------------------------------------------------ run 2 ---
#
# A POWER CYCLE, not a continuation.  The Startup-Sequence is put back to
# Commodore's, the installer's work has to be reached through its own
# `Execute S:User-Startup`, exactly as on the user's machine, and the only
# thing added is the line that runs the checks afterwards.

echo
echo "============================================================"
echo "  2/2  rebooting, and using the machine as its owner would"
echo "============================================================"

if [ "$(shasum "$HD/S/Startup-Sequence" | cut -d' ' -f1)" != "$STARTUP_SUM" ]; then
    echo "note: the installer also changed S:Startup-Sequence, diff against"
    echo "      the stock 3.1 one is worth reading before the reboot"
fi

# An ordinary Shell script, doing ordinary things, with every command's return
# code written down beside its output.  `Stack 200000` is the Shell's internal
# stack command.  It is NOT needed any more, clients/compat/amiga_argv.c
# swaps in 256 KB of its own before main() runs, and the ReadMe says so, and
# it stays here precisely because a cautious user will still type it: a client
# that mishandled an already-large Shell stack would fail nowhere else.
cat > "$HD/S/AmiNetXDuo-Check" <<EOF
; Written by install/test/run-workbench.sh.  Nothing here is installed
; by AmiNetXDuo, it is what a user would type.
FailAt 9999
Stack 200000

Echo >DH0:usercheck.txt "=== 1. the network, as S:User-Startup brought it up"
C:Wait 5
C:ShowNetStatus >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT network rc=\$RC"

Echo >>DH0:usercheck.txt "*N=== 2. fetch http://example.com/"
C:fetch http://example.com/ TO DH0:http-body.txt >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT fetch-http rc=\$RC"

Echo >>DH0:usercheck.txt "*N=== 3. fetch https://ftp.gnu.org/"
C:fetch https://ftp.gnu.org/ TO DH0:https-body.txt >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT fetch-https rc=\$RC"


Echo >>DH0:usercheck.txt "*N=== 4. arp, what answered on this network"
C:arp >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT arp rc=\$RC"

Echo >>DH0:usercheck.txt "*N=== done"
EOF
chmod 755 "$HD/S/AmiNetXDuo-Check"

startup_with 'FailAt 9999
Execute S:AmiNetXDuo-Check >DH0:check-console.txt
Echo >DH0:.done "$RC"'

rm -f "$HD/usercheck.txt" "$HD/http-body.txt" "$HD/https-body.txt"

# ---- the terminal, from another machine, while the Amiga is still up -------
#
# Not from inside the guest.  "Reachable" means reachable from somewhere else,
# and this backend is a bridge, so the host is somewhere else with its own
# address on the same LAN.  The guest's address is not known in advance
# (DHCP), and it does not have to be: ShowNetStatus writes it to DH0: as step
# 1 of the check script, and DH0: is a directory on this host, so it can be
# read while the machine is still running.
#
# Runs beside the boot rather than after it: httpd is gone the moment the
# emulator is killed.
TERM_PROBE="$ROOT/build/wb31-terminal-probe.txt"
PROBE_PID=""
if [ "$TERMINAL" = "1" ]; then
    : > "$TERM_PROBE"
    (
        guest=""
        for _ in $(seq 1 "$BOOT_TIMEOUT"); do
            sleep 1
            [ -f "$HD/usercheck.txt" ] || continue
            guest=$(sed -n 's/^  *address  *\([0-9][0-9.]*\).*/\1/p' \
                    "$HD/usercheck.txt" 2>/dev/null | head -1)
            [ -n "$guest" ] && break
        done
        if [ -z "$guest" ]; then
            echo "terminal_probe=no-address" >> "$TERM_PROBE"
            exit 0
        fi
        echo "terminal_guest_address=$guest" >> "$TERM_PROBE"
        for _ in $(seq 1 60); do
            code=$(curl -s -m 5 -o "$ROOT/build/wb31-terminal-body.html" \
                   -w '%{http_code}' "http://$guest/terminal" 2>/dev/null || true)
            if [ "$code" = "200" ]; then
                echo "terminal_http_code=200" >> "$TERM_PROBE"
                echo "terminal_bytes=$(wc -c < "$ROOT/build/wb31-terminal-body.html" \
                     | tr -d ' ')" >> "$TERM_PROBE"
                # The WebDAV half answers on the same port, and a 200 on / with
                # no /terminal would be a server that started without -T.
                echo "terminal_root_code=$(curl -s -m 5 -o /dev/null \
                     -w '%{http_code}' "http://$guest/" 2>/dev/null || echo 000)" \
                     >> "$TERM_PROBE"
                exit 0
            fi
            sleep 2
        done
        echo "terminal_http_code=${code:-none}" >> "$TERM_PROBE"
    ) &
    PROBE_PID=$!
fi

boot boot "$BOOT_TIMEOUT" net

if [ -n "$PROBE_PID" ]; then
    kill -TERM "$PROBE_PID" 2>/dev/null || true
    wait "$PROBE_PID" 2>/dev/null || true
    echo
    echo "---- the terminal, fetched from this host ----"
    cat "$TERM_PROBE" 2>/dev/null || echo "(the probe wrote nothing)"
fi

echo
echo "---- what the machine did ----"
cat "$HD/usercheck.txt" 2>/dev/null || echo "(DH0:usercheck.txt was never written)"

if [ -s "$HD/check-console.txt" ]; then
    echo
    echo "---- anything the script itself said ----"
    cat "$HD/check-console.txt"
fi

for body in http-body.txt https-body.txt; do
    [ -s "$HD/$body" ] || continue
    echo
    echo "---- $body ($(wc -c < "$HD/$body" | tr -d ' ') bytes, first 5 lines) ----"
    head -5 "$HD/$body"
done

# ------------------------------------------------------------- the verdict --

echo
echo "============================================================"
echo "  a real installed Workbench 3.1, four things a user does"
echo "============================================================"

bad=0

# A 68000 install carries no tls.library on purpose, so `fetch https://` there
# MUST fail: that is the product working, and scoring it as a failure makes the
# whole 68000 arm red for doing the right thing.  Keyed off what actually
# landed on the drive rather than off MODEL, so it stays true if the CPU split
# ever changes.
HAS_TLS=1
[ -f "$HD/Libs/tls.library" ] || HAS_TLS=0

report() {
    local label="$1" name="$2" expect="${3:-pass}" rc
    rc=$(sed -n "s/^RESULT $name rc=\(.*\)/\1/p" "$HD/usercheck.txt" 2>/dev/null \
         | head -1)
    if [ -z "$rc" ]; then
        printf '  %-34s NEVER RAN\n' "$label"
        bad=1
    elif [ "$expect" = "fail" ]; then
        if [ "$rc" = "0" ]; then
            printf '  %-34s rc=0  !! expected to fail, no tls.library\n' "$label"
            bad=1
        else
            printf '  %-34s rc=%s  (expected: no tls.library on this machine)\n' \
                   "$label" "$rc"
        fi
    elif [ "$rc" = "0" ]; then
        printf '  %-34s rc=0\n' "$label"
    else
        printf '  %-34s rc=%s\n' "$label" "$rc"
        bad=1
    fi
}

report "ShowNetStatus"                 network
report "fetch http://example.com/"     fetch-http
report "fetch https://ftp.gnu.org/"        fetch-https \
       "$([ "$HAS_TLS" = 1 ] && echo pass || echo fail)"
report "arp"                           arp

if [ "$TERMINAL" = "1" ]; then
    probe_code=$(sed -n 's/^terminal_http_code=//p' "$TERM_PROBE" 2>/dev/null \
                 | head -1)
    probe_bytes=$(sed -n 's/^terminal_bytes=//p' "$TERM_PROBE" 2>/dev/null | head -1)
    if [ "$probe_code" = "200" ] && [ "${probe_bytes:-0}" -gt 10000 ]; then
        printf '  %-34s %s bytes over the LAN\n' \
               "GET /terminal from this host" "$probe_bytes"
    else
        printf '  %-34s http_code=%s bytes=%s\n' \
               "GET /terminal from this host" "${probe_code:-none}" \
               "${probe_bytes:-0}"
        bad=1
    fi
fi

# The one that shipped.  0.17.0 and 0.17.1 deadlocked in bsd_address_changed()
# the moment a DHCP lease arrived, so AddNetInterface never returned from its
# OpenLibrary(), S:User-Startup never finished, and the machine stopped part
# way through its Startup-Sequence.  BOOT_STATUS 124 is that exact shape: the
# guest wrote no DH0:.done because it never got to the end of the boot.
#
# Said separately from the four above because "the machine did not finish
# booting" and "a command returned nonzero" are different failures, and the
# table alone reads as four things that merely did not run.
USER_BOOT_STATUS=$BOOT_STATUS
if [ "$BOOT_STATUS" = "124" ]; then
    echo
    echo "  !! the machine did not finish booting: no DH0:.done after ${BOOT_TIMEOUT}s."
    echo "     S:User-Startup runs AddNetInterface, which opens bsdsocket.library,"
    echo "     which brings the stack up and waits for DHCP.  A hang here is that"
    echo "     path.  CONFIGURE=STATIC in DEVS:NetInterfaces/eth0 tells the lease"
    echo "     apart from the driver and the library open."
fi

# ------------------------------------------------------------------ run 3 ---
#
# THE OTHER DIRECTION.  An installer that adds a line and cannot take it away
# again is not idempotent, it is cumulative, and the only way to find out is to
# install a third time on this same machine and answer the question the other
# way.  Nothing is re-staged: this is the drive the user already has, with
# everything the first two runs put on it.

if [ "$TERMINAL" = "1" ]; then
    echo
    echo "============================================================"
    echo "  3/3  installing again, answering the terminal question no"
    echo "============================================================"

    build_driver "$ROOT/build/installdrive-wb-$LEVEL-no" 1 ""
    cp "$ROOT/build/installdrive-wb-$LEVEL-no" "$HD/C/installdrive"
    chmod 755 "$HD/C/installdrive"

    startup_with 'FailAt 9999
C:installdrive >DH0:install-console.txt
Echo >DH0:.done "$RC"' nouserstartup

    boot uninstall "$INSTALL_TIMEOUT"
    if [ "$BOOT_STATUS" != "0" ]; then
        echo "!! the third install did not finish (status $BOOT_STATUS)"
        bad=1
    fi

    AFTER_HTTPD=$(startup_count 'httpd')
    AFTER_ASSIGN=$(startup_count 'Assign AmiNetXDuo:')
    AFTER_IFACE=$(startup_count 'AddNetInterface')
    AFTER_FOREIGN=no
    foreign_intact && AFTER_FOREIGN=yes

    echo
    echo "---- S:User-Startup after answering no ----"
    cat "$(user_startup)" 2>/dev/null || echo "(none)"
    echo
    echo "startup_httpd_lines=$AFTER_HTTPD"
    echo "startup_assign_lines=$AFTER_ASSIGN"
    echo "startup_addnetinterface_lines=$AFTER_IFACE"
    echo "startup_foreign_lines_intact=$AFTER_FOREIGN"

    if [ "$AFTER_HTTPD" != "0" ] || [ "$AFTER_ASSIGN" != "0" ]; then
        printf '  %-34s httpd=%s assign=%s\n' \
               "answering no removes both lines" "$AFTER_HTTPD" "$AFTER_ASSIGN"
        bad=1
    else
        printf '  %-34s both gone\n' "answering no removes both lines"
    fi
    if [ "$AFTER_IFACE" != "1" ]; then
        printf '  %-34s %s\n' \
               "AddNetInterface still there, once" "$AFTER_IFACE"
        bad=1
    else
        printf '  %-34s yes\n' "AddNetInterface still there, once"
    fi
    if [ "$AFTER_FOREIGN" != "yes" ]; then
        printf '  %-34s NO\n' "somebody else's lines intact"
        bad=1
    else
        printf '  %-34s yes\n' "somebody else's lines intact"
    fi
fi

echo
if [ "$bad" = "0" ] && [ "$USER_BOOT_STATUS" != "124" ]; then
    echo "==> PASS: a real Workbench 3.1, installed from the archive, does all four"
    [ "$KEEP" = "1" ] && echo "    (the drive is at $HD)"
    exit 0
fi
echo "==> FAIL: see the table above; the drive is left at $HD"
echo "    Nothing here is adjusted to make it pass, the failure IS the result."
exit 1
