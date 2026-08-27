#!/usr/bin/env bash
#
# Run an AmigaOS executable under Amiberry on Linux and capture its output.
#
#   tools/amiberry-run.sh [-t SECONDS] [-m MODEL] [-c CPU] [-N BOARD]...
#                         [-B BACKEND] [-a ARGS] <executable> [extra files...]
#
# -N takes WinUAE's board keys unchanged, AND MAY BE REPEATED (or given a
# comma-separated list) to put SEVERAL boards in one machine.  Each board gets
# an address of its own, and the second one is what makes a non-zero SANA-II
# unit reachable: anxnet.device numbers its units in probe order, so a machine
# with one board has only unit 0 to open.  -B is slirp, slirp_inbound or a host
# interface name, and the name goes in BARE, not `netmode=<name>`; an unmatched
# name silently selects slirp, so the run is verified against the emulator log.
# A host NIC needs CAP_NET_RAW on the amiberry binary, a tap or bridge
# CAP_NET_ADMIN; both are cleared by every relink and ignored on a nosuid mount.
# -m does not rebuild the executable: pass one built for that CPU.
# Serial is a listening TCP socket, not a file: the host has to RETRY the
# connect, and losing that race leaves an emulator waiting forever.
# Exit 4 (illegal instruction outside ROM) and 5 (wrong network backend) are rig
# faults, deliberately distinct from the guest's own codes; 124 is the timeout.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
TIMEOUT=120
MODEL=A1200
CPU=""
CLOCK=""
BOARD=""
# EXPANDED AS ${BOARDS[@]+...} EVERYWHERE BELOW.  Bash before 4.4 takes
# "${empty[@]}" as an unbound variable under set -u and takes the run out with
# a message about a name nobody wrote, and a run with no -N at all -- which is
# most of the emulator tier -- is exactly that case.
BOARDS=()
BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"
GUEST_ARGS="${AMINETXDUO_GUEST_ARGS:-}"

USAGE="usage: $0 [-t seconds] [-m model] [-c cpu] [-N board]... [-B backend] [-a args] <executable> [files...]"

while getopts "t:m:c:k:N:B:a:" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        k) CLOCK="$OPTARG" ;;
        N) IFS=, read -r -a _n <<< "$OPTARG"
           BOARDS+=("${_n[@]}") ;;
        B) BACKEND="$OPTARG" ;;
        a) GUEST_ARGS="$OPTARG" ;;
        *) echo "$USAGE" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

[ $# -ge 1 ] || { echo "$USAGE" >&2; exit 2; }

# One board key may appear once.  Amiberry has ONE config key per board type,
# so `-N a2065 -N a2065` writes a2065_rom_options twice and the second line
# wins: one card in the machine, silently, and an arm that believed it asked
# for two.  Two of a kind is not reachable from here and saying so is the whole
# of the fix.
for _i in ${BOARDS[@]+"${!BOARDS[@]}"}; do
    for _j in ${BOARDS[@]+"${!BOARDS[@]}"}; do
        [ "$_i" -lt "$_j" ] || continue
        [ "${BOARDS[$_i]}" != "${BOARDS[$_j]}" ] || {
            echo "-N ${BOARDS[$_i]} was given twice.  Amiberry keeps one" >&2
            echo "config key per board type, so the second would overwrite" >&2
            echo "the first and the machine would have one card in it." >&2
            echo "Two boards means two DIFFERENT boards." >&2
            exit 2; }
    done
done

# $BOARD is the FIRST board and every single-board path below still reads it,
# so a one-board run is byte for byte the run it always was.
BOARD="${BOARDS[0]:-}"

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
# The keys are WinUAE's, because Amiberry parses WinUAE's config file; the
# driver is staged by tools/sana2-stage.sh, not here.
#
# The MAC is set explicitly on a bridged run and DERIVED FROM THE RUN TAG: one
# fixed default aliases in the switch's and the router's ARP caches, and a
# broken arm then reads as a passing one.  The A2065 keeps only the last three
# bytes (a2065.cpp overwrites the first three); the NE2000 boards take all six.
#
# `-N ne2000_pcmcia` NEEDS A 68020.  68000, 68010, cpu_compatible=false and
# cpu_multiplier=16 all fail in card.resource's CIS walk, in gayle.cpp.  A CPU
# limit, not a model limit: an A600 networks perfectly well on -N a2065.
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
for _b in ${BOARDS[@]+"${BOARDS[@]}"}; do
    pcmcia_cpu_check "$_b"
done

# The keys themselves are in tools/emu-board.sh, shared with
# install/test/run-workbench.sh: the release gate writes its own config -- it
# has to, it boots Commodore's Startup-Sequence and this script overwrites one
# -- and a second copy of these keys is how that gate came to boot the A2065
# and nothing else.
# shellcheck source=emu-board.sh
. "$ROOT/tools/emu-board.sh"

board_lines() { # board mac
    emu_board_lines "$1" "$2" "$BACKEND" \
                    "${AMINETXDUO_AMIBERRY_BOARD_OPTIONS:-}" || exit 2
}

# THE ADDRESS FOR THE Nth BOARD.  Board 0 is the run's own address and is
# unchanged, so nothing that ever ran one board moves.  A later board goes
# through the same derivation under a modified tag rather than by arithmetic on
# the first address: two boards in one machine must differ from each other AND
# from every other run's, and the tag hash is what already guarantees the
# second half of that.
#
# AMINETXDUO_AMIBERRY_MAC PINS THE FIRST BOARD ONLY.  A caller that needs a
# fixed address for a second one has no board to pin it to today; the harnesses
# that pin (the release gate, the card sweeps) all boot one card.
board_mac() { # index
    if [ "$1" = 0 ]; then
        printf '%s\n' "$MAC"
    else
        emu_mac_for_tag "$TAG#$1"
    fi
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

# Functional bridged runs share the rig; measurements take it exclusively.
# Both use the same lock, so a throughput number cannot be collected while a
# second emulator is consuming the host NIC or CPUs.  SLIRP has neither shared
# resource and stays outside this interlock.
case "$BACKEND" in
    slirp|slirp_inbound|none) ;;
    *)
        if [ -n "${AMINETXDUO_RIG_EXCLUSIVE:-}" ]; then
            rig_claim_name bridged-rig \
                "$TAG ($BACKEND): $AMINETXDUO_RIG_EXCLUSIVE" || exit 2
            echo "==> exclusive bridged-rig measurement lock held"
        else
            rig_claim_name_shared bridged-rig "$TAG ($BACKEND) in $ROOT" || exit 2
            echo "==> shared bridged-rig lock held"
        fi
        ;;
