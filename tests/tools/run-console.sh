#!/usr/bin/env bash
#
# THE WORKBENCH SCREEN, OFF THE WIRE, DECODED, AS PIXELS.
#
#   tests/tools/run-console.sh [-a ADDRESS] [-p PORT] [-b BUILDDIR] [-m MODEL]
#                              [-B BACKEND] [-d DEPTH]... [-t SECONDS]
#                              [-s SECONDS] [-H CONSOLEHTML] [-o OUTDIR]
#
# WHAT IT PROVES
#
#   An emulated Amiga boots Commodore's own Workbench, serves /console, upgrades
#   a real WebSocket, and streams its screen.  tests/tools/console-probe.py
#   carries the whole receiver -- the WebSocket framing, the geom and pal words,
#   COPY, RAW, PackBits and PackBits-over-XOR -- and writes a PNG, so what is
#   asserted is that the pixels are a screen and not that a socket stayed open.
#   A session that connects and streams zeroes passes every other check there
#   is.
#
#   One boot per depth, because the depth is a property of the screen Workbench
#   opens and not of anything this can change while it is running.  Two planes
#   is what a stock 3.1 comes up as; four is what a person sets and is twice the
#   chip RAM to read per grab.
#
# BRIDGED, AND NOT SLIRP
#
#   -B ens18 puts the guest on the host's own LAN at a static address, which is
#   the only configuration a client can connect to and the only one a
#   measurement means anything on.  tools/amiberry-run.sh is not used here: the
#   screen under test is the one LoadWB opens, and that script writes a
#   Startup-Sequence of its own with no Workbench in it.
#
#   THE HOST RUNNING THE EMULATOR MAY NOT BE ABLE TO REACH ITS OWN GUEST.  A
#   frame this host sends to the guest's MAC never comes back to that NIC's
#   pcap, which is the same thing tests/tools/run-httpd.sh says at its own
#   client.  -c <user@host> runs the probe on a THIRD machine over ssh; without
#   it the probe runs here and a run that cannot reach the guest says so as
#   infra rather than as a failure of the thing under test.
#
# A FRESH MAC EVERY RUN
#
#   Derived from the process id, so a rerun is never answered out of the
#   router's ARP cache -- a reused address is how a defect hides.  The a2065's
#   LANCE then reports a DIFFERENT address on the wire; that is the emulator's
#   business and not this script's, since the guest is found by its static IP.
#
# WHAT IT PRINTS
#
#   key=value and an exit code:
#
#     d<N>_boot_seconds     how long the guest took to answer at all
#     d<N>_page_bytes       the console page it served
#     d<N>_geom             what the guest said its screen was
#     d<N>_fps              frames a second the probe actually received
#     d<N>_bytes_per_second and what they cost
#
#   -A scroll runs `dir SYS: ALL` in the Shell on the screen while the probe
#   watches, which is the worst case for anything that diffs tiles: every row
#   of the window moves on every frame.  -A idle, the default, measures the
#   other end of the range, where the encoder sends five bytes a frame.
#
#   -T <word> types that word and Return at the guest, as Amiga rawkey codes
#   down the same socket, and the run fails unless the screen then changes.
#   That is the input half's whole assertion: on an idle machine nothing else
#   moves a pixel, so a change after the keys and none before them is the keys
#   having arrived.  Use it with -A idle; a screen that is already scrolling
#   can prove nothing about what made it scroll.
#     d<N>_guest_fbstat     the guest's own counters: frames, bytes, and the
#                           ticks it spent grabbing and encoding
#     d<N>_png              a decoded frame, to look at
#     RESULT                PASS, FAIL or INFRA
#
#   0 pass, 1 a failed assertion, 2 infrastructure.
#
# WHAT IT NEEDS
#
#   The five Workbench 3.1 ADFs (AMINETXDUO_ADF_DIR), a Kickstart that matches
#   the model, a2065.device (AMINETXDUO_A2065), a 68020 Release build, and the
#   console page.  The page is the committed src/tools/web/console.html, so a
#   build host with no node has one; -H names another.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-$ROOT/build/cm}"
MODEL="${AMINETXDUO_EMU_MODEL:-A1200}"
BACKEND="${AMINETXDUO_CONSOLE_BACKEND:-ens18}"
ADDRESS=192.168.1.232
NETMASK=255.255.255.0
GATEWAY=192.168.1.1
PORT=8080
BOOT_MAX=240
PROBE_SECONDS=10
OUTDIR="$ROOT/build/console-out"
PAGE="${AMINETXDUO_CONSOLE_PAGE:-$ROOT/src/tools/web/console.html}"
CLIENT="${AMINETXDUO_CONSOLE_CLIENT:-}"
ACTIVITY=idle
TYPE=""
DEPTHS=()
# -R serves a GRAPHICS CARD instead of the chipset: Amiberry's uaegfx board,
# Picasso96 staged onto the drive, and Workbench put on an 8-bit RTG screen.
# The console's RTG path is 8-bit palette only, so the depth is not a choice.
RTG=0
P96DIR="${AMINETXDUO_P96_DIR:-$HOME/amiga-assets/p96}"
# What Amiberry's uaegfx calls 640x480; see AssignModeID() in its picasso96.
RTG_MODE_ID=0x50031000
RTG_W=640
RTG_H=480

