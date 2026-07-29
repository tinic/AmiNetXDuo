#!/usr/bin/env bash
#
# Run an AmigaOS executable under WinUAE on a remote Windows host and capture
# its output.  The WinUAE counterpart of tools/fsuae-run.sh.
#
#   tools/winuae-run.sh [-t SECONDS] [-m MODEL] [-c CPU] [-n] [-x] [-K]
#                       <executable> [extra files...]
#
# -m selects the machine.  A3000 is the default and the one that matters: a
# real 68030 with 32-bit motherboard RAM, which is the machine the project is
# actually aimed at.  A1200 (68EC020) and A4000 (68040) are also here.
# -c overrides the CPU on any model (68020/68030/68040/68060).
# -n attaches an emulated Commodore A2065 on WinUAE's SLIRP user-mode NAT
# (10.0.2.0/24, gateway and DHCP/DNS 10.0.2.2) -- the same network the FS-UAE
# harness gives you, so tests/netstack expectations carry over unchanged.
# -N picks a different card; see the board table below.
# -x turns warp mode off and asks for cycle accounting.  See the -x block.
# -K forces the AROS ROM even when a Kickstart is available.
#
# WHY THIS EXISTS AT ALL, given that FS-UAE already works:
#
#   * FS-UAE emulates ONE ethernet card, the A2065.  WinUAE emulates the
#     A2065, Ariadne, Ariadne II, Hydra, AmigaNet/LAN Rover, X-Surf, X-Surf-100
#     (Z2 and Z3) and three NE2000s (ISA, PCI, PCMCIA).  Every SANA-II driver
#     our installer offers except cnet.device has hardware here to run on.
#   * FS-UAE switches cycle accounting off for every CPU above a 68020, so no
#     68030/68040/68060 timing claim can be made on it at all.  WinUAE has
#     cycle-exact modes above the 68020.  Whether they are accurate ENOUGH is a
#     separate question this harness does not answer -- see -x.
#
# HOW IT WORKS
#
#   The Amiga side is deliberately identical to tools/fsuae-run.sh: the
#   executable and any extra files are staged into a directory that the guest
#   mounts as DH0:, s/Startup-Sequence runs the binary and writes DH0:.done
#   with its return code, ami_log() output goes out of the serial port, and the
#   exit status is the test's own or 124 on timeout.
#
#   The host side is different, because the emulator is on another machine and
#   is a GUI application:
#     * the staging directory is pushed over scp and pulled back afterwards, so
#       anything the guest writes still lands on this Mac;
#     * WinUAE is launched with PsExec into the interactive console session,
#       because it does not finish initialising in an SSH session's session 0;
#     * the serial port is a TCP listener that a helper on the Windows side
#       drains into a file, because WinUAE cannot write serial output to one;
#     * the run ends when the guest calls UAEquit, so the emulator exits by
#       itself rather than being killed on a timer.
#
#   tools/winuae/run.ps1 and tools/winuae/sercap.ps1 are the Windows half and
#   are pushed on every run, so this repository stays the source of truth.
#
# SETUP THE HOST NEEDS, ONCE: WinUAE installed, PSTools extracted to
# C:\aminetxduo\pstools.  --setup does the PsExec half from a PSTools.zip in
# the jenkins Downloads folder.
#
# AMINETXDUO_WINUAE_EXE picks the emulator; it defaults to the packaged
# C:\Program Files\WinUAE\winuae64.exe.  winbuilder also carries a build from
# WinUAE master at C:\winuae-patched\winuae64.exe, which is the one to use for
# anything bridged: 6.0.3 takes an access violation on an ethernet frame larger
# than 4000 bytes, and master does not.  Recipe and detail in docs/RESEARCH.md
# section 63.5.
#
# AMINETXDUO_WINUAE_ARGS passes WinUAE's own command line through, and every
# run brings the emulator log back as build/winuae-emulog-<tag>.txt.  With
# -a2065log2 that log holds every ethernet frame in both directions, which is
# the host-side view FS-UAE gives for free and the only one a bridged run has.
# tests/trace/a2065pcap.py --winuae turns it into a pcap.
#
# A bridged run that has to configure IPv6 needs one more thing on top of
# master: tools/winuae/a2065-multicast-loopback.patch, or the guest hears its
# own neighbour solicitations and configures no IPv6 address at all.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

