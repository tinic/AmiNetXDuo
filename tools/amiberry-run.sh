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
# The Linux counterpart of tools/winuae-run.sh.  Amiberry is WinUAE's core, so
# -N takes WinUAE's board keys unchanged and the board table below is
# tools/winuae-run.sh's.
#
# WHY THIS REPLACED FS-UAE
#
#   Amiberry runs genuinely headless, `headless=true` and SDL is never asked
#   for a video device, where FS-UAE needs an X server on a Linux runner.  It
#   emulates all nine ethernet boards rather than only the A2065.  And it has
#   backends that reach the real network: -B <interface> puts the guest on the
#   host's LAN with its own MAC, which is the thing SLIRP cannot do.
#   tools/enforcer-run.sh is the one runner left on FS-UAE, because Enforcer
#   needs a real MMU.
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
# EXIT STATUS
#
#   0..n  the test's own, read from DH0:.done
#   4     an illegal instruction outside ROM: the guest is built for a CPU
#         this machine does not have, so nothing under test ran
#   5     the run did not get the network backend it asked for, so anything
#         it proved about the LAN is worthless
#   124   the timeout expired with no DH0:.done
#
# 4 and 5 are DISTINCT FROM THE GUEST'S OWN CODES on purpose.  The backend
# fault used to be reported by overwriting a zero status with 1, and the one
# caller that reads it, tools/test-verdict.sh, then said "the guest exited 1"
# about a guest whose transcript ended `113 checks, 0 failures, PASS`.  A rig
# fault named as a code failure sends the reader to the wrong file.
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
#
# It is DERIVED FROM THE RUN TAG, near the tag itself below, because one fixed
# default is worse than no default at all: back-to-back runs then alias in the
# switch's and the router's ARP caches, the second run answers on a cache entry
# the first one put there, and a broken arm reads as a passing one.  That cost
# half an A/B on 2026-08-15.
# PCMCIA needs a 68020.  Rederived from scratch more than once, so it stops
# here rather than in an hour of somebody's afternoon.
#
# `-N ne2000_pcmcia` with cnet.device bisects to cpu_type and nothing else:
# 68000, 68010, cpu_compatible=false and cpu_multiplier=16 all answer
# `Could not add interface "eth0" (Input/output error)`, while 68020 leases,
# with or without address_space_24 or a slower multiplier.  ROM, chipset prefs,
# memory and cnet16.device were ruled out.  The two CPUs diverge inside
# card.resource's CIS walk, in gayle.cpp.
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
  alone: this is a CPU limit, not an A600 limit.

  -N a2065        works on an A600 at 68000, and is what you want for a 68000
  -c 68020        if the PCMCIA card itself is the thing under test
EOF
    exit 2
}
pcmcia_cpu_check "$BOARD"

# The keys themselves are in tools/emu-board.sh, shared with
# install/test/run-workbench.sh: the release gate writes its own config -- it
# has to, it boots Commodore's Startup-Sequence and this script overwrites one
# -- and a second copy of these keys is how that gate came to boot the A2065
# and nothing else.
# shellcheck source=emu-board.sh
. "$ROOT/tools/emu-board.sh"