say() { printf '%s=%s\n' "$1" "$2"; }

while getopts "a:p:b:m:B:d:t:s:H:o:c:g:A:T:R" opt; do
    case "$opt" in
        a) ADDRESS="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        d) DEPTHS+=("$OPTARG") ;;
        t) BOOT_MAX="$OPTARG" ;;
        s) PROBE_SECONDS="$OPTARG" ;;
        H) PAGE="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        c) CLIENT="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        A) ACTIVITY="$OPTARG" ;;
        T) TYPE="$OPTARG" ;;
        R) RTG=1 ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

[ ${#DEPTHS[@]} -gt 0 ] || DEPTHS=(2 4)
if [ "$RTG" = 1 ]; then
    DEPTHS=(8)
    for f in Libs/Picasso96API.library Libs/Picasso96/rtg.library \
             Libs/Picasso96/uaegfx.card Devs/Monitors/Picasso96; do
        [ -f "$P96DIR/$f" ] || {
            say error "no $P96DIR/$f"
            say hint "AMINETXDUO_P96_DIR must hold a Picasso96 install tree"
            say RESULT INFRA
            exit 2
        }
    done
fi

# --------------------------------------------------------------- the parts --

HTTPD="$BUILD/src/tools/httpd"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$HTTPD" "$BSD"; do
    [ -f "$f" ] || {
        say error "no $f"
        say hint "cmake --build $BUILD --parallel"
        say RESULT INFRA
        exit 2
    }
done

# The page is a committed artifact, src/tools/web/console.html, the same as the
# Shell's: what a released archive carries is what this serves.  Named as the
# one missing thing when it is not there rather than built -- never faked,
# because a placeholder would make the one check that the page is served pass
# against something that is not the page.
[ -f "$PAGE" ] || {
    say error "no console page at $PAGE"
    say hint "node tools/web/build-console.mjs, or -H <console.html>"
    say RESULT INFRA
    exit 2
}

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    say error "no a2065.device; set AMINETXDUO_A2065=<path>"
    say RESULT INFRA
    exit 2
}

# A Kickstart must match the model: the A1200 40.68 image does not boot an A600
# and the machine then never reaches a Shell, which reads as a crash in the
# thing under test.  Same rule and same variables as tools/amiberry-run.sh.
model_var=$(printf '%s' "$MODEL" | tr '[:lower:]' '[:upper:]' | tr -c 'A-Z0-9' '_')
model_var=${model_var%_}
eval "KICKSTART=\${AMINETXDUO_KICKSTART_$model_var:-}"
KICKSTART="${KICKSTART:-${AMINETXDUO_KICKSTART:-}}"
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    say error "no Kickstart for $MODEL"
    say hint "set AMINETXDUO_KICKSTART_$model_var or AMINETXDUO_KICKSTART"
    say RESULT INFRA
    exit 2
}

