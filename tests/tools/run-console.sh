#!/usr/bin/env bash
#
# THE WORKBENCH SCREEN, OFF THE WIRE, DECODED, AS PIXELS.
#
#   tests/tools/run-console.sh [-a ADDRESS] [-p PORT] [-b BUILDDIR] [-m MODEL]
#                              [-B BACKEND] [-d DEPTH]... [-C MODE]...
#                              [-t SECONDS] [-s SECONDS] [-H CONSOLEHTML]
#                              [-o OUTDIR] [-P]
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
#   -R serves a graphics card instead, and its depths are the card's: 8 for the
#   palette screen the console has always read, and 15, 16, 24 or 32 for a
#   truecolour one.  All four truecolour depths arrive as one thing on the
#   wire -- the Amiga downsamples to r5g6b5 before it sends -- so the geom word
#   says format 2 and depth 16 for every one of them, and what differs between
#   the arms is what the guest read from.  The bytes and the frame rate are
#   directly comparable with the 8-bit arm: the same Workbench, the same size,
#   two bytes a pixel instead of one.
#
#   -C ham6, -C ham8 and -C ehb serve the OTHER three chipset screens, the ones
#   whose planes are not a palette index.  Each is one boot, named by its mode
#   rather than by its depth, and what it serves is not Workbench.  ham8 is AGA
#   and needs an AGA model.
#
#   WORKBENCH DOES TAKE THESE MODES, which is not what this expected.  The
#   ScreenMode editor does not offer HAM or EHB in its list, but IPrefs reads
#   the file rather than the list: a screenmode.prefs naming 0x00021800 brings
#   Workbench up six or eight planes deep with the HAM bit set, and 0x00021080
#   brings it up on extra half-brite.  Measured, and every arm reports it as
#   <mode>_wb_*.  It takes the display ID and the depth and ignores the width:
#   a prefs file asking for 320x256 gets a 640-wide screen.
#
#   That is not enough to test against, which is why C:chipscreen exists.  A
#   Workbench desktop drawn through the HAM chain is five colours of noise --
#   nothing in it says whether the control codes were read in the right order
#   -- so chipscreen opens a screen of its own, draws a deterministic ramp on
#   it and leaves it in front.  The arm then asserts both halves: the wire
#   format the session reports, and that the picture on it is the one the guest
#   says it drew.
#
#   THE PICTURE IS THE POINT.  A HAM decode with its control codes permuted or
#   its data field in the wrong place still draws a plausible image, so an arm
#   that only checked the pixels were not all one colour would pass on a wrong
#   receiver.  tests/perf/chipscreen.c draws four bands -- base colours, then a
#   red, a green and a blue ramp in that order -- on a palette that is exact in
#   a 4-bit colour register, so the decoded PNG is something a person can hold
#   against what was drawn.
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
#     d<N>_guest_*          the guest's own counters, one key each: frames,
#                           bytes, the ticks it spent grabbing and encoding,
#                           and the ticks charged against its share of the CPU
#     d<N>_duty_cycle_pct   what fraction of the wall clock those came to
#     d<N>_png              a decoded frame, to look at
#     RESULT                PASS, FAIL or INFRA
#
#   An -R arm prints three more:
#
#     d<N>_rtg_format       the geom word's format: 1 palette, 2 truecolour
#     d<N>_rtg_wire_depth   and its depth, which is 16 for every truecolour arm
#     d<N>_rtg_mode         the display mode the guest opened, learned from the
#                           board rather than written down here; relearned once
#                           and reported as d<N>_rtg_mode_relearned when the
#                           first boot asked for a mode the board does not hold
#
#   A -C arm prints its own, under the mode's name rather than a depth:
#
#     <mode>_chipset_format     the geom word's format: 3 HAM6, 4 HAM8, 5 EHB.
#                               0 is the planar screen a failed staging falls
#                               back to, and the arm fails on it
#     <mode>_chipset_wire_depth and its depth, 6 or 8
#     <mode>_wb_*               the screen Workbench itself came up on, read
#                               before chipscreen put one in front of it
#     <mode>_chip_*             what C:chipscreen said on the guest: the mode
#                               OpenScreen gave it and the pattern it drew
#
#   A depth the board publishes no mode for is reported as d<N>_skipped and
#   the arm does not run.  Which modes an emulated uaegfx offers is the
#   emulator's answer to give, not a defect in anything here.
#
# -P, AND WHY A WORKBENCH SCREEN COULD NOT PROVE THE TRUECOLOUR PATHS
#
#   -R serves Workbench, and nobody has a reference picture of it.  A frame
#   decodes, holds many colours and changes over time whether or not the
#   readback exchanged red and blue on the way, so every assertion above passes
#   on a transposed picture.  That is not hypothetical: the two four-byte
#   RGBFTYPE codes whose alpha is last were written down the wrong way round in
#   src/tools/httprtg.c until 49ef2137, and the 32-bit format that uses one of
#   them is the format this board reports.
#
#   So -P serves a KNOWN picture instead.  tests/perf/rtgbars.c opens a
#   truecolour screen of the arm's depth and fills it with eight bands --
#   black, red, green, blue, yellow, magenta, cyan, white -- and
#   tests/tools/rtgbars-check.py reads them back off the decoded PNG.  Red at
#   one eighth across and blue at three eighths is the pair that tells a
#   transposition from a picture that is merely wrong, and the checker names it
#   as one.  The guest draws through LoadRGB32 and RectFill, so the conversion
#   into the card's format is the graphics driver's: a pattern this side packed
#   itself would need the same table the code under test uses and would agree
#   with a transposition instead of catching one.
#
#   -P also prints, per arm:
#
#     d<N>_rtg_card_depth   the card's own bits a pixel, off the readback word
#     d<N>_rtg_card_fmt     and its pixel format, by name
#     d<N>_bands_ok         how many of the eight bands came back right
#
#   THE FORMAT NAME IS WHAT SAYS WHICH PATH RAN.  Every truecolour screen is
#   downsampled to R5G6B5 before it is sent, so the geom word reads depth 16
#   for all of them; and the display database is no better, because a packed
#   24-bit mode and a 32-bit one are both depth 24 on it -- RGBA32 carries
#   twenty-four bits of colour in four bytes.  So a -P arm is named for the
#   Picasso96 format it exercises, the emulated board is configured to publish
#   that format and no other truecolour one, and the arm fails unless the
#   readback word names it:
#
#     -d 15   R5G5B5PC, two bytes a pixel        -d 24   R8G8B8, three
#     -d 16   R5G6B5PC, two bytes a pixel        -d 32   R8G8B8A8, four
#
#   Which modes the board publishes is read off the machine, not written down:
#   tests/perf/rtgmodes.c lists the display database into DH0:rtglist.txt on
#   every -P boot, and a depth with no mode at this size is reported as
#   d<N>_skipped.
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
PROBE_SECONDS_SET=0
OUTDIR="$ROOT/build/console-out"
PAGE="${AMINETXDUO_CONSOLE_PAGE:-$ROOT/src/tools/web/console.html}"
CLIENT="${AMINETXDUO_CONSOLE_CLIENT:-}"
ACTIVITY=idle
TYPE=""
# -L N measures how long the guest takes to act on a keystroke: N of them, each
# from a screen that has been still, timed to the frame that shows the
# character.  It needs something on the screen that echoes, so it goes with
# -A idle and a Shell in front, and it is the number that says whether
# producing a frame in bands got the input path looked at sooner.
LATENCY=""
DEPTHS=()
# -R serves a GRAPHICS CARD instead of the chipset: Amiberry's uaegfx board,
# Picasso96 staged onto the drive, and Workbench put on an RTG screen of the
# depth the arm asked for.  8 is the palette screen; 15, 16, 24 and 32 are the
# truecolour ones, and they all reach the wire as r5g6b5.
RTG=0
# -P is -R with a known picture on it instead of Workbench.  See the header.
PATTERN=0
P96DIR="${AMINETXDUO_P96_DIR:-$HOME/amiga-assets/p96}"
RTG_BOARD=uaegfx
RTGMODES=""
RTGBARS=""
# What Amiberry's uaegfx calls 640x480 at eight bits; see AssignModeID() in its
# picasso96.  There is no such constant for the truecolour depths and none is
# guessed at: rtg_mode_id() takes those off the board's own published list,
# which C:rtgmodes writes into DH0:rtglist.txt on every -P boot.
RTG_MODE_ID=0x50031000
RTG_W=640
RTG_H=480
RTG_DEPTHS="8 15 16 24 32"