HOST="${AMINETXDUO_WINUAE_HOST:-jenkins@winbuilder}"
RROOT="${AMINETXDUO_WINUAE_ROOT:-C:\\aminetxduo}"
RROOT_FWD="${RROOT//\\//}"
WINUAE_EXE="${AMINETXDUO_WINUAE_EXE:-C:\\Program Files\\WinUAE\\winuae64.exe}"
SESSION="${AMINETXDUO_WINUAE_SESSION:-1}"

TIMEOUT=180
MODEL=A3000
CPU=""
NETWORK=0
ACCURATE=0
FORCE_AROS=0
SETUP=0

for arg in "$@"; do
    [ "$arg" = "--setup" ] && SETUP=1
done

if [ "$SETUP" = "1" ]; then
    echo "==> preparing $HOST"
    ssh "$HOST" "mkdir $RROOT\\pstools 2>nul & mkdir $RROOT\\roms 2>nul & mkdir $RROOT\\tools 2>nul & mkdir $RROOT\\run 2>nul & powershell -NoProfile -Command \"if (-not (Test-Path '$RROOT\\pstools\\PsExec64.exe')) { Expand-Archive -Force 'C:\\Users\\jenkins\\Downloads\\PSTools.zip' '$RROOT\\pstools' }\"" >/dev/null 2>&1 || true
    ssh "$HOST" "if exist $RROOT\\pstools\\PsExec64.exe (echo PsExec64 present) else (echo PsExec64 MISSING)" 2>/dev/null
    ssh "$HOST" "if exist \"$WINUAE_EXE\" (echo WinUAE present) else (echo WinUAE MISSING)" 2>/dev/null
    exit 0
fi

BOARD=a2065

while getopts "t:m:c:nN:xK" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        n) NETWORK=1 ;;
        N) NETWORK=1; BOARD="$OPTARG" ;;
        x) ACCURATE=1 ;;
        K) FORCE_AROS=1 ;;
        *) echo "usage: $0 [-t seconds] [-m A3000|A1200|A4000] [-c cpu] [-n] [-N board] [-x] [-K] <executable> [files...]" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

# ------------------------------------------------------------- network cards --
#
# Every one of these was brought up and confirmed in WinUAE's own autoconfig
# dump, on this host, on 2026-07-26.  The second column is the SANA-II driver
# the Amiga side needs; NONE of them except a2065.device is in this repository
# or on the machine that wrote this, so a card being listed here means the
# hardware is available, not that we can drive it.
#
# The driver each one wants is in tools/sana2-stage.sh; none of them except
# a2065.device is in this repository.
#
#   key             card                              driver needed
#   a2065           Commodore A2065 (Am7990)          a2065.device
#   ariadne         Village Tronic Ariadne            ariadne.device
#   ariadne2        Village Tronic Ariadne II         ariadne_ii.device
#   hydra           Hydra Systems AmigaNet            hydra.device
#   eb920           ASDG LAN Rover / EB920            eb920.device
#   xsurf           Individual Computers X-Surf       x-surf.device
#   xsurf100z2      X-Surf-100 Zorro II               x-surf-100.device
#   xsurf100z3      X-Surf-100 Zorro III              x-surf-100.device
#   ne2000_pcmcia   RTL8019 PCMCIA (NE2000)           cnet.device
#
# ne2000_pcmcia is the odd one.  It is a PC Card behind Gayle, so it needs
# pcmcia=true and a machine that has a Gayle -- an A1200, not the A3000
# default -- and it is BOARD_NONAUTOCONFIG_BEFORE in WinUAE, so it never
# appears in the autoconfig board list however well it is working.  The only
# proof it is there is a driver opening it.
#
# The A2065 keeps its own legacy config key; everything else is an expansion
# board and is switched on with <name>_rom_file=:ENABLED.  All of them come up
# on SLIRP without being asked to -- slirp is WinUAE's default network device
# -- which is why there is no per-board backend option below.  a2065=none is
# the only "fit the card, wire it to nothing" setting proven to work; the
# equivalent for the other boards is UNTESTED.
case "$BOARD" in
    a2065)
        # slirp | slirp_inbound | none, or a host adapter to bridge onto.
        #
        # A bridged adapter is NOT just its pcap name.  WinUAE stores it as
        # rpcap://<pcap name> and keeps it in the board's rom_options, and the
        # legacy `a2065=' key alone is ignored -- even `a2065=slirp_inbound',
        # a name WinUAE hardcodes, comes back as 'slirp' in the log.  Both
        # lines are written, which is what WinUAE itself saves.
        #
        # AMINETXDUO_WINUAE_A2065 takes the bare pcap name (\Device\NPF_{...},
        # from `pcap_findalldevs'); the rpcap:// prefix is added here.
        #
        # The MAC is set rather than left empty.  An empty mac= leaves WinUAE
        # to invent one per run, so a DHCP server hands out a different lease
        # every time and nothing on the LAN can be given a reservation.
        #
        # Only the LAST THREE BYTES are ours.  WinUAE overwrites the first
        # three with Commodore's OUI (win32_uaenet.cpp: memcpy(tc->mac, uaemac,
        # 3)), so 02:41:4d:49:00:01 reaches the wire as 00:80:10:49:00:01 and
        # the locally-administered bit is gone.  Pick the tail to be unique on
        # the network; the head is not ours to choose.
        _a2065="${AMINETXDUO_WINUAE_A2065:-slirp}"
        case "$_a2065" in
            slirp|slirp_inbound|none)
                BOARD_LINE="a2065=$_a2065" ;;
            *)
                _mac="${AMINETXDUO_WINUAE_MAC:-02:41:4d:49:00:01}"
                BOARD_LINE="a2065_rom_file=:ENABLED
