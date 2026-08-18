#!/usr/bin/env bash
#
# Install the release archive on a REAL Workbench 3.1, reboot, and then use
# the machine the way its owner would.
#
#   install/test/run-workbench.sh [-b BUILDDIR] [-a ARCHIVE.lha]
#                                       [-l NOVICE|AVERAGE|EXPERT]
#                                       [-p CHOICE] [-N BOARD]
#                                       [-t SECONDS] [-T SECONDS] [-k] [-H]
#
# -N NAMES THE NETWORK CARD, from tests/tools/cards.sh, and it defaults to
# a2065 because that is the only card this gate has ever booted.  Every release
# this project has cut was gated on one card: the driver was staged by name and
# the emulator config held a2065_rom_file, with no way to ask for another.  The
# standing rule is that every card appears in the end-to-end, and one-card
# coverage has already let a regression reach a user once.
#
# The model follows the card unless AMINETXDUO_MODEL says otherwise --
# xsurf100z3 is a Zorro III board and an A1200 has a 24-bit address space, so
# Amiberry never maps it and the run reads as a driver that cannot find its
# hardware.  tools/emu-board.sh refuses that pair by name instead.
#
# WHAT -N PROVES, AND WHAT IT DOES NOT.  The Installer asks which card the
# machine has, and installdrive.c cannot answer an askchoice: a posted GADGETUP
# at an option gadget leaves the page's default in place (installdrive.c, the
# DRIVE_PICK_OPTIONS note, measured twice).  What it CAN do is arrive at a
# machine where the answer is already right: the script's detection loop scans
# DEVS: and DEVS:Networks for a driver it knows and makes that the page's
# default, so staging the driver first is how the card gets selected.
#
# That works only for a driver whose FILE NAME is in the script's list.  When
# it is not, the default falls back to the first entry -- the A2065 -- and the
# installer writes DEVICE=a2065.device for whatever card is really in the
# machine.  This run does not paper over that: it prints
#
#   installer_card_selected=yes|no   installer_device=<what was written>
#
# and, when the answer is no, rewrites the interface file after the install and
# says so with card_config=post-install.  Such a run proves THE STACK DRIVES
# THE CARD.  It does not prove the installer can select it, and the two are
# never reported as one thing.
#
# -p NAMES AN OPTION ON AN askchoice PAGE.  It takes `minimal` or `full`, and
# `-p minimal` is the one that matters: the second stack in the archive, with
# IPv6, mDNS, the packet filter, TLS, IPv4 multicast, the ARexx host and the
# TCP: handler compiled out.  Needs -l AVERAGE or -l EXPERT, like -H, because
# at NOVICE the page is never drawn.
#
# -p minimal DOES NOT WORK YET, and it fails rather than pretending.  The
# harness took the default of every askchoice in the script -- so the minimal
# stack has shipped in every archive and has never been installed or booted by
# any end-to-end run -- and driving the option turns out to need more than a
# posted GADGETUP: installdrive.c records both measurements and what to try
# next.  What is fixed here is the silence: the run asserts by byte count which
# of the two libraries landed, so an arm that asks for minimal and gets the
# full stack is a failure and not a pass.
#
# -H IS THE TERMINAL ARM, and it makes the run three installs instead of one:
#
#   1. a pre-existing S:User-Startup with another application's lines in it is
#      written BEFORE anything is installed, then the Installer runs twice,
#      both times answering "Yes, serve them" to the question about serving a
#      drawer and a Shell at boot.  Two runs, one httpd line and one assign
#      line, or the managed block was appended to rather than replaced;
#   2. the power cycle, and while the machine is up this host fetches
#      http://<the Amiga>/shell over the bridge, which is what "reachable"
#      means;
#   3. a third install answering the question the other way, which has to take
#      both lines away again and leave AddNetInterface and the other
#      application's lines exactly where they were.
#
# WHAT THE OTHER MACHINE DOES, in step 2, and it is the product's main path:
# GET /shell and compare Content-Length against the page on the drive; PUT
# a file and GET it back and compare the bytes; PUT our own release .lha and
# have the Amiga `lha x` it, then compare the extracted files against this
# host's copies; and run tests/tools/httpd-drill.py --terminal and
# tests/tools/wsterm-console.py, which is a Shell answering through the
# WebSocket.  Nothing here scores "the command completed".
#
# AMINETXDUO_PEER is that machine and there is no default.  The host running
# amiberry cannot be it: with uaenet_pcap on a shared NIC a frame this host
# sends to the guest's MAC does not come back round to that NIC's pcap, so
# every connection from here is refused.  Without a peer the -H run exits 3
# rather than passing.
#
# lha is LhA 2.15 from ~/amiga-assets/apps/lha-2.15, which is licensed
# third-party software: it is staged onto the test drive here and is in
# nothing this repository ships.  Absent, that one step says so and fails,
# rather than disappearing.
#
# It needs -l AVERAGE or -l EXPERT: at NOVICE the Installer draws no questions
# at all and there is nothing to answer.
#
# Exit status: 0 pass, 1 something under test failed, 2 an ingredient is
# missing on this machine, 3 nothing could reach the Amiga from outside.
#
# WHY THIS EXISTS, given that install/test/run-installer.sh already installs and
# boots.  That harness stages the machine itself: it makes an empty LIBS:, a
# DEVS: holding one driver and an S: with nothing in it, boots a bare
# directory hard drive with no Workbench on it at all, and drives its own
# boot script.  Every failure a user has reported so far, `fetch https://`
# not working, a command not seeing its arguments, happened on a real
# Workbench and was invisible here, because "works on the staging tree" and
# "works on a Workbench" are different claims and only the second one is the
# product.
#
# So this one does the whole thing end to end:
#
#   0. assembles a genuine Workbench 3.1 SYS: out of Commodore's five
#      floppies (workbench, fonts, locale, storage, extras) and caches it;
#   1. boots it, SetPatch, IPrefs, LoadWB, the real Startup-Sequence;
#   2. unpacks the release .lha into DH0:Unpacked/ the way a download
#      would arrive, and runs Install-AmiNetXDuo through Commodore's
#      Installer with installdrive clicking Proceed;
#   3. POWER CYCLES the machine.  The second boot runs the stock 3.1
#      Startup-Sequence untouched, which reaches `Execute S:User-Startup`
#      and starts the network from the line the installer wrote, nothing
#      the harness knows is in the path;
#   4. runs, from an ordinary Shell script, the four things a user does
#      first: look at the network, fetch an http: URL, fetch an https: URL,
#      and run the shipped commands WITH ARGUMENTS.
#
# Each of those four reports its own return code, and this script prints them
# as a table.  IT DOES NOT ADJUST THEM UNTIL THEY PASS: a failure here is the
# finding.
#
# The bare-hardware harness is untouched and still the one to run for
# installer-script coverage (five user levels, the static-address branch, the
# reinstall path).  This one covers the other axis: one install, on a machine
# that looks like the user's.
#
# -a TAKES THE ARCHIVE AS GIVEN, and that is the strongest form of this test:
#
#     gh release download v0.8.1 -D /tmp/rel
#     install/test/run-workbench.sh -a /tmp/rel/AmiNetXDuo-0.8.1.lha
#
# Without it the archive is built here, from this machine's toolchain, and a
# defect that lives in the RELEASE build, a different compiler, a different
# crt0, cannot show up.  Run it both ways before believing a release.
#
# The https step fetches a list of hosts and passes on the first one that
# answers.  What it asserts is that the trust store the installer just wrote
# verifies a real public CA chain, so the certificate has to come off the
# internet and not out of our own peer, and that makes the step depend on
# somebody else's server.  One server is a single point of failure:
# ftp.gnu.org, which this used to fetch, rate-limits this lab and then refuses
# both families for minutes -- 0 of 10 curl fetches, connect timeouts, at the
# same moment www.gnu.org answered 10 of 10.  A red run then said nothing
# about this stack.  Three hosts, and it goes red only when all three fail,
# which no single operator's policy can produce.
#
# A server also puts a clock on the handshake, started at accept() and stopped
# when the client's Finished arrives, and a 68020 does not finish inside every
# one of them.  Seconds a server holds a connection with no ClientHello on it,
# measured from this lab 2026-08-12:
#
#   www.google.com, dns.google                       10
#   www.7-zip.org                                    11
#   www.iana.org, www.rfc-editor.org (Cloudflare)    15
#   ftp.heanet.ie                                    30
#   ftp.acc.umu.se                                   43
#   aminet.net, www.kernel.org, ftp.nluug.nl,
#   mirror.leaseweb.com, mirrors.mit.edu,
#   eab.abime.net, mirror.init7.net                  60
#   ftp.tu-chemnitz.de, ftp.riken.jp                 92
#   www.zlib.net                                     152
#   www.amiga-news.de, www.pouet.net, ftp.tugraz.at,
#   os4depot.net, www.gnu.org, www.openbsd.org,
#   www.debian.org, ftp.funet.fi, ftp.fau.de,
#   ftp.lysator.liu.se, ftp.snt.utwente.nl           no close at 250
#
# Only the last group can be used.  Against www.iana.org the A1200 here took
# 20.1 s of handshake, so Cloudflare sent FIN 15.0 s in, five seconds before
# the client Finished went out, and the RST arrived before the request could
# be answered.  Handshake cost, A1200/68020, whole chain verified:
#
#   www.pouet.net       17.4 s   3 certificates, RSA-2048 leaf   IPv4
#   www.amiga-news.de   17.5 s   3 certificates, RSA-2048 leaf   IPv4
#   www.gnu.org         22.1 s   3 certificates, RSA-2048 leaf   IPv6
#   ftp.tugraz.at       24.4 s   3 certificates, ECDSA P-256     IPv4
#   www.openbsd.org     25.2 s   3 certificates, RSA-4096 leaf   IPv4
#   www.debian.org      25.4 s   3 certificates, RSA-4096 leaf   IPv4
#   os4depot.net        34.4 s   4 certificates, ECDSA P-256     IPv4
#
# The family is not chosen here.  fetch resolves once and connects once --
# src/tools/toolsock.c:840 takes the first usable answer and the library
# orders IPv6 first -- and does not fall back to the other address, so a name
# with an AAAA is fetched over IPv6 and a name without one over IPv4.
# www.gnu.org is first because it has both and this is the only place an
# https transfer over IPv6 happens end to end; the two behind it have no AAAA,
# so an IPv6 outage cannot take the whole list with it.
#
# On this rig it takes that outage every run, and it is the rig.  Amiberry's
# ethernet_fix_partial_csum (src/ethernet.cpp:172) returns at :181 for
# anything that is not ethertype 0x0800, so an off-LAN IPv6 segment reaches
# the guest carrying the pseudo-header sum rather than a finished checksum and
# the stack is right to drop it -- 560 of 560 inbound IPv6 frames on
# 2026-08-12, against 6351 of 6351 correct for a Linux client on the same
# server.  Measured from the guest the same day: 1 MB in 4.1 s over IPv4 and
# 11.3 to 64.3 s over IPv6, and an IPv6 connect to www.openbsd.org,
# www.debian.org, ftp.funet.fi, ftp.lysator.liu.se or ftp.snt.utwente.nl
# failing after 191 s while this host reached those addresses in under 2 s
# from the guest's own /64.  So the first host costs 191 s and then the second
# answers.  A repair for the other ethertypes is being written; when it lands
# the first host answers in 22 s and this step gets three minutes shorter.
# Every attempt is printed, so the day that line stops appearing is visible in
# the artifact.
#
# INGREDIENTS, none of which are ours to ship:
#
#   Workbench 3.1 ADFs   ~/amigaos/adf/amiga-wb31_{workbench,extras,fonts,
#                        locale,storage}.adf, or AMINETXDUO_ADF_DIR
#   Kickstart 3.1        AMINETXDUO_KICKSTART, else the A1200 40.68 image
#   Commodore Installer  build/Installer, or AMINETXDUO_INSTALLER
#   the card's driver    a2065.device from build/ or AMINETXDUO_A2065; every
#                        other card's from ~/amiga-assets/devs or
#                        AMINETXDUO_SANA2_STORE.  Most of them cannot be
#                        fetched (docs/RESEARCH.md 77 permits two of eight), so
#                        they arrive by somebody putting the file there.
#   amitools' xdftool    AMINETXDUO_XDFTOOL, or on PATH (pip install amitools)
#   lha                  to unpack the archive on the host; Lhasa will do
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="${AMINETXDUO_BUILD:-build/cm}"
ARCHIVE=""
LEVEL=NOVICE
INSTALL_TIMEOUT=420
BOOT_TIMEOUT=720
KEEP=0
TERMINAL=0
PICK=""
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"

