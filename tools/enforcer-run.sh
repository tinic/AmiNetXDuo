#!/usr/bin/env bash
#
# Run an AmigaOS executable under FS-UAE with Enforcer (and optionally MungWall)
# installed, and report every illegal memory access it makes.
#
#   tools/enforcer-run.sh [-t SECONDS] [-T TAG] [-m] [-M] <executable> [files...]
#
#     -t  timeout in seconds (default 240)
#     -T  run tag; isolates the staging drive, serial log, config and fs-uae
#         base directory, exactly like AMINETXDUO_RUN_TAG in fsuae-run.sh
#     -m  also install MungWall (AllocMem/FreeMem guard bands)
#     -M  install MungWall only, no Enforcer (works on a plain 68020)
#     -n  attach the emulated A2065 on SLIRP, exactly as fsuae-run.sh -n does
#
# WHY THIS IS NOT JUST fsuae-run.sh WITH ONE MORE FILE
#
#   * Enforcer needs a real MMU.  The A1200 model fs-uae-run.sh boots is a bare
#     68020 with no 68851, and on that machine Enforcer does not print its
#     "MMU is not available" message -- it WEDGES.  So this script overrides the
#     CPU to a 68030, which makes FS-UAE set mmu_model=68030 and cachesize=0.
#   * JIT must stay off.  With jit_compiler=1 FS-UAE logs "MMU emulation ...
#     is not JIT compatible", Enforcer installs anyway and then reports nothing
#     at all -- a silent false negative.  FS-UAE defaults to JIT off; do not
#     turn it on here.
#   * FS-UAE's directory-hard-drive handler lives in a trap ROM at $00F00000,
#     which Enforcer counts as illegal.  Without the FSPACE option every file
#     access produces a hit and the report is unreadable.  Hence FSPACE.
#   * MungWall must be installed BEFORE Enforcer: tools that SetFunction()
#     AllocMem/FreeMem and chain to the previous vector have to be stacked in
#     that order (mungwall.doc).
#
# Enforcer and MungWall are not redistributable, so they are not in this tree.
# Point AMINETXDUO_ENFORCER / AMINETXDUO_MUNGWALL at your copies if they are not
# where the defaults below expect them.
#
# Exit status is the test's own, read from DH0:.done, or 124 on timeout.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TIMEOUT=240
TAG="${AMINETXDUO_RUN_TAG:-enforcer}"
WANT_ENFORCER=1
WANT_MUNGWALL=0
NETWORK=0

while getopts "t:T:mMn" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        m) WANT_MUNGWALL=1 ;;
        n) NETWORK=1 ;;
        M) WANT_MUNGWALL=1; WANT_ENFORCER=0 ;;
        *) echo "usage: $0 [-t seconds] [-T tag] [-m|-M] <executable> [files...]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