a2065_rom_options=mac=$_mac,rpcap://$_a2065
a2065=rpcap://$_a2065" ;;
        esac
        ;;
    ariadne|ariadne2|hydra|eb920|xsurf|xsurf100z2|xsurf100z3)
               # AMINETXDUO_WINUAE_BOARD_OPTIONS is WinUAE's own per-board
               # settings string, e.g. irq=6 for the LAN Rover, whose driver
               # ships in an int2 and an int6 build and hangs on the wrong one.
               BOARD_LINE="${BOARD}_rom_file=:ENABLED"
               [ -z "${AMINETXDUO_WINUAE_BOARD_OPTIONS:-}" ] || BOARD_LINE="$BOARD_LINE
${BOARD}_rom_options=$AMINETXDUO_WINUAE_BOARD_OPTIONS" ;;
    ne2000_pcmcia)
               # inserted=true is what actually puts the card in the slot.
               # Without it WinUAE maps the Gayle windows, logs nothing, and
               # Kickstart's card.resource never initialises -- which reads
               # from the guest as a driver that cannot find its hardware.
               BOARD_LINE="pcmcia=true
ne2000pcmcia_rom_file=:ENABLED
ne2000pcmcia_rom_options=inserted=true
ne2000_pcmcia=${AMINETXDUO_WINUAE_A2065:-slirp}" ;;
    *)         echo "unknown network board $BOARD" >&2; exit 2 ;;
esac