while getopts "b:a:l:p:N:t:T:kH" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        a) ARCHIVE="$OPTARG" ;;
        l) LEVEL="$OPTARG" ;;
        p) PICK="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        t) INSTALL_TIMEOUT="$OPTARG" ;;
        T) BOOT_TIMEOUT="$OPTARG" ;;
        k) KEEP=1 ;;
        H) TERMINAL=1 ;;
        *) echo "usage: $0 [-b builddir] [-a archive.lha]" \
                "[-l NOVICE|AVERAGE|EXPERT] [-p choice] [-N board]" \
                "[-t seconds] [-T seconds] [-k] [-H]" >&2
           exit 2 ;;
    esac
done

# -H needs a level where the questions are drawn at all.  At NOVICE every
# ask... returns its default without showing anything, so a run that asked for
# the terminal and got NOVICE would install without it and pass every check
# below that does not look for it -- which is the vacuous pass this whole
# script exists not to produce.
if [ "$TERMINAL" = "1" ] && [ "$LEVEL" = "NOVICE" ]; then
    echo "-H needs -l AVERAGE or -l EXPERT: at NOVICE the Installer draws no" >&2
    echo "questions and the terminal one cannot be answered." >&2
    exit 2
fi

# -p takes a name, not a gadget number: the number is this file's business.
# "minimal" is the second option of the two-option stack page, which is the
# only askchoice in the script with two options -- the card question has nine.
PICK_SPEC=""
case "$PICK" in
"")        ;;
minimal)   PICK_SPEC="2:2" ;;
full)      PICK_SPEC="2:3" ;;
*)         echo "-p takes minimal or full, not \"$PICK\"" >&2; exit 2 ;;
esac

# It needs a level for the same reason -H does: at NOVICE the page is never
# drawn, the default is taken, and the run would install the FULL stack while
# claiming to test the minimal one -- the vacuous pass this file exists not to
# produce, so it is fatal here rather than a surprise in the verdict.
if [ -n "$PICK" ] && [ "$LEVEL" = "NOVICE" ]; then
    echo "-p needs -l AVERAGE or -l EXPERT: at NOVICE the Installer draws no" >&2
    echo "questions, so \"$PICK\" could not be chosen and the default would" >&2
    echo "be installed instead." >&2
    exit 2
fi

case "$LEVEL" in
    NOVICE|AVERAGE|EXPERT) ;;
    *) echo "unknown user level: $LEVEL" >&2; exit 2 ;;
esac

# The unpack below runs from inside DH0:Unpacked, so a relative -a would be
# resolved against the wrong directory and read as a missing archive.
case "$ARCHIVE" in
    ""|/*) ;;
    *)     ARCHIVE="$PWD/$ARCHIVE" ;;
esac
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

# ------------------------------------------------------------------- the card --
#
# ONE TABLE, tests/tools/cards.sh, shared with the two card sweeps.  A card this
# gate can boot that the sweeps cannot, or the other way round, is the same hole
# one level up: "every card" has to mean the same thing everywhere.
# shellcheck source=../../tests/tools/cards.sh
. "$ROOT/tests/tools/cards.sh"
# shellcheck source=../../tools/sana2-stage.sh
. "$ROOT/tools/sana2-stage.sh"
# shellcheck source=../../tools/emu-board.sh
. "$ROOT/tools/emu-board.sh"

BOARD_MODEL=$(cards_rows "$BOARD" | awk '{ print $2; exit }')
if [ -z "$BOARD_MODEL" ]; then
    echo "-N $BOARD is not a card in tests/tools/cards.sh.  The cards are:" >&2
    cards_rows | awk '{ printf "  %-14s %s\n", $1, $2 }' >&2
    echo >&2
    echo "Cards this project names that no arm can reach, and why:" >&2
    printf '%s\n' "$UNTESTABLE" | awk 'NF { printf "  %-14s %s\n", $1,
        substr($0, index($0, $2)) }' >&2
    exit 2
fi
GCC="${AMIGA_GCC:-$HOME/amigaos/tools/m68k-amigaos-gcc/bin/m68k-amigaos-gcc}"
NDK="${AMIGA_NDK:-$HOME/amigaos/tools/m68k-amigaos-gcc/m68k-amigaos/ndk-include}"

TAG="${AMINETXDUO_RUN_TAG:-wb31}"
HD="$ROOT/build/testhd-$TAG"

# ------------------------------------------------------------ ingredients --

INSTALLER="${AMINETXDUO_INSTALLER:-}"
if [ -z "$INSTALLER" ]; then
    for candidate in "$ROOT/build/Installer" "$HOME/amigaos/tools/Installer"; do
        [ -f "$candidate" ] && { INSTALLER="$candidate"; break; }
    done
fi
[ -n "$INSTALLER" ] && [ -f "$INSTALLER" ] || {
    echo "No Commodore Installer found." >&2
    echo "Set AMINETXDUO_INSTALLER=<path>, or put one in build/Installer." >&2
    exit 2
}

# THE VENDOR DRIVER, not anxnet.device, and that is a statement about what this
# gate is for.  The installer deliberately does not install anxnet.device
# (Install-AmiNetXDuo, the anxnet section): the archive carries it in
# Devs/Networks/ and a user copies it there by hand.  So what a machine
# installed from the archive opens is the driver that came with the card, and
# that is what this run has to boot.  AMINETXDUO_SANA2_VENDOR is set here for
# the same reason and can still be unset by a caller that wants the other one.
AMINETXDUO_SANA2_VENDOR="${AMINETXDUO_SANA2_VENDOR:-1}"
export AMINETXDUO_SANA2_VENDOR

# The A2065 keeps its own variable and its own search: it is Commodore's, it
# comes out of the OS sources rather than the asset store, and every existing
# caller of this script sets AMINETXDUO_A2065 and nothing else.
if [ "$BOARD" = a2065 ] && [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ]; then
    A2065="${AMINETXDUO_A2065:-}"
    if [ -z "$A2065" ]; then
        for candidate in \
            "$ROOT/build/a2065.device" \
            "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
        do
            [ -f "$candidate" ] && { A2065="$candidate"; break; }
        done
    fi
    [ -n "$A2065" ] && [ -f "$A2065" ] || {
        echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
        exit 2
    }
    AMINETXDUO_SANA2_DRIVER="$A2065"
    export AMINETXDUO_SANA2_DRIVER
fi

sana2_select "$BOARD" "$BUILD"
DRIVER_NAME="$SANA2_SEL_DRIVER"
DRIVER_PATH="${AMINETXDUO_SANA2_DRIVER:-$SANA2_SEL_PATH}"

# HANDED BACK, not just printed.  sana2_stage_driver copies
# $AMINETXDUO_SANA2_DRIVER and nothing else, so a path that was resolved here
# and left in a local variable means the card goes in the machine with no
# driver beside it -- which this script correctly refused to run, eight cards
# at a time, after saying it had found the file two lines earlier.
AMINETXDUO_SANA2_DRIVER="$DRIVER_PATH"
export AMINETXDUO_SANA2_DRIVER

# A MISSING DRIVER IS AN INGREDIENT, NOT A FAILURE.  The card would be in the
# machine with nothing able to open it, and the run would go red for a file
# this host has never had rather than for anything in the product.  Exit 2 is
# what this script already uses for that, and the sweep above it reports it as
# a skip.
[ -n "$DRIVER_PATH" ] && [ -f "$DRIVER_PATH" ] || {
    echo "No $DRIVER_NAME for -N $BOARD on this host." >&2
    echo "Put one in \$HOME/amiga-assets/devs, or name it:" >&2
    echo "  AMINETXDUO_SANA2_DRIVER=<path>  AMINETXDUO_SANA2_STORE=<dir>" >&2
    echo "Most of these cannot be fetched; docs/RESEARCH.md 77 has the licences." >&2
    exit 2
}

# THE ROM AND THE MODEL ARE A PAIR, and the wrong half of it is a silent
# failure: an A1200 booted on a CD32 ROM is not an A1200.  So the candidates
# here are A1200-or-better 3.1 images ONLY, named explicitly, and the one
# chosen is printed.  A CD32 image is never picked by accident because it is
# not in the list.
#
# The default is an A1200 for the handshake clock rather than for the
# instruction set.  tls.library runs on every processor, the 68000 included --
# one binary, AMINETXDUO_CPU=any -- but the servers in the list above hold a
# connection for a bounded time, and a 68000 takes a minute or two over a first
# handshake.  A 68000 run of this script is a real run; it is the https arm
# that has fewer hosts it can finish against.
#
# THE CARD PICKS IT when nothing else does.  tests/tools/cards.sh names the
# machine each board needs -- an A1200 for all but one -- and taking A1200
# unconditionally is what would boot xsurf100z3 into a machine that cannot map
# a Zorro III board, silently.
MODEL="${AMINETXDUO_MODEL:-$BOARD_MODEL}"
emu_board_model_check "$BOARD" "$MODEL" || exit 2
# The pairing above is enforced, not just described.  An A1200 40.68 ROM is
# AGA and expects an A1200; booting it on quickstart=A600 crashes the guest
# before Workbench comes up, and the crash presents as a boot that never
# finishes, so the run dies on INSTALL_TIMEOUT and leaves an empty drive.  An
# empty drive passes every "file is absent" assertion in this script.  Three
# runs of the 68000 arm were read as green that way.  So a model outside the
# AGA set must name its own ROM, and not naming one is fatal here rather than
# 420 seconds later.
#
# AMINETXDUO_KICKSTART_<MODEL> WINS OVER AMINETXDUO_KICKSTART.  The generic one
# is an A1200 AGA image in every environment that sets it, including the lab's
# own env.sh, which sets the A600 ROM two lines below it and annotates it "the
# 68000 machines need a 68000 ROM".  Reading the generic one first threw that
# away and put the AGA ROM back on the A600.
eval "KICKSTART=\${AMINETXDUO_KICKSTART_$MODEL:-}"
if [ -z "$KICKSTART" ]; then
    case "$MODEL" in
    A1200|A3000|A4000) KICKSTART="${AMINETXDUO_KICKSTART:-}" ;;
    esac
fi
if [ -z "$KICKSTART" ]; then
    case "$MODEL" in
    A1200|A3000|A4000)
        for candidate in \
            "$HOME/Downloads/Kickstart v3.1 rev 40.68 (1993)(Commodore)(A1200)[!].rom" \
            "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A1200)[!].rom" \
            "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A4000).rom" \
            "$HOME/Downloads/Kickstart v3.1 r40.68 (1993)(Commodore)(A3000).rom" \
            "$HOME/png2amiga_testing/kick31.rom"
        do
            [ -f "$candidate" ] && { KICKSTART="$candidate"; break; }
        done
        ;;
    *)
        echo "MODEL=$MODEL needs its own ROM: the default list is AGA and" >&2
        echo "an AGA ROM on a 68000 machine crashes before Workbench loads." >&2
        echo "Set AMINETXDUO_KICKSTART_$MODEL=<path> or AMINETXDUO_KICKSTART." >&2
        exit 2
        ;;
    esac
fi
[ -n "$KICKSTART" ] && [ -f "$KICKSTART" ] || {
    echo "No Kickstart 3.1 ROM; set AMINETXDUO_KICKSTART=<path>." >&2
    exit 2
}

# Amiberry, not fs-uae.  fs-uae opens an SDL window even with nothing to open
# it on, so on a headless machine it dies in seconds and the failure reads as
# a guest that never booted; and the Debian build's SLIRP is a stub that logs
# `stub, uae_slirp_start` and carries on, so the network half of this test
# would measure nothing.  tools/amiberry-run.sh's header has the long version.
AMIBERRY="${AMIBERRY:-$(command -v amiberry || true)}"
if [ -z "$AMIBERRY" ]; then
    for candidate in "$HOME/amiberry/build/amiberry" "$HOME/amiberry/amiberry"; do
        [ -x "$candidate" ] && { AMIBERRY="$candidate"; break; }
    done
fi
[ -n "$AMIBERRY" ] || { echo "amiberry not found; set AMIBERRY=<path>" >&2; exit 2; }

# The network backend the guest's A2065 is wired to, and it is A BRIDGE.  A
# bare interface name (`ens18`) puts the guest on the host's own LAN with its
# own MAC, which is what makes the DHCP, DNS, http and TLS checks below a
# statement about the machine a user gets.
#
# THERE IS NO SLIRP PATH HERE ANY MORE, and the reason is not taste.  This
# script is the release gate: it is what decides "shipped" rather than
# "tagged".  It defaulted to slirp for every run without -H, so the network arm
# of every release gate ever run -- 0.23.0 included -- proved DHCP, HTTP and
# TLS against slirp's 10.0.2.0/24 and against a gateway written in C inside the
# emulator, and never once against a LAN, a real DHCP server, a real router or
# a real neighbour.  A green gate that tested a different machine than the one
# people download is worse than no gate, because it is believed.
#
# So a backend that is not a bridge is refused by name, and a backend that
# cannot be worked out is refused with what to pass.  It is never substituted
# for: the whole failure above is a substitution nobody was told about.
#
# Nothing is guessed except the obvious: the interface the host's own default
# route goes out of is the one that reaches the LAN this test needs.  On a host
# where that is wrong, name it.
BACKEND="${AMINETXDUO_EMU_BACKEND:-${AMINETXDUO_AMIBERRY_BACKEND:-}}"
if [ -z "$BACKEND" ]; then
    BACKEND=$(ip -4 route show default 2>/dev/null |
              sed -n 's/.*[[:space:]]dev[[:space:]]\{1,\}\([^[:space:]]\{1,\}\).*/\1/p' |
              head -1)
