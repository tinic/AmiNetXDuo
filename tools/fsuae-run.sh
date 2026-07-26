#!/usr/bin/env bash
#
# Run an AmigaOS executable under FS-UAE and capture its output.
#
#   tools/fsuae-run.sh [-t SECONDS] [-m MODEL] [-c CPU] [-k MHZ] [-n]
#                      <executable> [extra files...]
#
# -m selects the machine profile. A1200 (the default) is the project floor:
# 68EC020 at 14 MHz with a 16-bit path to memory, and it is the ONLY profile
# whose timings mean anything -- FS-UAE turns cycle accounting off for every
# CPU above a 68020. A3000 gives a 68030 with 32-bit motherboard RAM, for
# exercising the code on that processor; it is not a timing profile and says
# so when it starts. Anything else is passed straight to FS-UAE.
#
# -k moves the CPU clock on a cycle-exact profile without disturbing the cycle
# accounting, so "what would this do at 25 MHz?" has a defensible answer even
# though "what would this do on an A3000?" does not. See the block below it.
#
# tests/perf/cpucal measures all of the above rather than assuming it; the
# calibration section of docs/RESEARCH.md is the write-up.
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
CLOCK=""

while getopts "t:m:c:k:n" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        n) NETWORK=1 ;;
        *) echo "usage: $0 [-t seconds] [-m model] [-c cpu] [-k MHz] [-n] <executable> [files...]" >&2; exit 2 ;;
    esac
done

# -m A3000 is a profile, not just a model string: FS-UAE's stock A3000 puts its
# 8 MB of expansion RAM on Zorro II, and a 32-bit path to memory is the whole
# reason an A3000 is interesting.  See the config block below.
#
# IT IS NOT A TIMING PROFILE.  Measured, not assumed -- tests/perf/cpucal on
# this profile reports MULU.L at 3.2 cycles against a real 68030's 44, Chip RAM
# and 32-bit Fast RAM at the same speed, and an implied 323 MHz clock.  FS-UAE
# switches cycle accounting OFF for every CPU above a 68020 (its own log says
# `cpu_speed = max`, `cpu_cycle_exact = false`), and none of accuracy,
# cpu_speed, cpu_frequency or cpu_multiplier turns it back on.  Use this
# profile to exercise the code on a 68030 -- MMU, caches, 32-bit addressing --
# and take timings from -m A1200, with -k if you want a different clock.
A3000_PROFILE=0
if [ "$MODEL" = "A3000" ]; then
    A3000_PROFILE=1
fi
shift $((OPTIND - 1))

[ $# -ge 1 ] || { echo "usage: $0 [-t seconds] [-m model] <executable> [files...]" >&2; exit 2; }

EXE="$1"; shift
[ -f "$EXE" ] || { echo "no such executable: $EXE" >&2; exit 2; }
EXE_NAME=$(basename "$EXE")

# ---------------------------------------------------------------- kickstart --

# AMINETXDUO_KICKSTART_EXT loads a second ROM image as FS-UAE's extended ROM.
# Kickstart 3.1 does not need one.  The AROS m68k ROM DOES: it ships as a pair
# of 512 KB images, and booting only the first one dies in Exec Bootstrap with
# "graphics.library could not open library hidd" before DOS ever starts.  With
# both, FS-UAE reports "1MiB ROM detected" and everything here works exactly as
# it does on 3.1 -- verified 2026-07-25 on the smoke, lifecycle and kernel-stop
# probes, identical check counts.  tools/fetch-aros-rom.sh sets both variables.
KICKSTART="${AMINETXDUO_KICKSTART:-}"
KICKSTART_EXT="${AMINETXDUO_KICKSTART_EXT:-}"

# The A3000 wants its own 3.1 image.  Not because the machine cannot run the
# A1200 one -- both are 40.68 and the difference is machine-specific -- but
# because a profile that silently boots the wrong ROM is the kind of detail
# that invalidates a measurement three weeks later.  The A1200 image is still
# the fallback, announced, so the profile keeps working where only one ROM is
# present.  Copyrighted either way: located, never committed.
KICKSTART_CANDIDATES=(
    "$HOME/Downloads/Kickstart v3.1 rev 40.68 (1993)(Commodore)(A1200)[!].rom"
    "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A1200)[!].rom"
    "$HOME/png2amiga_testing/kick31.rom"
)
if [ "$A3000_PROFILE" = "1" ]; then
    KICKSTART_CANDIDATES=(
        "${AMINETXDUO_KICKSTART_A3000:-}"
        "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A3000).rom"
        "$HOME/Downloads/Kickstart v3.1 rev 40.68 (1993)(Commodore)(A3000).rom"
        "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A4000).rom"
        "${KICKSTART_CANDIDATES[@]}"
    )
fi

if [ -z "$KICKSTART" ]; then
    for candidate in "${KICKSTART_CANDIDATES[@]}"; do
        [ -n "$candidate" ] || continue
        [ -f "$candidate" ] && { KICKSTART="$candidate"; break; }
    done
fi
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    cat >&2 <<'EOF'
No boot ROM found.  This harness needs one of:

  a Kickstart 3.1 ROM   -- copyrighted, not redistributable, not in CI:
                           export AMINETXDUO_KICKSTART=<path>
  the AROS m68k ROM     -- free (APL 1.1) and what CI uses:
                           eval "$(tools/fetch-aros-rom.sh --export)"
EOF
    exit 2
}
[ -z "$KICKSTART_EXT" ] || [ -f "$KICKSTART_EXT" ] || {
    echo "AMINETXDUO_KICKSTART_EXT=$KICKSTART_EXT does not exist" >&2
    exit 2
}

