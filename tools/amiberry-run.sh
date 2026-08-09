#!/usr/bin/env bash
#
# Run an AmigaOS executable under Amiberry on Linux and capture its output.
#
#   tools/amiberry-run.sh [-t SECONDS] [-m MODEL] [-c CPU] [-N BOARD]
#                         [-B BACKEND] [-a ARGS] <executable> [extra files...]
#
# -a passes arguments to the executable under test, `-a 'eth0 QUIET'`, which
# is the only way to reach a command that takes a parameter.  It is the same
# string as AMINETXDUO_GUEST_ARGS, which tools/winuae-run.sh already read; the
# flag wins when both are set.
#
# The Linux counterpart of tools/fsuae-run.sh and tools/winuae-run.sh.  Amiberry
# is WinUAE's core, so -N takes WinUAE's board keys unchanged and the board
# table below is tools/winuae-run.sh's.
#
# WHY THIS EXISTS ALONGSIDE fsuae-run.sh
#
#   Amiberry runs genuinely headless, `headless=true` and SDL is never asked
#   for a video device, where FS-UAE needs an X server on a Linux runner.  It
#   emulates all nine ethernet boards rather than only the A2065.  And it has
#   backends that reach the real network: -B <interface> puts the guest on the
#   host's LAN with its own MAC, which is the thing SLIRP cannot do.
#
# -B PICKS THE BACKEND, AND IT IS CHECKED
#
#   slirp           user-mode NAT, 10.0.2.0/24, gateway 10.0.2.2 (the default)
#   slirp_inbound   the same, with the standard ports forwarded in
#   <interface>     a host NIC (`ens18`), a bridge, or a tap device, the
#                   guest is then a machine on the host's own LAN
#
#   THE NAME GOES IN BARE, not as `netmode=<name>`.  It is an
#   EXPANSIONBOARD_MULTI, and cfgfile_read_rom_settings() picks the item by
#   looking for each candidate as an option of its own; `netmode=ens18` parses
#   as an option called netmode, matches no item, and selects index 0, which
#   is slirp.  `netmode=slirp` therefore appears to work and is doing nothing,
#   which is how the spelling survives.  WinUAE's own saved configs write the
#   bare name too (tools/winuae-run.sh: `mac=...,rpcap://...`).
#
#   ethernet_getselectionname() then returns "slirp" for any name it cannot
#   match, so a typo does not fail either: it comes up on NAT and passes.  This
#   script reads the backend back out of the emulator log and refuses the run
#   if it is not the one asked for.  docs/RESEARCH.md 76.7 found that trap;
#   this harness is what stops it costing anyone a day.
#
# -m PICKS THE MACHINE
#
#   Anything Amiberry's `quickstart=` accepts: A500, A600, A1200 (the default),
#   A3000, A4000.  Two things follow from it rather than having to be set:
#   the boot ROM, from AMINETXDUO_KICKSTART_<MODEL> if that is set, and the
#   architecture envsetup is built for, because a 68020 binary stops a 68000
#   machine with an illegal instruction before anything under test runs.
#
#   The executable under test is NOT rebuilt to match.  Pass one from a build
#   configured for the machine, `cmake -DAMINETXDUO_CPU=68000` for an A500 or
#   an A600, or the run dies the same way for the same reason.
#
#   A host NIC needs CAP_NET_RAW on the amiberry binary (libpcap), a tap or
#   bridge needs CAP_NET_ADMIN.  Both are cleared by every relink, so
#   `sudo setcap cap_net_admin,cap_net_raw=eip <binary>` is a per-build step --
#   and file capabilities are ignored on a nosuid mount, which /tmp usually is.
#
# SERIAL IS A SOCKET, NOT A FILE
#
#   openser() takes a TCP URI, a serial port name, INTERNAL_SERIAL or
#   LOOPBACK_SERIAL.  A path is not among them, so FS-UAE's
#   `serial_port = build/serial.log` has no equivalent; the replacement is a
#   listening socket the host drains with nc.  `/wait` makes the emulator block
#   until something connects, so the host has to RETRY, losing that race
#   leaves an emulator waiting forever and looks like a hang, not a lost log.
#
# Exit status is the test's own, read from DH0:.done, or 124 on timeout.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TIMEOUT=120
MODEL=A1200
CPU=""
CLOCK=""
BOARD=""
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"
GUEST_ARGS="${AMINETXDUO_GUEST_ARGS:-}"

