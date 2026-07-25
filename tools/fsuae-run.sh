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

while getopts "t:m:n" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        n) NETWORK=1 ;;
        *) echo "usage: $0 [-t seconds] [-m model] [-n] <executable> [files...]" >&2; exit 2 ;;
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
cat > "$HD/s/Startup-Sequence" <<EOF
c:envsetup
$EXE_NAME >DH0:stdout.txt
echo >DH0:.done "\$RC"
EOF

# ------------------------------------------------------------------ running --

CFG="$ROOT/build/test$TAG.fs-uae"
cat > "$CFG" <<EOF
[fs-uae]
amiga_model = $MODEL
kickstart_file = $KICKSTART
hard_drive_0 = $HD
hard_drive_0_label = DH0
fast_memory = 8192
serial_port = $SERIAL
fullscreen = 0
EOF

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

"$FSUAE" "$CFG" >"$ROOT/build/fsuae$TAG.log" 2>&1 &
FSUAE_PID=$!

status=124
elapsed=0
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    if [ -f "$HD/.done" ]; then
        status=$(tr -dc '0-9' < "$HD/.done" | head -c 4)
        status=${status:-0}
        break
    fi
    if ! kill -0 "$FSUAE_PID" 2>/dev/null; then
        echo "!! fs-uae exited early; see build/fsuae$TAG.log" >&2
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

kill "$FSUAE_PID" 2>/dev/null || true
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

if [ "$status" = "124" ]; then
    echo "==> TIMEOUT after ${TIMEOUT}s (no DH0:.done)"
else
    echo "==> exit status $status"
fi
exit "$status"