AMIBERRY="${AMIBERRY:-$(command -v amiberry || true)}"
if [ -z "$AMIBERRY" ]; then
    for candidate in "$HOME/amiberry/build/amiberry" "$HOME/amiberry/amiberry"; do
        [ -x "$candidate" ] && { AMIBERRY="$candidate"; break; }
    done
fi
[ -n "$AMIBERRY" ] || {
    say error "amiberry not found; set AMIBERRY=<path>"
    say RESULT INFRA
    exit 2
}

# ------------------------------------------------------ Workbench 3.1 SYS: --

# shellcheck source=tests/tools/wb31-sys.sh
. "$ROOT/tests/tools/wb31-sys.sh"

wb31_assemble "$ROOT/build/wb31-sys" || { say RESULT INFRA; exit 2; }
WB="$WB31_SYS"

# ------------------------------------------------------------- the drive ----

HD="$ROOT/build/console-dh0"

stage() {
    local depth="$1"

    rm -rf "$HD"
    mkdir -p "$HD/Public" "$HD/Console"
    cp -R "$WB/." "$HD/"

    cp "$HTTPD" "$HD/C/httpd"
    chmod 755 "$HD/C/httpd"

    mkdir -p "$HD/Libs" "$HD/Devs/Networks" "$HD/Devs/NetInterfaces"
    cp "$BSD" "$HD/Libs/bsdsocket.library"
    cp "$A2065" "$HD/Devs/Networks/a2065.device"
    cp "$PAGE" "$HD/Console/console.html"

    cat > "$HD/Devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
EOF

    echo "Hello from an Amiga." > "$HD/Public/readme.txt"

    # A shell whose FROM script never ends: `dir SYS: ALL` on a loop is the
    # worst case for anything that diffs tiles, because every row of the window
    # moves on every frame.  run-wbgrab.sh drives its capture the same way.
    cat > "$HD/S/scroller" <<'EOF'
Lab loop
Dir SYS: ALL
Skip loop BACK
EOF
    chmod 644 "$HD/S/scroller"

    if [ "$RTG" = 1 ]; then
        # Picasso96 as its Installer would leave it, minus everything a
        # headless uaegfx does not touch.  rtg.library loads uaegfx.card out
        # of LIBS:Picasso96/ and emulation.library is what answers
        # cybergraphics.library, so both of the console's readback families
        # are on the machine.
        cp -R "$P96DIR/Libs/." "$HD/Libs/"
        mkdir -p "$HD/Devs/Monitors"
        cp -R "$P96DIR/Devs/." "$HD/Devs/"
        wb31_screenmode_prefs_id "$HD" "$depth" "$RTG_MODE_ID" "$RTG_W" "$RTG_H"
    else
        wb31_screenmode_prefs "$HD" "$depth"
    fi
}

