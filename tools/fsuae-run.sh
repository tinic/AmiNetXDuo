#!/usr/bin/env bash
#
# Run an AmigaOS executable under FS-UAE and capture its output.
#
#   tools/fsuae-run.sh [-t SECONDS] [-m MODEL] [-n] <executable> [extra files...]
#
# -n attaches an emulated Commodore A2065 Ethernet card wired to FS-UAE's
# SLIRP user-mode NAT (10.0.2.0/24, gateway and DHCP/DNS server 10.0.2.2).
# The Amiga side then needs DEVS:a2065.device and a DEVS:NetInterfaces file;
# stage both by passing the directories as extra files.
#
# How it works:
#   * The executable and any extra files are staged into build/testhd/, which is
#     attached as a *directory* hard drive. Because it is a host directory,
#     anything the Amiga writes to DH0: lands straight back on the host -- so a
#     test can report results simply by writing a file.
#   * s/Startup-Sequence runs the executable, then writes DH0:.done so the host
#     knows the run finished rather than guessing from a timer.
#   * ami_log() output goes to the serial debug port, which FS-UAE writes to
#     build/serial.log (see src/common/compat.c).
#   * The host polls for .done, then kills the emulator.
#
# Exit status is the test's own, read from DH0:.done, or 124 on timeout.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TIMEOUT=60
MODEL=A1200
NETWORK=0
CPU=""

while getopts "t:m:c:n" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        n) NETWORK=1 ;;
        *) echo "usage: $0 [-t seconds] [-m model] [-c cpu] [-n] <executable> [files...]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

[ $# -ge 1 ] || { echo "usage: $0 [-t seconds] [-m model] <executable> [files...]" >&2; exit 2; }

EXE="$1"; shift
[ -f "$EXE" ] || { echo "no such executable: $EXE" >&2; exit 2; }
EXE_NAME=$(basename "$EXE")

# ---------------------------------------------------------------- kickstart --

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
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    echo "No Kickstart 3.1 ROM found. Set AMINETXDUO_KICKSTART=<path>." >&2
    exit 2
}

FSUAE="${FSUAE:-$(command -v fs-uae || true)}"
[ -n "$FSUAE" ] || { echo "fs-uae not found; set FSUAE=<path>" >&2; exit 2; }

# ------------------------------------------------------------------- staging --

# AMINETXDUO_RUN_TAG isolates a run: the staging drive, the serial log and the
# emulator config all get the suffix. Without it two runs started at the same
# time (two agents, two shells) share build/testhd and clobber each other.
TAG="${AMINETXDUO_RUN_TAG:-}"
[ -z "$TAG" ] || TAG="-$TAG"

HD="$ROOT/build/testhd$TAG"
SERIAL="$ROOT/build/serial$TAG.log"
rm -rf "$HD"
mkdir -p "$HD/s" "$HD/c" "$ROOT/build"
: > "$SERIAL"

cp "$EXE" "$HD/$EXE_NAME"
cp "$EXE" "$HD/c/$EXE_NAME"
for extra in "$@"; do
    cp -R "$extra" "$HD/"
done

# We boot a bare directory hard drive, so none of the assigns a normal Workbench
# boot makes exist -- ENV:, T:, ENVARC:, CLIPS:. Anything calling GetVar()/
# SetVar() fails without them. envsetup makes them via dos.library, which keeps
# the harness self-contained (no Workbench binaries to extract or stage).
# ENV: and T: are backed by host directories, so variables a test sets survive
# the run and can be inspected afterwards -- and pre-seeded by staging env/.
ENVSETUP="$ROOT/build/envsetup"
if [ ! -x "$ENVSETUP" ] || [ "$ROOT/tools/envsetup/envsetup.c" -nt "$ENVSETUP" ]; then
    GCC="${AMIGA_GCC:-$HOME/amigaos/tools/m68k-amigaos-gcc/bin/m68k-amigaos-gcc}"
    NDK="${AMIGA_NDK:-$HOME/amigaos/tools/m68k-amigaos-gcc/m68k-amigaos/ndk-include}"
    "$GCC" -O2 -m68020 -I"$NDK" -o "$ENVSETUP" "$ROOT/tools/envsetup/envsetup.c" \
        || { echo "failed to build envsetup" >&2; exit 2; }