esac

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
for _b in ${BOARDS[@]+"${BOARDS[@]}"}; do
    emu_board_mac_honoured "$_b" || _macless="$_b"
done
if [ -n "${_macless:-}" ]; then
    BOARD_MACLESS="$_macless"
    case "$BACKEND" in
        slirp|slirp_inbound) ;;
        *)
            if ! rig_claim_name "bridged-$BOARD_MACLESS" "$TAG ($BACKEND) in $ROOT"; then
                echo >&2
                echo "REFUSING to start a second bridged $BOARD_MACLESS run on this host." >&2
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
            echo "==> bridged $BOARD_MACLESS interlock held (mac= is ignored on this board)"
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
# code at or above the fail level, so without it a test that exits nonzero never
# reaches the line recording its status and the run merely times out.
#
# GUEST_ARGS goes in verbatim, so the AmigaDOS shell does the quoting.
#
# STACK_BYTES is a CEILING, not a size to grow when something does not fit.  A
# Shell hands a command 4096 by default and there is no MMU, so a program that
# runs past the end corrupts whatever is below rather than trapping; a command
# that outgrows a real user's stack must not pass here.  A guest that needs more
# brings its own, the way src/tools/fetch.c does with StackSwap().
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
# The LOWEST ceiling any board in the machine asks for: a PCMCIA card's
# windows collide with 8 MB of Zorro II Fast RAM whether or not there is a
# Zorro card beside it.
FASTMEM=8
for _b in ${BOARDS[@]+"${BOARDS[@]}"}; do
    _f=$(emu_board_fastmem "$_b" 8)
    [ "$_f" -ge "$FASTMEM" ] || FASTMEM="$_f"
done

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
_i=0
for _b in ${BOARDS[@]+"${BOARDS[@]}"}; do
    board_lines "$_b" "$(board_mac "$_i")" >> "$CFG"
    _i=$((_i + 1))
done

# AMINETXDUO_AMIBERRY_EXTRA appends raw `key=value` lines, semicolon separated,
# so the settings above can be swept without editing this file.
if [ -n "${AMINETXDUO_AMIBERRY_EXTRA:-}" ]; then
    printf '%s\n' "${AMINETXDUO_AMIBERRY_EXTRA}" | tr ';' '\n' >> "$CFG"
    echo "==> extra config: ${AMINETXDUO_AMIBERRY_EXTRA}"
fi

_i=0
for _b in ${BOARDS[@]+"${BOARDS[@]}"}; do
    echo "==> board$_i=$_b backend=$BACKEND mac=$(board_mac "$_i")"
    _i=$((_i + 1))
done
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