# Say so rather than quietly booting a different machine's ROM: a profile that
# silently substitutes one is still a working profile, but it is not the one
# the caller asked for and a measurement taken from it should say which.
if [ "$A3000_PROFILE" = "1" ] && [ "${KICKSTART#*A3000}" = "$KICKSTART" ]; then
    echo "==> no A3000 Kickstart found; falling back to $(basename "$KICKSTART")"
fi

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
    AMIGA_TOOLCHAIN_QUIET=1 . "$ROOT/tools/amiga-toolchain.sh"
    GCC="$AMIGA_GCC"
    NDK="$AMIGA_NDK"
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
serial_port = $SERIAL
fullscreen = 0
EOF

# -------------------------------------------------------------- A3000 tune --
#
# WHICH FAST RAM, and why it is not the same knob on both profiles.
#
#   fast_memory   is Zorro II: 8 MB at 0x200000, and on a machine whose
#                 expansion bus is 16 bits wide that is a 16-bit path.  It is
#                 the right answer for the A1200, which has no other option.
#   motherboard_ram is the A3000's own 32-bit RAM on the CPU bus.  It is the
#                 entire reason for having an A3000 profile: the interesting
#                 difference between these two machines is not 14 MHz against
#                 25, it is a 16-bit path to memory against a 32-bit one, and
#                 a profile that put the working set in Zorro II would measure
#                 the 68030 while throwing that away.
#
# WHAT IS DELIBERATELY NOT HERE.  An earlier version of this block also set
# cpu_model, cpu_frequency = 25000000, cpu_cycle_exact and blitter_cycle_exact.
# Every one of them was swept with tests/perf/cpucal and every one of them
# turned out to be inert: FS-UAE's A3000 quickstart runs AFTER the config file
# and overwrites all four (its log prints `set option "cpu_cycle_exact" to
# "false"` immediately after reading ours), and cpu_model is redundant anyway
# because the model supplies 68030 itself.  Four lines that look like they pin
# the machine down and do nothing are worse than none, so they are gone.
#
# uae_cpu_cycle_exact = true DOES get through -- the uae_ passthrough is
# applied last -- and it does re-enable cycle accounting.  It is still not
# here, because what it buys is not fidelity: the clock lands at an implied
# 3.3 MHz that no multiplier moves, MULU.L is charged 4 cycles against a real
# 68030's 44, and Chip RAM and 32-bit Fast RAM come out identical.  It is
# three times slower to run and wrong in different places.  Set it by hand
# through AMINETXDUO_FSUAE_EXTRA if you want to see that for yourself.
if [ "$A3000_PROFILE" = "1" ]; then
    cat >> "$CFG" <<EOF
motherboard_ram = 8192
EOF
    echo "==> A3000 profile: 68030, 8 MB 32-bit motherboard RAM (NOT a timing profile)"
else
    cat >> "$CFG" <<EOF
fast_memory = 8192
EOF
fi

# ------------------------------------------------------------- -k CLOCK MHz --
#
# Move the CPU clock while keeping the cycle accounting.  This is the only way
# in this emulator to ask "what would this code do on a faster Amiga?" and get
# an answer worth having: the A1200 model is cycle-exact (verified against
# published 68020 timings, docs/RESEARCH.md), and uae_cpu_multiplier scales its
# clock without disturbing that -- ADD.L stays at its published two cycles and
# MULU.L at the same 32 the model charges at 14 MHz, so nothing about the
# instruction accounting changes, only the rate.
#
# The unit is half the 7.09 MHz PAL chipset clock, i.e. ~3.5 MHz per step;
# measured, because the emulator documents it nowhere: 4 -> 13.95 MHz,
# 7 -> 24.5, 8 -> 28.01.
#
# Chip RAM deliberately does NOT scale with it -- it is chipset-bound, and the
# model has that right (a 2x clock buys 2.03x on Fast RAM and 1.49x on Chip).
# That is what makes the option useful rather than merely fast: the difference
# between the two is the part of the workload the bus owns.
#
# -k has no effect on -m A3000, because that model has no cycle accounting to
# preserve; the script says so rather than pretending.
if [ -n "$CLOCK" ]; then
    if [ "$A3000_PROFILE" = "1" ]; then
        echo "!! -k $CLOCK ignored: the A3000 profile has no cycle accounting to scale" >&2
    else
        MULT=$(( (CLOCK * 10 + 17) / 35 ))
        [ "$MULT" -ge 1 ] || MULT=1
        cat >> "$CFG" <<EOF