fi
cp "$ENVSETUP" "$HD/c/envsetup"
mkdir -p "$HD/env" "$HD/envarc" "$HD/t" "$HD/clips"

# The boot shell has no C: beyond what we stage, so keep this to builtins plus
# what we put in c/. Echo is internal to the 3.1 shell.
#
# stdout is redirected to a file on the host so Printf() output from a test is
# visible after the run -- otherwise it only ever reaches the emulator's console
# window and is lost.
# failat is essential: AmigaDOS aborts a script as soon as a command returns a
# code at or above the fail level (10 by default), so without it any test that
# exits nonzero never reaches the line that records its status -- the run just
# times out and the failure looks like a hang.
cat > "$HD/s/Startup-Sequence" <<EOF
failat 9999
c:envsetup
$EXE_NAME >DH0:stdout.txt
echo >DH0:.done "\$RC"
EOF

# ------------------------------------------------------------------ running --

# Every fs-uae instance otherwise shares ~/FS-UAE for its cache, logs, save
# states and floppy overlays, so two concurrent runs fight over those files and
# one of them quits early -- which looks exactly like a crash in the code under
# test. Give each run a private base directory.
# --------------------------------------------------------------- serialise --
#
# Concurrent fs-uae instances interfere even with per-run base_dir isolation:
# three separate workstreams independently reported runs dying with a premature
# uae_quit, and in every case the run that died shared the machine with another.
# The symptom is indistinguishable from a crash in the code under test, which
# makes it expensive -- an agent chases a phantom bug instead of its own work.
#
# Runs therefore queue on an exclusive lock. A directory is the lock, because
# mkdir is atomic everywhere and macOS ships no flock(1). The owning PID is
# recorded so a lock left by a killed run can be reclaimed rather than wedging
# the queue forever.
#
# AMINETXDUO_NO_LOCK=1 opts out; AMINETXDUO_LOCK_WAIT caps the wait (default
# 2400s -- a conformance run plus boot approaches five minutes and several may
# be queued ahead).
LOCKDIR="$ROOT/build/.fsuae.lock"
LOCK_WAIT="${AMINETXDUO_LOCK_WAIT:-2400}"
LOCK_HELD=0

release_lock() {
    [ "$LOCK_HELD" = "1" ] || return 0
    rm -rf "$LOCKDIR" 2>/dev/null || true
    LOCK_HELD=0
}

if [ "${AMINETXDUO_NO_LOCK:-0}" != "1" ]; then
    mkdir -p "$ROOT/build"
    waited=0
    while ! mkdir "$LOCKDIR" 2>/dev/null; do
        owner=$(cat "$LOCKDIR/pid" 2>/dev/null || echo "")
        if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then
            echo "==> reclaiming lock from dead pid $owner" >&2
            rm -rf "$LOCKDIR" 2>/dev/null || true
            continue
        fi
        if [ "$waited" -ge "$LOCK_WAIT" ]; then
            echo "!! waited ${waited}s for the emulator lock; proceeding anyway" >&2
            break
        fi
        [ "$waited" = 0 ] && echo "==> another run holds the emulator; queueing"
        sleep 5
        waited=$((waited + 5))
    done
    if [ -d "$LOCKDIR" ]; then
        echo $$ > "$LOCKDIR/pid" 2>/dev/null || true
        LOCK_HELD=1
    fi
fi

FSUAE_BASE="$ROOT/build/fsuae-base$TAG"
mkdir -p "$FSUAE_BASE"

CFG="$ROOT/build/test$TAG.fs-uae"
cat > "$CFG" <<EOF
[fs-uae]
floppy_drive_volume = 0
floppy_drive_volume_empty = 0
base_dir = $FSUAE_BASE
amiga_model = $MODEL
kickstart_file = $KICKSTART
hard_drive_0 = $HD
hard_drive_0_label = DH0
fast_memory = 8192
serial_port = $SERIAL
fullscreen = 0
EOF

# -c 68030 etc. The project floor is a 68020 (A1200), but a full 68030 has an
# MMU -- which is what Enforcer needs -- and different cache behaviour, so the
# same binaries must be exercised on both.
if [ -n "$CPU" ]; then
    cat >> "$CFG" <<EOF