board_lines() {
    emu_board_lines "$1" "$MAC" "$BACKEND" \
                    "${AMINETXDUO_AMIBERRY_BOARD_OPTIONS:-}" || exit 2
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

# THE SERIAL PORT IS ALLOCATED, NOT DERIVED.  It used to be
#
#     PORT=$((12000 + $(printf '%s' "$TAG" | cksum | cut -d' ' -f1) % 900))
#
# and $TAG is the PER-ARM tag, so two checkouts running the same arm always got
# the same number and two unrelated arms collided one time in 900.  With
# `serial_port=.../wait` the emulator listens and blocks until something
# connects, and nothing checked that the something was ours: on 2026-08-25
# three readers were measured on 12714 at once, and what came out was a
# transcript with two interfaces on one address and an arm that hung at 185 s
# while its faster siblings passed in 16 s.  tools/emu-rig-lock.sh has the long
# version and the mechanism.
#
# rig_claim_port both locks the number against every other harness in this tree
# and bind-probes it against everything else on the host, and HOLDS it -- the
# descriptor stays open for the life of this script, so the reservation cannot
# lapse between the probe and the emulator's own bind.
# shellcheck source=emu-rig-lock.sh
. "$ROOT/tools/emu-rig-lock.sh"
rig_claim_port "amiberry $TAG" || exit 2
PORT="$RIG_PORT"

# AND NO ORPHANED READER IS AIMED AT IT.  rig_port_readers has the mechanism
# and the reason it is anchored the way it is.  The reader's own pid goes in a
# file further down and the trap kills it on every path out, so this run cannot
# create one; this is the belt for the runs that started before that fix, or
# under a shell that was killed with -9.
#
# NOT KILLED FROM HERE.  Another agent's live run is indistinguishable from an
# orphan at this distance and reaping one would be worse than the collision.
# It refuses and names the process instead, which takes two seconds to act on.
_stale=$(rig_port_readers "$PORT")
if [ -n "$_stale" ]; then
    echo >&2
    echo "REFUSING to boot: a serial reader is already aimed at port $PORT." >&2
    printf '  %s\n' "$_stale" >&2
    echo >&2
    echo "  Nothing is listening there yet, so the port passed the bind" >&2
    echo "  probe -- and the moment this run's emulator binds it, that" >&2
    echo "  reader connects and takes this guest's transcript." >&2
    echo "  It is left alone rather than killed: from here an orphan and" >&2
    echo "  another agent's live run look identical." >&2
    exit 2
fi

# A RUN TOKEN, printed by the guest into its own transcript.  Recorded here so
# that a log read a week later can be matched to the run that produced it
# without believing a filename.  See the envsetup line in the Startup-Sequence
# and the check after the run.
RUNTOKEN="$(printf '%s-%s-%s' "$$" "$PORT" "$(date +%s)")"

# One MAC per tag, for the reason at the board table above.  The derivation and
# the reasoning behind its shape live in tools/emu-mac.sh, which is where every
# harness that needs one reads it from: install/test/run-workbench.sh had a
# pinned address of its own and a second copy of this would have been the next
# one to drift.
#
# AMINETXDUO_AMIBERRY_MAC still wins, because those harnesses set it
# deliberately and one of them wants a reservation to hold across runs.
# shellcheck source=emu-mac.sh
. "$ROOT/tools/emu-mac.sh"
MAC="${AMINETXDUO_AMIBERRY_MAC:-$(emu_mac_for_tag "$TAG")}"

# EXCEPT ON THE ONE BOARD WHERE THE EMULATOR THROWS THE MAC AWAY.  Amiberry
# instantiates the PCMCIA NE2000 with no autoconfig record at all
# (`ne2000->init(ne2000_board_state, NULL)`, gayle.cpp:1590), so the mac= we
# just wrote never reaches ne2000_init_2() and it falls back to the HOST
# INTERFACE's address.  Measured across every bridged pcmcia run on this rig:
# nine different mac= values in nine configs, and `NE2000: 'ens18'
# 3E:24:11:93:E8:8B` in all nine logs -- ens18's own address.  The A2065 is not
# affected; it takes the same option through a2065.cpp:1516 and honours it.
#
# So two bridged pcmcia guests are two machines at two addresses under ONE
# hardware address, which is not a collision anything reports.  The frames
# arrive, every neighbour cache on the LAN keeps whichever answered last, and
# what fails is an assertion somewhere else entirely -- a peer that reached the
# other run's listener, an arp table with the wrong owner in it.
#
# It cannot be fixed from here, so it is DETECTED instead: one bridged pcmcia
# run at a time on a host, and the second is refused with a sentence that says
# what to do.  SLIRP runs are untouched -- each has a NAT of its own and no
# shared segment to poison.
if [ -n "$BOARD" ] && ! emu_board_mac_honoured "$BOARD"; then
    case "$BACKEND" in
        slirp|slirp_inbound) ;;
        *)
            if ! rig_claim_name "bridged-$BOARD" "$TAG ($BACKEND) in $ROOT"; then
                echo >&2
                echo "REFUSING to start a second bridged $BOARD run on this host." >&2
                echo >&2
                echo "  Amiberry ignores mac= for this board and gives every" >&2
                echo "  guest the host interface's own address (gayle.cpp:1590)," >&2
                echo "  so two of these on one LAN are one hardware address at" >&2
                echo "  two IP addresses and they poison each other's ARP." >&2
                echo >&2
                echo "  Serialize them: wait for the run above to finish." >&2
                echo "  -B slirp needs no interlock, and -N a2065 honours mac=" >&2
                echo "  and may be run bridged in parallel." >&2
                exit 2
            fi
            echo "==> bridged $BOARD interlock held (mac= is ignored on this board)"
            ;;
    esac