USAGE="usage: $0 [-t seconds] [-m model] [-c cpu] [-N board] [-B backend] [-a args] <executable> [files...]"

while getopts "t:m:c:k:N:B:a:" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        a) GUEST_ARGS="$OPTARG" ;;
        *) echo "$USAGE" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

[ $# -ge 1 ] || { echo "$USAGE" >&2; exit 2; }

EXE="$1"; shift
[ -f "$EXE" ] || { echo "no such executable: $EXE" >&2; exit 2; }
EXE_NAME=$(basename "$EXE")

. "$ROOT/tools/amiberry-resolve.sh"
amiberry_resolve || exit 2

# ---------------------------------------------------------------- kickstart --

# AMINETXDUO_KICKSTART_<MODEL> beats AMINETXDUO_KICKSTART, because one ROM does
# not boot every machine: an A600 wants 37.350 or 40.63, and the A1200's 40.68
# leaves it at a black screen with nothing on the serial port to say why.  The
# model name is uppercased and anything but a letter or digit becomes _, so
# A500+ reads AMINETXDUO_KICKSTART_A500_.
_ks_var="AMINETXDUO_KICKSTART_$(printf '%s' "$MODEL" | tr '[:lower:]' '[:upper:]' | tr -c 'A-Z0-9' '_')"
_ks_var="${_ks_var%_}"

KICKSTART="${!_ks_var:-${AMINETXDUO_KICKSTART:-}}"
KICKSTART_EXT="${AMINETXDUO_KICKSTART_EXT:-}"

# quickstart=A3000 selects the machine, not the ROM, so an A1200 Kickstart
# boots on A3000 hardware and the mismatch shows up later as a device that
# will not open.  AMINETXDUO_KICKSTART_A3000 is what the asset store exports
# for exactly this.
if [ "$MODEL" = A3000 ] && [ -n "${AMINETXDUO_KICKSTART_A3000:-}" ]; then
    KICKSTART="$AMINETXDUO_KICKSTART_A3000"
fi
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    cat >&2 <<'EOF'
No boot ROM.  Either:

  export AMINETXDUO_KICKSTART=<kickstart.rom>  # a real one, not redistributable
EOF
    exit 2
}
[ -z "$KICKSTART_EXT" ] || [ -f "$KICKSTART_EXT" ] || {
    echo "AMINETXDUO_KICKSTART_EXT=$KICKSTART_EXT does not exist" >&2; exit 2
}

# A 68020+ machine's ROM uses 68020 instructions, so asking for a 68000 CPU on
# one boots nothing: no guru, no serial output, just a run that reaches the
# timeout.  That looks exactly like a slow test and has been misread as one
# more than once.  Refuse it here instead, and name the model to use.
case "$MODEL" in
    A1200|A3000|A4000|A4000T|CD32)
        case "$CPU" in
            68000|68010)
                echo "-m $MODEL -c $CPU cannot boot: the $MODEL Kickstart needs" >&2
                echo "a 68020.  For a 68000 use -m A600 (or A500), which reads" >&2
                echo "AMINETXDUO_KICKSTART_A600." >&2
                exit 2 ;;
        esac ;;
esac

# The other half of the same trap: a model with no AMINETXDUO_KICKSTART_<MODEL>
# silently falls back to the generic one, which in the lab is an A1200 ROM.  On
# a 68000-class model that is the mismatch above with nothing to point at it.
if [ -z "${!_ks_var:-}" ]; then
    case "$MODEL" in
        A500|A500_|A600|A1000|A2000|CDTV)
            echo "$_ks_var is not set; falling back to AMINETXDUO_KICKSTART" >&2
            echo "($(basename "$KICKSTART")).  If that is an A1200 ROM this run" >&2
            echo "will boot to a black screen.  Set $_ks_var." >&2 ;;
    esac
fi