# SIGPIPE is ignored: the emulator writes guest payload to host sockets with
# plain send(), and a peer that hangs up first otherwise takes it down
# mid-instruction with no guru and a truncated log.
#
# --log is not optional: without it write_log() goes to a file that is off by
# default and the backend assertion below has nothing to read.  It is UNBOUNDED
# (measured 87 MB per 30 s on an RTG guest), so it goes through tools/logcap.sh,
# THROUGH A FIFO rather than a pipeline, because $! after `a | b` is b and
# AMIBERRY_PID has to be amiberry.  It degrades to the plain redirect if the
# capper is missing: opening a FIFO for writing blocks until a reader opens it,
# so that would hang the run rather than lose a log.
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
# "Is the process listening on $PORT the amiberry we started" HAS NO ANSWER
# here: amiberry carries file capabilities, and a process that gains them from
# its executable is non-dumpable, so /proc/<pid>/fd is unreadable even by its
# owner and ss(8) declines to name it.
#
# What is checked is what has an answer: the reserved port is bound at all, and
# the guest's own token below, which outlives the rig anyway.
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
    #
    # A LINE THAT IS STILL ARRIVING IS NOT A DIFFERENT LINE.  grep matches the
    # last line of a file whether or not it ends in a newline, and a serial
    # transcript is being written a byte at a time: the first poll after the
    # guest speaks sees `ANXD-RUN 1310` of `ANXD-RUN 1310034-12846-1787698200`
    # and a plain != called our own guest a stranger and took the arm down with
    # `run_rc=2`, measured on the 68060 cpuspeed arm.  So a token that is a
    # PREFIX of the expected one is "not yet", and only a token that DIVERGES
    # is somebody else -- which still stops at the first differing byte, which
    # is what this check is for.  TOKEN_SEEN is set when the whole token has
    # arrived, so the poll keeps looking until it decides.
    if [ "$TOKEN_SEEN" = 0 ] && [ -s "$SERIAL" ]; then
        _tok=$(tr -d '\r' < "$SERIAL" 2>/dev/null |
               grep -m1 '^ANXD-RUN ' || true)
        if [ -n "$_tok" ]; then
            case "ANXD-RUN $RUNTOKEN" in
            "$_tok"*)
                [ "$_tok" = "ANXD-RUN $RUNTOKEN" ] && TOKEN_SEEN=1
                ;;
            *)
                TOKEN_SEEN=1
                echo >&2
                echo "!! THIS IS NOT OUR GUEST -- stopping at its first line." >&2
                echo "!!   expected: ANXD-RUN $RUNTOKEN" >&2
                echo "!!   arriving: $_tok" >&2
                echo "!! Something else is on serial port $PORT.  Lock" >&2
                echo "!! directory: $(rig_lockdir)" >&2
                exit 2
                ;;
            esac
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
# envsetup prints `ANXD-RUN <token>` as its first act, so the ARTEFACT itself
# says which run wrote it.  THE POLARITY MATTERS: a DIFFERENT token is fatal, a
# foreign guest; NO token is only a note, because a guest can die in the ROM
# before AmigaDOS runs a line.  THE CR IS NOT OPTIONAL -- Exec's raw serial
# output ends every line `\r\n`, and an anchored `$` match without stripping it
# reports this run's own token as foreign.
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
# A guest built for a newer CPU than the machine stops in its own C constructor
# before anything under test runs: no serial, no stdout.txt, the machine idling.
# The emulator log says so, and this is the artifact rather than a grep of the
# source, so it catches a binary somebody staged by hand.  ROM is excluded:
# Kickstart probes the CPU with MOVEC at boot and takes that exception itself.
#
# `|| true` because a clean run is the case where both greps match nothing:
# under `set -o pipefail` the pipeline is then 1, an assignment carries its
# command substitution's status, and `set -e` took the script out right here.
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
    # THIS IS ALMOST ALWAYS THE LEVEL, not the serial path.  AMI_ERROR,
    # AMI_WARN and AMI_INFO are compiled into every build, but ami_log_level()
    # starts at AMI_LOG_WARN: a run in which nothing went wrong writes nothing.
    # What is left is whatever the guest program puts on the port itself with
    # RawPutChar, and a ToolsSmoke guest puts nothing.
    echo "(empty, nothing was written to the serial port)"
    echo "  ami_log_level() starts at AMI_LOG_WARN, so a run that hit no"
    echo "  warning and no error is silent, and the guest program writes to"
    echo "  the port only if it carries a RawPutChar tracer of its own.  For"
    echo "  the AMI_LOG_INFO tier, stage ENV:ANXDLOGLEVEL=2 into the drive"
    echo "  (serial_log_stage_env in tools/serial-log.sh)."
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