[ $# -ge 1 ] || { echo "usage: $0 [-t seconds] [-m model] [-n] <executable> [files...]" >&2; exit 2; }

EXE="$1"; shift
[ -f "$EXE" ] || { echo "no such executable: $EXE" >&2; exit 2; }
EXE_NAME=$(basename "$EXE")

TAG="${AMINETXDUO_RUN_TAG:-winuae}"
# The remote run directory, the serial port and the local staging directory are
# all keyed on the tag so two runs never share any of them.  The emulator
# itself is still serialised by a mutex on the Windows side -- one interactive
# session, one machine.
PORT=$((11000 + $(printf '%s' "$TAG" | cksum | cut -d' ' -f1) % 900))

HD="$ROOT/build/winuae-testhd-$TAG"
RRUN="$RROOT\\run\\$TAG"
RRUN_FWD="$RROOT_FWD/run/$TAG"

# ------------------------------------------------------------------- models --
#
# chipmem_size and friends are in 512 KB units despite what WinUAE's own help
# text says; 4 is 2 MB.  Verified from the emulator's memory map, not assumed:
# `2048K/1 = 2048K ID* C32 Chip memory`.
#
# a3000mem_size is the A3000/A4000's own RAMSEY memory on the CPU bus -- 32
# bits wide -- and it is the entire point of having an A3000 profile, exactly
# as motherboard_ram is under FS-UAE.  fastmem_size on these machines is Zorro
# II, i.e. a 16-bit path, and putting the working set there would measure the
# 68030 while throwing away the reason to use one.  Confirmed in the memory
# map as `07800000 8M/1 = 8M ID* F32 RAMSEY memory (low)`.
case "$MODEL" in
    A3000)
        CPU_MODEL=68030; FPU_MODEL=68882; CHIPSET=ecs; COMPAT=A3000
        CHIPMEM=4; MBMEM=8; FASTMEM=0
        ROM_NAMES=("Kickstart v3.1 r40.68 (1993)(Commodore)(A3000).rom"
                   "Kickstart v3.1 rev 40.68 (1993)(Commodore)(A3000).rom")
        ;;
    A4000)
        CPU_MODEL=68040; FPU_MODEL=68040; CHIPSET=aga; COMPAT=A4000
        CHIPMEM=4; MBMEM=16; FASTMEM=0
        ROM_NAMES=("Kickstart v3.1 r40.68 (1993)(Commodore)(A4000).rom"
                   "Kickstart v3.1 rev 40.68 (1993)(Commodore)(A4000).rom")
        ;;
    A1200)
        CPU_MODEL=68020; FPU_MODEL=0; CHIPSET=aga; COMPAT=A1200
        CHIPMEM=4; MBMEM=0; FASTMEM=8
        ROM_NAMES=("Kickstart v3.1 r40.68 (1993)(Commodore)(A1200)[!].rom"
                   "Kickstart v3.1 rev 40.68 (1993)(Commodore)(A1200)[!].rom")
        ;;
    *)
        echo "unknown model $MODEL (want A3000, A4000 or A1200)" >&2; exit 2 ;;
esac
[ -z "$CPU" ] || CPU_MODEL="$CPU"

# The A1200's PCMCIA credit-card window starts at $600000, and 8 MB of Zorro II
# Fast RAM at $200000 runs straight over it.  Kickstart's card.resource then
# declines to initialise and every PCMCIA driver reports that it cannot find
# its hardware -- which is what a real A1200 with 8 MB of Z2 Fast does too.
# 4 MB stops short of the window; the rest comes back as Zorro III, out of the
# way, so a run on this board has as much memory as a run on any other.
Z3MEM=0
if [ "$BOARD" = ne2000_pcmcia ] && [ "$FASTMEM" -gt 4 ]; then
    Z3MEM=$((FASTMEM - 4)); FASTMEM=4
fi

# Enforcer traps through a real MMU. A 68020 has none, and mmu_model=68020 is
# not a thing WinUAE accepts, so its PMOVEs take an F-line exception and it
# spins there: the log fills with `B-Trap F017` and nothing else runs. Move up
# to the smallest CPU that has an MMU on board.
WANT_ENFORCER="${AMINETXDUO_WINUAE_ENFORCER:-0}"
if [ "$WANT_ENFORCER" = "1" ]; then
    case "$CPU_MODEL" in
        68030|68040|68060) ;;
        *) echo "==> Enforcer needs an MMU: $CPU_MODEL -> 68030"
           CPU_MODEL=68030; [ "$FPU_MODEL" = 0 ] && FPU_MODEL=68882 ;;
    esac
fi

# ---------------------------------------------------------------- boot ROM --
#
# Kickstart first, AROS as the fallback -- the reverse of what CI wants, and
# deliberately so.  This harness exists to run the code on the machine the
# users have, and the A3000 Kickstart is that machine's ROM.  The AROS pair is
# still here because it is free and because CI cannot have a Kickstart, and it
# has been verified against the same probes (tools/fetch-aros-rom.sh).
KICK="${AMINETXDUO_KICKSTART:-}"
KICK_EXT="${AMINETXDUO_KICKSTART_EXT:-}"

if [ "$FORCE_AROS" = "1" ]; then
    KICK=""; KICK_EXT=""
fi

if [ -z "$KICK" ] && [ "$FORCE_AROS" = "0" ]; then
    for name in "${ROM_NAMES[@]}"; do
        for dir in "$HOME/Downloads" "$HOME/amigaos/build/rom" "$ROOT/build"; do
            [ -f "$dir/$name" ] && { KICK="$dir/$name"; break 2; }
        done
    done
fi