# ------------------------------------------------------------------- boards --
#
# The keys are WinUAE's, because Amiberry parses WinUAE's config file.  See
# tools/winuae-run.sh for what each board is and which driver it wants; the
# driver is staged by tools/sana2-stage.sh, not here.
#
# The MAC is set explicitly on a bridged run.  Left alone the emulator invents
# one, so a DHCP server hands out a different lease every time and nothing on
# the LAN can hold a reservation.  The A2065 keeps only the last three bytes --
# a2065.cpp overwrites the first three with Commodore's 00:80:10, while the
# NE2000 boards take the whole address.
MAC="${AMINETXDUO_AMIBERRY_MAC:-02:41:4d:49:00:01}"

# PCMCIA needs a 68020.  Rederived from scratch more than once, so it stops
# here rather than in an hour of somebody's afternoon.
#
# `-N ne2000_pcmcia` with cnet.device bisects to cpu_type and nothing else:
# 68000, 68010, cpu_compatible=false and cpu_multiplier=16 all answer
# `Could not add interface "eth0" (Input/output error)`, while 68020 leases,
# with or without address_space_24 or a slower multiplier.  ROM, chipset prefs,
# memory and cnet16.device were ruled out.  The two CPUs diverge inside
# card.resource's CIS walk; docs/BACKLOG.md carries the gayle.cpp detail.
#
# This is a CPU limit and NOT a model limit.  An A600 networks perfectly well
# on -N a2065, which is the answer if what you wanted was a 68000.
pcmcia_cpu_check() {
    case "$1" in
        ne2000_pcmcia) ;;
        *) return 0 ;;
    esac

    # Empty CPU means the model's default, and every model whose default is a
    # 68000 is one this refuses anyway.
    case "${CPU:-}" in
        68020|68030|68040|68060) return 0 ;;
    esac
    case "${CPU:-}$MODEL" in
        A1200|A3000|A4000|CD32) return 0 ;;
    esac

    cat >&2 <<EOF
ne2000_pcmcia needs a 68020 or better; this run is ${CPU:-the $MODEL default}.

  card.resource cannot walk the card's CIS tuples on a 68000 or a 68010 under
  Amiberry, and AddNetInterface answers
  'Could not add interface "eth0" (Input/output error)'.  Bisected to cpu_type
  alone -- see docs/BACKLOG.md, "PCMCIA is a CPU limit, not an A600 limit".

  -N a2065        works on an A600 at 68000, and is what you want for a 68000
  -c 68020        if the PCMCIA card itself is the thing under test
EOF
    exit 2
}
pcmcia_cpu_check "$BOARD"

board_lines() {
    case "$1" in
        "")            : ;;
        a2065)         printf 'a2065_rom_file=:ENABLED\na2065_rom_options=mac=%s,%s\n' "$MAC" "$BACKEND" ;;
        ariadne|ariadne2|hydra|eb920|xsurf|xsurf100z2|xsurf100z3)
                       printf '%s_rom_file=:ENABLED\n%s_rom_options=mac=%s,%s%s\n' \
                              "$1" "$1" "$MAC" "$BACKEND" \
                              "${AMINETXDUO_AMIBERRY_BOARD_OPTIONS:+,$AMINETXDUO_AMIBERRY_BOARD_OPTIONS}" ;;
        # inserted=true is what puts the card in the slot; without it Gayle's
        # windows are mapped, nothing is logged, and card.resource never
        # initialises, which reads from the guest as a driver that cannot
        # find its hardware.  Needs a machine with a Gayle: an A600 or an A1200.
        ne2000_pcmcia) printf 'pcmcia=true\nne2000pcmcia_rom_file=:ENABLED\nne2000pcmcia_rom_options=inserted=true,mac=%s,%s\n' "$MAC" "$BACKEND" ;;
        *)             echo "unknown network board $1" >&2; exit 2 ;;
    esac
}

# ------------------------------------------------------------------ staging --