# The stock 3.1 Startup-Sequence with the tail replaced.  LoadWB stays, since
# the screen under test is the one it opens; only EndCLI goes, because it would
# take the boot shell away before the server was started.
#
# `C:Wait 6` after LoadWB: Workbench draws its backdrop, its title bar and the
# disk icons after LoadWB returns, and a server started at once would be
# serving a screen that is still being drawn.
startup_for() {
    sed -e '/^EndCLI/d' "$WB/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
    cat >> "$HD/S/Startup-Sequence" <<EOF

FailAt 9999
C:Wait 6
EOF
    # AMINETXDUO_CONSOLE_PROBE_CMD runs on the guest, after Workbench is up and
    # before httpd, with its output on the serial port.  It exists so a question
    # about the SCREEN the server is about to serve -- what depth it really
    # opened, which BitMap layout it really got -- can be answered by the guest
    # rather than assumed by the harness.  Unset by default and nothing is
    # written when it is.
    if [ -n "${AMINETXDUO_CONSOLE_PROBE_CMD:-}" ]; then
        cp "$AMINETXDUO_CONSOLE_PROBE_BIN" "$HD/C/$(basename "$AMINETXDUO_CONSOLE_PROBE_BIN")"
        cat >> "$HD/S/Startup-Sequence" <<EOF
$AMINETXDUO_CONSOLE_PROBE_CMD
EOF
    fi

    # DH0 is a host directory, so anything the guest writes there is readable
    # from the outside at once.  This is the only way to ask an RTG boot what
    # it actually found: whether rtg.library loaded, whether it saw a board,
    # and what Intuition ended up opening.
    [ "$RTG" = 1 ] && cat >> "$HD/S/Startup-Sequence" <<'EOF'
C:Version >DH0:rtg-ver.txt LIBS:Picasso96/rtg.library FILE
C:Version >>DH0:rtg-ver.txt LIBS:Picasso96API.library FILE
C:Version >>DH0:rtg-ver.txt Picasso96API.library
C:Version >>DH0:rtg-ver.txt cybergraphics.library
C:List >DH0:rtg-libs.txt LIBS:Picasso96
C:List >>DH0:rtg-libs.txt DEVS:Monitors
C:Avail >DH0:rtg-avail.txt
EOF

    cat >> "$HD/S/Startup-Sequence" <<EOF
Run >DH0:httpd.txt <NIL: C:httpd DH0:Public $PORT -C CONSOLEPAGE DH0:Console/console.html -v
EOF
    # The BOOT shell does the scrolling, in the window that is already on the
    # screen.  `NewShell CON:...` was tried first and opened nothing that ever
    # appeared in a grab -- no window, no changed tile, over twenty seconds --
    # so this drives the one window that is demonstrably there.  Last, because
    # it never returns.
    [ "$ACTIVITY" = "scroll" ] && cat >> "$HD/S/Startup-Sequence" <<'EOF'
Execute S:scroller
EOF
    chmod 755 "$HD/S/Startup-Sequence"
}

# ------------------------------------------------------------ the emulator --

# Amiberry links SDL2 with no driver of its own, so without this it asks for a
# video device, finds a stale DISPLAY from a failed X11 forward, and aborts in
# about a second -- which reads as a guest that never booted.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
[ "${SDL_VIDEODRIVER}" = "dummy" ] && unset DISPLAY WAYLAND_DISPLAY || true

# Fresh per run: a reused MAC lets the router's cache answer for a guest that
# never came up, and the defect then looks like a pass.
MAC=$(printf '02:41:4d:49:%02x:%02x' $(( ($$ >> 8) & 0xff )) $(( $$ & 0xff )))

# TERM, and then KILL.  A headless Amiberry does not always go on TERM, and one
# that survives the harness leaves a guest ANSWERING AT THIS ADDRESS -- which
# the next run then measures instead of its own.  That happened: a run reported
# a one-second boot because the previous run's guest was still there.
EMU_PID=""
cleanup() {
    local n=0
    [ -n "$EMU_PID" ] || return 0
    kill -TERM "$EMU_PID" 2>/dev/null || true
    while [ "$n" -lt 10 ] && kill -0 "$EMU_PID" 2>/dev/null; do
        sleep 1
        n=$((n + 1))
    done
    kill -KILL "$EMU_PID" 2>/dev/null || true
    wait "$EMU_PID" 2>/dev/null || true
    EMU_PID=""
    return 0
}
trap cleanup EXIT INT TERM HUP

EMULOG=""

