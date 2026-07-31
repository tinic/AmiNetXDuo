#!/usr/bin/env bash
#
# Run an AmigaOS executable under Amiberry on Linux and capture its output.
#
#   tools/amiberry-run.sh [-t SECONDS] [-m MODEL] [-c CPU] [-N BOARD]
#                         [-B BACKEND] <executable> [extra files...]
#
# The Linux counterpart of tools/fsuae-run.sh and tools/winuae-run.sh.  Amiberry
# is WinUAE's core, so -N takes WinUAE's board keys unchanged and the board
# table below is tools/winuae-run.sh's.
#
# WHY THIS EXISTS ALONGSIDE fsuae-run.sh
#
#   Amiberry runs genuinely headless -- `headless=true` and SDL is never asked
#   for a video device, where FS-UAE needs an X server on a Linux runner.  It
#   emulates all nine ethernet boards rather than only the A2065.  And it has
#   backends that reach the real network: -B <interface> puts the guest on the
#   host's LAN with its own MAC, which is the thing SLIRP cannot do.
#
# -B PICKS THE BACKEND, AND IT IS CHECKED
#
#   slirp           user-mode NAT, 10.0.2.0/24, gateway 10.0.2.2 (the default)
#   slirp_inbound   the same, with the standard ports forwarded in
#   <interface>     a host NIC (`ens18`), a bridge, or a tap device -- the
#                   guest is then a machine on the host's own LAN
#
#   THE NAME GOES IN BARE, not as `netmode=<name>`.  It is an
#   EXPANSIONBOARD_MULTI, and cfgfile_read_rom_settings() picks the item by
#   looking for each candidate as an option of its own; `netmode=ens18` parses
#   as an option called netmode, matches no item, and selects index 0 -- which
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
#   configured for the machine -- `cmake -DAMINETXDUO_CPU=68000` for an A500 or
#   an A600 -- or the run dies the same way for the same reason.
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
#   until something connects, so the host has to RETRY -- losing that race
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
BOARD=""
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"

while getopts "t:m:c:N:B:" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        *) echo "usage: $0 [-t seconds] [-m model] [-c cpu] [-N board] [-B backend] <executable> [files...]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

[ $# -ge 1 ] || { echo "usage: $0 [-t seconds] [-m model] [-N board] [-B backend] <executable> [files...]" >&2; exit 2; }

EXE="$1"; shift
[ -f "$EXE" ] || { echo "no such executable: $EXE" >&2; exit 2; }
EXE_NAME=$(basename "$EXE")

AMIBERRY="${AMIBERRY:-}"
if [ -z "$AMIBERRY" ]; then
    for candidate in "$(command -v amiberry || true)" "$HOME/amiberry/build/amiberry"; do
        [ -n "$candidate" ] && [ -x "$candidate" ] && { AMIBERRY="$candidate"; break; }
    done
fi
[ -n "$AMIBERRY" ] && [ -x "$AMIBERRY" ] || {
    echo "amiberry not found; set AMIBERRY=<path>" >&2; exit 2
}

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
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    cat >&2 <<'EOF'
No boot ROM.  Either:

  eval "$(tools/fetch-aros-rom.sh --export)"   # free, what CI uses
  export AMINETXDUO_KICKSTART=<kickstart.rom>  # a real one, not redistributable
EOF
    exit 2
}
[ -z "$KICKSTART_EXT" ] || [ -f "$KICKSTART_EXT" ] || {
    echo "AMINETXDUO_KICKSTART_EXT=$KICKSTART_EXT does not exist" >&2; exit 2
}

# ------------------------------------------------------------------- boards --
#
# The keys are WinUAE's, because Amiberry parses WinUAE's config file.  See
# tools/winuae-run.sh for what each board is and which driver it wants; the
# driver is staged by tools/sana2-stage.sh, not here.
#
# The MAC is set explicitly on a bridged run.  Left alone the emulator invents
# one, so a DHCP server hands out a different lease every time and nothing on
# the LAN can hold a reservation.  The A2065 keeps only the last three bytes --
# a2065.cpp overwrites the first three with Commodore's 00:80:10 -- while the
# NE2000 boards take the whole address.
MAC="${AMINETXDUO_AMIBERRY_MAC:-02:41:4d:49:00:01}"

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
        # initialises -- which reads from the guest as a driver that cannot
        # find its hardware.  Needs a machine with a Gayle: an A600 or an A1200.
        ne2000_pcmcia) printf 'pcmcia=true\nne2000pcmcia_rom_file=:ENABLED\nne2000pcmcia_rom_options=inserted=true,mac=%s,%s\n' "$MAC" "$BACKEND" ;;
        *)             echo "unknown network board $1" >&2; exit 2 ;;
    esac
}