TAG="${AMINETXDUO_RUN_TAG:-amiberry}"
HD="$ROOT/build/amiberry-testhd-$TAG"
SERIAL="$ROOT/build/amiberry-serial-$TAG.log"
# The same output with a host timestamp per line, written alongside rather than
# into $SERIAL: everything that greps the serial log anchors to the start of a
# line.  See tools/serial-timestamp.py.
SERIALTS="$ROOT/build/amiberry-serial-$TAG.stamped.log"
UAELOG="$ROOT/build/amiberry-$TAG.log"
CFG="$ROOT/build/amiberry-$TAG.uae"

# One listening port per tag, so two tagged runs never collide.  Same hashing
# as tools/winuae-run.sh.
PORT=$((12000 + $(printf '%s' "$TAG" | cksum | cut -d' ' -f1) % 900))

rm -rf "$HD"
mkdir -p "$HD/s" "$HD/c" "$HD/env" "$HD/envarc" "$HD/t" "$HD/clips" "$ROOT/build"
: > "$SERIAL"

cp "$EXE" "$HD/$EXE_NAME"
cp "$EXE" "$HD/c/$EXE_NAME"
for extra in "$@"; do
    cp -R "$extra" "$HD/"
done

# A bare directory hard drive has none of the assigns a Workbench boot makes,
# so envsetup creates ENV:, ENVARC:, T: and CLIPS: before anything under test
# runs.  Its architecture follows the machine: a 68020 build stops an
# unexpanded A500 with an illegal instruction before a line of ours executes,
# and the failure then looks like the thing being tested.
case "${CPU:-}${MODEL:-}" in
    *68000*|*A500*|*A600*|*A2000*) ENVSETUP_ARCH="-m68000" ;;
    *)                             ENVSETUP_ARCH="-m68020" ;;
esac
ENVSETUP_ARCH="${AMINETXDUO_ENVSETUP_ARCH:-$ENVSETUP_ARCH}"
ENVSETUP="$ROOT/build/envsetup${ENVSETUP_ARCH}"
if [ ! -x "$ENVSETUP" ] || [ "$ROOT/tools/envsetup/envsetup.c" -nt "$ENVSETUP" ]; then
    AMIGA_TOOLCHAIN_QUIET=1 . "$ROOT/tools/amiga-toolchain.sh"
    "$AMIGA_GCC" -O2 $ENVSETUP_ARCH -I"$AMIGA_NDK" -o "$ENVSETUP" \
        "$ROOT/tools/envsetup/envsetup.c" \
        || { echo "failed to build envsetup" >&2; exit 2; }
fi
cp "$ENVSETUP" "$HD/c/envsetup"

# failat is essential: AmigaDOS aborts a script the moment a command returns a
# code at or above the fail level, so without it a test that exits nonzero
# never reaches the line recording its status and the run merely times out.
#
# GUEST_ARGS goes in verbatim, so the AmigaDOS shell does the quoting, which
# is what a command with a ReadArgs template wants.  Empty by default, and then
# the line is the one this script always wrote.
cat > "$HD/s/Startup-Sequence" <<EOF
failat 9999
c:envsetup
$EXE_NAME${GUEST_ARGS:+ $GUEST_ARGS} >DH0:stdout.txt
echo >DH0:.done "\$RC"
EOF

# ------------------------------------------------------------------- config --

# 8 MB of Zorro II Fast RAM covers 0x200000-0x9fffff, and an A1200's PCMCIA
# windows are at 0x600000 (common) and 0xa00000 (attribute).  They overlap, on
# the real machine as well as here, which is why an A1200 with a PCMCIA card
# cannot have 8 MB on the trapdoor bus.  Under emulation the collision does not
# announce itself: the card is logged as inserted, the backend opens, and the
# driver simply fails to find it, cnet.device came back with
# `cannot open cnet.device unit 0 (-1)` and nothing else.  4 MB stops short of
# 0x600000 and the same run works.
FASTMEM=8
[ "$BOARD" = ne2000_pcmcia ] && FASTMEM=4

# AMINETXDUO_Z3MEM=<MB> adds Zorro III fast RAM.  The stack sizes its packet
# pool and its receive window off AvailMem(), so "how much memory does the
# machine have" is a real variable in its behaviour, and an accelerated Amiga
# with 128 MB is a configuration this 8 MB default never reaches.
Z3MEM="${AMINETXDUO_Z3MEM:-0}"