fi
case "$BACKEND" in
"")
    echo "No network backend, and this test needs the guest ON THE LAN:" >&2
    echo "the host has no default route naming an interface to bridge onto." >&2
    echo "Name one:  AMINETXDUO_EMU_BACKEND=<host interface>" >&2
    exit 2
    ;;
slirp|slirp_inbound|none)
    echo "AMINETXDUO_EMU_BACKEND=$BACKEND is not a bridge, and this test" >&2
    echo "asserts what a machine on a LAN does: a DHCP lease from the" >&2
    echo "network's own server, a router that is a router, and a peer that" >&2
    echo "can reach the guest.  None of that exists behind slirp." >&2
    echo "Name a host interface instead:  AMINETXDUO_EMU_BACKEND=<interface>" >&2
    exit 2
    ;;
esac

# ONE MAC PER TAG, derived, not pinned.  A fixed address here put every run of
# this harness on the bridge under 52:54:00:c0:ff:ee, beside the demo instance
# and beside other checkouts' guests: two machines at two addresses sharing one
# hardware address, so a peer's neighbour cache cannot tell them apart and the
# arp step above reports whichever answered last.
#
# tools/emu-mac.sh is the one implementation of this, shared with
# tools/amiberry-run.sh rather than repeated here.  AMINETXDUO_EMU_MAC still
# wins, for a run that wants a reservation to hold.
# shellcheck source=../../tools/emu-mac.sh
. "$ROOT/tools/emu-mac.sh"
MAC="${AMINETXDUO_EMU_MAC:-$(emu_mac_for_tag "$TAG")}"

XDFTOOL="${AMINETXDUO_XDFTOOL:-}"
if [ -z "$XDFTOOL" ]; then
    for candidate in \
        "$(command -v xdftool || true)" \
        "$HOME/.venvs/amitools/bin/xdftool" \
        "$HOME/amigaos/tools/amitools/bin/xdftool"
    do
        [ -n "$candidate" ] && [ -x "$candidate" ] && { XDFTOOL="$candidate"; break; }
    done
fi

command -v lha >/dev/null 2>&1 || {
    echo "lha not found, needed to unpack the release archive on the host." >&2
    exit 2
}

ADFDIR="${AMINETXDUO_ADF_DIR:-$HOME/amigaos/adf}"

# WHERE THE OTHER MACHINE IS.
#
# Everything -H asserts about the running server is asserted from somewhere
# else, because "reachable" means reachable from another machine and because
# THE HOST RUNNING AMIBERRY IS NOT ANOTHER MACHINE: with uaenet_pcap on a
# shared NIC, a frame this host sends to the guest's MAC does not come back
# round to that NIC's pcap, so every connection from here is refused and the
# refusal says nothing about the server.  Measured on playhouse3: from the
# emulating host, connection refused every time; from a third machine on the
# same LAN, HTTP/1.1 200 with the right Content-Length.
#
# An ssh target, and the drills are copied to it.  Without one the -H stage
# reports infrastructure and fails; it never passes for want of a peer.
PEER="${AMINETXDUO_PEER:-}"

echo "==> model $MODEL on $(basename "$KICKSTART")"
echo "==> card $BOARD, driver $DRIVER_NAME from $DRIVER_PATH"

# ------------------------------------------------------ Workbench 3.1 SYS: --
#
# Five floppies make one hard drive.  The layout is Commodore's own: the
# Workbench disk IS the root, and the other four are the drawers the HD
# installer would have made for them.  Extras3.1 goes in Extras/ rather than
# being merged into the root, which is what the stock Startup-Sequence expects
# when it says `Assign L: Extras3.1:L DEFER`.
#
# Cached, because unpacking five ADFs on every run is a minute of nothing.
# The stamp is outside the tree so it cannot end up on the emulated disk.

WB="$ROOT/build/wb31-sys"
WBSTAMP="$ROOT/build/.wb31-sys.stamp"

wb_adfs=()
for disk in workbench fonts locale storage extras; do
    wb_adfs+=("$ADFDIR/amiga-wb31_$disk.adf")
done

wb_stale=0
[ -d "$WB" ] && [ -f "$WBSTAMP" ] || wb_stale=1
for adf in "${wb_adfs[@]}"; do
    [ -f "$adf" ] || {
        echo "missing $adf" >&2
        echo "Set AMINETXDUO_ADF_DIR to the directory holding the WB 3.1 set." >&2
        exit 2
    }
    [ "$adf" -nt "$WBSTAMP" ] && wb_stale=1
done

