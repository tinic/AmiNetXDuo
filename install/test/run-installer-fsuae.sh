#!/usr/bin/env bash
#
# Install AmiNetXDuo onto a bare Amiga, then boot it and see if the network
# comes up from nothing but what the installer wrote.
#
#   install/test/run-installer-fsuae.sh [-b BUILDDIR] [-t SECONDS] [-c CPU]
#                                       [-l NOVICE|AVERAGE|EXPERT|RERUN]
#
# Two FS-UAE runs against the same staging directory:
#
#   1. A clean machine -- an empty LIBS:, a DEVS: holding only the card's
#      driver, an S: with nothing in it -- plus the unpacked distribution
#      archive and Commodore's Installer.  installdrive starts the Installer
#      on Install-AmiNetXDuo and clicks Proceed until it finishes.
#   2. The same machine, now installed, with the emulated A2065 attached to
#      SLIRP.  bootcheck reads S:User-Startup, runs whatever the installer
#      put in it, and then pings the gateway.
#
# Because FS-UAE mounts the staging directory as a real hard drive, whatever
# the Installer writes lands on the host and can be read afterwards -- which
# is what makes this a test of the generated files and not just of the
# script's syntax.
#
# The Installer is not ours to ship.  Point AMINETXDUO_INSTALLER at a copy
# (it is on the Workbench 3.1 Install disk, in the root of the volume), or
# drop one in build/Installer.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="${AMINETXDUO_BUILD:-build/cm}"
TIMEOUT=300
CPU=""
KEEP=0
LEVEL=NOVICE

while getopts "b:t:c:l:k" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        l) LEVEL="$OPTARG" ;;
        k) KEEP=1 ;;
        *) echo "usage: $0 [-b builddir] [-t seconds] [-c cpu]" \
                "[-l NOVICE|AVERAGE|EXPERT|RERUN] [-k]" >&2
           exit 2 ;;
    esac
done

SKIP_BOOT=0
case "$LEVEL" in
    NOVICE|AVERAGE|EXPERT) DRIVE_FLAGS=(-DDRIVE_LEVEL="\"$LEVEL\"") ;;
    # RERUN installs twice over itself: the second pass is the one that has
    # to notice an existing configuration and leave it alone.
    RERUN)  DRIVE_FLAGS=(-DDRIVE_LEVEL='"NOVICE"' -DDRIVE_RUNS=2) ;;
    # STATIC answers "no" to the DHCP question, which is the only route to
    # the four validated address prompts and to a static interface file.
    # The address it then writes is 192.168.1.10, which is not on the
    # emulator's 10.0.2.0/24 SLIRP network -- so this one checks the files
    # and stops there rather than pretending a ping should work.
    STATIC) DRIVE_FLAGS=(-DDRIVE_LEVEL='"AVERAGE"' -DDRIVE_NO_ON_YESNO=1)
            SKIP_BOOT=1 ;;
    *) echo "unknown user level: $LEVEL" >&2; exit 2 ;;
esac

case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

GCC="${AMIGA_GCC:-$HOME/amigaos/tools/m68k-amigaos-gcc/bin/m68k-amigaos-gcc}"
NDK="${AMIGA_NDK:-$HOME/amigaos/tools/m68k-amigaos-gcc/m68k-amigaos/ndk-include}"

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
    echo "It is in the root of the Workbench 3.1 Install disk." >&2
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

# The distribution tree is what the user would actually unpack.
#
# One CPU drawer, not the release's three: this boots a single emulated A1200,
# so the other two would be built and packed to be ignored.  The installer's
# CPU pick is still exercised -- it has to resolve 68020 to Libs/68020-40 and
# find the library there, and it aborts if it does not.
AMINETXDUO_DIST_CPUS="68020-40" \
"$ROOT/dist/make-dist.sh" -b "$BUILD" >/dev/null || {
    echo "dist/make-dist.sh failed" >&2
    exit 2
}
DIST="$ROOT/build/dist/AmiNetXDuo"

echo "==> building installdrive ($LEVEL) and bootcheck"
DRIVER="$ROOT/build/installdrive-$LEVEL"
"$GCC" -O2 -m68020 -Wall -Wextra "${DRIVE_FLAGS[@]}" -I"$NDK" \
       -o "$DRIVER" "$ROOT/install/test/installdrive.c" || exit 2
"$GCC" -O2 -m68020 -Wall -Wextra -I"$NDK" \
       -o "$ROOT/build/bootcheck" "$ROOT/install/test/bootcheck.c" || exit 2

# ---------------------------------------------------------------- staging --
#
# A machine with nothing on it: LIBS: empty, DEVS: holding only the card
# driver, S: empty.  The harness itself supplies s/ and c/.

