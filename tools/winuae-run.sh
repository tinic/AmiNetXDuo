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
#   key           card                         driver needed
#   a2065         Commodore A2065 (Am7990)      a2065.device      <- have it
#   ariadne       Village Tronic Ariadne        ariadne.device
#   ariadne2      Village Tronic Ariadne II     ariadne2.device
#   hydra         Hydra Systems AmigaNet        amiganet.device
#   eb920         ASDG LAN Rover / EB920        (ASDG's own)
#   xsurf         Individual Computers X-Surf   xsurf.device
#   xsurf100z2    X-Surf-100 Zorro II           xsurf100.device
#   xsurf100z3    X-Surf-100 Zorro III          xsurf100.device
#
# cnet.device has no hardware here: WinUAE emulates no C-Net card.
#
# The A2065 keeps its own legacy config key; everything else is an expansion
# board and is switched on with <name>_rom_file=:ENABLED.  All of them come up
# on SLIRP without being asked to -- slirp is WinUAE's default network device
# -- which is why there is no per-board backend option below.  a2065=none is
# the only "fit the card, wire it to nothing" setting proven to work; the
# equivalent for the other boards is UNTESTED.
case "$BOARD" in
    a2065)     BOARD_LINE="a2065=${AMINETXDUO_WINUAE_A2065:-slirp}" ;;
    ariadne|ariadne2|hydra|eb920|xsurf|xsurf100z2|xsurf100z3)
               BOARD_LINE="${BOARD}_rom_file=:ENABLED" ;;
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
for extra in "$@"; do
    cp -R "$extra" "$HD/"
done

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
UAEQUIT="${AMINETXDUO_UAEQUIT:-$ROOT/build/uaequit}"
if [ ! -f "$UAEQUIT" ]; then
    scp -q "$HOST:C:/Program\\ Files/WinUAE/Amiga\\ Programs/UAEquit" "$UAEQUIT" 2>/dev/null || true
fi
QUIT_LINE=""
[ -f "$UAEQUIT" ] && { cp "$UAEQUIT" "$HD/c/uaequit"; QUIT_LINE="c:uaequit"; }

cat > "$HD/s/Startup-Sequence" <<EOF
failat 9999
c:envsetup
$EXE_NAME >DH0:stdout.txt
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
cpu_24bit_addressing=false
chipset=$CHIPSET
chipset_compatible=$COMPAT
chipmem_size=$CHIPMEM
bogomem_size=0
fastmem_size=$FASTMEM
a3000mem_size=$MBMEM
z3mem_size=0
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
[ "$NETWORK" = "1" ] && echo "==> $BOARD on SLIRP (10.0.2.0/24, gateway 10.0.2.2)"
[ "$ACCURATE" = "1" ] && echo "==> cycle accounting on, warp off (fidelity UNVERIFIED -- see the header)"
echo "==> boot ROM $(basename "$KICK")"

ssh "$HOST" "if exist $RRUN rmdir /s /q $RRUN" >/dev/null 2>&1 || true
ssh "$HOST" "mkdir $RRUN" >/dev/null 2>&1 || true
scp -q -r "$HD" "$HOST:$RRUN_FWD/hd"
scp -q "$CFG" "$HOST:$RRUN_FWD/config.uae"
scp -q "$ROOT/tools/winuae/run.ps1" "$ROOT/tools/winuae/sercap.ps1" "$HOST:$RROOT_FWD/tools/"

RESULT=$(ssh "$HOST" "powershell -NoProfile -ExecutionPolicy Bypass -File $RROOT\\tools\\run.ps1 -Config $RRUN\\config.uae -Hd $RRUN\\hd -Timeout $TIMEOUT -SerialPort $PORT -Serial $RRUN\\serial.log -WinUAE \"$WINUAE_EXE\" -PsExec $RROOT\\pstools\\PsExec64.exe -Tools $RROOT\\tools -Session $SESSION" 2>/dev/null | tr -d '\r' | grep '^WINUAE-RESULT' || true)

# Everything the guest wrote comes back, so DH0: behaves the way it does under
# FS-UAE: a test reports results simply by writing a file.
SERIAL="$ROOT/build/winuae-serial-$TAG.log"
rm -rf "$HD"
scp -q -r "$HOST:$RRUN_FWD/hd" "$HD" 2>/dev/null || mkdir -p "$HD"
scp -q "$HOST:$RRUN_FWD/serial.log" "$SERIAL" 2>/dev/null || : > "$SERIAL"

REASON=$(printf '%s' "$RESULT" | sed -n 's/.*reason=\([a-z]*\).*/\1/p')
RC=$(printf '%s' "$RESULT" | sed -n 's/.*rc=\([0-9-]*\).*/\1/p')

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
    done|quit)
        status=${RC:-0}
        echo "==> exit status $status ($RESULT)"
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
        echo "---- WinUAE log (faults and warnings) ----"
        ssh "$HOST" "findstr /i \"illegal exception guru trap error unknown\" \"C:\\Users\\Public\\Documents\\Amiga Files\\WinUAE\\winuaelog.txt\"" 2>/dev/null | tail -15 || echo "(none logged)"
        status=124
        ;;
esac

exit "$status"