cat > "$CFG" <<EOF
config_description=AmiNetXDuo $TAG
use_gui=no
headless=true
quickstart=$MODEL,0
kickstart_rom_file=$KICKSTART
fastmem_size=$FASTMEM
z3mem_size=$Z3MEM
floppy0type=-1
nr_floppies=0
uaehf0=dir,rw,DH0:DH0:$HD,0
serial_port=tcp://127.0.0.1:$PORT/wait
EOF

[ -z "$KICKSTART_EXT" ] || echo "kickstart_ext_rom_file=$KICKSTART_EXT" >> "$CFG"
[ -z "$CPU" ] || echo "cpu_type=$CPU" >> "$CFG"

# ------------------------------------------------------------- -k CLOCK MHz --
#
# Move the CPU clock and keep the cycle accounting, the same thing
# tools/fsuae-run.sh -k did through uae_cpu_multiplier.  Amiberry is WinUAE's
# core and spells it cpu_multiplier; measured here with tests/perf/cpucal on
# 2026-08-04, because neither documents the unit:
#
#     multiplier 2   ADD.L 311.8 ns   6.41 MHz
#     multiplier 4   ADD.L 152.9 ns  13.08 MHz   (the default A1200)
#     multiplier 8   ADD.L  75.8 ns  26.40 MHz
#
# Linear at 3.27 MHz a step, and ADD.L stays at its published two cycles at
# every one of them, so the instruction accounting is untouched and only the
# rate moves.  fs-uae measured 3.5 a step on the same instruction; the two are
# the same knob, and a figure taken under one is not comparable with the other.
#
# Chip RAM deliberately does not scale with it: it is chipset bound, and the
# difference between a Fast RAM number and a Chip RAM one is the part of a
# workload the bus owns, which is what makes the option worth having.
if [ -n "$CLOCK" ]; then
    MULT=$(( (CLOCK * 100 + 163) / 327 ))
    [ "$MULT" -ge 1 ] || MULT=1
    echo "cpu_multiplier=$MULT" >> "$CFG"
    echo "==> CPU clock: multiplier $MULT, nominally $((MULT * 327 / 100)) MHz"
fi
board_lines "$BOARD" >> "$CFG"

# AMINETXDUO_AMIBERRY_EXTRA appends raw `key=value` lines, semicolon separated,
# so the settings above can be swept without editing this file.
if [ -n "${AMINETXDUO_AMIBERRY_EXTRA:-}" ]; then
    printf '%s\n' "${AMINETXDUO_AMIBERRY_EXTRA}" | tr ';' '\n' >> "$CFG"
    echo "==> extra config: ${AMINETXDUO_AMIBERRY_EXTRA}"
fi

[ -z "$BOARD" ] || echo "==> $BOARD on backend '$BACKEND', MAC $MAC"
echo "==> $EXE_NAME under $MODEL (timeout ${TIMEOUT}s)"

# ------------------------------------------------------------------ running --

AMIBERRY_PID=""
NC_PID=""
cleanup() {
    [ -n "$NC_PID" ] && kill -TERM "$NC_PID" 2>/dev/null || true
    NC_PID=""
    [ -n "$AMIBERRY_PID" ] || return 0
    kill -TERM "$AMIBERRY_PID" 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 "$AMIBERRY_PID" 2>/dev/null || { AMIBERRY_PID=""; return 0; }
        sleep 0.5
    done
    echo "!! amiberry $AMIBERRY_PID ignored SIGTERM; sending SIGKILL" >&2
    kill -KILL "$AMIBERRY_PID" 2>/dev/null || true
    AMIBERRY_PID=""
}
trap cleanup EXIT INT TERM HUP

# headless=true stops Amiberry OPENING A WINDOW; it does not stop it asking SDL
# for a video subsystem.  osdep_platform_init_sdl() calls SDL_Init with
# SDL_INIT_VIDEO before any config is read, and aborts the process when there is
# no driver, so a run over ssh works only for as long as X11 forwarding does,
# and dies with "No available video device" the moment it does not.  The dummy
# driver is what makes this reproducible on a headless runner.  DISPLAY is
# cleared for the same reason: a stale one is worse than none.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
[ "${SDL_VIDEODRIVER}" = "dummy" ] && unset DISPLAY WAYLAND_DISPLAY || true