STAGE="$ROOT/build/installer-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs" "$STAGE/Unpacked"
cp "$A2065" "$STAGE/devs/a2065.device"

# The archive goes in DH0:Unpacked/, not DH0:, so that the drawer the
# installer creates (SYS:AmiNetXDuo, since a machine with no Work: partition
# gets the boot volume as @default-dest) is not the drawer it is reading
# from.
cp -R "$DIST" "$STAGE/Unpacked/AmiNetXDuo"
cp "$INSTALLER" "$STAGE/Unpacked/AmiNetXDuo/Installer"
chmod 755 "$STAGE/Unpacked/AmiNetXDuo/Installer"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-installer-$LEVEL}"
HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

# --------------------------------------------------------------- run one ---

echo "============================================================"
echo "  1/2  installing onto a bare machine, user level $LEVEL"
echo "============================================================"

set +e
"$ROOT/tools/fsuae-run.sh" -t "$TIMEOUT" "${CPUARG[@]}" \
    "$DRIVER" \
    "$STAGE/libs" "$STAGE/devs" "$STAGE/Unpacked"
INSTALL_STATUS=$?
set -e

echo
echo "---- installdrive report ----"
cat "$HD/installdrive.txt" 2>/dev/null || echo "(none)"

echo
echo "---- Installer stdout ----"
cat "$HD/installer-out.txt" 2>/dev/null || echo "(none)"

echo
echo "---- Installer log ----"
cat "$HD/install-log.txt" 2>/dev/null || echo "(none written)"

# ---------------------------------------------------- what did it write? ---

echo
echo "============================================================"
echo "  what the installer put on the disk"
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

check_file libs/bsdsocket.library
check_file libs/usergroup.library
for cmd in AddNetInterface Online Offline ShowNetStatus ping netstat host; do
    check_file "c/$cmd"
done
check_file devs/NetInterfaces/eth0
check_file devs/Internet/routes
check_file devs/Internet/name_resolution
check_file devs/Internet/hosts
check_file devs/Internet/protocols
check_file devs/Internet/services
check_file devs/Internet/networks
check_file s/User-Startup

for f in devs/NetInterfaces/eth0 devs/Internet/routes \
         devs/Internet/name_resolution s/User-Startup; do
    [ -f "$HD/$f" ] || continue
    echo
    echo "---- $f ----"
    cat "$HD/$f"
done

if [ "$INSTALL_STATUS" != "0" ] || [ "$fail" != "0" ]; then
    echo
    echo "!! the install run did not complete cleanly"
    echo "   installdrive exit status: $INSTALL_STATUS"
    [ "$KEEP" = "1" ] || echo "   (staging left in $HD)"
    exit 1
fi

if [ "$SKIP_BOOT" = "1" ]; then
    echo
    echo "============================================================"
    echo "  static configuration check"
    echo "============================================================"
    bad=0
    want() {
        if grep -qx "$2" "$HD/$1"; then
            printf '  ok      %-34s %s\n' "$1" "$2"
        else
            printf '  WRONG   %-34s expected %s\n' "$1" "$2"
            bad=1
        fi
    }
    want devs/NetInterfaces/eth0      "CONFIGURE=STATIC"
    want devs/NetInterfaces/eth0      "ADDRESS=192.168.1.10"
    want devs/NetInterfaces/eth0      "NETMASK=255.255.255.0"
    want devs/Internet/routes         "DEFAULT=192.168.1.1"
    want devs/Internet/name_resolution "nameserver 192.168.1.1"
    want devs/Internet/name_resolution "hostname amiga"
    if [ "$bad" = "0" ]; then
        echo
        echo "==> PASS: the static configuration is what was asked for"
        exit 0
    fi
    echo
    echo "==> FAIL: the static configuration is not what was asked for"
    exit 1
fi

# --------------------------------------------------------------- run two ---
#
# Boot the machine the installer just made.  The staging directory that
# fsuae-run.sh built for run one *is* that machine, so it is handed straight
# back as the extras for run two -- nothing is re-staged, and in particular
# nothing this script knows is written into DEVS: between the two runs.

echo
echo "============================================================"
echo "  2/2  booting it, with an A2065 on SLIRP"
echo "============================================================"

# tools/fsuae-run.sh cannot be used here.  It wipes and re-creates the
# staging drive and writes its own s/Startup-Sequence, and the whole point of
# this run is to boot the machine run one produced, in place and untouched
# apart from the boot script.  So the emulator is driven directly, using the
# same lock directory so that runs from other work still serialise.
#
# Everything else -- LIBS:, C:, DEVS:, S:User-Startup -- is exactly what the
# Installer left.