boot() {
    local depth="$1"
    local cfg="$ROOT/build/console-d$depth.uae"

    EMULOG="$ROOT/build/amiberry-console-d$depth.log"

    cat > "$cfg" <<EOF
config_description=AmiNetXDuo console depth $depth
use_gui=no
headless=true
quickstart=$MODEL,0
kickstart_rom_file=$KICKSTART
fastmem_size=8
floppy0type=-1
nr_floppies=0
uaehf0=dir,rw,DH0:DH0:$HD,0
a2065_rom_file=:ENABLED
a2065_rom_options=mac=$MAC,$BACKEND
EOF

    # The graphics card: uaegfx, which is board type 0 and therefore what a
    # gfxcard_size on its own asks for.  8 MB is a good deal more than a
    # 640x480x8 screen needs and leaves Picasso96 room for its own buffers.
    # rtg_modes is the emulator's RGBFF_ mask and it is written out because a
    # minimal config does not go through the path that fills in the default:
    # 0x112 is RGBFF_CLUT | RGBFF_R5G6B5PC | RGBFF_R8G8B8A8, and the first of
    # those is the only format the console serves.  A mask without bit 1 is a
    # board that offers no palette mode and a Workbench that quietly stays on
    # the chipset.
    #
    # cpu_24bit_addressing=no IS NOT OPTIONAL AND IT FAILS SILENTLY WITHOUT IT.
    # uaegfx is a Zorro III board and a quickstart model comes up with a 24-bit
    # address space; the emulator then says "Z3 RTG and 24bit address space are
    # not compatible" into a log nothing reads, drops the board, and Workbench
    # comes up on the chipset.  Every assertion in this harness still passes on
    # that -- it is a screen, it has colours, it changes -- and the only thing
    # that says the card was never there is the depth in the geom word.
    [ "$RTG" = 1 ] && cat >> "$cfg" <<EOF
cpu_24bit_addressing=no
gfxcard_size=8
rtg_modes=0x112
EOF

    # NOT --log.  It writes about a megabyte a second, playhouse3 is shared,
    # and a five-minute run left 427 MB of it here; nothing in this harness
    # reads it, because the backend assertion below is a better one.
    ( trap '' PIPE; exec "$AMIBERRY" -f "$cfg" ) >"$EMULOG" 2>&1 &
    EMU_PID=$!
}

# ---------------------------------------------------------------- the probe --
#
# On this host, or on a third machine over ssh.  The script and its output both
# travel, so a remote probe is the same file producing the same key=value.
probe() {
    local out="$1"
    shift
    if [ -z "$CLIENT" ]; then
        python3 "$ROOT/tests/tools/console-probe.py" "$@" > "$out" 2>&1
        return $?
    fi
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT" \
        "python3 - $*" < "$ROOT/tests/tools/console-probe.py" > "$out" 2>&1
}

fetch_page() {
    if [ -z "$CLIENT" ]; then
        curl -s -m 8 -o /dev/null -w '%{http_code} %{size_download}' \
             "http://$ADDRESS:$PORT/console" 2>/dev/null || true
    else
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT" \
            "curl -s -m 8 -o /dev/null -w '%{http_code} %{size_download}' \
             'http://$ADDRESS:$PORT/console'" 2>/dev/null || true
    fi
}

alive() {
    if [ -z "$CLIENT" ]; then
        curl -s -m 4 -o /dev/null -w '%{http_code}' \
             "http://$ADDRESS:$PORT/" 2>/dev/null || true
    else
        ssh -o BatchMode=yes -o ConnectTimeout=8 "$CLIENT" \
            "curl -s -m 4 -o /dev/null -w '%{http_code}' \
             'http://$ADDRESS:$PORT/'" 2>/dev/null || true
    fi
}

# ------------------------------------------------------------------- run ----