# -C serves a chipset screen whose planes are not a palette index: HAM6, HAM8
# or extra half-brite.  One boot per mode, and the screen comes from
# C:chipscreen rather than from LoadWB -- see the header.  320x256 because
# both HAM and EHB are lores modes.
CHIPSET=()
CHIPSCREEN=""
CHIP_W=320
CHIP_H=256
CHIP_MODES="ham6 ham8 ehb"
# The chipset screen this arm is on, empty on every other kind of arm.  A
# global for the same reason RTG is one: stage() and startup_for() both need
# it and neither is called from anywhere else.
CHIP=""

say() { printf '%s=%s\n' "$1" "$2"; }

while getopts "a:p:b:m:B:d:C:t:s:H:o:c:g:A:T:L:RP" opt; do
    case "$opt" in
        a) ADDRESS="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        B) BACKEND="$OPTARG" ;;
        d) DEPTHS+=("$OPTARG") ;;
        C) CHIPSET+=("$OPTARG") ;;
        t) BOOT_MAX="$OPTARG" ;;
        s) PROBE_SECONDS="$OPTARG"; PROBE_SECONDS_SET=1 ;;
        H) PAGE="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        c) CLIENT="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        A) ACTIVITY="$OPTARG" ;;
        L) LATENCY="$OPTARG" ;;
        T) TYPE="$OPTARG" ;;
        R) RTG=1 ;;
        P) RTG=1; PATTERN=1 ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

if [ "$RTG" = 1 ] && [ ${#CHIPSET[@]} -gt 0 ]; then
    say error "-C is a chipset screen and -R and -P are a card's; one run serves one of them"
    say RESULT INFRA
    exit 2
fi

if [ ${#CHIPSET[@]} -gt 0 ]; then
    for m in "${CHIPSET[@]}"; do
        case " $CHIP_MODES " in
            *" $m "*) ;;
            *) say error "-C takes one of $CHIP_MODES, not $m"
               say RESULT INFRA
               exit 2 ;;
        esac
    done
    # HAM8 IS AGA, so an ECS machine opens no such screen and the arm would be
    # asserting against whatever OpenScreen handed back instead.  Named here
    # rather than discovered on the guest: a boot costs minutes and the model
    # is on the command line.
    case " ${CHIPSET[*]} " in
        *" ham8 "*)
            case "$MODEL" in
                A1200|a1200|A4000|a4000|CD32|cd32) ;;
                *) say error "-C ham8 is an AGA mode and $MODEL is not an AGA machine"
                   say hint "-m A1200"
                   say RESULT INFRA
                   exit 2 ;;
            esac ;;
    esac
    CHIPSCREEN="${AMINETXDUO_CHIPSCREEN:-$BUILD/tests/perf/chipscreen}"
    [ -f "$CHIPSCREEN" ] || {
        say error "no $CHIPSCREEN"
        say hint "cmake --build $BUILD --parallel --target chipscreen"
        say RESULT INFRA
        exit 2
    }
elif [ "$RTG" = 1 ]; then
    # -R alone is the 8-bit arm it has always been and -P alone runs the four
    # truecolour depths, because those four are what it exists to prove and
    # three of them had never been run at all.  -d names any of them, and only
    # the depths the card can hold: a chipset depth here would ask Picasso96
    # for a screen it cannot open and the run would come up planar, which is
    # the one failure this file works hardest to make impossible.
    if [ ${#DEPTHS[@]} -eq 0 ]; then
        if [ "$PATTERN" = 1 ]; then DEPTHS=(15 16 24 32); else DEPTHS=(8); fi
    fi
    for d in "${DEPTHS[@]}"; do
        case " $RTG_DEPTHS " in
            *" $d "*) ;;
            *) say error "-R takes a card depth, one of $RTG_DEPTHS, not $d"
               say RESULT INFRA
               exit 2 ;;
        esac
    done
    # A -P ARM NEEDS A WINDOW LONGER THAN ONE FRAME, and ten seconds is not
    # one.  A 640x480 truecolour frame is 614400 bytes and this link carries
    # about ten thousand a second, so the FIRST frame occupies most of a
    # minute; every frame after it is a few changed tiles and costs nothing.
    # At -s 45 the whole session was that one frame, no second frame could
    # differ from it, and --min-changed failed an arm whose picture was
    # perfect.  At 120 the same arm sees 114 frames and 20 of them differ.
    if [ "$PATTERN" = 1 ] && [ "$PROBE_SECONDS_SET" = 0 ]; then
        PROBE_SECONDS=120
    fi

    if [ "$PATTERN" = 1 ]; then
        RTGMODES="${AMINETXDUO_RTGMODES:-$BUILD/tests/perf/rtgmodes}"
        RTGBARS="${AMINETXDUO_RTGBARS:-$BUILD/tests/perf/rtgbars}"
        for f in "$RTGMODES" "$RTGBARS"; do
            [ -f "$f" ] || {
                say error "no $f"
                say hint "cmake --build $BUILD --parallel --target rtgmodes rtgbars"
                say RESULT INFRA
                exit 2
            }
        done
        [ -f "$ROOT/tests/tools/rtgbars-check.py" ] || {
            say error "no $ROOT/tests/tools/rtgbars-check.py"
            say RESULT INFRA
            exit 2
        }
    fi
    for f in Libs/Picasso96API.library Libs/Picasso96/rtg.library \
             Libs/Picasso96/uaegfx.card Devs/Monitors/Picasso96; do
        [ -f "$P96DIR/$f" ] || {
            say error "no $P96DIR/$f"
            say hint "AMINETXDUO_P96_DIR must hold a Picasso96 install tree"
            say RESULT INFRA
            exit 2
        }
    done
else
    [ ${#DEPTHS[@]} -gt 0 ] || DEPTHS=(2 4)
fi

# ------------------------------------------------------------ the card ------
#
# What the geom word has to say for a given guest depth.  Four truecolour
# depths land on one wire format because the Amiga downsamples before it
# sends, so the arm asserts the pair rather than the depth it asked for.
rtg_wire_format() {
    case "$1" in
        8)  printf 1 ;;
        15|16|24|32) printf 2 ;;
        *)  printf 0 ;;
    esac
}

rtg_wire_depth() {
    case "$1" in
        8) printf 8 ;;
        *) printf 16 ;;
    esac
}