cpu = $CPU
EOF
    echo "==> CPU override: $CPU"
fi

if [ "$NETWORK" = "1" ]; then
    cat >> "$CFG" <<EOF
network_card = a2065
uae_a2065 = slirp
EOF
    echo "==> A2065 + SLIRP attached (10.0.2.0/24, gateway 10.0.2.2)"
fi

echo "==> $EXE_NAME under $MODEL (timeout ${TIMEOUT}s)"

# FS-UAE needs a real OpenGL context -- SDL_VIDEODRIVER=dummy makes it die with
# "[GLAD] Failed to initialize OpenGL context", so a window is opened by default.
# AMINETXDUO_HEADLESS=1 tries the dummy driver anyway, for hosts where it works.
if [ "${AMINETXDUO_HEADLESS:-0}" = "1" ]; then
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
    export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
else
    export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
fi


# ---------------------------------------------------------------- cleanup ---
#
# Without this, two things leak emulator processes:
#   * fs-uae ignoring SIGTERM (it can wedge in SDL/GL teardown), and
#   * this script being killed -- an interrupted run orphans its child, which
#     then sits forever holding a window and a CPU.
# Escalate TERM -> KILL, and run it from a trap so it happens even when the
# script does not reach its own exit path.
FSUAE_PID=""
cleanup_emulator() {
    [ -n "$FSUAE_PID" ] || return 0
    kill -TERM "$FSUAE_PID" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 "$FSUAE_PID" 2>/dev/null || { FSUAE_PID=""; return 0; }
        sleep 0.5
    done
    echo "!! fs-uae $FSUAE_PID ignored SIGTERM; sending SIGKILL" >&2
    kill -KILL "$FSUAE_PID" 2>/dev/null || true
    FSUAE_PID=""
}
cleanup_all() { cleanup_emulator; release_lock; }
trap cleanup_all EXIT INT TERM HUP


"$FSUAE" "$CFG" >"$ROOT/build/fsuae$TAG.log" 2>&1 &
FSUAE_PID=$!

status=124
elapsed=0
EARLY_EXIT=0
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    if [ -f "$HD/.done" ]; then
        status=$(tr -dc '0-9' < "$HD/.done" | head -c 4)
        status=${status:-0}
        break
    fi
    if ! kill -0 "$FSUAE_PID" 2>/dev/null; then
        echo "!! fs-uae exited early after ${elapsed}s" >&2
        EARLY_EXIT=1
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

cleanup_emulator
wait "$FSUAE_PID" 2>/dev/null || true

# ------------------------------------------------------------------- output --

echo "---- serial ($SERIAL) ----"
if [ -s "$SERIAL" ]; then
    cat "$SERIAL"
else
    echo "(empty -- no ami_log output reached the serial port)"
fi

for produced in "$HD"/*.txt "$HD"/*.log; do
    [ -f "$produced" ] || continue
    echo "---- $(basename "$produced") ----"
    cat "$produced"
done

# A crash the guard caught leaves this behind even when nothing else survives.
if [ -f "$HD/crash.txt" ]; then
    echo "---- CRASH ----"
    cat "$HD/crash.txt"
fi

# When the emulator dies rather than the program exiting, the emulator log is
# the only evidence left -- show it rather than making the caller go find it.
if [ "$EARLY_EXIT" = "1" ]; then
    echo "---- fs-uae log (tail) ----"
    tail -15 "$ROOT/build/fsuae$TAG.log" 2>/dev/null
    UAELOG="$FSUAE_BASE/Cache/Logs/fs-uae.log.txt"
    if [ -f "$UAELOG" ]; then
        echo "---- UAE core log: faults and warnings ----"
        grep -iE "illegal|exception|guru|trap|bus error|address error|unknown" \
            "$UAELOG" 2>/dev/null | tail -15 || echo "(none logged)"
    fi
fi

if [ "$status" = "124" ]; then
    echo "==> TIMEOUT after ${TIMEOUT}s (no DH0:.done)"
else
    echo "==> exit status $status"
fi
exit "$status"