KICKSTART="${AMINETXDUO_KICKSTART:-}"
if [ -z "$KICKSTART" ]; then
    for candidate in \
        "$HOME/Downloads/Kickstart v3.1 rev 40.68 (1993)(Commodore)(A1200)[!].rom" \
        "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A1200)[!].rom" \
        "$HOME/png2amiga_testing/kick31.rom"
    do
        [ -f "$candidate" ] && { KICKSTART="$candidate"; break; }
    done
fi
[ -n "$KICKSTART" ] || { echo "No Kickstart ROM; set AMINETXDUO_KICKSTART" >&2
                         exit 2; }
FSUAE="${FSUAE:-$(command -v fs-uae || true)}"
[ -n "$FSUAE" ] || { echo "fs-uae not found" >&2; exit 2; }

cp "$ROOT/build/bootcheck" "$HD/c/bootcheck"
rm -f "$HD/.done" "$HD/stdout.txt"

# The one thing this run stages: a boot script that runs bootcheck.  It does
# NOT execute S:User-Startup itself -- bootcheck reads that file and runs the
# commands out of it, so that a startup line the installer got wrong shows up
# as a failure rather than being papered over by the harness.
cat > "$HD/s/Startup-Sequence" <<'EOF'
failat 9999
c:envsetup
c:bootcheck >DH0:stdout.txt
echo >DH0:.done "$RC"
EOF

LOCKDIR="$ROOT/build/.fsuae.lock"
lock_held=0
if [ "${AMINETXDUO_NO_LOCK:-0}" != "1" ]; then
    waited=0
    while ! mkdir "$LOCKDIR" 2>/dev/null; do
        owner=$(cat "$LOCKDIR/pid" 2>/dev/null || echo "")
        if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then
            rm -rf "$LOCKDIR" 2>/dev/null || true
            continue
        fi
        [ "$waited" = 0 ] && echo "==> another run holds the emulator; queueing"
        sleep 5
        waited=$((waited + 5))
        [ "$waited" -lt 2400 ] || break
    done
    [ -d "$LOCKDIR" ] && { echo $$ > "$LOCKDIR/pid"; lock_held=1; }
fi

BOOTCFG="$ROOT/build/installer-boot.fs-uae"
BOOTSERIAL="$ROOT/build/serial-installer-boot.log"
BOOTBASE="$ROOT/build/fsuae-base-installer-boot"
mkdir -p "$BOOTBASE"
: > "$BOOTSERIAL"

cat > "$BOOTCFG" <<EOF
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
base_dir = $BOOTBASE
amiga_model = A1200
kickstart_file = $KICKSTART
hard_drive_0 = $HD
hard_drive_0_label = DH0
fast_memory = 8192
serial_port = $BOOTSERIAL
fullscreen = 0
network_card = a2065
uae_a2065 = slirp
EOF
[ -z "$CPU" ] || echo "cpu = $CPU" >> "$BOOTCFG"

export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"

FSUAE_PID=""
cleanup_boot() {
    [ -n "$FSUAE_PID" ] && kill -TERM "$FSUAE_PID" 2>/dev/null || true
    [ "$lock_held" = "1" ] && rm -rf "$LOCKDIR" 2>/dev/null || true
}
trap cleanup_boot EXIT INT TERM HUP

"$FSUAE" "$BOOTCFG" >"$ROOT/build/fsuae-installer-boot.log" 2>&1 &
FSUAE_PID=$!

BOOT_STATUS=124
elapsed=0
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    if [ -f "$HD/.done" ]; then
        BOOT_STATUS=$(tr -dc '0-9' < "$HD/.done" | head -c 4)
        BOOT_STATUS=${BOOT_STATUS:-0}
        break
    fi
    kill -0 "$FSUAE_PID" 2>/dev/null || { echo "!! fs-uae exited early" >&2
                                          break; }
    sleep 1
    elapsed=$((elapsed + 1))
done

kill -TERM "$FSUAE_PID" 2>/dev/null || true
wait "$FSUAE_PID" 2>/dev/null || true
FSUAE_PID=""

echo
echo "---- serial ----"
cat "$BOOTSERIAL" 2>/dev/null | tail -60 || true

echo
echo "---- bootcheck report ----"
cat "$HD/bootcheck.txt" 2>/dev/null || echo "(none)"

echo
if [ "$BOOT_STATUS" = "0" ]; then
    echo "==> PASS: the network came up from what the installer wrote"
else
    echo "==> FAIL: bootcheck exit status $BOOT_STATUS"
fi
exit "$BOOT_STATUS"