fi

rm -rf "$HD"
mkdir -p "$HD/s" "$HD/c" "$HD/env" "$HD/envarc" "$HD/t" "$HD/clips" "$ROOT/build"
: > "$SERIAL"

cp "$EXE" "$HD/$EXE_NAME"
cp "$EXE" "$HD/c/$EXE_NAME"
# Extra files, into the root of the drive.  A DRAWER whose name is already
# there is MERGED and not nested: c/ exists because the tool under test goes
# in it, so `cp -R c "$HD/"` would have made DH0:C/C and left the Shell with
# no commands and no error.  A caller staging a Workbench C: means the one
# that is there.
for extra in "$@"; do
    dest="$HD/$(basename "$extra")"
    if [ -d "$extra" ] && [ -d "$dest" ]; then
        cp -R "$extra/." "$dest/"
    else
        cp -R "$extra" "$HD/"
    fi
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
# STACK, because the default is 4 KB and nothing here ever raised it.
# AmigaOS gives a Startup-Sequence command whatever the Shell's default is,
# 4096 bytes on 3.1, and there is no MMU: a program that runs past the end of
# it corrupts whatever is below rather than trapping. tests/tls/tls_https
# entered _nx_secure_tls_session_start() and never came back -- no crash, no
# output, a wedge that survived a 900 s ceiling -- while the same handshake
# through tls.library completed in 100.4 s from a Shell that had more.
# 8192, and it is a CEILING rather than a size to grow when something does not
# fit. A Shell hands a command 4096 by default and a considerate Shell-Startup
# raises it to about this; past that the harness is no longer running the
# program the way a user runs it, and a shipped command that outgrows a real
# stack would pass every test here and crash for them.
#
# A guest program that needs more brings its own, the way src/tools/fetch.c
# does with StackSwap() at :1028 -- which is exactly why fetch was immune to
# the overrun that wedged the TLS test on 4096.
STACK_BYTES="${AMINETXDUO_GUEST_STACK:-8192}"

# envsetup takes the run token and prints it out the serial port before
# anything else runs, so the FIRST line of every transcript says which run
# produced it.  A transcript is otherwise anonymous: the reader connects to a
# socket and writes what arrives, and if the socket belonged to another
# emulator there is nothing in the bytes to say so.  That is not a hypothetical
# -- it is what happened on 2026-08-25, and the arms that read a foreign
# transcript reported findings about a guest they never booted.
cat > "$HD/s/Startup-Sequence" <<EOF
failat 9999
c:envsetup $RUNTOKEN
stack $STACK_BYTES
$EXE_NAME${GUEST_ARGS:+ $GUEST_ARGS} >DH0:stdout.txt
echo >DH0:.done "\$RC"
EOF

# ------------------------------------------------------------------- config --