# amitools writes the volume's contents into the directory it is given, but
# older versions make a subdirectory named after the volume; cope with both
# rather than depending on which one is installed.
unpack_adf() {
    local adf="$1" into="$2" marker="$3" inner
    rm -rf "$into"
    mkdir -p "$into"
    "$XDFTOOL" "$adf" unpack "$into" >/dev/null
    [ -e "$into/$marker" ] && return 0
    inner=$(find "$into" -maxdepth 1 -mindepth 1 -type d | head -1)
    [ -n "$inner" ] && [ -e "$inner/$marker" ] || {
        echo "!! $(basename "$adf") did not unpack to something holding $marker" >&2
        exit 2
    }
    mv "$inner"/* "$into"/ 2>/dev/null || true
    mv "$inner"/.[!.]* "$into"/ 2>/dev/null || true
    rmdir "$inner" 2>/dev/null || true
}

if [ "$wb_stale" = "1" ]; then
    [ -n "$XDFTOOL" ] && [ -x "$XDFTOOL" ] || {
        echo "amitools' xdftool not found, needed to unpack the Workbench ADFs." >&2
        echo "  pip install amitools, or set AMINETXDUO_XDFTOOL=<path>" >&2
        exit 2
    }
    echo "==> assembling Workbench 3.1 into $WB (five ADFs)"
    SCRATCH="$ROOT/build/.wb31-unpack"
    rm -rf "$SCRATCH" "$WB"
    mkdir -p "$SCRATCH" "$WB"
    for pair in "workbench:S/Startup-Sequence" "fonts:topaz.font" \
                "locale:Catalogs" "storage:DOSDrivers" "extras:Tools"; do
        unpack_adf "$ADFDIR/amiga-wb31_${pair%%:*}.adf" \
                   "$SCRATCH/${pair%%:*}" "${pair##*:}"
    done
    cp -R "$SCRATCH/workbench/." "$WB/"
    for pair in "fonts:Fonts" "locale:Locale" "storage:Storage" "extras:Extras"; do
        src="${pair%%:*}"; dst="${pair##*:}"
        mkdir -p "$WB/$dst"
        cp -R "$SCRATCH/$src/." "$WB/$dst/"
    done
    rm -rf "$SCRATCH"

    # A directory hard drive takes its Amiga protection bits from the host's
    # mode bits, and xdftool unpacks everything 0644, which would leave
    # every command in C: without its E bit.  LhA on a real Amiga restores
    # the bits from the archive; here the host has to.
    chmod -R a+rx "$WB"

    : > "$WBSTAMP"
    for want in C/Assign C/LoadWB Libs/version.library S/Startup-Sequence \
                Fonts/topaz.font Locale/Catalogs Storage/DOSDrivers \
                Devs/system-configuration Extras/Tools; do
        [ -e "$WB/$want" ] || { echo "!! assembled SYS: has no $want" >&2; exit 2; }
    done
else
    echo "==> Workbench 3.1 tree is current ($WB)"
fi

# --------------------------------------------------------- the release .lha --
#
# THE ARTIFACT, not a directory that happens to hold the same files.  What a
# user has is an archive they downloaded, so that is what gets unpacked here.

if [ -z "$ARCHIVE" ]; then
    echo "==> building the distribution archive"
    AMINETXDUO_DIST_NO_MINIMAL=1 \
        "$ROOT/dist/make-dist.sh" -b "$BUILD" >"$ROOT/build/$TAG-make-dist.log" 2>&1 || {
        echo "dist/make-dist.sh failed, see build/$TAG-make-dist.log" >&2
        tail -20 "$ROOT/build/$TAG-make-dist.log" >&2
        exit 2
    }
    VERSION=$("$ROOT/tools/version.sh" --product)
    ARCHIVE="$ROOT/build/dist/AmiNetXDuo-$VERSION.lha"
fi
[ -f "$ARCHIVE" ] || { echo "no such archive: $ARCHIVE" >&2; exit 2; }
echo "==> archive $(basename "$ARCHIVE") ($(wc -c < "$ARCHIVE" | tr -d ' ') bytes)"

# ------------------------------------------------------------ the machine --

# -m68000, not -m68020: this runs INSIDE the guest, and the guest is whatever
# MODEL says.  Built for a 68020 it executed TST.L A0 on the A600 and took an
# illegal instruction before the Installer ever started, so the boot never
# finished, INSTALL_TIMEOUT fired, and the drive was left empty -- which reads
# as a clean pass to every "this file is absent" check below.  Nothing here
# needs 68020 codegen.
#
# $1 names the binary, $2 is how many times it runs the Installer, $3 is the
# label of the yes/no button to press instead of the first one (empty: press
# the first, which is every question's default), $4 names an askchoice option
# to select before Proceed as "<options>:<gadget id>" (empty: take that page's
# default).  See installdrive.c on why an option is named by number and not by
# text: it carries none.
build_driver() {
    local out="$1" runs="$2" label="$3" pick="${4:-}"
    local opts=0 gid=0
    if [ -n "$pick" ]; then
        opts=${pick%%:*}
        gid=${pick##*:}
    fi
    "$GCC" -O2 -m68000 -Wall -Wextra -DDRIVE_LEVEL="\"$LEVEL\"" \
           -DDRIVE_RUNS="$runs" -DDRIVE_YES_LABEL="\"$label\"" \
           -DDRIVE_PICK_OPTIONS="$opts" -DDRIVE_PICK_ID="$gid" -I"$NDK" \
           -o "$out" "$ROOT/install/test/installdrive.c" || exit 2
}

# -H installs TWICE in the first boot, both times saying yes to the terminal.
# One run proves the lines get written; two prove the block is rewritten and
# not appended to, which is the property a user with an existing S:User-Startup
# is relying on.
YES_LABEL=""
DRIVE_RUNS=1
if [ "$TERMINAL" = "1" ]; then
    YES_LABEL="Yes, serve them"
    DRIVE_RUNS=2
fi

echo "==> building installdrive ($LEVEL, $DRIVE_RUNS run(s)${YES_LABEL:+, \"$YES_LABEL\"}${PICK:+, picking \"$PICK\"})"
DRIVER="$ROOT/build/installdrive-$TAG-$LEVEL"
build_driver "$DRIVER" "$DRIVE_RUNS" "$YES_LABEL" "$PICK_SPEC"

rm -rf "$HD"
mkdir -p "$HD"
cp -R "$WB/." "$HD/"
# THE DRIVER, WHERE A USER WOULD HAVE PUT IT, and before the Installer runs.
#
# This is the only lever this harness has on the card question.  The script's
# detection loop looks for a driver it knows in DEVS: and DEVS:Networks and
# makes it the card page's default; installdrive.c clicks Proceed, which takes
# that default.  So a driver whose file name the script recognises is selected,
# and one whose name it does not know is not -- measured after the install
# below rather than assumed here.
#
# tools/sana2-stage.sh decides where it goes: DEVS: for the A2065, which is
# where Commodore's own tests put it, and DEVS:Networks for a third-party
# driver, which is where one is really installed.
sana2_stage_driver "$BOARD" "$HD/Devs"
STAGED_AT="$HD/Devs${SANA2_DIR:+/$SANA2_DIR}/$SANA2_DRIVER"
[ -f "$STAGED_AT" ] || {
    echo "!! $SANA2_DRIVER was not staged onto the test drive" >&2
    exit 2
}
cp "$DRIVER" "$HD/C/installdrive"
chmod 755 "$STAGED_AT" "$HD/C/installdrive"

# SOMEBODY ELSE'S S:User-Startup, written before the installer ever runs.
#
# The installer's stated contract is that it replaces its own marked block and
# leaves every other application's lines alone, and that is the thing most
# worth not breaking: this file is where every Amiga application on the machine
# has put its line.  So the file exists, with lines in it that are nothing to
# do with us, and each phase below checks they are all still there, in order,
# byte for byte.
FOREIGN_LINES=(
    "; -- SomeOtherApp 2.4 --"
    "Assign OTHERAPP: DH0:OtherApp"
    "C:SetPatch QUIET"
    "; -- end SomeOtherApp --"
)
mkdir -p "$HD/S"
printf '%s\n' "${FOREIGN_LINES[@]}" > "$HD/S/User-Startup"
chmod 644 "$HD/S/User-Startup"

# AmigaDOS does not care about case and this host does, so a file the guest
# wrote as `s/user-startup` is not `S/User-Startup` to `[ -f ]`.  Every name
# below is resolved the way the guest would, one component at a time; without
# this the installer looks like it skipped the one file it did write.
amiga_path() {
    local cur="$HD" part next
    local -a parts
    IFS=/ read -ra parts <<< "$1"
    for part in "${parts[@]}"; do
        next=$(ls -1 "$cur" 2>/dev/null |
               awk -v p="$part" 'tolower($0) == tolower(p) { print; exit }')
        [ -n "$next" ] || return 1
        cur="$cur/$next"
    done
    printf '%s\n' "$cur"
}

user_startup() { amiga_path S/User-Startup 2>/dev/null || true; }

foreign_intact() {
    local f line
    f=$(user_startup)
    [ -n "$f" ] && [ -f "$f" ] || return 1
    for line in "${FOREIGN_LINES[@]}"; do
        grep -Fqx -- "$line" "$f" || return 1
    done
    return 0
}

# How many times a pattern appears in S:User-Startup.  Duplicates are the
# failure mode a second install has, so this counts rather than tests.
startup_count() {
    local f n
    f=$(user_startup)
    [ -n "$f" ] && [ -f "$f" ] || { echo 0; return; }
    # grep -c prints 0 AND exits 1 when it matches nothing, so `|| echo 0`
    # would print it twice and every arithmetic test downstream reads "0\n0".
    n=$(grep -c -- "$1" "$f" 2>/dev/null) || n=0
    echo "${n:-0}"
}

# The download, where a download would be: its own drawer, not the one the
# installer is going to create.
mkdir -p "$HD/Unpacked"
( cd "$HD/Unpacked" && lha -xfq "$ARCHIVE" ) >/dev/null 2>&1 || \
( cd "$HD/Unpacked" && lha xf "$ARCHIVE" ) >/dev/null 2>&1 || {
    echo "could not unpack $ARCHIVE" >&2; exit 2; }
[ -d "$HD/Unpacked/AmiNetXDuo" ] || {
    echo "the archive did not unpack to an AmiNetXDuo drawer" >&2
    ls -la "$HD/Unpacked" >&2
    exit 2
}
cp "$INSTALLER" "$HD/Unpacked/AmiNetXDuo/Installer"
chmod -R a+rx "$HD/Unpacked"

# LhA 2.15, from the asset store, and NOT from this repository: it is
# third-party and licensed, so it is staged onto the test drive here and goes
# into nothing dist/make-dist.sh packs.  Workbench 3.1 has no archiver, and
# "a PC puts an archive on the Amiga and the Amiga unpacks it" is the path
# this product is mostly for, so the -H run below needs one.  The package
# carries a build per CPU the same way we do and lha_68020 traps on a 68000,
# so the model picks -- the rule is tools/demo.sh's, not a second one.
LHADIR="${AMINETXDUO_DEMO_LHA:-$HOME/amiga-assets/apps/lha-2.15}"
case "$MODEL" in
    A4000*) LHABIN=lha_68040 ;;
    A500*|A600*|A1000*|A2000*) LHABIN=lha_68k ;;
    *) LHABIN=lha_68020 ;;
esac
HAVE_LHA=0
if [ -f "$LHADIR/$LHABIN" ]; then
    cp -f "$LHADIR/$LHABIN" "$HD/C/lha"
    chmod 755 "$HD/C/lha"
    HAVE_LHA=1
    echo "==> lha staged, $LHABIN for $MODEL"
else
    echo "==> no lha at $LHADIR/$LHABIN" >&2
fi

# WHAT IS IN THE ARCHIVE, said out loud BEFORE anything is installed.  The
# https: check below cannot pass if the archive has no tls.library, and an
# archive built from a tree configured without TLS is an ordinary thing to
# have lying around, so the difference between "the product is broken" and
# "you packed a build that never had it" is stated here rather than left for
# somebody to work out from a return code.
echo "==> the archive holds:"
for f in Libs/bsdsocket.library Libs/usergroup.library \
         Libs/tls.library Devs/Internet/certificates \
         C/fetch C/ssh; do
    if [ -f "$HD/Unpacked/AmiNetXDuo/$f" ]; then
        printf '      %-36s %s bytes\n' "$f" \
               "$(wc -c < "$HD/Unpacked/AmiNetXDuo/$f" | tr -d ' ')"
    else
        printf '      %-36s ABSENT\n' "$f"
    fi
done
if [ ! -f "$HD/Unpacked/AmiNetXDuo/Libs/tls.library" ]; then
    echo "!! This archive has NO tls.library, so the https: check cannot pass"
    echo "!! and its failure will say nothing about the product.  Build the"
    echo "!! archive from a tree configured with -DAMINETXDUO_TLS=ON."
fi

# ------------------------------------------------------------- the emulator --
#
# tools/amiberry-run.sh cannot drive these runs: it wipes the staging drive and
# writes its own Startup-Sequence, and the whole point here is a machine that
# boots Commodore's.  So the emulator is started directly, with the same
# config that harness generates, and behind a lock of its own -- the only one
# left in the tree -- so a run here does not share the host with anything else.

LOCKDIR="$ROOT/build/.emu.lock"
SLOTDIR="$ROOT/build/.emu.slots"
PERFWAIT="$ROOT/build/.emu.perfwait"
LOCK_HELD=0

slots_busy() { ls -d "$SLOTDIR"/*/ 2>/dev/null | wc -l | tr -d ' '; }