if [ -z "$KICK" ]; then
    echo "==> no $MODEL Kickstart found; using the AROS ROM pair"
    eval "$("$ROOT/tools/fetch-aros-rom.sh" --export)"
    KICK="$AMINETXDUO_KICKSTART"
    KICK_EXT="$AMINETXDUO_KICKSTART_EXT"
fi
[ -f "$KICK" ] || { echo "boot ROM $KICK does not exist" >&2; exit 2; }

# Remote ROM names are stable so the push happens once per ROM, not per run.
# The size check is enough to notice a swapped file: every Amiga ROM in play is
# either 512 KB or 1 MB, and a different ROM under the same name is the kind of
# thing you would only do on purpose.
push_rom() {
    local local_path="$1" remote_name="$2" size
    size=$(wc -c < "$local_path" | tr -d ' ')
    if ! ssh "$HOST" "powershell -NoProfile -Command \"if ((Test-Path '$RROOT\\roms\\$remote_name') -and ((Get-Item '$RROOT\\roms\\$remote_name').Length -eq $size)) { 'HAVE' } else { 'NEED' }\"" 2>/dev/null | grep -q HAVE; then
        scp -q "$local_path" "$HOST:$RROOT_FWD/roms/$remote_name"
    fi
}

ROM_REMOTE="rom-$(printf '%s' "$KICK" | cksum | cut -d' ' -f1).rom"
push_rom "$KICK" "$ROM_REMOTE"
ROM_EXT_REMOTE=""
if [ -n "$KICK_EXT" ] && [ -f "$KICK_EXT" ]; then
    ROM_EXT_REMOTE="rom-$(printf '%s' "$KICK_EXT" | cksum | cut -d' ' -f1).ext"
    push_rom "$KICK_EXT" "$ROM_EXT_REMOTE"
fi

# ------------------------------------------------------------------ staging --

rm -rf "$HD"
mkdir -p "$HD/s" "$HD/c" "$HD/env" "$HD/envarc" "$HD/t" "$HD/clips" "$ROOT/build"

cp "$EXE" "$HD/$EXE_NAME"
cp "$EXE" "$HD/c/$EXE_NAME"

# AMINETXDUO_WINUAE_ENFORCER=1 installs Enforcer before the executable and
# turns the MMU on below.  FS-UAE cannot bridge, so tools/enforcer-run.sh
# cannot watch a run on a real network; WinUAE emulates the 68030 MMU, so this
# is the only way to put Enforcer on a bridged fault.
ENFORCER_BIN="${AMINETXDUO_ENFORCER:-$HOME/amiga-os-src/os-source/v40/aug/bin/enforcer}"
if [ "$WANT_ENFORCER" = "1" ]; then
    [ -f "$ENFORCER_BIN" ] || {
        echo "Enforcer not found at $ENFORCER_BIN; set AMINETXDUO_ENFORCER=<path>." >&2
        exit 2; }
    cp "$ENFORCER_BIN" "$HD/c/enforcer"

    # Enforcer is a resident tool started with `run`; without a wait after it
    # the program under test starts before it has installed.
    WAITSECS="$ROOT/build/waitsecs"
    if [ ! -x "$WAITSECS" ] || [ "$ROOT/tools/enforcer/waitsecs.c" -nt "$WAITSECS" ]; then
        AMIGA_TOOLCHAIN_QUIET=1 . "$ROOT/tools/amiga-toolchain.sh"
        "$AMIGA_GCC" -O2 -m68020 -I"$AMIGA_NDK" -o "$WAITSECS" \
            "$ROOT/tools/enforcer/waitsecs.c" \
            || { echo "failed to build waitsecs" >&2; exit 2; }
    fi
    cp "$WAITSECS" "$HD/c/waitsecs"
fi
for extra in "$@"; do
    cp -R "$extra" "$HD/"
done

# ENV: is DH0:env, which the extras loop cannot write into: cp -R of a
# directory called `env' would land in DH0:env/env.  Drivers that read their
# settings from ENV: -- x-surf-100.device reads ENV:sana2/ -- need the contents
# merged instead.
if [ -n "${AMINETXDUO_GUEST_ENVDIR:-}" ]; then
    [ -d "${AMINETXDUO_GUEST_ENVDIR}" ] || {
        echo "AMINETXDUO_GUEST_ENVDIR is not a directory: ${AMINETXDUO_GUEST_ENVDIR}" >&2
        exit 2; }
    cp -R "${AMINETXDUO_GUEST_ENVDIR}/." "$HD/env/"