[ $# -ge 1 ] || { echo "usage: $0 [-t seconds] [-T tag] [-m|-M] <executable> [files...]" >&2; exit 2; }

EXE="$1"; shift
[ -f "$EXE" ] || { echo "no such executable: $EXE" >&2; exit 2; }
EXE_NAME=$(basename "$EXE")

ENFORCER="${AMINETXDUO_ENFORCER:-$HOME/amiga-os-src/os-source/v40/aug/bin/enforcer}"
MUNGWALL="${AMINETXDUO_MUNGWALL:-$HOME/amiga-os-src/os-source/aug.cats/bin/mungwall}"

if [ "$WANT_ENFORCER" = "1" ] && [ ! -f "$ENFORCER" ]; then
    echo "Enforcer not found at $ENFORCER; set AMINETXDUO_ENFORCER=<path>." >&2
    exit 2
fi
if [ "$WANT_MUNGWALL" = "1" ] && [ ! -f "$MUNGWALL" ]; then
    echo "MungWall not found at $MUNGWALL; set AMINETXDUO_MUNGWALL=<path>." >&2
    exit 2
fi

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

AMIGA_TOOLCHAIN_QUIET=1 . "$ROOT/tools/amiga-toolchain.sh"
GCC="$AMIGA_GCC"
NDK="$AMIGA_NDK"

# ------------------------------------------------------------------ staging --

HD="$ROOT/build/testhd-$TAG"
SERIAL="$ROOT/build/serial-$TAG.log"
CFG="$ROOT/build/test-$TAG.fs-uae"
BASE="$ROOT/build/fsuae-base-$TAG"

rm -rf "$HD"
mkdir -p "$HD/s" "$HD/c" "$HD/env" "$HD/envarc" "$HD/t" "$HD/clips" "$BASE" "$ROOT/build"
: > "$SERIAL"

cp "$EXE" "$HD/$EXE_NAME"
cp "$EXE" "$HD/c/$EXE_NAME"
for extra in "$@"; do cp -R "$extra" "$HD/"; done

# waitsecs: the bare boot has no C:Wait, and a resident debugging tool needs a
# few seconds to install before the program under test starts.
WAITSECS="$ROOT/build/waitsecs"
if [ ! -x "$WAITSECS" ] || [ "$ROOT/tools/enforcer/waitsecs.c" -nt "$WAITSECS" ]; then
    "$GCC" -O2 -m68020 -I"$NDK" -o "$WAITSECS" "$ROOT/tools/enforcer/waitsecs.c" \
        || { echo "failed to build waitsecs" >&2; exit 2; }
fi
cp "$WAITSECS" "$HD/c/waitsecs"

ENVSETUP="$ROOT/build/envsetup"
if [ -x "$ENVSETUP" ]; then
    cp "$ENVSETUP" "$HD/c/envsetup"
elif [ -f "$ROOT/tools/envsetup/envsetup.c" ]; then
    "$GCC" -O2 -m68020 -I"$NDK" -o "$ENVSETUP" "$ROOT/tools/envsetup/envsetup.c" \
        && cp "$ENVSETUP" "$HD/c/envsetup"
fi

[ "$WANT_MUNGWALL" = "1" ] && cp "$MUNGWALL" "$HD/c/mungwall"
[ "$WANT_ENFORCER" = "1" ] && cp "$ENFORCER" "$HD/c/enforcer"

{
    # AmigaDOS aborts a script the moment a command returns at or above the
    # fail level (10 by default), so without this a test that returns 20 never
    # reaches the "echo >DH0:.done" line and a plain failure looks like a hang.
    echo "failat 9999"
    [ -f "$HD/c/envsetup" ] && echo "c:envsetup"
    if [ "$WANT_MUNGWALL" = "1" ]; then
        echo "run >NIL: <NIL: c:mungwall NAMETAG SHOWHUNK"
        echo "c:waitsecs 3"
    fi
    if [ "$WANT_ENFORCER" = "1" ]; then
        echo "run >NIL: <NIL: c:enforcer FSPACE"
        echo "c:waitsecs 5"
    fi
    echo "$EXE_NAME >DH0:stdout.txt"
    echo "echo >DH0:.done \"\$RC\""
} > "$HD/s/Startup-Sequence"

# ------------------------------------------------------------------ running --

{
    echo "[fs-uae]"
    echo "floppy_drive_volume = 0"
    echo "floppy_drive_volume_empty = 0"
    echo "base_dir = $BASE"
    echo "amiga_model = A1200"
    echo "kickstart_file = $KICKSTART"
    echo "hard_drive_0 = $HD"
    echo "hard_drive_0_label = DH0"
    echo "fast_memory = 8192"
    echo "serial_port = $SERIAL"
    echo "fullscreen = 0"
    if [ "$WANT_ENFORCER" = "1" ]; then
        # 68030 => FS-UAE sets mmu_model=68030 and cachesize=0, which is what
        # Enforcer needs. Do NOT add jit_compiler=1: it silences Enforcer.
        echo "cpu = 68030"
    fi
    if [ "$NETWORK" = "1" ]; then
        echo "network_card = a2065"
        echo "uae_a2065 = slirp"
    fi
} > "$CFG"

what="enforcer"
[ "$WANT_MUNGWALL" = "1" ] && what="$what+mungwall"
[ "$WANT_ENFORCER" = "0" ] && what="mungwall"
echo "==> $EXE_NAME under $what (timeout ${TIMEOUT}s, tag $TAG)"

export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"

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
trap cleanup_emulator EXIT INT TERM HUP


# SIGPIPE ignored, inherited across execve() -- see the long note in
# tools/fsuae-run.sh.  Without it a guest that writes to a peer which has
# already hung up kills the EMULATOR, which under -n is exactly the situation
# this script exists to look at.
( trap '' PIPE; exec "$FSUAE" "$CFG" ) >"$ROOT/build/fsuae-$TAG.log" 2>&1 &
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
        echo "!! fs-uae exited early after ${elapsed}s; see build/fsuae-$TAG.log" >&2
        EARLY_EXIT=1
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

cleanup_emulator
FSUAE_RC=0
wait "$EMU_PID" 2>/dev/null || FSUAE_RC=$?

if [ "$EARLY_EXIT" = "1" ] && [ "$FSUAE_RC" -gt 128 ]; then
    echo "!! fs-uae was KILLED BY SIGNAL $((FSUAE_RC - 128)) -- host-side, not a guru." >&2
fi

# ------------------------------------------------------------------- output --

echo "---- serial ($SERIAL) ----"
[ -s "$SERIAL" ] && cat "$SERIAL" || echo "(empty)"

for produced in "$HD"/*.txt "$HD"/*.log; do
    [ -f "$produced" ] || continue
    echo "---- $(basename "$produced") ----"
    cat "$produced"
done

hits=$(grep -c -E "^(LONG|WORD|BYTE)-(READ|WRITE)" "$SERIAL" 2>/dev/null || true)
walls=$(grep -c "were hit!" "$SERIAL" 2>/dev/null || true)
echo "==> Enforcer hits: ${hits:-0}   MungWall wall hits: ${walls:-0}"

if [ "$status" = "124" ]; then
    echo "==> TIMEOUT after ${TIMEOUT}s (no DH0:.done)"
else
    echo "==> exit status $status"
fi

# A run that reports hits has not passed, whatever the program's own status is.
if [ "${hits:-0}" != "0" ] || [ "${walls:-0}" != "0" ]; then
    exit 30
fi
exit "$status"