reap_stale() {
    local d owner
    for d in "$LOCKDIR" "$PERFWAIT" "$SLOTDIR"/*; do
        [ -d "$d" ] || continue
        owner=$(cat "$d/pid" 2>/dev/null || echo "")
        if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then
            rm -rf "$d" 2>/dev/null || true
        fi
    done
}

take_lock() {
    [ "${AMINETXDUO_NO_LOCK:-0}" = "1" ] && return 0
    local waited=0 limit="${AMINETXDUO_LOCK_WAIT:-2400}"
    mkdir -p "$SLOTDIR"
    while ! mkdir "$PERFWAIT" 2>/dev/null; do
        reap_stale
        [ "$waited" -ge "$limit" ] && break
        [ "$waited" = 0 ] && echo "==> another exclusive run is queued; waiting"
        sleep 5; waited=$((waited + 5))
    done
    echo $$ > "$PERFWAIT/pid" 2>/dev/null || true
    while [ "$(slots_busy)" -gt 0 ] || ! mkdir "$LOCKDIR" 2>/dev/null; do
        reap_stale
        if [ "$waited" -ge "$limit" ]; then
            mkdir -p "$LOCKDIR"; break
        fi
        [ "$waited" = 0 ] && echo "==> waiting for the emulator to go quiet"
        sleep 5; waited=$((waited + 5))
    done
    echo $$ > "$LOCKDIR/pid" 2>/dev/null || true
    LOCK_HELD=1
}

release_lock() {
    [ "$LOCK_HELD" = "1" ] || return 0
    rm -rf "$LOCKDIR" "$PERFWAIT" 2>/dev/null || true
    LOCK_HELD=0
}

EMU_PID=""
SERIAL_PID=""
LOGCAP_PID=""
LOGPIPE=""
cleanup() {
    if [ -n "$EMU_PID" ]; then
        kill -TERM "$EMU_PID" 2>/dev/null || true
        sleep 1
        kill -KILL "$EMU_PID" 2>/dev/null || true
        EMU_PID=""
    fi
    [ -n "$SERIAL_PID" ] && kill -TERM "$SERIAL_PID" 2>/dev/null || true
    SERIAL_PID=""
    release_lock
}
trap cleanup EXIT INT TERM HUP

# The headless switch.  Amiberry links SDL2 with no driver of its own, so
# without this it asks for a video device, finds a stale DISPLAY from an ssh
# X11 forward that failed, and aborts in about a second -- which reads as a
# guest that never booted.  DISPLAY is cleared for the same reason: a stale one
# is worse than none.  tools/amiberry-run.sh does exactly this.
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
[ "${SDL_VIDEODRIVER}" = "dummy" ] && unset DISPLAY WAYLAND_DISPLAY || true

# One boot of the machine as it stands.  $1 names the run, $2 is the timeout,
# $3 is "net" to attach the A2065 to the bridge.  Returns the guest's own exit
# status out of DH0:.done, or 124.
#
# EVERY ARTEFACT CARRIES $TAG.  These names held the literal string "wb31"
# while AMINETXDUO_RUN_TAG renamed only the test drive, so two arms in one
# checkout -- the default install and the minimal one, say -- wrote over each
# other's serial log, emulator log and config, and took the same serial port
# besides.  That is how a diagnosis gets read off the wrong run, which is the
# one failure mode a harness must not have.
BOOT_STATUS=0
boot() {
    local name="$1" timeout="$2" net="${3:-}"
    local cfg="$ROOT/build/$TAG-$name.uae"
    local serial="$ROOT/build/serial-$TAG-$name.log"
    local elapsed=0
    local port

    # One listening port per run name, so two runs never collide.  Same
    # hashing as tools/amiberry-run.sh.
    port=$((12000 + $(printf '%s' "$TAG-$name" | cksum | cut -d' ' -f1) % 900))

    : > "$serial"
    rm -f "$HD/.done"

    cat > "$cfg" <<EOF
config_description=AmiNetXDuo $TAG $name
use_gui=no
headless=true
quickstart=$MODEL,0
kickstart_rom_file=$KICKSTART
fastmem_size=8
floppy0type=-1
nr_floppies=0
uaehf0=dir,rw,DH0:DH0:$HD,0
serial_port=tcp://127.0.0.1:$port/wait
EOF
    if [ "$net" = "net" ]; then
        # tools/emu-board.sh, shared with tools/amiberry-run.sh.  These keys
        # lived here as two literal a2065 lines, which is what made this gate
        # a one-card gate.
        emu_board_lines "$BOARD" "$MAC" "$BACKEND" >> "$cfg" || exit 2
    fi

    echo "==> booting ($name, $BOARD, timeout ${timeout}s, network $([ "$net" = net ] && echo "$BACKEND" || echo off))"
    # SIGPIPE ignored for the reason tools/amiberry-run.sh ignores it: SLIRP
    # writes guest payload to host sockets without MSG_NOSIGNAL, so a peer that
    # hangs up first otherwise kills the emulator and it looks like a guru.
    #
    # Through tools/logcap.sh for the reason tools/amiberry-run.sh does it:
    # --log is unbounded, ~3.3 GB an hour, and it has filled playhouse3 three
    # times.  It matters MORE here than anywhere -- this is the longest thing
    # in the tree, boot() is called for three installs and their power cycles,
    # each writes its own log, and every one of them is kept.  Nothing reads
    # these; they are artifacts, so the cap costs nothing but disk that was
    # never wanted.  Degrades to the plain redirect if the capper is missing,
    # because opening a FIFO for writing blocks until a reader appears.
    local uaelog="$ROOT/build/amiberry-$TAG-$name.log"
    LOGPIPE="$ROOT/build/amiberry-$TAG-$name.logpipe"
    rm -f "$LOGPIPE"
    if [ -x "$ROOT/tools/logcap.sh" ] && mkfifo "$LOGPIPE" 2>/dev/null; then
        "$ROOT/tools/logcap.sh" < "$LOGPIPE" > "$uaelog" &
        LOGCAP_PID=$!
    else
        echo "!! no tools/logcap.sh; $uaelog is UNCAPPED for this boot" >&2
        rm -f "$LOGPIPE"
        LOGPIPE=""
    fi
    if [ -n "$LOGPIPE" ]; then
        ( trap '' PIPE; exec "$AMIBERRY" --log -f "$cfg" ) >"$LOGPIPE" 2>&1 &
    else
        ( trap '' PIPE; exec "$AMIBERRY" --log -f "$cfg" ) >"$uaelog" 2>&1 &
    fi
    EMU_PID=$!

    # serial_port=.../wait blocks the emulator until something connects, so
    # retry until it is listening or the emulator is gone.
    (
        for _ in $(seq 1 60); do
            kill -0 "$EMU_PID" 2>/dev/null || exit 0
            nc 127.0.0.1 "$port" >> "$serial" 2>/dev/null && exit 0
            sleep 0.5
        done
    ) &
    SERIAL_PID=$!

    BOOT_STATUS=124
    while [ "$elapsed" -lt "$timeout" ]; do
        if [ -f "$HD/.done" ]; then
            BOOT_STATUS=$(tr -dc '0-9' < "$HD/.done" | head -c 4)
            BOOT_STATUS=${BOOT_STATUS:-0}
            break
        fi
        kill -0 "$EMU_PID" 2>/dev/null || {
            echo "!! amiberry exited early after ${elapsed}s" >&2
            break
        }
        sleep 1
        elapsed=$((elapsed + 1))
    done

    kill -TERM "$EMU_PID" 2>/dev/null || true
    wait "$EMU_PID" 2>/dev/null || true
    EMU_PID=""
    kill -TERM "$SERIAL_PID" 2>/dev/null || true
    SERIAL_PID=""
    # The capper still holds the tail of this boot in its ring.  boot() is
    # called again straight after, so it has to be finished with before the
    # next one takes the same names.
    if [ -n "$LOGCAP_PID" ]; then
        wait "$LOGCAP_PID" 2>/dev/null || true
        LOGCAP_PID=""
    fi
    [ -z "$LOGPIPE" ] || { rm -f "$LOGPIPE"; LOGPIPE=""; }

    echo "    ($name finished after ${elapsed}s, status $BOOT_STATUS)"
    if [ -s "$serial" ]; then
        echo "---- serial ----"
        tail -40 "$serial"
    fi
}

# The stock 3.1 Startup-Sequence, with the tail replaced.  LoadWB stays --
# the Installer draws on the Workbench screen and a user has one, and only
# `EndCLI`, which would take the boot shell away before our line runs, goes.
#
# $2 = "nouserstartup" also drops `Execute S:User-Startup`, which is what a
# boot into the Installer has to be once the machine has already been
# installed once.  A running stack holds LIBS:bsdsocket.library open and the
# copy then fails -- the script says so itself, in @special-msg: "the network
# is already running.  Reboot and run this installer again before doing
# anything else."  This is that reboot.
STARTUP_SUM=""
startup_with() {
    local tail_cmds="$1" mode="${2:-}"
    if [ "$mode" = "nouserstartup" ]; then
        sed -e '/^EndCLI/d' -e '/Execute S:User-Startup/d' \
            "$WB/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
    else
        sed -e '/^EndCLI/d' "$WB/S/Startup-Sequence" > "$HD/S/Startup-Sequence"
    fi
    printf '\n%s\n' "$tail_cmds" >> "$HD/S/Startup-Sequence"
    chmod 755 "$HD/S/Startup-Sequence"
    STARTUP_SUM=$(shasum "$HD/S/Startup-Sequence" | cut -d' ' -f1)
}

take_lock

# ------------------------------------------------------------------ run 1 ---

echo
echo "============================================================"
echo "  1/2  Workbench 3.1 boots, then installs the archive ($LEVEL)"
echo "============================================================"

startup_with 'FailAt 9999
C:installdrive >DH0:install-console.txt
Echo >DH0:.done "$RC"'

boot install "$INSTALL_TIMEOUT"
INSTALL_STATUS=$BOOT_STATUS

echo
echo "---- installdrive report ----"
cat "$HD/installdrive.txt" 2>/dev/null || echo "(none)"
echo
echo "---- Installer log ----"
cat "$HD/install-log.txt" 2>/dev/null || echo "(none written)"

# ------------------------------------------- what a real Workbench now has --

echo
echo "============================================================"
echo "  what the installer put on a real Workbench"
echo "============================================================"

fail=0

check_file() {
    local real
    if real=$(amiga_path "$1") && [ -f "$real" ]; then
        printf '  ok      %s\n' "$1"
    else
        printf '  MISSING %s\n' "$1"
        fail=1
    fi
}

check_file Libs/bsdsocket.library
check_file Libs/usergroup.library

# WHICH OF THE TWO STACKS LANDED, by byte count against the archive's own two
# copies.  Without this a -p "Minimal" run whose click missed the page would
# install the full stack and pass every check below it, which is the vacuous
# pass this file exists not to produce -- and the harness could not reach that
# page at all until installdrive.c learned DRIVE_PICK_LABEL, so `minimal` had
# never been installed by any run.
STACK_INSTALLED=unknown
_stack_real=$(amiga_path Libs/bsdsocket.library || true)
if [ -n "$_stack_real" ] && [ -f "$_stack_real" ]; then
    _stack_bytes=$(wc -c < "$_stack_real" | tr -d ' ')
    _full_bytes=$(wc -c < "$HD/Unpacked/AmiNetXDuo/Libs/bsdsocket.library" \
                  2>/dev/null | tr -d ' ')
    _min_bytes=$(wc -c < "$HD/Unpacked/AmiNetXDuo/Libs/minimal/bsdsocket.library" \
                 2>/dev/null | tr -d ' ')
    [ "$_stack_bytes" = "${_full_bytes:-x}" ] && STACK_INSTALLED=full
    [ "$_stack_bytes" = "${_min_bytes:-x}" ] && STACK_INSTALLED=minimal
    echo "stack_installed=$STACK_INSTALLED bytes=$_stack_bytes"
else
    echo "stack_installed=none"
fi

case "$PICK" in
""|full) WANT_STACK=full ;;
minimal) WANT_STACK=minimal ;;
esac
if [ "$STACK_INSTALLED" != "$WANT_STACK" ]; then
    echo "!! asked for the $WANT_STACK stack and $STACK_INSTALLED was installed"
    fail=1
fi
for cmd in AddNetInterface Online Offline ShowNetStatus ping netstat host fetch; do
    check_file "C/$cmd"
done
check_file Devs/NetInterfaces/eth0
check_file Devs/Internet/name_resolution
check_file S/User-Startup

# tls.library and the trust store are what https: needs, and their absence is
# the first thing to know if the https: check fails.
for f in Libs/tls.library Devs/Internet/certificates; do
    real=$(amiga_path "$f" || true)
    if [ -n "$real" ] && [ -f "$real" ]; then
        printf '  ok      %-32s %s bytes\n' "$f" "$(wc -c < "$real" | tr -d ' ')"
    else
        printf '  ABSENT  %s\n' "$f"
    fi
done

echo
echo "---- S:User-Startup ----"
cat "$(amiga_path S/User-Startup 2>/dev/null)" 2>/dev/null || echo "(none)"
echo "---- DEVS:NetInterfaces/eth0 ----"
cat "$(amiga_path Devs/NetInterfaces/eth0 2>/dev/null)" 2>/dev/null || echo "(none)"

# ------------------------------------- did the INSTALLER pick the card? ------
#
# THE MEASUREMENT, not a workaround.  The card page's default is whichever
# driver the script's detection loop found in DEVS: or DEVS:Networks, and
# installdrive.c takes the default of every page, so a driver the script
# recognises IS selected and one it does not is not.  Which of the two happened
# decides what this run is allowed to claim, so it is read off the file the
# installer wrote rather than inferred.
#
# When the installer got it wrong the interface file is rewritten here, before
# the power cycle, and the run goes on -- per-card coverage of the stack is
# worth having and is not the same claim.  card_config says which it is and
# the two are never one line.
INSTALLER_DEVICE=""
IFACE_FILE=$(amiga_path Devs/NetInterfaces/eth0 2>/dev/null || true)
if [ -n "$IFACE_FILE" ] && [ -f "$IFACE_FILE" ]; then
    INSTALLER_DEVICE=$(sed -n 's/^DEVICE=//p' "$IFACE_FILE" | head -1 |
                       tr -d '\r' | sed 's/[[:space:]]*$//')
fi

CARD_SELECTED=no
case "$INSTALLER_DEVICE" in
    "$SANA2_DRIVER"|"DEVS:Networks/$SANA2_DRIVER") CARD_SELECTED=yes ;;
esac

# THE FILE NAMES THE SCRIPT LOOKS FOR, READ OUT OF THE SCRIPT THAT RAN.  A
# driver on that list that was NOT selected is a regression and reddens this
# run; a driver that is not on it could not have been selected, which is a
# defect in the installer and not in this boot, so it is reported and the run
# carries on.
#
# NOT A COPY OF THE LIST.  It was one, and a copy of a list is a list that goes
# stale the day the original is corrected -- which is the same shape as the
# defect this measures.  It is taken from the unpacked archive rather than from
# the tree, so what is asserted about is the script a user runs.
INSTALLER_KNOWN_DRIVERS=$(awk '
    /\(set DET_INDEX/     { on = 1; next }
    /\(set CARD_DEFAULT/  { exit }
    on {
        line = $0
        while (match(line, /"[A-Za-z0-9_.-]+\.device"/)) {
            print substr(line, RSTART + 1, RLENGTH - 2)
            line = substr(line, RSTART + RLENGTH)
        }
    }
' "$HD/Unpacked/AmiNetXDuo/Install-AmiNetXDuo" 2>/dev/null)

INSTALLER_KNOWS_DRIVER=no
for _known in $INSTALLER_KNOWN_DRIVERS; do
    [ "$_known" = "$SANA2_DRIVER" ] && { INSTALLER_KNOWS_DRIVER=yes; break; }
done

# An empty list is not "the installer knows nothing", it is this awk having
# lost the shape it reads, and it would turn every card green-by-omission.
if [ -z "$INSTALLER_KNOWN_DRIVERS" ]; then
    echo "!! could not read the detection list out of the archive's"
    echo "   Install-AmiNetXDuo, so installer_knows_driver means nothing here."
    fail=1
fi
echo "installer_detects=$(printf '%s' "$INSTALLER_KNOWN_DRIVERS" | tr '\n' ',')"

CARD_CONFIG=installer
if [ "$CARD_SELECTED" = "no" ] && [ "$INSTALLER_KNOWS_DRIVER" = "yes" ]; then
    echo
    echo "!! $SANA2_DRIVER IS one of the eight names Install-AmiNetXDuo:524-533"
    echo "   detects, and the installer still wrote"
    echo "   DEVICE=${INSTALLER_DEVICE:-nothing}.  Detection is broken, or the"
    echo "   driver was not staged where the loop looks."
    fail=1
fi
if [ "$CARD_SELECTED" = "no" ]; then
    CARD_CONFIG=post-install
    echo
    echo "!! THE INSTALLER DID NOT SELECT THIS CARD."
    echo "   asked for $BOARD, whose driver is $SANA2_DRIVER, and the"
    echo "   installer wrote DEVICE=${INSTALLER_DEVICE:-nothing}."
    echo "   Install-AmiNetXDuo:521-542 scans DEVS: and DEVS:Networks for a"
    echo "   driver on a list of eight file names and makes the first hit the"
    echo "   card page's default; installdrive.c cannot answer an askchoice"
    echo "   any other way.  A driver whose file name is not on that list"
    echo "   leaves CARD_DEFAULT at 0, which is the A2065."
    echo "   The interface file is rewritten now, so what follows measures"
    echo "   whether THE STACK drives $BOARD -- not whether the installer"
    echo "   can select it, which this run has just shown it cannot."
    if [ -f "$HD/Devs/NetInterfaces/eth0" ]; then
        sana2_stage_interface "$BOARD" "$HD/Devs"
        echo "---- DEVS:NetInterfaces/eth0, after the rewrite ----"
        cat "$HD/Devs/NetInterfaces/eth0"
    else
        echo "!! there is no $HD/Devs/NetInterfaces/eth0 to rewrite"
        fail=1
    fi
fi

echo
echo "card_board=$BOARD"
echo "card_driver=$SANA2_DRIVER"
echo "card_model=$MODEL"
echo "installer_device=${INSTALLER_DEVICE:-none}"
echo "installer_knows_driver=$INSTALLER_KNOWS_DRIVER"
echo "installer_card_selected=$CARD_SELECTED"
echo "card_config=$CARD_CONFIG"

# Machine-readable, one key per line, so nothing downstream has to read prose.
TERM_LINES=0
TERM_ASSIGNS=0
FOREIGN=no
foreign_intact && FOREIGN=yes
TERM_LINES=$(startup_count 'httpd')
TERM_ASSIGNS=$(startup_count 'Assign AmiNetXDuo:')

echo
echo "startup_foreign_lines_intact=$FOREIGN"
echo "startup_httpd_lines=$TERM_LINES"
echo "startup_assign_lines=$TERM_ASSIGNS"
echo "startup_installer_runs=$DRIVE_RUNS"

if [ "$FOREIGN" != "yes" ]; then
    echo
    echo "!! S:User-Startup lost lines that were not ours.  The installer's"
    echo "   contract is that it touches only its own marked block."
    fail=1
fi

if [ "$TERMINAL" = "1" ]; then
    # Two installs, both answering yes: exactly one of each line, or the block
    # was appended to rather than replaced.
    if [ "$TERM_LINES" != "1" ] || [ "$TERM_ASSIGNS" != "1" ]; then
        echo
        echo "!! after $DRIVE_RUNS installs answering yes, S:User-Startup has"
        echo "   $TERM_LINES httpd line(s) and $TERM_ASSIGNS assign line(s); one of each"
        echo "   is the only right answer."
        fail=1
    fi
elif [ "$TERM_LINES" != "0" ]; then
    echo
    echo "!! nobody asked for httpd and S:User-Startup has $TERM_LINES httpd line(s)"
    fail=1
fi

if [ "$INSTALL_STATUS" != "0" ] || [ "$fail" != "0" ]; then
    echo
    echo "!! the install run did not complete cleanly (status $INSTALL_STATUS)"
    echo "   the drive is left at $HD"

    # THE DIAGNOSIS, NOT A HINT.  At NOVICE the script aborts outright when its
    # detection loop found nothing and there is no configuration to keep --
    # Install-AmiNetXDuo:633-641, "The installer found no network card driver
    # in DEVS: or DEVS:Networks" -- and the loop only knows eight file names.
    # A machine with this card and this driver therefore cannot install the
    # archive at the default user level at all, and that reads from the log as
    # an install that failed rather than as a script that refused.
    if [ "$LEVEL" = "NOVICE" ] && [ "$INSTALLER_KNOWS_DRIVER" = "no" ]; then
        echo
        echo "   WHY: $SANA2_DRIVER is not one of the eight file names"
        echo "   Install-AmiNetXDuo:524-533 detects, so DET_INDEX stayed -1,"
        echo "   and with no existing configuration to keep the abort at"
        echo "   Install-AmiNetXDuo:633-641 fires before anything is written."
        echo "   At NOVICE there is no card page to answer instead.  A user"
        echo "   with a $BOARD cannot install this archive at the default"
        echo "   user level.  -l AVERAGE draws the page; it still defaults to"
        echo "   the A2065, which is the other half of the same defect."
    fi
    echo
    echo "workbench_e2e=FAIL board=$BOARD model=$MODEL driver=$SANA2_DRIVER" \
         "card_config=$CARD_CONFIG installer_card_selected=$CARD_SELECTED" \
         "stack=$STACK_INSTALLED boot_status=install-$INSTALL_STATUS"
    exit 1
fi

# ------------------------------------------------------------------ run 2 ---
#
# A POWER CYCLE, not a continuation.  The Startup-Sequence is put back to
# Commodore's, the installer's work has to be reached through its own
# `Execute S:User-Startup`, exactly as on the user's machine, and the only
# thing added is the line that runs the checks afterwards.

echo
echo "============================================================"
echo "  2/2  rebooting, and using the machine as its owner would"
echo "============================================================"

if [ "$(shasum "$HD/S/Startup-Sequence" | cut -d' ' -f1)" != "$STARTUP_SUM" ]; then
    echo "note: the installer also changed S:Startup-Sequence, diff against"
    echo "      the stock 3.1 one is worth reading before the reboot"
fi

# -H adds a fifth step, and it is the one the machine is for: wait for the
# other machine to have finished putting files here over WebDAV, unpack the
# archive it put, and copy what arrived out of RAM: onto DH0: so the host can
# compare the bytes against what it sent.
#
# Waited rather than handshaken.  DH0: is a directory on the host, so a
# handshake file would work, but AmigaDOS's Lab/Skip loop is a worse thing to
# get wrong than a fixed window is to spend, and the window is bounded by
# BOOT_TIMEOUT either way.  The peer writes its results long before it ends.
CHECK_TAIL=""
if [ "$TERMINAL" = "1" ]; then
    CHECK_TAIL=$(cat <<'EOF'

Echo >>DH0:usercheck.txt "*N=== 5. what the other machine put here over WebDAV"
C:Wait 240
C:List RAM: >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT davlist rc=$RC"
; A DRAWER THAT DID NOT EXIST A MOMENT AGO.  LhA asks
; "already exists, overwrite? (Y/N/A/S/Q):" on a second
; extraction and waits for a keystroke there is nobody to
; type, and a wait with nothing to end it is a run that
; times out rather than a run that fails.
C:Delete RAM:Unpack ALL QUIET FORCE
C:MakeDir RAM:Unpack
C:lha -q x RAM:payload.lha RAM:Unpack/ >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT lha-x rc=$RC"
C:MakeDir DH0:DavOut
C:Copy RAM:payload.txt DH0:DavOut QUIET
Echo >>DH0:usercheck.txt "RESULT davcopy rc=$RC"
C:Copy RAM:Unpack DH0:DavOut/Unpack ALL QUIET
Echo >>DH0:usercheck.txt "RESULT unpackcopy rc=$RC"
EOF
)
fi

# The hosts the https step tries, in order, and the AmigaDOS that tries them.
# Nested IF rather than a loop, because the Shell has no loop and because the
# nesting is what makes the second host cost nothing on a run where the first
# one answered: 17 s, not 35.  `Set fhrc $RC` first, since the Echo that
# records the return code is itself a command and clears the condition flags.
HTTPS_HOSTS=(www.gnu.org www.amiga-news.de www.pouet.net)

https_steps() {
    local i=1 pad="" h

    for h in "${HTTPS_HOSTS[@]}"; do
        printf '%sC:fetch https://%s/ TO DH0:https-body.txt >>DH0:usercheck.txt\n' \
               "$pad" "$h"
        printf '%sSet fhrc $RC\n' "$pad"
        printf '%sEcho >>DH0:usercheck.txt "RESULT fetch-https-%d rc=$fhrc"\n' \
               "$pad" "$i"
        if [ "$i" -lt "${#HTTPS_HOSTS[@]}" ]; then
            printf '%sIF NOT "$fhrc" EQ "0"\n' "$pad"
            pad="$pad  "
        fi
        i=$((i + 1))
    done

    while [ -n "$pad" ]; do
        pad="${pad#  }"
        printf '%sENDIF\n' "$pad"
    done
}
HTTPS_STEPS=$(https_steps)

# An ordinary Shell script, doing ordinary things, with every command's return
# code written down beside its output.  `Stack 200000` is the Shell's internal
# stack command.  It is NOT needed any more, clients/compat/amiga_argv.c
# swaps in 256 KB of its own before main() runs, and the ReadMe says so, and
# it stays here precisely because a cautious user will still type it: a client
# that mishandled an already-large Shell stack would fail nowhere else.
cat > "$HD/S/AmiNetXDuo-Check" <<EOF
; Written by install/test/run-workbench.sh.  Nothing here is installed
; by AmiNetXDuo, it is what a user would type.
FailAt 9999
Stack 200000

Echo >DH0:usercheck.txt "=== 1. the network, as S:User-Startup brought it up"
C:Wait 5
C:ShowNetStatus >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT network rc=\$RC"

Echo >>DH0:usercheck.txt "*N=== 2. fetch http://example.com/"
C:fetch http://example.com/ TO DH0:http-body.txt >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT fetch-http rc=\$RC"

Echo >>DH0:usercheck.txt "*N=== 3. fetch https://, first host that answers"
$HTTPS_STEPS


Echo >>DH0:usercheck.txt "*N=== 4. arp, what answered on this network"
C:arp >>DH0:usercheck.txt
Echo >>DH0:usercheck.txt "RESULT arp rc=\$RC"
$CHECK_TAIL
Echo >>DH0:usercheck.txt "*N=== done"
EOF
chmod 755 "$HD/S/AmiNetXDuo-Check"

startup_with 'FailAt 9999
Execute S:AmiNetXDuo-Check >DH0:check-console.txt
Echo >DH0:.done "$RC"'

rm -f "$HD/usercheck.txt" "$HD/http-body.txt" "$HD/https-body.txt"

# ---- the terminal, from another machine, while the Amiga is still up -------
#
# Not from inside the guest.  "Reachable" means reachable from somewhere else,
# and this backend is a bridge, so the host is somewhere else with its own
# address on the same LAN.  The guest's address is not known in advance
# (DHCP), and it does not have to be: ShowNetStatus writes it to DH0: as step
# 1 of the check script, and DH0: is a directory on this host, so it can be
# read while the machine is still running.
#
# Runs beside the boot rather than after it: httpd is gone the moment the
# emulator is killed.
TERM_PROBE="$ROOT/build/$TAG-peer-drill.txt"
PROBE_PID=""
PAYLOAD_TXT="$ROOT/build/$TAG-payload.txt"
PAYLOAD_LHA="$ARCHIVE"

if [ "$TERMINAL" = "1" ]; then
    : > "$TERM_PROBE"
    printf 'put over WebDAV by %s at %s\n' "$(hostname)" "$(date -u +%FT%TZ)" \
        > "$PAYLOAD_TXT"

    # Under $HOME on the peer, not /tmp: /tmp there is a 2 GB tmpfs shared
    # with everything else on the box and it was 100% full the first time this
    # ran, which reads as "the peer is unreachable" and is not.
    PEERDIR=".aminetxduo-e2e"
    if [ -z "$PEER" ]; then
        echo "peer=none" >> "$TERM_PROBE"
    else
        if ssh -o BatchMode=yes "$PEER" "rm -rf $PEERDIR && mkdir -p $PEERDIR" &&
           scp -q -o BatchMode=yes \
                "$ROOT/tests/tools/httpd-drill.py" \
                "$ROOT/tests/tools/wsterm-console.py" \
                "$ROOT/install/test/peer-drill.sh" \
                "$PAYLOAD_TXT" "$PAYLOAD_LHA" "$PEER:$PEERDIR/"; then
            :
        else
            echo "peer=unreachable" >> "$TERM_PROBE"
            PEER=""
        fi
    fi

    if [ -n "$PEER" ]; then
        (
            guest=""
            for _ in $(seq 1 "$BOOT_TIMEOUT"); do
                sleep 1
                [ -f "$HD/usercheck.txt" ] || continue
                guest=$(sed -n 's/^  *address  *\([0-9][0-9.]*\).*/\1/p' \
                        "$HD/usercheck.txt" 2>/dev/null | head -1)
                [ -n "$guest" ] && break
            done
            if [ -z "$guest" ]; then
                echo "peer=no-guest-address" >> "$TERM_PROBE"
                exit 0
            fi
            echo "guest_address=$guest" >> "$TERM_PROBE"
            ssh -o BatchMode=yes "$PEER" \
                "sh $PEERDIR/peer-drill.sh $guest $PEERDIR/$(basename "$PAYLOAD_TXT") \
                 $PEERDIR/$(basename "$PAYLOAD_LHA") $PEERDIR" \
                >> "$TERM_PROBE" 2>&1
        ) &
        PROBE_PID=$!
    fi