uae_cpu_multiplier = $MULT
EOF
        echo "==> CPU clock: multiplier $MULT, nominally $((MULT * 35 / 10)).$((MULT * 35 % 10)) MHz"
    fi
fi

# AMINETXDUO_FSUAE_EXTRA appends raw `key = value` lines, semicolon separated.
# It exists so the knobs above can be swept without editing this file: the
# A3000 profile's settings were chosen by sweeping them, and the next person
# to doubt them should be able to repeat that in one line.
if [ -n "${AMINETXDUO_FSUAE_EXTRA:-}" ]; then
    printf '%s\n' "${AMINETXDUO_FSUAE_EXTRA}" | tr ';' '\n' >> "$CFG"
    echo "==> extra config: ${AMINETXDUO_FSUAE_EXTRA}"
fi

if [ -n "$KICKSTART_EXT" ]; then
    cat >> "$CFG" <<EOF
kickstart_ext_file = $KICKSTART_EXT
EOF
    echo "==> extended ROM: $(basename "$KICKSTART_EXT")"
fi

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
    # AMINETXDUO_FSUAE_A2065 picks the card's backend.  `slirp` is the user-mode
    # NAT everything else in the tree runs on; `none` fits the card and leaves
    # it wired to nothing, which is the only way to give the guest an Ethernet
    # interface that opens, links up and never hears an answer -- what a DHCP
    # timeout and the RFC 3927 fallback need (tests/netstack/run-dhcp3927.sh).
    A2065_BACKEND="${AMINETXDUO_FSUAE_A2065:-slirp}"
    cat >> "$CFG" <<EOF
network_card = a2065
uae_a2065 = $A2065_BACKEND
EOF
    if [ "$A2065_BACKEND" = "slirp" ]; then
        echo "==> A2065 + SLIRP attached (10.0.2.0/24, gateway 10.0.2.2)"
    else
        echo "==> A2065 attached with backend '$A2065_BACKEND' (no host network)"
    fi
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


# Host wall clock across the emulator's whole life.  This is the only figure
# the harness produces that the emulator cannot fake, and it is what catches a
# model running the CPU unthrottled: a probe that reports 4 seconds of
# EMULATED time inside a run that took 40 seconds of HOST time was throttled,
# and one that reports 0.2 seconds inside the same 40 was not.
WALL_START=$(date +%s)

# SIGPIPE MUST BE IGNORED, AND IT IS NOT DECORATION
#
#   fs-uae's SLIRP writes the guest's TCP payload to a real host socket with
#   plain send()/write().  When the far end has hung up, that returns EPIPE
#   AND raises SIGPIPE, whose default disposition kills the process -- so any
#   guest that keeps talking to a peer which closed first takes the EMULATOR
#   down, mid-instruction, with no guru, no DH0:.done and a log file truncated
#   on a 4 KB boundary because stdio never flushed.  It reads exactly like the
#   Amiga crashed.  It is not the Amiga; on real hardware there is no such
#   signal.  A socket is spared only by SO_NOSIGPIPE or by sending with
#   MSG_NOSIGNAL, and the fs-uae measured here (3.2.35 on macOS) demonstrably
#   does neither: without this line it exits 141 every time.
#
#   Ignoring it here is inherited across execve() -- SIG_IGN is, a handler is
#   not -- so fs-uae starts with SIGPIPE ignored and send() merely returns
#   EPIPE, which its own error path already handles.  The subshell keeps the
#   disposition off this script, whose own pipelines want the default.
( trap '' PIPE; exec "$FSUAE" "$CFG" ) >"$ROOT/build/fsuae$TAG.log" 2>&1 &
FSUAE_PID=$!

# cleanup_emulator() clears FSUAE_PID, so keep a copy for the wait below --
# otherwise the exit status is the shell complaining about an empty pid.
EMU_PID=$FSUAE_PID

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
FSUAE_RC=0
wait "$EMU_PID" 2>/dev/null || FSUAE_RC=$?

# Say WHY the emulator went, not just that it did.  A death by signal is a
# host-side event and never the program under test failing; reporting it as
# "exited early" is what sent one investigation after an Amiga bug that was
# not there.
if [ "$EARLY_EXIT" = "1" ] && [ "$FSUAE_RC" -gt 128 ]; then
    echo "!! fs-uae was KILLED BY SIGNAL $((FSUAE_RC - 128)) -- a host-side" >&2
    echo "!! death, not a guru.  Nothing about the emulated machine follows" >&2
    echo "!! from it." >&2
fi

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
    echo "==> exit status $status after $(( $(date +%s) - WALL_START ))s of host wall clock"
fi
exit "$status"