# The RGBFF mask the emulated board publishes.  0x112 is what the 8-bit arm
# has always had -- RGBFF_CLUT | RGBFF_R5G6B5PC | RGBFF_R8G8B8A8 -- and it
# already carries a 16-bit and a 32-bit mode as well as the palette one.  The
# other two bits are added only for the arm that needs them, so no arm that
# passes today gets a different board.
rtg_modes_mask() {
    case "$1" in
        15) printf '0x132' ;;   # + RGBFF_R5G5B5PC
        24) printf '0x116' ;;   # + RGBFF_R8G8B8
        *)  printf '0x112' ;;
    esac
}

# ------------------------------------------- what the board calls a depth ---
#
# THE ARM'S NAME IS NOT THE DATABASE'S DEPTH, and reading `-d 32` as one is why
# the first truecolour arm skipped itself.  Asked for its modes, this board
# publishes three depths and no others:
#
#   rtgmode=50031000 640x480 depth=8  pixfmt=0  LUT8
#   rtgmode=50031100 640x480 depth=16 pixfmt=7  RGB16PC
#   rtgmode=50031302 640x480 depth=24 pixfmt=13 RGBA32
#
# There is no depth 32 in a display database.  RGBA32 is four bytes a pixel
# with twenty-four bits of colour in them, and CyberGraphX reports the colour,
# so the FORMAT is what separates a 32-bit screen from a packed 24-bit one --
# both are depth 24.  The arm is therefore named for the format it exercises
# and asserts on that; the depth is only how the mode is found.
rtg_db_depth() {
    case "$1" in
        8)     printf 8 ;;
        15)    printf 15 ;;
        16)    printf 16 ;;
        24|32) printf 24 ;;
        *)     printf 0 ;;
    esac
}

# The Picasso96 RGBFTYPE name the readback word must report for each arm.  This
# is the assertion that says which path ran: everything downstream of the card
# is R5G6B5 whatever this is.
rtg_card_fmt() {
    case "$1" in
        8)  printf CLUT ;;
        15) printf R5G5B5PC ;;
        16) printf R5G6B5PC ;;
        24) printf R8G8B8 ;;
        32) printf R8G8B8A8 ;;
        *)  printf none ;;
    esac
}

# -P ONLY: CLUT for Workbench, and exactly ONE truecolour format beside it.
#
# A mask with two truecolour bits in it publishes two modes at the shape the
# arm wants, and on this board two of them -- packed RGB24 and RGBA32 -- are
# both depth 24, so asking the display database for a depth would not say which
# one came back.  One bit at a time makes the answer unambiguous, and the arm
# then asserts the format name rather than hoping.
#
# -R keeps rtg_modes_mask() above untouched: its 8 and 16-bit arms pass today
# on a board with three formats on it and there is no reason to give them a
# different one.
rtg_pattern_mask() {
    case "$1" in
        8)  printf '0x002' ;;   # RGBFF_CLUT
        15) printf '0x022' ;;   # + RGBFF_R5G5B5PC
        16) printf '0x012' ;;   # + RGBFF_R5G6B5PC
        24) printf '0x006' ;;   # + RGBFF_R8G8B8
        32) printf '0x102' ;;   # + RGBFF_R8G8B8A8
        *)  printf '0x112' ;;
    esac
}

# Every mode the display database holds, as C:rtgmodes printed it during a
# boot: `rtgmode=<id> <w>x<h> depth=<n>`.  Kept across arms and across runs so
# that only the first RTG boot of a machine has to discover anything.
RTG_MODES_SEEN=""

rtg_mode_from_list() {
    local f="$1" d="$2"
    [ -n "$f" ] && [ -f "$f" ] || return 0
    awk -v want="depth=$d" -v size="${RTG_W}x${RTG_H}" \
        '/^rtgmode=/ { if ($2 == size && $3 == want) { print substr($1, 9)
                                                       exit } }' "$f"
}

# The mode ID to write into screenmode.prefs for a depth, or nothing when the
# board has not been asked yet.  A named override first, then what the board
# itself published, then the one constant that is known from the emulator's
# source.  NOT a table of guesses: a wrong ID opens no screen, Workbench stays
# on the chipset, and every other check in this file passes on that.
rtg_mode_id() {
    local d="$1" id=""
    eval "id=\${AMINETXDUO_RTG_MODE_ID_$d:-}"
    if [ -n "$id" ]; then printf '%s' "$id"; return 0; fi
    for f in "${AMINETXDUO_RTG_MODE_LIST:-}" "$RTG_MODES_SEEN"; do
        id=$(rtg_mode_from_list "$f" "$d")
        if [ -n "$id" ]; then printf '0x%s' "$id"; return 0; fi
    done
    # The one constant, and it is not depth 8's alone.  Amiberry's uaegfx
    # derives a mode ID from the width and the height and from nothing else --
    # AssignModeID() in its picasso96.cpp looks up (w, h) in a table and
    # returns 0x50001000 | id << 16 -- so 640x480 is 0x50031000 at 8, 15, 16,
    # 24 and 32 bits alike, and the depth is chosen by the screenmode.prefs
    # field beside it.  A board that numbers its modes some other way needs
    # AMINETXDUO_RTG_MODE_ID_<depth>, which is why the override is first.
    printf '%s' "$RTG_MODE_ID"
    return 0
}

# ---------------------------------------------------------------- the arms --
#
# An arm is one boot.  A plain or -R arm is named by its depth and a -C arm by
# its mode, so the loop reads these rather than the token it was given and
# nothing below has to know which kind of run it is in.

arm_tag() {
    case "$1" in
        ham6|ham8|ehb) printf '%s' "$1" ;;
        *)             printf 'd%s' "$1" ;;
    esac
}

arm_depth() {
    case "$1" in
        ham6|ehb) printf 6 ;;
        ham8)     printf 8 ;;
        *)        printf '%s' "$1" ;;
    esac
}

# The PAL mode ID a -C arm opens on: the monitor key with the mode's own key
# OR'd into it.  PAL_MONITOR_ID is 0x00021000, HAM_KEY is 0x0800 and
# EXTRAHALFBRITE_KEY is 0x0080, all from graphics/modeid.h.  Lores in both
# cases -- HIRES_KEY, 0x8000, is not set -- because HAM and EHB are lores
# modes and 320x256 is the shape the chipset produces them in.  HAM8 is the
# same HAM key eight planes deep, which is why it is AGA and the other two are
# not.
arm_mode_id() {
    case "$1" in
        ham6|ham8) printf '0x00021800' ;;
        ehb)       printf '0x00021080' ;;
        *)         printf '' ;;
    esac
}

# What rfb_geom.format must be for a -C arm.  0 is the plain planar screen a
# failed staging falls back to and is the one value this exists to refuse.
arm_wire_format() {
    case "$1" in
        ham6) printf 3 ;;
        ham8) printf 4 ;;
        ehb)  printf 5 ;;
        *)    printf 0 ;;
    esac
}

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