fi

boot boot "$BOOT_TIMEOUT" net

if [ -n "$PROBE_PID" ]; then
    kill -TERM "$PROBE_PID" 2>/dev/null || true
    wait "$PROBE_PID" 2>/dev/null || true
fi
if [ "$TERMINAL" = "1" ]; then
    echo
    echo "---- what the other machine did to the Amiga ----"
    cat "$TERM_PROBE" 2>/dev/null || echo "(the peer wrote nothing)"
fi

echo
echo "---- what the machine did ----"
cat "$HD/usercheck.txt" 2>/dev/null || echo "(DH0:usercheck.txt was never written)"

if [ -s "$HD/check-console.txt" ]; then
    echo
    echo "---- anything the script itself said ----"
    cat "$HD/check-console.txt"
fi

for body in http-body.txt https-body.txt; do
    [ -s "$HD/$body" ] || continue
    echo
    echo "---- $body ($(wc -c < "$HD/$body" | tr -d ' ') bytes, first 5 lines) ----"
    head -5 "$HD/$body"
done

# ------------------------------------------------------------- the verdict --

echo
echo "============================================================"
echo "  a real installed Workbench 3.1, four things a user does"
echo "============================================================"

bad=0

# A minimal install carries no tls.library, so `fetch https://` there MUST
# fail: that is the product working, and scoring it as a failure makes the arm
# red for doing the right thing.  Keyed off what actually landed on the drive
# rather than off MODEL, which is what kept this correct when the processor
# stopped deciding it: every processor gets tls.library now, the 68000
# included, and leaving encryption out is a feature choice.
HAS_TLS=1
[ -f "$HD/Libs/tls.library" ] || HAS_TLS=0

