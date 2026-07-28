#!/usr/bin/env bash
#
# Install the release archive on a REAL Workbench 3.1, reboot, and then use
# the machine the way its owner would.
#
#   install/test/run-workbench-fsuae.sh [-b BUILDDIR] [-a ARCHIVE.lha]
#                                       [-l NOVICE|AVERAGE|EXPERT]
#                                       [-t SECONDS] [-T SECONDS] [-k]
#
# WHY THIS EXISTS, given that run-installer-fsuae.sh already installs and
# boots.  That harness stages the machine itself: it makes an empty LIBS:, a
# DEVS: holding one driver and an S: with nothing in it, boots a bare
# directory hard drive with no Workbench on it at all, and drives its own
# boot script.  Every failure a user has reported so far -- `fetch https://`
# not working, a command not seeing its arguments -- happened on a real
# Workbench and was invisible here, because "works on the staging tree" and
# "works on a Workbench" are different claims and only the second one is the
# product.
#
# So this one does the whole thing end to end:
#
#   0. assembles a genuine Workbench 3.1 SYS: out of Commodore's five
#      floppies (workbench, fonts, locale, storage, extras) and caches it;
#   1. boots it -- SetPatch, IPrefs, LoadWB, the real Startup-Sequence;
#   2. unpacks the release .lha into DH0:Unpacked/ the way a download
#      would arrive, and runs Install-AmiNetXDuo through Commodore's
#      Installer with installdrive clicking Proceed;
#   3. POWER CYCLES the machine.  The second boot runs the stock 3.1
#      Startup-Sequence untouched, which reaches `Execute S:User-Startup`
#      and starts the network from the line the installer wrote -- nothing
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
#     install/test/run-workbench-fsuae.sh -a /tmp/rel/AmiNetXDuo-0.8.1.lha
#
# Without it the archive is built here, from this machine's toolchain, and a
# defect that lives in the RELEASE build -- a different compiler, a different
# crt0 -- cannot show up.  Run it both ways before believing a release.
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

while getopts "b:a:l:t:T:k" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        a) ARCHIVE="$OPTARG" ;;
        l) LEVEL="$OPTARG" ;;
        t) INSTALL_TIMEOUT="$OPTARG" ;;
        T) BOOT_TIMEOUT="$OPTARG" ;;
        k) KEEP=1 ;;
        *) echo "usage: $0 [-b builddir] [-a archive.lha]" \
                "[-l NOVICE|AVERAGE|EXPERT] [-t seconds] [-T seconds] [-k]" >&2
           exit 2 ;;
    esac
done

case "$LEVEL" in
    NOVICE|AVERAGE|EXPERT) ;;
    *) echo "unknown user level: $LEVEL" >&2; exit 2 ;;
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
KICKSTART="${AMINETXDUO_KICKSTART:-}"
if [ -z "$KICKSTART" ]; then
    for candidate in \
        "$HOME/Downloads/Kickstart v3.1 rev 40.68 (1993)(Commodore)(A1200)[!].rom" \
        "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A1200)[!].rom" \
        "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A4000).rom" \
        "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A3000).rom" \
        "$HOME/png2amiga_testing/kick31.rom"
    do
        [ -f "$candidate" ] && { KICKSTART="$candidate"; break; }
    done
fi
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    echo "No Kickstart 3.1 ROM; set AMINETXDUO_KICKSTART=<path>." >&2
    exit 2
}

FSUAE="${FSUAE:-$(command -v fs-uae || true)}"
[ -n "$FSUAE" ] || { echo "fs-uae not found; set FSUAE=<path>" >&2; exit 2; }

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
    echo "lha not found -- needed to unpack the release archive on the host." >&2
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
        echo "amitools' xdftool not found -- needed to unpack the Workbench ADFs." >&2
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
    # mode bits, and xdftool unpacks everything 0644 -- which would leave
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
        echo "dist/make-dist.sh failed -- see build/wb31-make-dist.log" >&2
        tail -20 "$ROOT/build/wb31-make-dist.log" >&2
        exit 2
    }
    VERSION=$("$ROOT/tools/version.sh" --product)
    ARCHIVE="$ROOT/build/dist/AmiNetXDuo-$VERSION.lha"
fi
[ -f "$ARCHIVE" ] || { echo "no such archive: $ARCHIVE" >&2; exit 2; }
echo "==> archive $(basename "$ARCHIVE") ($(wc -c < "$ARCHIVE" | tr -d ' ') bytes)"

# ------------------------------------------------------------ the machine --