# ------------------------------------------------------- the monitor icon ----
#
# WHAT THE MONITOR READS BEFORE IT LOADS ANYTHING, and it is not in the file.
#
# devs/monitors/Picasso96 is one executable that drives every card Picasso96
# supports; which .card it opens comes from the TOOLTYPES of the icon beside
# it, which InstallPicasso96 writes in P_InstallCard.  Without an icon it
# loads no board driver, publishes no resolutions, and Workbench comes up on
# the chipset -- silently, because nothing on an Amiga complains about a
# missing .info.
#
# So the icon is generated rather than hand-copied: the archive ships
# Picasso96.info, which carries a PicassoIV's tooltypes and would be wrong
# here in exactly the way that is hardest to notice.  A minimal WBTOOL icon
# with a 8x8 one-plane image and the three tooltypes the installer sets that
# are not disabled -- the parenthesised ones it writes are off by convention.
rtg_monitor_icon() {
    local out="$1" board="$2"

    AMINETXDUO_ICON_BOARD="$board" python3 - "$out" <<'EOF'
import os, struct, sys

board = os.environ["AMINETXDUO_ICON_BOARD"]
# NO SettingsFile.  The one the archive ships is configured for a PicassoIV --
# its own installer says so and tells you to re-attach it with Picasso96Mode --
# and a settings file naming another board is worse than none: uaegfx reports
# its own resolutions and wants no timing list at all.
tools = ["IgnoreMask=Yes",
         "BoardType=" + board]

W, H, D = 8, 8, 1
rowbytes = ((W + 15) // 16) * 2

# struct Gadget, 44 bytes, inside the DiskObject at offset 4.  GadgetRender is
# non-NULL so the Image below it is read; SelectRender and GadgetText are not.
gadget = struct.pack(">LhhhhHHHLLLLLHL",
                     0,            # NextGadget
                     0, 0, W, H,   # LeftEdge, TopEdge, Width, Height
                     0x0004,       # Flags: GADGIMAGE
                     0, 1,         # Activation, GadgetType (BOOLGADGET)
                     1,            # GadgetRender, any non-zero
                     0, 0, 0, 0,   # SelectRender, GadgetText, Mutual, Special
                     0, 0)         # GadgetID, UserData
assert len(gadget) == 44, len(gadget)

# struct DiskObject: magic, version, the gadget, then the pointers that say
# which of the sections after it are present.
obj = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
    ">BBLLLLLLL",
    3, 0,       # do_Type = WBTOOL, pad
    0,          # DefaultTool: none
    1,          # ToolTypes: present
    0x80000000, 0x80000000,   # CurrentX, CurrentY = NO_ICON_POSITION
    0,          # DrawerData
    0,          # ToolWindow
    4096)       # StackSize
assert len(obj) == 78, len(obj)

image = struct.pack(">hhhhhLBBL", 0, 0, W, H, D, 1, 0x1, 0x0, 0)
bits = bytes([0xFF] + [0x81] * (H - 2) + [0xFF]) if rowbytes == 1 else \
       b"".join(struct.pack(">H", v) for v in
                [0xFF00] + [0x8100] * (H - 2) + [0xFF00])

tt = struct.pack(">L", (len(tools) + 1) * 4)
for t in tools:
    b = t.encode("latin-1") + b"\0"
    tt += struct.pack(">L", len(b)) + b

with open(sys.argv[1], "wb") as fh:
    fh.write(obj + image + bits + tt)
EOF
}

# ------------------------------------------------------------- the drive ----

HD="$ROOT/build/console-dh0"