fi

# Same reasoning as the FS-UAE harness: a bare directory hard drive has none of
# the assigns a Workbench boot makes, and anything calling GetVar()/SetVar()
# fails without them.  envsetup builds them through dos.library.
ENVSETUP="$ROOT/build/envsetup"
if [ ! -x "$ENVSETUP" ] || [ "$ROOT/tools/envsetup/envsetup.c" -nt "$ENVSETUP" ]; then
    AMIGA_TOOLCHAIN_QUIET=1 . "$ROOT/tools/amiga-toolchain.sh"
    "$AMIGA_GCC" -O2 -m68020 -I"$AMIGA_NDK" -o "$ENVSETUP" "$ROOT/tools/envsetup/envsetup.c" \
        || { echo "failed to build envsetup" >&2; exit 2; }
fi
cp "$ENVSETUP" "$HD/c/envsetup"

# UAEquit is WinUAE's own Amiga-side "stop the emulator" program, shipped with
# it.  Running it as the last line of the boot script is what lets a run END
# rather than be killed: WinUAE exits on its own, flushes its log, and the host
# never has to guess from a timer.  If it is missing the harness still works --
# the host falls back to killing the emulator once DH0:.done appears.
# AMINETXDUO_GUEST_ARGS is appended to the command the guest runs.
GUEST_ARGS="${AMINETXDUO_GUEST_ARGS:-}"

UAEQUIT="${AMINETXDUO_UAEQUIT:-$ROOT/build/uaequit}"
if [ ! -f "$UAEQUIT" ]; then
    scp -q "$HOST:C:/Program\\ Files/WinUAE/Amiga\\ Programs/UAEquit" "$UAEQUIT" 2>/dev/null || true
fi
QUIT_LINE=""
[ -f "$UAEQUIT" ] && { cp "$UAEQUIT" "$HD/c/uaequit"; QUIT_LINE="c:uaequit"; }

# AMINETXDUO_GUEST_PRECMD runs before the executable, one command per line.
# A command that needs the network up -- nc, ping, anything using
# bsdsocket.library rather than linking the stack -- wants an
# AddNetInterface here, and the library and DEVS:NetInterfaces staged as
# extra files.
# Enforcer needs a real MMU and no JIT cache: it traps through the MMU tables,
# and a cached translation would let a bad access through unseen.
ENFORCER_MMU=""
if [ "$WANT_ENFORCER" = "1" ]; then
    ENFORCER_MMU="mmu_model=$CPU_MODEL
cachesize=0"
fi

ENFORCER_LINES=""
if [ "$WANT_ENFORCER" = "1" ]; then
    ENFORCER_LINES="run >NIL: <NIL: c:enforcer FSPACE
c:waitsecs 5"
fi

cat > "$HD/s/Startup-Sequence" <<EOF
failat 9999
c:envsetup
$ENFORCER_LINES
${AMINETXDUO_GUEST_PRECMD:-}
$EXE_NAME $GUEST_ARGS >DH0:stdout.txt
echo >DH0:.done "\$RC"
$QUIT_LINE
EOF

# ------------------------------------------------------------------- config --