echo "==> building installdrive ($LEVEL)"
DRIVER="$ROOT/build/installdrive-wb-$LEVEL"
"$GCC" -O2 -m68020 -Wall -Wextra -DDRIVE_LEVEL="\"$LEVEL\"" -I"$NDK" \
       -o "$DRIVER" "$ROOT/install/test/installdrive.c" || exit 2

rm -rf "$HD"
mkdir -p "$HD"
cp -R "$WB/." "$HD/"
cp "$A2065" "$HD/Devs/a2065.device"
cp "$DRIVER" "$HD/C/installdrive"
chmod 755 "$HD/Devs/a2065.device" "$HD/C/installdrive"

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
# have lying around -- so the difference between "the product is broken" and
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
# tools/fsuae-run.sh cannot drive these runs: it wipes the staging drive and
# writes its own Startup-Sequence, and the whole point here is a machine that
# boots Commodore's.  So the emulator is started directly, with the same
# config that harness generates -- and with its lock, in its measurement lane,
# so a run here does not share the host with anything else.

LOCKDIR="$ROOT/build/.fsuae.lock"
SLOTDIR="$ROOT/build/.fsuae.slots"
PERFWAIT="$ROOT/build/.fsuae.perfwait"
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

FSUAE_PID=""
cleanup() {
    if [ -n "$FSUAE_PID" ]; then
        kill -TERM "$FSUAE_PID" 2>/dev/null || true
        sleep 1
        kill -KILL "$FSUAE_PID" 2>/dev/null || true
        FSUAE_PID=""
    fi
    release_lock
}
trap cleanup EXIT INT TERM HUP

export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"

# One boot of the machine as it stands.  $1 names the run, $2 is the timeout,
# $3 is "net" to attach the A2065 to SLIRP.  Returns the guest's own exit
# status out of DH0:.done, or 124.
BOOT_STATUS=0
boot() {
    local name="$1" timeout="$2" net="${3:-}"
    local cfg="$ROOT/build/wb31-$name.fs-uae"
    local serial="$ROOT/build/serial-wb31-$name.log"
    local base="$ROOT/build/fsuae-base-wb31-$name"
    local elapsed=0

    mkdir -p "$base"
    : > "$serial"
    rm -f "$HD/.done"

    cat > "$cfg" <<EOF
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
base_dir = $base
amiga_model = $MODEL
kickstart_file = $KICKSTART
hard_drive_0 = $HD
hard_drive_0_label = DH0
fast_memory = 8192
serial_port = $serial
fullscreen = 0
EOF
    if [ "$net" = "net" ]; then
        cat >> "$cfg" <<EOF
network_card = a2065
uae_a2065 = slirp
EOF
    fi

    echo "==> booting ($name, timeout ${timeout}s)"
    # SIGPIPE ignored for the same reason tools/fsuae-run.sh ignores it: SLIRP
    # writes guest payload to host sockets without MSG_NOSIGNAL, so a peer that
    # hangs up first otherwise kills the emulator and it looks like a guru.
    ( trap '' PIPE; exec "$FSUAE" "$cfg" ) >"$ROOT/build/fsuae-wb31-$name.log" 2>&1 &
    FSUAE_PID=$!

    BOOT_STATUS=124
    while [ "$elapsed" -lt "$timeout" ]; do
        if [ -f "$HD/.done" ]; then
            BOOT_STATUS=$(tr -dc '0-9' < "$HD/.done" | head -c 4)
            BOOT_STATUS=${BOOT_STATUS:-0}
            break
        fi
        kill -0 "$FSUAE_PID" 2>/dev/null || {
            echo "!! fs-uae exited early after ${elapsed}s" >&2
            break
        }
        sleep 1
        elapsed=$((elapsed + 1))
    done

    kill -TERM "$FSUAE_PID" 2>/dev/null || true
    wait "$FSUAE_PID" 2>/dev/null || true
    FSUAE_PID=""

    echo "    ($name finished after ${elapsed}s, status $BOOT_STATUS)"
    if [ -s "$serial" ]; then
        echo "---- serial ----"
        tail -40 "$serial"
    fi
}