WALL_START=$(date +%s)

# A host without python3 keeps the reader it always had, and loses only the
# stamped copy.
SERIAL_READER=""
if command -v python3 > /dev/null 2>&1; then
    SERIAL_READER="python3 -u $ROOT/tools/serial-timestamp.py"
    : > "$SERIALTS"
fi

# SIGPIPE is ignored for the same reason tools/fsuae-run.sh ignores it: the
# emulator writes guest payload to host sockets with plain send(), and a peer
# that hangs up first otherwise takes the emulator down mid-instruction with no
# guru and a truncated log, which reads exactly like the Amiga crashed.
#
# --log is not optional here.  Without it write_log() goes to Amiberry's own
# amiberry_log.txt, which is off by default, and the emulator's stdout carries
# one line about an IPC socket, so the backend assertion below has nothing to
# read and a bridged run that silently came up on SLIRP cannot be told from one
# that did not.
( trap '' PIPE; exec "$AMIBERRY" --log -f "$CFG" ) >"$UAELOG" 2>&1 &
AMIBERRY_PID=$!

# serial_port=.../wait blocks the emulator until this connects, so retry until
# it does.  A single attempt loses the race often enough to matter, and the
# failure is not a missing log, it is an emulator that waits forever.
(
    for _ in $(seq 1 60); do
        kill -0 "$AMIBERRY_PID" 2>/dev/null || exit 0
        # Both readers append to $SERIAL identically; the python one also
        # stamps.  It exits non-zero without writing when it cannot connect,
        # so the retry above behaves as it did with nc alone.
        if [ -n "$SERIAL_READER" ]; then
            $SERIAL_READER 127.0.0.1 "$PORT" "$SERIAL" "$SERIALTS" \
                2>/dev/null && exit 0
        else
            nc 127.0.0.1 "$PORT" >> "$SERIAL" 2>/dev/null && exit 0
        fi
        sleep 0.5
    done
) &
NC_PID=$!

status=124
elapsed=0
EARLY_EXIT=0
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    if [ -f "$HD/.done" ]; then
        status=$(tr -dc '0-9' < "$HD/.done" | head -c 4)
        status=${status:-0}
        break
    fi
    if ! kill -0 "$AMIBERRY_PID" 2>/dev/null; then
        echo "!! amiberry exited early after ${elapsed}s" >&2
        EARLY_EXIT=1
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

EMU_PID=$AMIBERRY_PID
cleanup
wait "$EMU_PID" 2>/dev/null || true

# ------------------------------------------- illegal instruction assertion --
#
# The emulator says so, and nothing else has to be believed.
#
# A guest binary built for a newer CPU than the machine stops in its own C
# constructor before a line of the thing under test runs: no serial, no
# stdout.txt, the machine idling at 50 fps.  It reads as "the stack does not
# work on this processor" and costs a day.  It cost one on 2026-08-07, when
# CheckRunner carried `tst.l a0` -- 68020 only -- into an A600, and the log had
# said so from the first run:
#
#     Illegal instruction: 4a88 at 002186FA -> 00F80AD2
#
# ROM is excluded: Kickstart probes the CPU with MOVEC at boot on purpose and
# takes the exception itself.  Anything outside ROM is ours.
#
# This is the artifact, not a grep of the source.  It catches a wrong-CPU
# binary however it was built, including one somebody staged by hand.
# `|| true` because a clean run is the case where both greps match nothing:
# under `set -o pipefail` the pipeline is then 1, an assignment carries its
# command substitution's status, and `set -e` took the script out right here.
# Everything below -- the backend assertion, the serial dump, the guest's own
# exit status -- was unreachable on any run that did NOT crash, and the harness
# returned 1 for it.  Callers that read the guest's report rather than the exit
# status did not notice.
_illegal=$( { grep -aE "Illegal instruction: [0-9a-f]+ at [0-9A-F]+" "$UAELOG" 2>/dev/null |
              grep -avE "at 00F[0-9A-F]{5}" | head -3; } || true)