mkdir -p "$OUTDIR"
say address "$ADDRESS:$PORT"
say backend "$BACKEND"
say mac "$MAC"
say model "$MODEL"
say client "${CLIENT:-this host}"
say page "$PAGE"
say activity "$ACTIVITY"

# NOTHING MAY ALREADY BE AT THIS ADDRESS.
#
# The MAC is fresh every run but the address is not, and a guest left over from
# an earlier run answers on it exactly as this one's would.  A run that measures
# the previous run's machine is worse than a run that does not start.
if [ "$(alive)" = "200" ]; then
    say error "something already answers on http://$ADDRESS:$PORT/"
    say hint "an earlier guest is still up, or -a names an address in use"
    say RESULT INFRA
    exit 2
fi

VERDICT=pass

for depth in "${DEPTHS[@]}"; do
    tag="d$depth"

    stage "$depth"
    startup_for
    boot "$depth"

    started=$(date +%s)
    up=no
    # BOOT_MAX IS SECONDS, so the loop is bounded by the clock and not by a
    # count of attempts.  Each attempt costs a curl, and a curl at an address
    # nothing answers for costs its whole -m: counting attempts made the
    # ceiling 240 on a reachable guest and half an hour on an unreachable one,
    # which is the case the ceiling exists for.
    deadline=$(( started + BOOT_MAX ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        sleep 1
        kill -0 "$EMU_PID" 2>/dev/null || break
        [ "$(alive)" = "200" ] && { up=yes; break; }
    done
    say "${tag}_boot_seconds" "$(( $(date +%s) - started ))"

    if [ "$up" != yes ]; then
        say "${tag}_up" no
        say "${tag}_emulog" "$EMULOG"
        cleanup
        VERDICT=infra
        continue
    fi

    # BRIDGED IS PROVEN BY THE ADDRESS, not by a line in a log.  The interface
    # is configured STATIC on the host's own LAN and the client is another
    # machine on it: a guest that quietly came up on SLIRP is behind NAT at
    # 10.0.2.15 and cannot be reached from there at all, so an answer from
    # $ADDRESS is the assertion.  (Amiberry logs no UAENET line for the a2065,
    # so the string tools/amiberry-run.sh greps for is not available here.)
    say "${tag}_bridged" "confirmed by an answer at $ADDRESS from ${CLIENT:-this host}"

    # The page, from disk, on the same address the socket upgrades on.
    read -r code bytes <<<"$(fetch_page)"
    say "${tag}_page_status" "${code:-none}"
    say "${tag}_page_bytes" "${bytes:-0}"
    [ "${code:-0}" = "200" ] && [ "${bytes:-0}" -gt 1000 ] || VERDICT=fail

    typing=()
    [ -z "$TYPE" ] || typing=(--type "$TYPE")

    set +e
    probe "$OUTDIR/$tag-probe.txt" \
        "$ADDRESS" "$PORT" --seconds "$PROBE_SECONDS" \
        --png "$OUTDIR/$tag.png" --pfs "$OUTDIR/$tag.pfs" ${typing[@]+"${typing[@]}"}
    rc=$?
    set -e

    while IFS='=' read -r k v; do
        [ -n "$k" ] || continue
        say "${tag}_$k" "$v"
    done < <(grep '=' "$OUTDIR/$tag-probe.txt" || true)

    case "$rc" in
        0) ;;
        2) VERDICT=infra ;;
        *) VERDICT=fail ;;
    esac

    # A PNG the probe wrote here rather than on the client machine is not one
    # this host can show, so it is copied back.
    if [ -n "$CLIENT" ] && [ "$rc" = "0" ]; then
        say "${tag}_png_on" "$CLIENT"
    fi

    cleanup
done

say outdir "$OUTDIR"
say RESULT "$(printf '%s' "$VERDICT" | tr '[:lower:]' '[:upper:]')"

case "$VERDICT" in
    pass)  exit 0 ;;
    fail)  exit 1 ;;
    *)     exit 2 ;;
esac