CFG="$ROOT/build/winuae-$TAG.uae"
{
cat <<EOF
config_description=AmiNetXDuo $MODEL ($TAG)
use_gui=no
win32.logfile=true
kickstart_rom_file=$RROOT\\roms\\$ROM_REMOTE
EOF
[ -z "$ROM_EXT_REMOTE" ] || echo "kickstart_ext_rom_file=$RROOT\\roms\\$ROM_EXT_REMOTE"
cat <<EOF
cpu_type=$CPU_MODEL
cpu_model=$CPU_MODEL
fpu_model=$FPU_MODEL
${ENFORCER_MMU:-}
cpu_24bit_addressing=false
chipset=$CHIPSET
chipset_compatible=$COMPAT
chipmem_size=$CHIPMEM
bogomem_size=0
fastmem_size=$FASTMEM
a3000mem_size=$MBMEM
z3mem_size=$Z3MEM
rtc=MSM6242B
nr_floppies=0
floppy0type=-1
sound_output=none
win32.serial_port=TCP://0.0.0.0:$PORT/wait
serial_direct=true
filesystem2=rw,DH0:DH0:$RRUN\\hd,0
uaehf0=dir,rw,DH0:DH0:$RRUN\\hd,0
EOF

# -x: no warp, and ask for cycle accounting.
#
# WHAT IS AND IS NOT CLAIMED HERE.  Turning these on is not the same as the
# numbers being right, and nothing in this repository has yet measured WinUAE's
# 68030 against published timings the way tests/perf/cpucal measured FS-UAE's
# 68020.  What can be said is narrower and still worth having: FS-UAE prints
# `cpu_cycle_exact = false` for every CPU above a 68020 and no option turns it
# back on, so a 68030 timing taken there is not a timing at all.  WinUAE at
# least has the machinery.  Treat -x output as a measurement of WinUAE until
# somebody runs cpucal under it.
if [ "$ACCURATE" = "1" ]; then
cat <<EOF
cpu_speed=real
cpu_compatible=true
cpu_cycle_exact=true
cpu_memory_cycle_exact=true
blitter_cycle_exact=true
cpu_data_cache=true
warp=false
EOF
else
# Warp for correctness runs.  It takes an A3000 Kickstart boot from about 30
# seconds of host wall clock to about 7, and a correctness run does not care
# how many cycles it took.
cat <<EOF
cpu_speed=max
cpu_compatible=false
warp=true
EOF
fi

# The A2065 is the one card FS-UAE and WinUAE both emulate, so a test written
# against the FS-UAE harness runs here unchanged.  `slirp` is the same
# user-mode NAT; `none` fits the card and wires it to nothing, which is how you
# get an interface that opens, links up and never hears an answer.
if [ "$NETWORK" = "1" ]; then
    echo "$BOARD_LINE"
fi

# Raw `key=value` lines, semicolon separated, so the settings above can be
# swept without editing this file.
if [ -n "${AMINETXDUO_WINUAE_EXTRA:-}" ]; then
    printf '%s\n' "${AMINETXDUO_WINUAE_EXTRA}" | tr ';' '\n'
fi
} > "$CFG"

# ------------------------------------------------------------------- running --

echo "==> $EXE_NAME under WinUAE $MODEL/$CPU_MODEL on $HOST (timeout ${TIMEOUT}s)"
if [ "$NETWORK" = "1" ]; then
    case "${AMINETXDUO_WINUAE_A2065:-slirp}" in
        slirp)          echo "==> $BOARD on SLIRP (10.0.2.0/24, gateway 10.0.2.2)" ;;
        slirp_inbound)  echo "==> $BOARD on SLIRP with ports 21-23,80 forwarded" ;;
        none)           echo "==> $BOARD present and wired to nothing" ;;
        *)              echo "==> $BOARD bridged onto ${AMINETXDUO_WINUAE_A2065}" 
                        echo "    MAC ${AMINETXDUO_WINUAE_MAC:-02:41:4d:49:00:01}; the address comes from the real network" ;;
    esac
fi
[ "$ACCURATE" = "1" ] && echo "==> cycle accounting on, warp off (fidelity UNVERIFIED -- see the header)"
echo "==> boot ROM $(basename "$KICK")"

# AMINETXDUO_WINUAE_ARGS is WinUAE's own command line, space separated.
# -a2065log2 is the one that matters here: the A2065 then dumps every frame it
# handles, both directions, as hex into the emulator log, which comes back as
# build/winuae-emulog-<tag>.txt and feeds tests/trace/a2065pcap.py.  That is
# the host-side view FS-UAE gives unconditionally.
EMULOG="$ROOT/build/winuae-emulog-$TAG.txt"

ssh "$HOST" "if exist $RRUN rmdir /s /q $RRUN" >/dev/null 2>&1 || true
ssh "$HOST" "mkdir $RRUN" >/dev/null 2>&1 || true
scp -q -r "$HD" "$HOST:$RRUN_FWD/hd"
scp -q "$CFG" "$HOST:$RRUN_FWD/config.uae"
scp -q "$ROOT/tools/winuae/run.ps1" "$ROOT/tools/winuae/sercap.ps1" "$HOST:$RROOT_FWD/tools/"