# ------------------------------------------------------------------ staging --

TAG="${AMINETXDUO_RUN_TAG:-amiberry}"
HD="$ROOT/build/amiberry-testhd-$TAG"
SERIAL="$ROOT/build/amiberry-serial-$TAG.log"
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
cat > "$HD/s/Startup-Sequence" <<EOF
failat 9999
c:envsetup
$EXE_NAME >DH0:stdout.txt
echo >DH0:.done "\$RC"
EOF

# ------------------------------------------------------------------- config --

# 8 MB of Zorro II Fast RAM covers 0x200000-0x9fffff, and an A1200's PCMCIA
# windows are at 0x600000 (common) and 0xa00000 (attribute).  They overlap, on
# the real machine as well as here, which is why an A1200 with a PCMCIA card
# cannot have 8 MB on the trapdoor bus.  Under emulation the collision does not
# announce itself: the card is logged as inserted, the backend opens, and the
# driver simply fails to find it -- cnet.device came back with
# `cannot open cnet.device unit 0 (-1)` and nothing else.  4 MB stops short of
# 0x600000 and the same run works.
FASTMEM=8
[ "$BOARD" = ne2000_pcmcia ] && FASTMEM=4

cat > "$CFG" <<EOF
config_description=AmiNetXDuo $TAG
use_gui=no
headless=true
quickstart=$MODEL,0
kickstart_rom_file=$KICKSTART
fastmem_size=$FASTMEM
floppy0type=-1
nr_floppies=0
uaehf0=dir,rw,DH0:DH0:$HD,0
serial_port=tcp://127.0.0.1:$PORT/wait
EOF

[ -z "$KICKSTART_EXT" ] || echo "kickstart_ext_rom_file=$KICKSTART_EXT" >> "$CFG"
[ -z "$CPU" ] || echo "cpu_type=$CPU" >> "$CFG"
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
# no driver -- so a run over ssh works only for as long as X11 forwarding does,
# and dies with "No available video device" the moment it does not.  The dummy
# driver is what makes this reproducible on a headless runner.  DISPLAY is
# cleared for the same reason: a stale one is worse than none.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
[ "${SDL_VIDEODRIVER}" = "dummy" ] && unset DISPLAY WAYLAND_DISPLAY || true

WALL_START=$(date +%s)

# SIGPIPE is ignored for the same reason tools/fsuae-run.sh ignores it: the
# emulator writes guest payload to host sockets with plain send(), and a peer
# that hangs up first otherwise takes the emulator down mid-instruction with no
# guru and a truncated log -- which reads exactly like the Amiga crashed.
#
# --log is not optional here.  Without it write_log() goes to Amiberry's own
# amiberry_log.txt, which is off by default, and the emulator's stdout carries
# one line about an IPC socket -- so the backend assertion below has nothing to
# read and a bridged run that silently came up on SLIRP cannot be told from one
# that did not.
( trap '' PIPE; exec "$AMIBERRY" --log -f "$CFG" ) >"$UAELOG" 2>&1 &
AMIBERRY_PID=$!

# serial_port=.../wait blocks the emulator until this connects, so retry until
# it does.  A single attempt loses the race often enough to matter, and the
# failure is not a missing log -- it is an emulator that waits forever.
(
    for _ in $(seq 1 60); do
        kill -0 "$AMIBERRY_PID" 2>/dev/null || exit 0
        nc 127.0.0.1 "$PORT" >> "$SERIAL" 2>/dev/null && exit 0
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
else
    echo "(empty -- no ami_log output reached the serial port)"
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
else
    echo "==> exit status $status after $(( $(date +%s) - WALL_START ))s of host wall clock"
fi
exit "$status"