report() {
    local label="$1" name="$2" rc
    rc=$(sed -n "s/^RESULT $name rc=\(.*\)/\1/p" "$HD/usercheck.txt" 2>/dev/null \
         | head -1)
    if [ -z "$rc" ]; then
        printf '  %-34s NEVER RAN\n' "$label"
        bad=1
    elif [ "$rc" = "0" ]; then
        printf '  %-34s rc=0\n' "$label"
    else
        printf '  %-34s rc=%s\n' "$label" "$rc"
        bad=1
    fi
}

# The return code of one https attempt, and the last thing fetch said before
# it, which since 279327c4 names the fault -- an alert, a record that would
# not decrypt, a read that ran out of time -- instead of one sentence for all
# three.  Printing it is the difference between a red run that says which host
# failed how and one that says the step failed.
https_attempt() {
    awk -v want="RESULT fetch-https-$1 rc=" '
        index($0, want) == 1 {
            print substr($0, length(want) + 1) "\t" err
            found = 1
            exit
        }
        /^RESULT fetch-https-/ { err = ""; next }
        /^fetch: / { err = $0 }
        END { if (!found) print "\t" }
    ' "$HD/usercheck.txt" 2>/dev/null
}

https_ok=0
https_via=""
https_tried=""
https_i=0
for https_host in "${HTTPS_HOSTS[@]}"; do
    https_i=$((https_i + 1))
    https_line=$(https_attempt "$https_i")
    https_rc=${https_line%%$'\t'*}
    https_err=${https_line#*$'\t'}

    if [ -z "$https_rc" ]; then
        # A host after the one that answered was never meant to run.
        if [ "$https_ok" = "0" ]; then
            https_tried="$https_tried$(printf '\n      %-22s NEVER RAN' \
                                       "$https_host")"
        fi
    elif [ "$https_rc" = "0" ]; then
        https_ok=1
        [ -n "$https_via" ] || https_via="$https_host"
    else
        https_tried="$https_tried$(printf '\n      %-22s rc=%s  %s' \
                                   "$https_host" "$https_rc" "$https_err")"
    fi
done

# fetch writes the output file only once it has a response, so on a run where
# every host failed there is none, and `wc -c <` on it fails the whole script
# under set -e at the exact moment the verdict is being printed.
https_bytes=0
if [ -f "$HD/https-body.txt" ]; then
    https_bytes=$(wc -c < "$HD/https-body.txt" | tr -d ' ')
fi

report "ShowNetStatus"                 network
report "fetch http://example.com/"     fetch-http

# An install with no tls.library on it, which is the minimal stack rather than
# any particular processor, has to fail against every host.
if [ "$HAS_TLS" = "0" ]; then
    if [ "$https_ok" = "1" ]; then
        printf '  %-34s rc=0 from %s  !! no tls.library here\n' \
               "fetch https://" "$https_via"
        bad=1
    else
        printf '  %-34s no host answered (expected: no tls.library)\n' \
               "fetch https://"
    fi
elif [ "$https_ok" = "1" ] && [ "${https_bytes:-0}" -gt 0 ]; then
    printf '  %-34s rc=0 from %s, %s bytes%s\n' "fetch https://" \
           "$https_via" "$https_bytes" "$https_tried"
else
    # Empty body with rc=0 is its own fault and not a refusal: the chain
    # verified and nothing arrived, which is what the 0.21.1 run hit.
    printf '  %-34s NO HOST ANSWERED, %s body bytes%s\n' "fetch https://" \
           "${https_bytes:-0}" "$https_tried"
    bad=1
fi

report "arp"                           arp

if [ "$TERMINAL" = "1" ]; then
    key() { sed -n "s/^$1=//p" "$TERM_PROBE" 2>/dev/null | head -1; }

    # No peer is INFRASTRUCTURE, and it gets its own exit status.  A run that
    # could not reach the machine from anywhere else has not tested the server
    # and must not read as one that did.
    case "$(key peer)" in
        none)
            echo
            echo "!! AMINETXDUO_PEER is not set, so nothing could talk to the"
            echo "   Amiga from another machine.  The host running amiberry"
            echo "   cannot: with uaenet_pcap on a shared NIC its own frames"
            echo "   do not reach the guest.  Set AMINETXDUO_PEER=<ssh target>."
            exit 3 ;;
        unreachable|no-guest-address)
            echo
            echo "!! the peer could not be prepared, or the Amiga never"
            echo "   reported an address: $(key peer)"
            exit 3 ;;
    esac

    # The terminal.  Content-Length is the assertion: it is the server saying
    # how big the page -T found, and the page the installer put on this drive
    # is the size to expect.
    want=$(wc -c < "$HD/AmiNetXDuo/Terminal/shell.html" 2>/dev/null | tr -d ' ')
    got_len=$(key terminal_content_length)
    if [ "$(key terminal_status)" = "200" ] && [ -n "$want" ] &&
       [ "$got_len" = "$want" ]; then
        printf '  %-34s 200, Content-Length %s\n' \
               "GET /shell, from the peer" "$got_len"
    else
        printf '  %-34s status=%s length=%s wanted=%s\n' \
               "GET /shell, from the peer" "$(key terminal_status)" \
               "${got_len:-none}" "${want:-?}"
        bad=1
    fi

    if [ "$(key dav_roundtrip_identical)" = "yes" ]; then
        printf '  %-34s bytes identical\n' "WebDAV PUT then GET"
    else
        printf '  %-34s put=%s get=%s identical=%s\n' "WebDAV PUT then GET" \
               "$(key dav_put_status)" "$(key dav_get_status)" \
               "$(key dav_roundtrip_identical)"
        bad=1
    fi

    # ON THE AMIGA'S DISK, not just back out of the server.  The guest copied
    # what arrived in the served drawer onto DH0:, which is a directory on this
    # host, so the bytes the peer sent are compared against the bytes an
    # AmigaDOS Copy wrote.
    if [ -f "$HD/DavOut/payload.txt" ] &&
       cmp -s "$PAYLOAD_TXT" "$HD/DavOut/payload.txt"; then
        printf '  %-34s bytes identical\n' "the file, on the Amiga's disk"
    else
        printf '  %-34s NOT THERE OR DIFFERENT\n' "the file, on the Amiga's disk"
        bad=1
    fi

    # lha x of the archive the peer put there.  Skipped LOUDLY, with its own
    # line, when the asset store has no lha: a step that did not run must not
    # be an absent line.
    if [ "$HAVE_LHA" = "0" ]; then
        printf '  %-34s NOT RUN, no %s in %s\n' \
               "lha x of the archive" "$LHABIN" "$LHADIR"
        bad=1
    else
        lha_rc=$(sed -n 's/^RESULT lha-x rc=//p' "$HD/usercheck.txt" 2>/dev/null \
                 | head -1)
        # Two files out of our own archive, compared against the copies this
        # host has: one text, one binary, both from drawers deep enough that a
        # truncated unpack cannot produce them by accident.
        unpack_ok=1
        for rel in AmiNetXDuo/Terminal/shell.html AmiNetXDuo/ReadMe; do
            here="$HD/Unpacked/$rel"
            there="$HD/DavOut/Unpack/$rel"
            [ -f "$here" ] || continue
            cmp -s "$here" "$there" || unpack_ok=0
        done
        if [ "${lha_rc:-x}" = "0" ] && [ "$unpack_ok" = "1" ]; then
            printf '  %-34s rc=0, extracted bytes identical\n' \
                   "lha x of the archive"
        else
            printf '  %-34s rc=%s extracted-identical=%s\n' \
                   "lha x of the archive" "${lha_rc:-NEVER RAN}" "$unpack_ok"
            bad=1
        fi
    fi

    for drill in httpd_drill wsterm_console; do
        rc=$(key "${drill}_rc")
        if [ "$rc" = "0" ]; then
            printf '  %-34s rc=0\n' "$drill, from the peer"
        else
            printf '  %-34s rc=%s\n' "$drill, from the peer" \
                   "${rc:-$(key "$drill")}"
            bad=1
        fi
    done