RESULT=$(ssh "$HOST" "powershell -NoProfile -ExecutionPolicy Bypass -File $RROOT\\tools\\run.ps1 -Config $RRUN\\config.uae -Hd $RRUN\\hd -Timeout $TIMEOUT -SerialPort $PORT -Serial $RRUN\\serial.log -WinUAE \"$WINUAE_EXE\" -PsExec $RROOT\\pstools\\PsExec64.exe -Tools $RROOT\\tools -Session $SESSION -ExtraArgs \"${AMINETXDUO_WINUAE_ARGS:-}\"" 2>/dev/null | tr -d '\r' | grep '^WINUAE-RESULT' || true)

# Everything the guest wrote comes back, so DH0: behaves the way it does under
# FS-UAE: a test reports results simply by writing a file.
SERIAL="$ROOT/build/winuae-serial-$TAG.log"
rm -rf "$HD"
scp -q -r "$HOST:$RRUN_FWD/hd" "$HD" 2>/dev/null || mkdir -p "$HD"
scp -q "$HOST:$RRUN_FWD/serial.log" "$SERIAL" 2>/dev/null || : > "$SERIAL"
rm -f "$EMULOG"
scp -q "$HOST:$RRUN_FWD/emulog.txt" "$EMULOG" 2>/dev/null || true

REASON=$(printf '%s' "$RESULT" | sed -n 's/.*reason=\([a-z]*\).*/\1/p')
RC=$(printf '%s' "$RESULT" | sed -n 's/.*rc=\([0-9-]*\).*/\1/p')
EXITCODE=$(printf '%s' "$RESULT" | sed -n 's/.*exit=\(0x[0-9A-Fa-f]*\).*/\1/p')
[ -n "$EXITCODE" ] || EXITCODE="an exception Windows would not report"

winuae_log_tail() {
    echo "---- WinUAE log (faults and warnings) ----"
    ssh "$HOST" "findstr /i \"illegal exception guru trap error unknown\" \"C:\\Users\\Public\\Documents\\Amiga Files\\WinUAE\\winuaelog.txt\"" 2>/dev/null | tail -15 || echo "(none logged)"
}

echo "---- serial ($SERIAL) ----"
if [ -s "$SERIAL" ]; then cat "$SERIAL"; else echo "(empty -- no ami_log output reached the serial port)"; fi

for produced in "$HD"/*.txt "$HD"/*.log; do
    [ -f "$produced" ] || continue
    case "$produced" in *.uaem) continue ;; esac
    echo "---- $(basename "$produced") ----"
    cat "$produced"
done

[ -f "$HD/crash.txt" ] && { echo "---- CRASH ----"; cat "$HD/crash.txt"; }

case "$REASON" in
    done)
        status=${RC:-0}
        echo "==> exit status $status ($RESULT)"
        ;;
    quit)
        # UAEquit is the normal end, so RC is there. Without it the emulator
        # went away before the guest finished, and reporting the empty RC as 0
        # turns that into a pass -- which is how a WinUAE that died mid-suite
        # read as a truncated but successful run for three attempts.
        if [ -n "$RC" ]; then
            status=$RC
            echo "==> exit status $status ($RESULT)"
        else
            echo "!! WinUAE exited before the guest wrote DH0:.done [$RESULT]" >&2
            winuae_log_tail
            status=125
        fi
        ;;
    crash)
        # The guest did not reset: the emulator process took a Windows
        # exception. docs/RESEARCH.md 63.4 has the one that bites here, a
        # frame larger than 4000 bytes arriving on a bridged A2065.
        echo "!! WinUAE died with $EXITCODE; the guest did not reset [$RESULT]" >&2
        echo "!! see docs/RESEARCH.md 63.4 -- on a bridged run this is usually an oversized" >&2
        echo "!! receive frame, and the Windows Application event log names winuae64.exe" >&2
        winuae_log_tail
        status=125
        ;;
    busy)
        echo "!! another WinUAE run holds the machine and did not release it in time" >&2
        status=125
        ;;
    nowinuae|nopsexec|nostart)
        echo "!! the Windows host is not set up: $REASON -- run $0 --setup" >&2
        status=125
        ;;
    *)
        echo "==> TIMEOUT after ${TIMEOUT}s (no DH0:.done) [$RESULT]"
        winuae_log_tail
        status=124
        ;;
esac

exit "$status"