if [ -n "$_illegal" ]; then
    echo "!! ILLEGAL INSTRUCTION outside ROM -- a guest binary is built for a" >&2
    echo "!! newer CPU than this machine ($MODEL${CPU:+, -c $CPU}):" >&2
    printf '%s\n' "$_illegal" | sed 's/^/!!   /' >&2
    echo "!! Nothing after this point ran.  Rebuild that binary for the" >&2
    echo "!! machine; tools/lint-guest-arch.sh finds the build line." >&2
    ILLEGAL_SEEN=1
fi

# ------------------------------------------------------- backend assertion --
#
# A bridged run that quietly came up on SLIRP passes every check and proves
# nothing, so read the backend back out of the log rather than trusting the
# config.  UAENET logs the interface it opened; SLIRP does not go through
# UAENET at all, and the board lines carry the name either way.
if [ -n "$BOARD" ]; then
    case "$BACKEND" in
        slirp|slirp_inbound|none) ;;
        *)
            if grep -q "UAENET: '$BACKEND' open successful" "$UAELOG" 2>/dev/null; then
                echo "==> backend confirmed: uaenet opened '$BACKEND'"
                grep -oE "(7990|NE2000): '[^']*' [0-9A-Fa-f:]+" "$UAELOG" | head -1 || true
            else
                echo "!! ASKED FOR '$BACKEND' AND DID NOT GET IT." >&2
                echo "!! ethernet_getselectionname() falls back to slirp for any name" >&2
                echo "!! it cannot match, so this run was almost certainly on NAT and" >&2
                echo "!! anything it proved about the LAN is worthless.  From the log:" >&2
                grep -E "UAENET|7990:|NE2000:|slirp" "$UAELOG" 2>/dev/null | head -10 >&2
                [ "$status" = "0" ] && status=1
            fi ;;
    esac
fi

# ------------------------------------------------------------------- output --

echo "---- serial ($SERIAL) ----"
if [ -s "$SERIAL" ]; then
    cat "$SERIAL"
    [ ! -s "$SERIALTS" ] || echo "(same output, timestamped: $SERIALTS)"
else
    echo "(empty, no ami_log output reached the serial port)"
fi

for produced in "$HD"/*.txt "$HD"/*.log; do
    [ -f "$produced" ] || continue
    echo "---- $(basename "$produced") ----"
    cat "$produced"
done

if [ "$EARLY_EXIT" = "1" ]; then
    echo "---- amiberry log (tail) ----"
    tail -20 "$UAELOG" 2>/dev/null
fi

if [ "$status" = "124" ]; then
    echo "==> TIMEOUT after ${TIMEOUT}s (no DH0:.done)"
    # A timeout with NOTHING on the serial port is a different fault from a
    # slow one, and the two are indistinguishable unless this says so.  The
    # machine never reached the program: the model, the CPU and the ROM
    # disagree, or the binary is built for an architecture this machine does
    # not have -- a 68020 binary halts a 68000 on an illegal instruction
    # before a line of it executes.  A merely slow run still prints its banner
    # within seconds, so raising -t cannot fix this one.
    if [ ! -s "$SERIAL" ]; then
        echo "!! NOT ONE BYTE reached the serial port in ${TIMEOUT}s." >&2
        echo "!! This is not a slow run: the machine never ran the program." >&2
        echo "!! Check -m $MODEL / -c ${CPU:-default} / $(basename "$KICKSTART")" >&2
        echo "!! and that $EXE_NAME is built for that CPU.  Raising -t will not" >&2
        echo "!! help." >&2
    fi
else
    echo "==> exit status $status after $(( $(date +%s) - WALL_START ))s of host wall clock"
fi

# An illegal instruction outside ROM means nothing under test ran, so the run's
# own exit status describes nothing.  Exit 4, distinct from a timeout or a
# genuine failure, so a caller can tell "wrong binary" from "wrong code".
if [ "${ILLEGAL_SEEN:-0}" = "1" ]; then
    exit 4
fi
exit "$status"