fi

# The one that shipped.  0.17.0 and 0.17.1 deadlocked in bsd_address_changed()
# the moment a DHCP lease arrived, so AddNetInterface never returned from its
# OpenLibrary(), S:User-Startup never finished, and the machine stopped part
# way through its Startup-Sequence.  BOOT_STATUS 124 is that exact shape: the
# guest wrote no DH0:.done because it never got to the end of the boot.
#
# Said separately from the four above because "the machine did not finish
# booting" and "a command returned nonzero" are different failures, and the
# table alone reads as four things that merely did not run.
USER_BOOT_STATUS=$BOOT_STATUS
if [ "$BOOT_STATUS" = "124" ]; then
    echo
    echo "  !! the machine did not finish booting: no DH0:.done after ${BOOT_TIMEOUT}s."
    echo "     S:User-Startup runs AddNetInterface, which opens bsdsocket.library,"
    echo "     which brings the stack up and waits for DHCP.  A hang here is that"
    echo "     path.  CONFIGURE=STATIC in DEVS:NetInterfaces/eth0 tells the lease"
    echo "     apart from the driver and the library open."
fi

# ------------------------------------------------------------------ run 3 ---
#
# THE OTHER DIRECTION.  An installer that adds a line and cannot take it away
# again is not idempotent, it is cumulative, and the only way to find out is to
# install a third time on this same machine and answer the question the other
# way.  Nothing is re-staged: this is the drive the user already has, with
# everything the first two runs put on it.

if [ "$TERMINAL" = "1" ]; then
    echo
    echo "============================================================"
    echo "  3/3  installing again, answering the terminal question no"
    echo "============================================================"

    build_driver "$ROOT/build/installdrive-$TAG-$LEVEL-no" 1 "" "$PICK_SPEC"
    cp "$ROOT/build/installdrive-$TAG-$LEVEL-no" "$HD/C/installdrive"
    chmod 755 "$HD/C/installdrive"

    startup_with 'FailAt 9999
C:installdrive >DH0:install-console.txt
Echo >DH0:.done "$RC"' nouserstartup

    boot uninstall "$INSTALL_TIMEOUT"
    if [ "$BOOT_STATUS" != "0" ]; then
        echo "!! the third install did not finish (status $BOOT_STATUS)"
        bad=1
    fi

    AFTER_HTTPD=$(startup_count 'httpd')
    AFTER_ASSIGN=$(startup_count 'Assign AmiNetXDuo:')
    AFTER_IFACE=$(startup_count 'AddNetInterface')
    AFTER_FOREIGN=no
    foreign_intact && AFTER_FOREIGN=yes

    echo
    echo "---- S:User-Startup after answering no ----"
    cat "$(user_startup)" 2>/dev/null || echo "(none)"
    echo
    echo "startup_httpd_lines=$AFTER_HTTPD"
    echo "startup_assign_lines=$AFTER_ASSIGN"
    echo "startup_addnetinterface_lines=$AFTER_IFACE"
    echo "startup_foreign_lines_intact=$AFTER_FOREIGN"

    if [ "$AFTER_HTTPD" != "0" ] || [ "$AFTER_ASSIGN" != "0" ]; then
        printf '  %-34s httpd=%s assign=%s\n' \
               "answering no removes both lines" "$AFTER_HTTPD" "$AFTER_ASSIGN"
        bad=1
    else
        printf '  %-34s both gone\n' "answering no removes both lines"
    fi
    if [ "$AFTER_IFACE" != "1" ]; then
        printf '  %-34s %s\n' \
               "AddNetInterface still there, once" "$AFTER_IFACE"
        bad=1
    else
        printf '  %-34s yes\n' "AddNetInterface still there, once"
    fi
    if [ "$AFTER_FOREIGN" != "yes" ]; then
        printf '  %-34s NO\n' "somebody else's lines intact"
        bad=1
    else
        printf '  %-34s yes\n' "somebody else's lines intact"
    fi
fi

# ONE LINE A SWEEP CAN READ.  Everything above it is for a person; this is for
# the caller, which must never have to grep prose to find out what happened.
# card_config is on it because a green run that configured the card after the
# install proves something different from one the installer configured, and a
# sweep that dropped that distinction would report nine installer passes when
# it had four.
WB_VERDICT=PASS
[ "$bad" = "0" ] && [ "$USER_BOOT_STATUS" != "124" ] || WB_VERDICT=FAIL
echo
echo "workbench_e2e=$WB_VERDICT board=$BOARD model=$MODEL driver=$SANA2_DRIVER" \
     "card_config=$CARD_CONFIG installer_card_selected=$CARD_SELECTED" \
     "stack=$STACK_INSTALLED boot_status=$USER_BOOT_STATUS"

if [ "$WB_VERDICT" = "PASS" ]; then
    echo "==> PASS: a real Workbench 3.1, installed from the archive, does all four"
    [ "$KEEP" = "1" ] && echo "    (the drive is at $HD)"
    exit 0
fi
echo "==> FAIL: see the table above; the drive is left at $HD"
echo "    Nothing here is adjusted to make it pass, the failure IS the result."
exit 1