stage() {
    local depth="$1" mode_id="${2:-}"

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
        #
        # THE ARCHIVE'S OWN uaegfx.card IS THE RIGHT ONE, and no other copy is
        # needed from anywhere.  This used to delete it and demand a "modern
        # stub from WinUAE" in its place, on the reading that Picasso96 2.0's
        # 1998 card drives the obsolete uaelib trap.  It does not: with the
        # archive's card staged unchanged, the emulator logs
        #
        #   uaegfx.card 3.4 init @0020E2DC
        #   P96 RESINFO: 0020E380-0020EB60 (42,2016)
        #   uaegfx.card magic code: 00F04800-00F0494A BI=00263BC4
        #   SetSwitch() - Picasso96 640x480x8 - immediate
        #
        # -- the card finds the library uaegfx_card_install() builds in the
        # boot ROM through the magic-code handshake, and never touches the
        # trap.  Not one run has printed the obsolete-hook line.
        #
        # `Picasso96: Could not create graphics board context for 'uaegfx'` is
        # NOT the symptom it was read as.  DEVS:Monitors/<board> is already run
        # by the stock Startup-Sequence, so the harness's own second run of it
        # is a duplicate init, and that message is what a duplicate init says
        # on a board that came up perfectly.  It is in every green run here.
        cp -R "$P96DIR/Libs/." "$HD/Libs/"
        mkdir -p "$HD/Devs/Monitors"
        # THE MONITOR IS NAMED AFTER THE BOARD, and that is not cosmetic.
        # InstallPicasso96's P_InstallCard copies devs/monitors/Picasso96 with
        # (newname #_boardname) and writes BoardType=<boardname> into the icon
        # it drops beside it; the one file drives every card P96 supports and
        # the name is how it knows which .card to load.  Staged as `Picasso96`
        # it loads nothing, publishes no resolutions, and Workbench comes up on
        # the chipset with every check here still passing.
        cp "$P96DIR/Devs/Monitors/Picasso96" "$HD/Devs/Monitors/$RTG_BOARD"
        rtg_monitor_icon "$HD/Devs/Monitors/$RTG_BOARD.info" "$RTG_BOARD"
        # No mode ID for this depth yet means the board has not been asked what
        # it publishes.  The prefs file is written with the 8-bit one so the
        # boot is a normal RTG boot and its listing can be read; the arm then
        # restages with the answer.  Writing nothing instead would leave
        # Workbench on the chipset and cost the same boot.
        if [ "$PATTERN" = 1 ]; then
            # WORKBENCH AT EIGHT BITS, and the arm's depth is nothing to do
            # with it.  The screen under test on a -P arm is the one rtgbars
            # opens, and it opens it on a mode the MACHINE picked; Workbench is
            # only here so the boot is an ordinary RTG boot.  Writing a
            # truecolour mode ID into screenmode.prefs would put a guess back
            # on the critical path, which is the thing -P was built to remove.
            wb31_screenmode_prefs_id "$HD" 8 "$RTG_MODE_ID" "$RTG_W" "$RTG_H"
        else
            wb31_screenmode_prefs_id "$HD" "$depth" "${mode_id:-$RTG_MODE_ID}" \
                                     "$RTG_W" "$RTG_H"
        fi
        # The lister and the picture.  rtgmodes opens nothing and returns, so
        # it is safe on the boot path; rtgbars opens the screen the arm is
        # about to be measured on.
        if [ "$PATTERN" = 1 ]; then
            cp "$RTGMODES" "$HD/C/rtgmodes"; chmod 755 "$HD/C/rtgmodes"
            cp "$RTGBARS"  "$HD/C/rtgbars";  chmod 755 "$HD/C/rtgbars"
        fi
    elif [ -n "$CHIP" ]; then
        # The prefs file first, so that the run also answers whether a 3.1
        # Workbench asked for one of these modes takes it -- C:chipscreen
        # reports the mode it finds the Workbench screen on before it opens
        # its own.  Nothing here depends on the answer: the screen the console
        # serves is the one chipscreen opens either way.
        wb31_screenmode_prefs_id "$HD" "$depth" "$(arm_mode_id "$CHIP")" \
                                 "$CHIP_W" "$CHIP_H"
        cp "$CHIPSCREEN" "$HD/C/chipscreen"
        chmod 755 "$HD/C/chipscreen"
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
    local depth="${1:-}"
    local db_depth
    db_depth=$(rtg_db_depth "${depth:-0}")

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
    #
    # AND NO SCREEN OF ITS OWN on an -R arm.  This ran a prober that opened
    # one, which put a PUBLIC, QUIET, EMPTY screen in front of Workbench -- and the front
    # screen is the one the console serves, so the session read a card screen
    # correctly and streamed 640x480 of colour zero.  Every RTG assertion
    # passed on it: format 1, a palette, a decoded frame, one pixel value.
    # screenmode.prefs puts WORKBENCH on the card and the geom word's format
    # says so per run, so there is nothing for a second screen to insure
    # against -- an -R run that comes up planar is meant to fail, not to be
    # rescued by a blank screen that hides what Workbench did.
    # AND IT NO LONGER RUNS THE MONITOR, because it took the guest down before
    # it ever reached the httpd line below.
    #
    # `DEVS:Monitors/uaegfx` was here to prove the board had been found.  By
    # the time S:Startup-Sequence reaches it, LoadWB has already brought the
    # board up -- the emulator log shows SetSwitch() on a Picasso96 640x480
    # screen about fifteen seconds in -- so running the monitor a SECOND time
    # meets a board context that already exists.  It printed "Could not create
    # graphics board context for 'uaegfx'" and then took the machine with it:
    # a Gary timeout and a B-Trap at PC=ffffffff, with everything after it in
    # the sequence, httpd included, never reached.  From outside that is
    # d8_up=no and RESULT=INFRA, which reads as a network or staging fault
    # rather than as the guest having crashed on a diagnostic.  The message
    # was doubly misleading: it is a symptom of the board WORKING.
    #
    # The mode lister went with it, and has come back as C:rtgmodes on the -P
    # arm alone: it opens nothing, always returns, and is linked and printed
    # the way a command is rather than the way a test is.  Its predecessor was
    # neither, and died at a garbage PC before its first line of output.
    #
    # The remaining lines are all reads.  None of them opens a board, a screen
    # or a card, so none of them can do this again.
    [ "$RTG" = 1 ] && cat >> "$HD/S/Startup-Sequence" <<'EOF'
C:Version >DH0:rtg-ver.txt LIBS:Picasso96/rtg.library FILE
C:Version >>DH0:rtg-ver.txt LIBS:Picasso96API.library FILE
C:Version >>DH0:rtg-ver.txt Picasso96API.library
C:Version >>DH0:rtg-ver.txt cybergraphics.library
C:List >DH0:rtg-libs.txt LIBS:Picasso96
C:List >>DH0:rtg-libs.txt DEVS:Monitors
C:Avail >DH0:rtg-avail.txt
EOF

    # THE MODE LISTER, AND THEN THE PICTURE.
    #
    # rtgmodes writes the whole display database into a file on DH0, which is a
    # host directory, so the harness reads the board's real answer while the
    # machine is still booting.  It opens nothing and it returns, which is the
    # whole reason it is a separate program from rtgbars: a lister that fell
    # into an open-and-wait path stopped a boot on this very line once, with
    # the server below it never reached.
    #
    # rtgbars then opens the truecolour screen the arm measures and stays.  It
    # is Run, so the sequence goes on; C:Wait after it, because httpd grabs the
    # front screen and a server started before the bands were drawn would open
    # its session on whatever was in front at the time.
    if [ "$PATTERN" = 1 ]; then
        cat >> "$HD/S/Startup-Sequence" <<EOF
C:rtgmodes >DH0:rtglist.txt
Run >NIL: <NIL: C:rtgbars $db_depth $RTG_W $RTG_H
C:Wait 5
EOF
    fi

    # THE SCREEN THE -C ARMS SERVE, and it is not Workbench's.  Run rather than
    # called, because chipscreen does not return: it opens the screen, draws on
    # it and waits, so a plain call would stop the boot on this line with the
    # server two lines below it.  The wait after it is the drawing -- eight
    # planes of 320x256, a byte at a time, is a few seconds on an emulated
    # 68020 -- and httpd must not start on a half-drawn picture.
    #
    # THE REPORT IS AN ARGUMENT, not a redirection.  `Run cmd >file` binds the
    # redirection to Run, which then gives the process it starts an output
    # stream of its own: the file collects Run's `[CLI 3]` line and not one
    # word from the program.  The redirection here is kept anyway, because it
    # is where Run says whether it managed to start anything at all.
    #
    # DEPTH 0 FIRST, and it is not a duplicate.  chipscreen at depth 0 reports
    # the Workbench screen and RETURNS, so it is the one call here whose
    # completion the boot can observe: a report file that never appears says
    # the program did not run, which a Run line -- whose child is off on its
    # own process with its own output stream -- cannot distinguish from a
    # program that ran and refused.  It is also the answer to whether
    # screenmode.prefs moved Workbench, read before this puts a screen of its
    # own in front of it.
    if [ -n "$CHIP" ]; then
        cat >> "$HD/S/Startup-Sequence" <<EOF
C:chipscreen $(arm_mode_id "$CHIP") $CHIP_W $CHIP_H 0 DH0:chipscreen-wb.txt
Run >DH0:chipscreen-run.txt <NIL: C:chipscreen $(arm_mode_id "$CHIP") $CHIP_W $CHIP_H $depth DH0:chipscreen.txt
C:Wait 15
EOF
    fi

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
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"

#
# AN RTG BOARD PUBLISHES THE HOST'S DISPLAY MODES, SO A HEADLESS HOST HAS NONE.
#
# Amiberry's addresolutions() builds the whole Picasso96 resolution list out of
# the modes SDL enumerates for the host, and BOTH headless SDL drivers report
# none: `dummy` and `offscreen` each log "0 display modes.", the board then
# calls InitCard with an empty list -- "P96 RESINFO: 00000000-00000000 (0,0)"
# -- and the Amiga side has a graphics card with no resolutions on it.  Every
# library loads, the board is mapped, and Workbench comes up on the chipset.
#
# So the RTG arm gets a real X server with a real mode on it.  One host mode is
# enough: the FAKE-mode substitution in the same loop fills in every standard
# resolution smaller than it, which is where 640x480 comes from.  Xvfb is not
# a workaround for a missing display here, it is the source of the data the
# board reports.
XVFB_PID=""
if [ "$RTG" = 1 ] && [ -z "${AMINETXDUO_CONSOLE_NO_XVFB:-}" ]; then
    command -v Xvfb >/dev/null 2>&1 || {
        say error "no Xvfb, and an RTG board has no modes without one"
        say hint "apt install xvfb, or set AMINETXDUO_CONSOLE_NO_XVFB=1 and \
supply a DISPLAY that enumerates modes"
        say RESULT INFRA
        exit 2
    }
    # :90 to :149, and the width is not arbitrary.  Ten was enough for one
    # person; with three checkouts on this host running RTG arms at once every
    # slot was taken, and what that reads as from outside is RESULT=INFRA on a
    # run that never started the emulator.  A display number costs nothing.
    XDISP=""
    for n in $(seq 90 149); do
        [ -e "/tmp/.X11-unix/X$n" ] || { XDISP=":$n"; break; }
    done
    [ -n "$XDISP" ] || { say error "no free X display in :90..:149"; \
                         say RESULT INFRA; exit 2; }
    Xvfb "$XDISP" -screen 0 1280x1024x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 2
    export DISPLAY="$XDISP"
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
    say xvfb "$XDISP 1280x1024x24 pid $XVFB_PID"
else
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
    [ "${SDL_VIDEODRIVER}" = "dummy" ] && unset DISPLAY WAYLAND_DISPLAY || true
fi

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

reap_xvfb() {
    [ -n "$XVFB_PID" ] || return 0
    kill -TERM "$XVFB_PID" 2>/dev/null || true
    wait "$XVFB_PID" 2>/dev/null || true
    XVFB_PID=""
    return 0
}
trap 'cleanup; reap_xvfb' EXIT INT TERM HUP

EMULOG=""

boot() {
    local tag="$1"
    local depth="$2"
    local mask=""
    local cfg="$ROOT/build/console-$tag.uae"

    EMULOG="$ROOT/build/amiberry-console-$tag.log"

    cat > "$cfg" <<EOF
config_description=AmiNetXDuo console $tag
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
    # minimal config does not go through the path that fills in the default.
    # rtg_modes_mask() says what each depth needs; a mask without RGBFF_CLUT is
    # a board that offers no palette mode and a Workbench that quietly stays on
    # the chipset, and the same is true of a truecolour arm whose bit is
    # missing.
    #
    # cpu_24bit_addressing=no IS NOT OPTIONAL AND IT FAILS SILENTLY WITHOUT IT.
    # uaegfx is a Zorro III board and a quickstart model comes up with a 24-bit
    # address space; the emulator then says "Z3 RTG and 24bit address space are
    # not compatible" into a log nothing reads, drops the board, and Workbench
    # comes up on the chipset.  Every assertion in this harness still passes on
    # that -- it is a screen, it has colours, it changes -- and the only thing
    # that says the card was never there is the depth in the geom word.
    if [ "$RTG" = 1 ]; then
        if [ "$PATTERN" = 1 ]; then
            mask=$(rtg_pattern_mask "$depth")
        else
            mask=$(rtg_modes_mask "$depth")
        fi
        cat >> "$cfg" <<EOF
cpu_24bit_addressing=no
gfxcard_size=8
rtg_modes=$mask
EOF
    fi

    # NOT --log.  It writes about a megabyte a second, playhouse3 is shared,
    # and a five-minute run left 427 MB of it here; nothing in this harness
    # reads it, because the backend assertion below is a better one.
    # --log, because without it this file was 155 bytes and said nothing at
    # all about a guest that had crashed on its own Startup-Sequence.  Finding
    # that needed Amiberry run by hand outside the harness, which is the one
    # thing a harness must not make necessary.  With it the log carries the
    # autoconfig board list, the Picasso96 SetSwitch, and the trap.
    ( trap '' PIPE; exec "$AMIBERRY" --log -f "$cfg" ) >"$EMULOG" 2>&1 &
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
    # Quoted one argument at a time, because "$*" is one string that the
    # remote shell splits again on every space in it.  --type "dir SYS:" and
    # anything else carrying a space arrived as several arguments, the probe
    # rejected them, and the run reported INFRA with an empty probe file --
    # which reads as the guest never answering rather than as this line.
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT" \
        "python3 - $(printf '%q ' "$@")" \
        < "$ROOT/tests/tools/console-probe.py" > "$out" 2>&1
}

# ONE HTTP CLIENT, AND IT IS THE ONE THE CLIENT MACHINE ALREADY HAS TO HAVE.
#
# These fetches used curl, and the probe travels as python3 -- so a client with
# python3 and no curl (playhouse4 carries wget instead) answered every probe and
# no fetch.  What that looks like from here is not "curl is missing": alive()
# returns the empty string on every attempt, the boot loop runs to BOOT_MAX, and
# the run reports d8_up=no on a guest that was serving the whole time.  Two
# 240-second RTG runs were spent on it.  python3 is already a hard requirement
# for the client, so the fetches use it too and the dependency list is one line
# shorter.
HTTP_GET_PY='
import sys, urllib.request, urllib.error
url, t = sys.argv[1], float(sys.argv[2])
try:
    with urllib.request.urlopen(url, timeout=t) as r:
        print(r.status, len(r.read()))
except urllib.error.HTTPError as e:
    print(e.code, 0)
except Exception:
    print(0, 0)
'

# url seconds -> "<status> <bytes>", "0 0" when nothing answered.
http_get() {
    local out
    if [ -z "$CLIENT" ]; then
        out=$(python3 -c "$HTTP_GET_PY" "$1" "$2" 2>/dev/null || true)
    else
        out=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT" \
              "python3 - '$1' '$2'" <<<"$HTTP_GET_PY" 2>/dev/null || true)
    fi
    printf '%s' "${out:-0 0}"
}

fetch_page() {
    http_get "http://$ADDRESS:$PORT/console" 8
}

alive() {
    local s
    s=$(http_get "http://$ADDRESS:$PORT/" 4)
    printf '%s' "${s%% *}"
}

# The board's own mode list, off the guest, while it is still booting.
#
# ONLY A -P BOOT PRODUCES ONE.  An earlier lister wrote it and was taken out of
# the Startup-Sequence: it could fall into its own open-and-wait path and stop
# the boot on that line.  tests/perf/rtgmodes.c is the replacement and it can
# do neither -- it opens nothing and always returns -- so a -P boot runs it and
# this reads what it wrote.
#
# DH0 is a host directory, so the file is here the moment the guest closes it,
# which is well before the server answers.  A plain -R boot writes no list and
# this still returns failure for it, leaving that arm exactly as it was.
rtg_wait_modes() {
    local limit="${1:-120}" n=0

    [ "$PATTERN" = 1 ] || return 1

    while [ "$n" -lt "$limit" ]; do
        if [ -f "$HD/rtglist.txt" ] &&
           grep -q '^result=listed' "$HD/rtglist.txt" 2>/dev/null; then
            cp "$HD/rtglist.txt" "$RTG_MODES_SEEN"
            return 0
        fi
        kill -0 "$EMU_PID" 2>/dev/null || return 1
        sleep 1
        n=$((n + 1))
    done
    return 1
}

# ------------------------------------------------------------------- run ----

mkdir -p "$OUTDIR"
RTG_MODES_SEEN="$OUTDIR/rtgmodes.txt"
say address "$ADDRESS:$PORT"
say backend "$BACKEND"
say mac "$MAC"
say model "$MODEL"
say client "${CLIENT:-this host}"
say page "$PAGE"
say activity "$ACTIVITY"
if [ "$PATTERN" = 1 ]; then
    say pattern "rtgbars colour bars, checked with tests/tools/rtgbars-check.py"
fi

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
ran=0

# One list, whichever kind of run this is: depths on a plain or -R run, mode
# names on a -C one.
if [ ${#CHIPSET[@]} -gt 0 ]; then
    ARMS=("${CHIPSET[@]}")
else
    ARMS=("${DEPTHS[@]}")
fi

for arm in "${ARMS[@]}"; do
    tag=$(arm_tag "$arm")
    depth=$(arm_depth "$arm")
    CHIP=""
    [ ${#CHIPSET[@]} -gt 0 ] && CHIP="$arm"
    skipped=no

    mode_id=$(rtg_mode_id "$depth")
    stage "$depth" "$mode_id"
    startup_for "$depth"
    started=$(date +%s)
    boot "$tag" "$depth"

    # The mode ID comes off the board rather than out of this file.
    #
    # 640x480x8 is 0x50031000 because somebody read the emulator's source; the
    # truecolour ones have no such constant here, and a screenmode.prefs naming
    # a mode the board does not publish opens nothing at all.  Workbench then
    # comes up on the chipset and every check except the format word passes on
    # it.  So the first boot of a depth is allowed to be a boot that learns:
    # the listing arrives before the server does, and the arm restages on it.
    # Nothing is measured from this -- the boot clock above is what
    # d<N>_boot_seconds has always been, and the wait happens inside it.
    if [ "$RTG" = 1 ]; then
        if rtg_wait_modes 120; then
            say "${tag}_rtg_modes_seen" \
                "$(grep -c '^rtgmode=' "$RTG_MODES_SEEN" 2>/dev/null || true)"
            learned=$(rtg_mode_from_list "$RTG_MODES_SEEN" \
                                        "$(rtg_db_depth "$depth")")
            if [ -z "$learned" ]; then
                # Not a failure of anything under test: Amiberry's uaegfx
                # publishes what the RGBFF mask and the host's own display
                # modes give it, and whether that includes a 15 or a 24-bit
                # mode is the board's answer to give.  The arm says so and
                # does not run.
                say "${tag}_skipped" "the board publishes no\
 ${RTG_W}x${RTG_H} mode at depth $(rtg_db_depth "$depth"); see\
 $RTG_MODES_SEEN"
                cleanup
                skipped=yes
            elif [ "$PATTERN" = 1 ]; then
                # NOTHING IS RESTAGED ON A -P ARM.  The screen it measures is
                # the one rtgbars opens, and rtgbars asks the machine which
                # mode to open rather than being told; the list is read here
                # for the skip decision above and for the record below.
                mode_id="0x$learned"
            elif [ "0x$learned" != "$mode_id" ]; then
                say "${tag}_rtg_mode_relearned" "0x$learned, not\
 ${mode_id:-none}"
                cleanup
                mode_id="0x$learned"
                stage "$depth" "$mode_id"
                startup_for "$depth"
                started=$(date +%s)
                boot "$tag" "$depth"
            fi
            [ "$skipped" = yes ] || say "${tag}_rtg_mode" "$mode_id"
        else
            say "${tag}_rtg_mode" "${mode_id:-none}"
        fi
    fi
    if [ "$skipped" = yes ]; then
        continue
    fi
    ran=$(( ran + 1 ))

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

    latency=()
    [ -z "$LATENCY" ] || latency=(--latency "$LATENCY")

    # A frozen picture passes everything else.
    #
    # A readback route that reads nothing wins the speed probe, and the console
    # then serves one grab for the whole session: frames arrive, the sequence
    # is whole, the picture has colours in it, and every check that only looks
    # at the socket says PASS.  --min-changed is the one that asks the guest --
    # it fails a session in which no frame differed from the one before it --
    # and it can only be asked when something on the screen is moving, so it
    # goes on when the run is scrolling or typing and not on an idle one.
    #
    # It is a truecolour thing here only because the arms it is new to are the
    # truecolour ones; the hole it closes is the same on all three formats, and
    # the 8-bit and planar arms are left exactly as they were.
    moving=()
    if [ "$RTG" = 1 ] && [ "$(rtg_wire_format "$depth")" = 2 ] &&
       { [ "$PATTERN" = 1 ] || [ "$ACTIVITY" = "scroll" ] ||
         [ -n "$TYPE" ]; }; then
        # A -P arm may ask for this on an idle machine, which no other arm can:
        # rtgbars blinks a square once a second precisely so that a session
        # served one frozen grab for ever cannot pass a colour check.
        moving=(--min-changed 2)
    fi

    set +e
    probe "$OUTDIR/$tag-probe.txt" \
        "$ADDRESS" "$PORT" --seconds "$PROBE_SECONDS" \
        --png "$OUTDIR/$tag.png" --pfs "$OUTDIR/$tag.pfs" \
        ${latency[@]+"${latency[@]}"} \
        ${typing[@]+"${typing[@]}"} ${moving[@]+"${moving[@]}"}
    rc=$?
    set -e

    while IFS='=' read -r k v; do
        [ -n "$k" ] || continue
        say "${tag}_$k" "$v"
    done < <(grep '=' "$OUTDIR/$tag-probe.txt" || true)

    #
    # -R MUST FAIL IF THE SESSION DID NOT COME UP ON A CARD.
    #
    # Every other check in this file passes on the chipset screen Workbench
    # falls back to when the board is missing: it is a screen, it has more
    # than one colour, its palette is not black, and it changes.  A run that
    # tested the planar path while reporting on the RTG one is worse than a
    # run that fails, and it happened three times here -- once on a 24-bit
    # address space, once on an RGBFF mask without CLUT, once on a monitor
    # file staged under the wrong name.  The seventh number of the geom word
    # is rfb_geom.format, and 0 is the only value that says the chipset: 1 is
    # the card's palette screen and 2 is a truecolour one downsampled to
    # r5g6b5.  Which of the two an arm must see is its depth's business, so the
    # third number is checked as well -- a 16-bit arm that came up on the 8-bit
    # Workbench screen would otherwise be a card session and pass.
    #
    # EVERY geom, not the geom.  A session gets a fresh geometry whenever the
    # screen it serves changes, and this read all of them into one variable:
    # the first run in which the front screen changed resolution mid-session
    # put two lines in `fmt`, and the assertion then failed a perfectly good
    # card session with "says format 1<newline>1, not 1".  sort -u collapses a
    # session that stayed on the card to the one value, and a session that
    # dropped to the chipset half way through still fails -- which is the case
    # this check exists for and the one a last-line-wins fix would have lost.
    if [ "$RTG" = 1 ]; then
        want_fmt=$(rtg_wire_format "$depth")
        want_depth=$(rtg_wire_depth "$depth")
        fmt=$(awk '/^geom=/ { print $NF }' "$OUTDIR/$tag-probe.txt" 2>/dev/null \
              | sort -u | tr '\n' ' ' || true)
        fmt=${fmt% }
        wire_depth=$(awk '/^geom=/ { print $3 }' "$OUTDIR/$tag-probe.txt" \
                     2>/dev/null | sort -u | tr '\n' ' ' || true)
        wire_depth=${wire_depth% }
        say "${tag}_rtg_format" "${fmt:-none}"
        say "${tag}_rtg_wire_depth" "${wire_depth:-none}"
        if [ "${fmt:-0}" != "$want_fmt" ] ||
           [ "${wire_depth:-0}" != "$want_depth" ]; then
            say "${tag}_error" "-R asked for a $depth-bit card screen, which is\
 format $want_fmt depth $want_depth on the wire, and the geom word says format\
 ${fmt:-none} depth ${wire_depth:-none}"
            VERDICT=fail
        fi
    fi

    #
    # -P: WHICH TRUECOLOUR PATH RAN, AND WHETHER RED IS STILL RED.
    #
    # The geom check above cannot answer either question.  Every truecolour
    # depth is downsampled to R5G6B5 before it is sent, so format 2 depth 16 is
    # what a 15, 24 and 32-bit screen all say, and the picture behind it
    # decodes and has colours in it whichever way round its channels are.  Two
    # things close that: the readback word carries the card's own depth and the
    # name of its pixel format, and the bands rtgbars drew are a picture with a
    # known answer.
    if [ "$PATTERN" = 1 ]; then
        cp "$HD/rtgbars.txt" "$OUTDIR/$tag-rtgbars.txt" 2>/dev/null || true
        cp "$HD/rtglist.txt" "$OUTDIR/$tag-rtglist.txt" 2>/dev/null || true

        word=$(sed -n 's/^word_unknown=\(rtg .*\)$/\1/p' \
               "$OUTDIR/$tag-probe.txt" 2>/dev/null | tail -1 || true)
        card_depth=$(printf '%s' "$word" | tr ' ' '\n' \
                     | sed -n 's/^depth=//p' | tail -1 || true)
        card_fmt=$(printf '%s' "$word" | tr ' ' '\n' \
                   | sed -n 's/^fmt=//p' | tail -1 || true)
        want_db_depth=$(rtg_db_depth "$depth")
        want_card_fmt=$(rtg_card_fmt "$depth")
        say "${tag}_rtg_card_depth" "${card_depth:-none}"
        say "${tag}_rtg_card_fmt" "${card_fmt:-none}"

        # THE FORMAT IS THE ASSERTION.  Depth alone cannot separate a packed
        # 24-bit screen from a 32-bit one -- this board publishes both as depth
        # 24 -- and nothing downstream of the card can separate either from a
        # 16-bit one, since all three leave as R5G6B5.  The name in the
        # readback word is what says which of the converter's rows ran.
        if [ "${card_fmt:-none}" != "$want_card_fmt" ] ||
           [ "${card_depth:-0}" != "$want_db_depth" ]; then
            say "${tag}_error" "-P asked for the $want_card_fmt path, which is\
 depth $want_db_depth on this board, and the readback word says depth\
 ${card_depth:-none} format ${card_fmt:-none}"
            VERDICT=fail
        fi

        # The PNG is written where the probe ran, which on -c is another
        # machine.  Fetched rather than checked there: the checker is part of
        # this tree and the client is only required to have python3.
        if [ -n "$CLIENT" ]; then
            scp -q -o BatchMode=yes "$CLIENT:$OUTDIR/$tag.png" \
                "$OUTDIR/$tag.png" 2>/dev/null || true
        fi

        if [ -f "$OUTDIR/$tag.png" ]; then
            set +e
            python3 "$ROOT/tests/tools/rtgbars-check.py" "$OUTDIR/$tag.png" \
                > "$OUTDIR/$tag-bars.txt" 2>&1
            brc=$?
            set -e
            say "${tag}_bands_ok" \
                "$(sed -n 's/^bands_ok=//p' "$OUTDIR/$tag-bars.txt" \
                   | tail -1 || true)"
            swapped=$(sed -n 's/^red_blue_exchanged=//p' \
                      "$OUTDIR/$tag-bars.txt" | tail -1 || true)
            [ -z "$swapped" ] || say "${tag}_red_blue_exchanged" "$swapped"
            say "${tag}_bars_report" "$OUTDIR/$tag-bars.txt"
            case "$brc" in
                0) ;;
                2) VERDICT=infra ;;
                *) say "${tag}_error" "the bands rtgbars drew did not come\
 back: see $OUTDIR/$tag-bars.txt"
                   VERDICT=fail ;;
            esac
        else
            say "${tag}_error" "no decoded frame at $OUTDIR/$tag.png to check"
            VERDICT=fail
        fi
    fi

    #
    # -C MUST FAIL IF THE SESSION DID NOT COME UP ON THE MODE IT ASKED FOR.
    #
    # The same rule as the -R block above and for the same reason: format 0 is
    # the plain planar screen every other check in this file passes on, and a
    # run that tested the planar path while reporting on HAM is worse than a
    # run that fails.  The failure is a real possibility rather than a
    # precaution -- the ScreenMode editor filters these modes out, so a
    # Workbench that stayed PAL hires is what a broken staging leaves behind,
    # and it is a perfectly good two-plane screen.
    #
    # The guest's own account is read as well.  It says which mode OpenScreen
    # actually gave chipscreen and which pattern it therefore drew, so a
    # mismatch between the picture and the geom word has both halves in the
    # output rather than one.  EVERY geom, collapsed with sort -u, for the
    # reason the -R block gives: a session that dropped to another screen half
    # way through must still fail.
    if [ -n "$CHIP" ]; then
        want_fmt=$(arm_wire_format "$CHIP")
        want_depth="$depth"
        fmt=$(awk '/^geom=/ { print $NF }' "$OUTDIR/$tag-probe.txt" 2>/dev/null \
              | sort -u | tr '\n' ' ' || true)
        fmt=${fmt% }
        wire_depth=$(awk '/^geom=/ { print $3 }' "$OUTDIR/$tag-probe.txt" \
                     2>/dev/null | sort -u | tr '\n' ' ' || true)
        wire_depth=${wire_depth% }
        say "${tag}_chipset_format" "${fmt:-none}"
        say "${tag}_chipset_wire_depth" "${wire_depth:-none}"

        # The depth 0 pass, which ran to completion in the boot and says what
        # Workbench itself came up on.
        if [ -f "$HD/chipscreen-wb.txt" ]; then
            cp "$HD/chipscreen-wb.txt" "$OUTDIR/$tag-chipscreen-wb.txt"
            while IFS='=' read -r k v; do
                case "$k" in wb_*) say "${tag}_$k" "$v" ;; esac
            done < <(grep '=' "$HD/chipscreen-wb.txt" || true)
        else
            say "${tag}_chip_loaded" "no; C:chipscreen wrote no report at depth 0"
        fi

        guest="$HD/chipscreen.txt"
        if [ -f "$guest" ]; then
            cp "$guest" "$OUTDIR/$tag-chipscreen.txt"
            while IFS='=' read -r k v; do
                [ -n "$k" ] || continue
                say "${tag}_chip_$k" "$v"
            done < <(grep '=' "$guest" || true)
        else
            say "${tag}_chip_result" "C:chipscreen wrote no report to DH0:"
            say "${tag}_chip_run" \
                "$(tr '\n' ' ' < "$HD/chipscreen-run.txt" 2>/dev/null || true)"
            VERDICT=fail
        fi

        drew=$(awk -F= '/^pattern=/ { print $2 }' "$guest" 2>/dev/null || true)
        if [ "$drew" != "$CHIP" ]; then
            say "${tag}_error" "C:chipscreen drew ${drew:-nothing}, not $CHIP,\
 so the picture on the wire is not the one this arm asserts against"
            VERDICT=fail
        fi

        if [ "${fmt:-0}" != "$want_fmt" ] ||
           [ "${wire_depth:-0}" != "$want_depth" ]; then
            say "${tag}_error" "-C asked for a $CHIP screen, which is format\
 $want_fmt depth $want_depth on the wire, and the geom word says format\
 ${fmt:-none} depth ${wire_depth:-none}"
            VERDICT=fail
        fi
    fi

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
say arms_run "$ran"

# A run where every arm was skipped is not a pass.  The board publishing no
# mode at a depth is a fair reason for one arm not to run and no reason at all
# to report that the thing under test is good, which is what PASS would say.
if [ "$ran" = 0 ]; then
    say error "no arm ran: every depth asked for was skipped"
    VERDICT=infra
fi

say RESULT "$(printf '%s' "$VERDICT" | tr '[:lower:]' '[:upper:]')"

case "$VERDICT" in
    pass)  exit 0 ;;
    fail)  exit 1 ;;
    *)     exit 2 ;;
esac