# The board decides the ceiling, and the reasoning is in tools/emu-board.sh
# beside the rest of what a board needs from the machine.  It was here, where
# install/test/run-workbench.sh could not see it, and that is how the release
# gate came to write fastmem_size=8 under a PCMCIA card: cnet.device answered
# `cannot open cnet.device unit 0 (-1)` and nothing else.
FASTMEM=$(emu_board_fastmem "$BOARD" 8)

# AMINETXDUO_FASTMEM=<MB> takes it away again, 0 included.  The pool and the
# receive window come off AvailMem(), so the 1 MB machine in
# src/sana2/sana2_internal.h is a configuration and not a hypothesis, and it
# is only reachable from here: Z3MEM only ever adds.
FASTMEM="${AMINETXDUO_FASTMEM:-$FASTMEM}"

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
# cpu_model, not cpu_type, for anything above the 68000.
#
# Amiberry takes `cpu_type=68030' and answers
#   "no CPU emulation cores available CPU=680000!"
# then aborts in about two seconds -- which reads exactly like a guest that
# failed to boot, and cost an investigation an arm before anyone noticed the
# emulator had never started. `-c 68000' happens to work, so the two callers
# that use it were never affected and this stayed hidden.
#
# The docs/RESEARCH note at the top of this file about cpu_type bisecting the
# ne2000_pcmcia failure predates this and refers to the 68000 case.
if [ -n "$CPU" ]; then
    case "$CPU" in
        68000) echo "cpu_type=$CPU"  >> "$CFG" ;;
        *)     echo "cpu_model=$CPU" >> "$CFG" ;;
    esac
fi

# ------------------------------------------------------------- -k CLOCK MHz --
#
# Move the CPU clock and keep the cycle accounting.  Amiberry is WinUAE's
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
LOGCAP_PID=""
LOGPIPE=""

# WHERE THE ORPHANS CAME FROM.  $NC_PID is the retry SUBSHELL, and the reader
# is a child of it; killing the subshell left `python3 tools/serial-timestamp.py`
# running, still connected, still holding a socket, for as long as the machine
# was up.  `pgrep -af serial-timestamp.py` on playhouse3 routinely showed
# readers belonging to harnesses that had exited hours before, and the first
# thing anyone diagnosing a collision had to do was work out which of them were
# alive.  The subshell writes the reader's pid into $READERPID and cleanup
# kills that too, so a run reaps what it started -- on every path out,
# including the failure ones, because the trap covers them all.
READERPID="$ROOT/build/amiberry-$TAG.readerpid"
rm -f "$READERPID"