# The stock 3.1 Startup-Sequence, with the tail replaced.  LoadWB stays --
# the Installer draws on the Workbench screen and a user has one -- and only
# `EndCLI`, which would take the boot shell away before our line runs, goes.
STARTUP_SUM=""
startup_with() {
    local tail_cmds="$1"
    sed -e '/^EndCLI/d' "$WB/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
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
    if [ -f "$HD/$1" ]; then
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
    if [ -f "$HD/$f" ]; then
        printf '  ok      %-32s %s bytes\n' "$f" "$(wc -c < "$HD/$f" | tr -d ' ')"
    else
        printf '  ABSENT  %s\n' "$f"
    fi
done

echo
echo "---- S:User-Startup ----"
cat "$HD/S/User-Startup" 2>/dev/null || echo "(none)"
echo "---- DEVS:NetInterfaces/eth0 ----"
cat "$HD/Devs/NetInterfaces/eth0" 2>/dev/null || echo "(none)"

if [ "$INSTALL_STATUS" != "0" ] || [ "$fail" != "0" ]; then
    echo
    echo "!! the install run did not complete cleanly (status $INSTALL_STATUS)"
    echo "   the drive is left at $HD"
    exit 1
fi

# ------------------------------------------------------------------ run 2 ---
#
# A POWER CYCLE, not a continuation.  The Startup-Sequence is put back to
# Commodore's -- the installer's work has to be reached through its own
# `Execute S:User-Startup`, exactly as on the user's machine -- and the only
# thing added is the line that runs the checks afterwards.

echo
echo "============================================================"
echo "  2/2  rebooting, and using the machine as its owner would"
echo "============================================================"

if [ "$(shasum "$HD/S/Startup-Sequence" | cut -d' ' -f1)" != "$STARTUP_SUM" ]; then
    echo "note: the installer also changed S:Startup-Sequence -- diff against"
    echo "      the stock 3.1 one is worth reading before the reboot"
fi

# An ordinary Shell script, doing ordinary things, with every command's return
# code written down beside its output.  `Stack 200000` is the Shell's internal
# stack command.  It is NOT needed any more -- clients/compat/amiga_argv.c
# swaps in 256 KB of its own before main() runs, and the ReadMe says so -- and
# it stays here precisely because a cautious user will still type it: a client
# that mishandled an already-large Shell stack would fail nowhere else.
cat > "$HD/S/AmiNetXDuo-Check" <<EOF
; Written by install/test/run-workbench-fsuae.sh.  Nothing here is installed
; by AmiNetXDuo -- it is what a user would type.
FailAt 9999
Stack 200000

Echo >DH0:usercheck.txt "=== 1. the network, as S:User-Startup brought it up"
C:Wait 5
C:ShowNetStatus >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT network rc=\$RC"

Echo >>DH0:usercheck.txt "*N=== 2. fetch http://example.com/"
C:fetch http://example.com/ TO DH0:http-body.txt >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT fetch-http rc=\$RC"

Echo >>DH0:usercheck.txt "*N=== 3. fetch https://tls-v1-2.badssl.com/"
C:fetch https://tls-v1-2.badssl.com/ TO DH0:https-body.txt >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT fetch-https rc=\$RC"

Echo >>DH0:usercheck.txt "*N=== 4. arp -- what answered on this network"
C:arp >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT arp rc=\$RC"

Echo >>DH0:usercheck.txt "*N=== done"
EOF
chmod 755 "$HD/S/AmiNetXDuo-Check"

startup_with 'FailAt 9999
Execute S:AmiNetXDuo-Check >DH0:check-console.txt
Echo >DH0:.done "$RC"'

rm -f "$HD/usercheck.txt" "$HD/http-body.txt" "$HD/https-body.txt"
boot boot "$BOOT_TIMEOUT" net

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
report() {
    local label="$1" name="$2" rc
    rc=$(sed -n "s/^RESULT $name rc=\(.*\)/\1/p" "$HD/usercheck.txt" 2>/dev/null \
         | head -1)
    if [ -z "$rc" ]; then
        printf '  %-34s NEVER RAN\n' "$label"
        bad=1
    elif [ "$rc" = "0" ]; then
        printf '  %-34s rc=0\n' "$label"
    else
        printf '  %-34s rc=%s\n' "$label" "$rc"
        bad=1
    fi
}

report "ShowNetStatus"                 network
report "fetch http://example.com/"     fetch-http
report "fetch https://...badssl.com/"  fetch-https
report "arp"                           arp

echo
if [ "$bad" = "0" ] && [ "$BOOT_STATUS" != "124" ]; then
    echo "==> PASS: a real Workbench 3.1, installed from the archive, does all four"
    [ "$KEEP" = "1" ] && echo "    (the drive is at $HD)"
    exit 0
fi
echo "==> FAIL: see the table above; the drive is left at $HD"
echo "    Nothing here is adjusted to make it pass -- the failure IS the result."
exit 1
