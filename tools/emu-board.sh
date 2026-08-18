#!/usr/bin/env bash
#
# One network board -> the emulator config lines that put it in the machine.
#
#   . tools/emu-board.sh
#   emu_board_lines "$BOARD" "$MAC" "$BACKEND" >> "$cfg"
#
# WHY IT IS SHARED.  tools/amiberry-run.sh had the only copy, and every harness
# that wanted a card had to go through that script -- which wipes the staging
# drive and writes its own Startup-Sequence.  install/test/run-workbench.sh
# cannot do that (it boots Commodore's own Startup-Sequence, which is the whole
# point of the release gate), so it wrote the config itself and hardcoded the
# A2065.  The result was a release gate that booted one card, for every release
# this project has cut.  A second copy of these keys would have the two drift;
# one function cannot.
#
# The keys are WinUAE's, because Amiberry parses WinUAE's config file.  See
# tests/tools/cards.sh for what each board is and which machine it needs, and
# tools/sana2-stage.sh for the driver that opens it.  This file only puts the
# card in the slot.
#
# THE MAC IS SET EXPLICITLY.  Left alone the emulator invents one, so a DHCP
# server hands out a different lease every run and nothing on the LAN can hold
# a reservation.  The A2065 keeps only the last three bytes -- a2065.cpp
# overwrites the first three with Commodore's 00:80:10 -- while the NE2000
# boards take the whole address.  tools/emu-mac.sh derives one per run tag.
#
# SPDX-License-Identifier: MIT

# Config lines for a board.  $1 board, $2 MAC, $3 host interface to bridge
# onto, $4 extra rom_options (optional, comma-joined onto the end).
#
# An unknown board is fatal: a run that quietly booted a machine with no card
# in it reads as a driver that cannot find its hardware, which is a diagnosis
# nobody can make from the artefacts.
emu_board_lines() { # board mac backend [extra-options]
    local board="$1" mac="$2" backend="$3" extra="${4:-}"

    case "$board" in
    "")
        : ;;
    a2065)
        printf 'a2065_rom_file=:ENABLED\na2065_rom_options=mac=%s,%s%s\n' \
               "$mac" "$backend" "${extra:+,$extra}" ;;
    ariadne|ariadne2|hydra|eb920|xsurf|xsurf100z2|xsurf100z3)
        printf '%s_rom_file=:ENABLED\n%s_rom_options=mac=%s,%s%s\n' \
               "$board" "$board" "$mac" "$backend" "${extra:+,$extra}" ;;
    # inserted=true is what puts the card in the slot; without it Gayle's
    # windows are mapped, nothing is logged, and card.resource never
    # initialises, which reads from the guest as a driver that cannot find its
    # hardware.  Needs a machine with a Gayle: an A600 or an A1200.
    ne2000_pcmcia)
        printf 'pcmcia=true\nne2000pcmcia_rom_file=:ENABLED\nne2000pcmcia_rom_options=inserted=true,mac=%s,%s%s\n' \
               "$mac" "$backend" "${extra:+,$extra}" ;;
    *)
        echo "unknown network board $board" >&2
        return 2 ;;
    esac
}

# HOW MUCH FAST RAM A BOARD LEAVES ROOM FOR.  $1 board, $2 what the caller
# would otherwise use.  Prints a figure in MB.
#
# 8 MB of Zorro II Fast RAM covers 0x200000-0x9fffff, and an A1200's PCMCIA
# windows are at 0x600000 (common) and 0xa00000 (attribute).  They overlap, on
# the real machine as well as here, which is why an A1200 with a PCMCIA card
# cannot have 8 MB on the trapdoor bus.  Under emulation the collision does not
# announce itself: the card is logged as inserted, the backend opens, and the
# driver simply fails to find it.  4 MB stops short of 0x600000.
#
# IT IS HERE RATHER THAN IN ONE HARNESS because it is a property of the board,
# and a caller that writes its own emulator config has no way to know it.
# install/test/run-workbench.sh wrote fastmem_size=8 for every run and had
# never met a PCMCIA card, because that gate only ever booted the A2065; the
# first sweep that gave it one came back with the whole nine-card table green
# except ne2000_pcmcia, whose guest said "Network stack: not started" with a
# correctly written interface file and a card the emulator log calls inserted.
# The tell is in the emulator log: a working run reaches
# `PCMCIA NE2000 IO configured = 60` and that one never did.
emu_board_fastmem() { # board default-MB
    case "$1" in
    ne2000_pcmcia) echo 4 ;;
    *)             echo "${2:-8}" ;;
    esac
}

# WHAT THE EMULATOR SAYS WHEN THE GUEST HAS REALLY TAKEN THE CARD, as a grep
# pattern for the --log output.  Prints nothing for a board that has no such
# line, and a caller must treat that as "cannot check" rather than as a pass.
#
# This exists because of ne2000_pcmcia and it is worth being exact about what
# it distinguishes.  `PCMCIA: '...' inserted=1` is Amiberry saying the card is
# in the slot, and it is printed whether or not anything can reach it -- both
# the failing run and the working one had it.  `PCMCIA NE2000 IO configured`
# is card.resource having walked the CIS tuples and mapped the card's I/O
# window, which only happens once the guest can see it, and ONLY the working
# run had that.  The difference between the two was diagnosed by hand out of
# two logs; asserting on the second one is what stops the next silent version
# of this needing the same afternoon.
#
# The Zorro boards have no equivalent: their autoconfig is logged as a board
# list at reset whether or not a driver ever opens them, so there is no line
# that means "the guest took it".  For those, the check the run already has --
# does the machine reach the network after the power cycle -- is the proof,
# and this returns nothing rather than inventing a weaker one.
emu_board_log_proof() { # board
    case "$1" in
    ne2000_pcmcia) echo 'PCMCIA NE2000 IO configured' ;;
    *)             echo "" ;;
    esac
}

# The machine a board needs, checked rather than described.  Prints nothing and
# returns 0 when the pair can boot; prints why and returns 1 when it cannot.
#
# Both of these are silent failures in the emulator, which is why they are
# here: the board is simply never instantiated, nothing is logged, and the run
# reads as a driver that cannot find its card.
emu_board_model_check() { # board model
    local board="$1" model="$2"

    case "$board" in
    xsurf100z3)
        # BOARD_AUTOCONFIG_Z3 (expansion.cpp:6476) maps only with
        # cs_z3autoconfig AND a 32-bit address space (expansion.cpp:565).  An
        # A1200 is address_space_24 = true (cfgfile.cpp:9289) and sets neither.
        case "$model" in
        A3000*|A4000*) return 0 ;;
        *)
            echo "$board is a Zorro III board and $model has a 24-bit address" >&2
            echo "space, so Amiberry never maps it.  Use an A3000 or an A4000." >&2
            return 1 ;;
        esac ;;
    ne2000_pcmcia)
        # A Gayle, and a 68020: card.resource cannot walk the card's CIS
        # tuples on a 68000 or a 68010 under Amiberry, and AddNetInterface
        # answers 'Could not add interface "eth0" (Input/output error)'.
        # Bisected to cpu_type alone -- tools/amiberry-run.sh has the long
        # version.
        case "$model" in
        A1200*|A600*) return 0 ;;
        *)
            echo "$board needs a machine with a Gayle: an A1200 or an A600." >&2
            return 1 ;;
        esac ;;
    esac
    return 0
}