cleanup() {
    local rp
    rp=$(cat "$READERPID" 2>/dev/null || true)
    [ -n "$NC_PID" ] && kill -TERM "$NC_PID" 2>/dev/null || true
    NC_PID=""
    [ -n "$rp" ] && kill -TERM "$rp" 2>/dev/null || true
    rm -f "$READERPID"
    if [ -n "$AMIBERRY_PID" ]; then
        kill -TERM "$AMIBERRY_PID" 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$AMIBERRY_PID" 2>/dev/null || { AMIBERRY_PID=""; break; }
            sleep 0.5
        done
        if [ -n "$AMIBERRY_PID" ]; then
            echo "!! amiberry $AMIBERRY_PID ignored SIGTERM; sending SIGKILL" >&2
            kill -KILL "$AMIBERRY_PID" 2>/dev/null || true
            AMIBERRY_PID=""
        fi
    fi
    # And the port goes back only once nothing of ours can still be on it.
    rig_release_port
    return 0
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

# SIGPIPE is ignored: the
# emulator writes guest payload to host sockets with plain send(), and a peer
# that hangs up first otherwise takes the emulator down mid-instruction with no
# guru and a truncated log, which reads exactly like the Amiga crashed.
#
# --log is not optional here.  Without it write_log() goes to Amiberry's own
# amiberry_log.txt, which is off by default, and the emulator's stdout carries
# one line about an IPC socket, so the backend assertion below has nothing to
# read and a bridged run that silently came up on SLIRP cannot be told from one
# that did not.
#
# AND IT IS UNBOUNDED, so it goes through tools/logcap.sh.  Measured at 87 MB
# per 30 s on an RTG guest, 3.3 GB an hour; it has filled playhouse3 three
# times, 30 GB across five stale logs on the last sweep with 27.6 GB of that in
# one file.  What it writes is a run of identical `Denise queue without lock!`
# lines, and the disk is the smaller cost: an arm whose serial log is 0 bytes
# while that runs reads exactly like a driver hang in the code under test.
# Collapsed it is one line and a count, which is a sentence somebody can act
# on.  The cap keeps the head, where every line anything greps for is printed
# -- the board and MAC lines, UAENET's, the first illegal instruction -- and a
# ring of the last lines for the `tail -20` below.
#
# THROUGH A FIFO rather than a pipeline, because $! after `a | b` is b, and
# AMIBERRY_PID has to be amiberry: cleanup kills it and the loop below watches
# it for an early exit.
#
# And it degrades to the plain redirect if the capper is not there: opening a
# FIFO for writing blocks until something opens it for reading, so a missing
# reader would hang the run rather than lose a log.
LOGPIPE="$ROOT/build/amiberry-$TAG.logpipe"
rm -f "$LOGPIPE"
if [ -x "$ROOT/tools/logcap.sh" ] && mkfifo "$LOGPIPE" 2>/dev/null; then
    "$ROOT/tools/logcap.sh" < "$LOGPIPE" > "$UAELOG" &
    LOGCAP_PID=$!
else
    echo "!! no tools/logcap.sh; $UAELOG is UNCAPPED for this run" >&2
    rm -f "$LOGPIPE"
    LOGPIPE=""
fi
if [ -n "$LOGPIPE" ]; then
    ( trap '' PIPE; exec "$AMIBERRY" --log -f "$CFG" ) >"$LOGPIPE" 2>&1 &
else
    ( trap '' PIPE; exec "$AMIBERRY" --log -f "$CFG" ) >"$UAELOG" 2>&1 &
fi
AMIBERRY_PID=$!

# ------------------------------------------- WHOSE LISTENER IS ON THIS PORT --
#
# The reservation above makes a collision very unlikely.  Being able to SAY SO
# is a different and stronger property, and it is the one that was missing:
# nothing checked, so nothing could report, and an arm read a foreign guest and
# published findings about a machine it never booted.
#
# WHAT CANNOT BE DONE HERE, so nobody spends an afternoon on it again.  The
# obvious check is "is the process listening on $PORT the amiberry we started",
# and on this rig it has NO ANSWER.  Amiberry carries file capabilities --
#
#     amiberry cap_net_admin,cap_net_raw=eip
#
# which the bridged backends need -- and a process that gains capabilities from
# its executable is marked NON-DUMPABLE: the kernel reparents its /proc entry
# to root, so /proc/<pid>/fd is unreadable even by the user who started it, and
# ss(8) declines to name it for the same reason.  Measured: a live amiberry
# listening on 127.0.0.1:12709 shows as `LISTEN 0 1 127.0.0.1:12709` with an
# EMPTY Process column, while sshd's socket in the same output carries its pid.
#
# So the check that is made is the one that has an answer:
#
#   the port we reserved is bound at all, which says the emulator got the
#     number this script chose rather than failing over to something else;
#   the GUEST'S OWN TOKEN, below and in the run loop, which is a stronger
#     statement than socket ownership anyway -- it is the machine under test
#     saying which run it belongs to, in the artefact that outlives the rig.
LISTEN_INODE=""
for _ in $(seq 1 40); do
    kill -0 "$AMIBERRY_PID" 2>/dev/null || break
    LISTEN_INODE=$(rig_listen_inode "$PORT" || true)
    [ -n "$LISTEN_INODE" ] && break
    sleep 0.5
done

if [ -z "$LISTEN_INODE" ]; then
    echo "==> serial port $PORT: nothing bound it yet"
elif rig_pid_holds_socket "$AMIBERRY_PID" "$LISTEN_INODE" 2>/dev/null; then
    echo "==> serial port $PORT, held by amiberry $AMIBERRY_PID"
else
    echo "==> serial port $PORT bound (owner opaque: amiberry has file caps)"
fi

# serial_port=.../wait blocks the emulator until this connects, so retry until
# it does.  A single attempt loses the race often enough to matter, and the
# failure is not a missing log, it is an emulator that waits forever.
#
# THE READER'S PID GOES IN A FILE.  $! here is the subshell, and the reader is
# its child; cleanup needs the child, or it leaks (see $READERPID above).  The
# port lock is closed inside the subshell for the same reason in reverse: an
# inherited descriptor would keep the port reserved for as long as any stray
# reader lived, which is the leak this whole file is about, only quieter.
(
    [ -z "${RIG_PORT_FD:-}" ] || eval "exec ${RIG_PORT_FD}>&-" 2>/dev/null || true
    reader=""
    trap '[ -z "$reader" ] || kill -TERM "$reader" 2>/dev/null; exit 0' TERM INT
    for _ in $(seq 1 60); do
        kill -0 "$AMIBERRY_PID" 2>/dev/null || exit 0
        # Both readers append to $SERIAL identically; the python one also
        # stamps.  It exits non-zero without writing when it cannot connect,
        # so the retry above behaves as it did with nc alone.
        if [ -n "$SERIAL_READER" ]; then
            $SERIAL_READER 127.0.0.1 "$PORT" "$SERIAL" "$SERIALTS" \
                2>/dev/null &
        else
            nc 127.0.0.1 "$PORT" >> "$SERIAL" 2>/dev/null &
        fi
        reader=$!
        echo "$reader" > "$READERPID"
        wait "$reader" && exit 0
        sleep 0.5
    done
) &
NC_PID=$!

status=124
elapsed=0
EARLY_EXIT=0
TOKEN_SEEN=0
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    # THE PAIRING CHECK, AS SOON AS THERE IS ANYTHING TO CHECK.  The same
    # assertion is made again after the run, over the finished file, but by
    # then a foreign transcript has already been read for the whole timeout --
    # and on 2026-08-25 that was 185 s of an arm being driven by somebody
    # else's guest.  One grep of the first line a second stops it at the first
    # line.  Costs nothing: $SERIAL is empty until the guest speaks.
    if [ "$TOKEN_SEEN" = 0 ] && [ -s "$SERIAL" ]; then
        _tok=$(tr -d '\r' < "$SERIAL" 2>/dev/null |
               grep -m1 '^ANXD-RUN ' || true)
        if [ -n "$_tok" ]; then
            TOKEN_SEEN=1
            if [ "$_tok" != "ANXD-RUN $RUNTOKEN" ]; then
                echo >&2
                echo "!! THIS IS NOT OUR GUEST -- stopping at its first line." >&2
                echo "!!   expected: ANXD-RUN $RUNTOKEN" >&2
                echo "!!   arriving: $_tok" >&2
                echo "!! Something else is on serial port $PORT.  Lock" >&2
                echo "!! directory: $(rig_lockdir)" >&2
                exit 2
            fi
        fi
    fi
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

# The capper still has the tail of the run in its ring, so nothing may read
# $UAELOG until it has reached EOF and printed it.  Every assertion below reads
# that file.
if [ -n "$LOGCAP_PID" ]; then
    wait "$LOGCAP_PID" 2>/dev/null || true
    LOGCAP_PID=""
fi
[ -z "$LOGPIPE" ] || rm -f "$LOGPIPE"

# ------------------------------------------------- WHOSE TRANSCRIPT IS THIS --
#
# The listener check above asked the question of the host, before a byte was
# read.  This asks it of the ARTEFACT, which is the copy that survives: a
# transcript on disk a week later has no pid, no port and no lock directory,
# only the bytes the guest sent.  envsetup prints `ANXD-RUN <token>` as its
# first act, so a transcript either opens with this run's token or it did not
# come from this run.
#
# THE POLARITY MATTERS.  A DIFFERENT token is fatal: it is a foreign guest, and
# every assertion downstream would be reading someone else's machine.  NO token
# is only a note -- a guest can die in the ROM before AmigaDOS runs a line of
# the Startup-Sequence, and that is a real failure with its own diagnosis
# further down; turning it into "the transcript is foreign" would put the wrong
# word on it.
#
# THE CR IS NOT OPTIONAL.  Exec's raw serial output turns the LF into CR LF, so
# every line in $SERIAL ends `\r\n`; an anchored `$` match without stripping it
# reports this run's own token as foreign, which is a check that fires on
# everything and is therefore worse than no check.  Caught the first time this
# ran.
_foreign=$(tr -d '\r' < "$SERIAL" 2>/dev/null | grep -a '^ANXD-RUN ' |
           grep -av "^ANXD-RUN $RUNTOKEN\$" | head -1 || true)
if [ -n "$_foreign" ]; then
    echo >&2
    echo "!! THIS IS NOT THIS RUN'S TRANSCRIPT." >&2
    echo "!!   expected: ANXD-RUN $RUNTOKEN" >&2
    echo "!!   found:    $_foreign" >&2
    echo "!! $SERIAL was written by another emulator's guest." >&2
    exit 2
fi
if ! tr -d '\r' < "$SERIAL" 2>/dev/null |
        grep -qa "^ANXD-RUN $RUNTOKEN\$"; then
    echo "!! no run token in $SERIAL: the guest did not reach c:envsetup" >&2
fi

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
                BACKEND_MISSING=1
            fi ;;
    esac
fi

# ------------------------------------------------------------------- output --

echo "---- serial ($SERIAL) ----"
# serial_bytes is a field a caller can read.  Thirteen of twenty serial logs in
# one session were 0 bytes and every harness assertion reading one of them
# passed, so the size is part of the run's report and not a thing to infer.
echo "serial=$SERIAL serial_bytes=$(wc -c < "$SERIAL" | tr -d ' ')"
if [ -s "$SERIAL" ]; then
    cat "$SERIAL"
    [ ! -s "$SERIALTS" ] || echo "(same output, timestamped: $SERIALTS)"
else
    # THIS IS ALMOST ALWAYS THE BUILD, not the serial path.  AMI_ERROR,
    # AMI_WARN and AMI_INFO compile to `if (0)` unless AMINETXDUO_LOG is
    # defined and it is OFF by default, so a library built the ordinary way
    # never writes a byte here.  What is left is whatever the guest program
    # puts on the port itself with RawPutChar, and a ToolsSmoke guest puts
    # nothing.  Measured on this rig: tests/stack wrote 0 bytes against a
    # default build and 3,847 against -DAMINETXDUO_LOG=ON, same minute, same
    # machine.
    echo "(empty, nothing was written to the serial port)"
    echo "  The library's AMI_ERROR/AMI_WARN/AMI_INFO calls compile to nothing"
    echo "  without -DAMINETXDUO_LOG=ON, and the guest program writes to the"
    echo "  port only if it carries a RawPutChar tracer of its own.  Rebuild"
    echo "  with -DAMINETXDUO_LOG=ON -DAMINETXDUO_LOG_LEVEL=2 for a log."
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

# Same argument for the backend: a run that came up on something other than
# what it asked for proved nothing about the LAN, and that is a fault of the
# rig rather than of the guest.  It used to be reported as status 1, which
# tools/test-verdict.sh rendered as "the guest exited 1" over a transcript
# that said PASS.
if [ "${BACKEND_MISSING:-0}" = "1" ]; then
    exit 5
fi
exit "$status"
