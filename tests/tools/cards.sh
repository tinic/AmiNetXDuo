#!/usr/bin/env bash
#
# The cards this project supports, one table, sourced by every sweep.
#
#   tests/tools/run-cardsweep.sh   IPv4 bytes both ways, counted off the box
#   tests/tools/run-cardsweep6.sh  IPv6 to a destination past the router
#
# ONE TABLE, because a card added to one sweep has to be a card the other
# sweep boots.  The two sweeps disagreeing about what "every card" means is
# the same hole that let x-surf-100 reach a user, one level up.
#
# SPDX-License-Identifier: MIT

# ------------------------------------------------------------- the cards ----
#
# board  model  address  mac-tail
#
# The MAC tail is the only part that is ours: the A2065's LANCE overwrites the
# first three bytes with Commodore's OUI (tools/amiberry-run.sh:178-181), so
# two cards that differ only in the head reach the wire as one machine.  These
# differ in the last byte and none of them is the demo's :77.  The head is the
# SWEEP's, not the card's: run-cardsweep.sh puts 02:41:4d:49 in front of these
# and run-cardsweep6.sh puts 02:41:4d:4b, so the two sweeps cannot be one
# machine to the LAN's DHCP server or to a neighbour cache.
#
# The address is what run-cardsweep.sh gives the guest, static and outside
# what the lab's DHCP hands out, because its peer has to call IN.
# run-cardsweep6.sh takes a lease instead -- nothing calls in there -- so it
# reads this column only to keep the table one shape.
#
# eb920 NEEDS AMIBERRY ef28da7e OR LATER.  Before it, `toariadne2()` decoded the
# LAN Rover's buffer window as `(addr & 0xc000) == 0x8000` with
# `maddr = addr & 16383`, so only the first 16K of the card's 32K was visible.
# eb920.device programs PSTART=0x06 PSTOP=0x80, and the moment the receive ring
# crossed page 0x40 the driver read zeroes where the packet header was, stopped
# clearing ENISR_RX, and receive died with `bnry=7f curr=4b isr=01`: online, a
# DHCP lease, one ARP request out, and then the 300 s ceiling with zero bytes
# either way.  Run with AMIBERRY_NE2000_STATS=1 to get those registers back.
#
# xsurf100z3 is the one card on another machine.  BOARD_AUTOCONFIG_Z3
# (expansion.cpp:6476) maps only with cs_z3autoconfig AND a 32-bit address
# space (expansion.cpp:565).  An A1200 is address_space_24 = true
# (cfgfile.cpp:9289) and sets neither, so the board is never seen; an A3000
# sets both (cfgfile.cpp:9489, :10421).
CARDS="
a2065          A1200  192.168.1.241  0c:01
ariadne        A1200  192.168.1.242  0c:02
ariadne2       A1200  192.168.1.244  0c:03
hydra          A1200  192.168.1.245  0c:04
eb920          A1200  192.168.1.246  0c:05
xsurf          A1200  192.168.1.247  0c:06
xsurf100z2     A1200  192.168.1.248  0c:07
xsurf100z3     A3000  192.168.1.250  0c:08
ne2000_pcmcia  A1200  192.168.1.251  0c:09
"

# TWO CARDS IN ONE MACHINE CANNOT BE TESTED HERE, and it is the emulator, not
# the harness.  Amiberry holds one network board of each family in file-scope
# statics -- a2065.cpp has one `romtype`, one `boardram` and one `csr[]` for
# the whole LANCE family, and qemuvga/ne2000.cpp has one `ne2000_data` for the
# DP8390 family -- so only the board named by -N is ever instantiated.  Adding
# a second through AMINETXDUO_AMIBERRY_EXTRA writes it into the config and it
# never appears, silently, with nothing in the log.  Tried both ways round and
# across families (a2065 + ariadne2): one board, always.
#
# What that leaves untested in anxnet.device: unit numbering across more than
# one board, and CARD= picking the right one of several.  Both are exercised
# by the pinning arithmetic in the host test, and neither has ever run on a
# machine holding two cards.

# Cards this project names that no arm can reach, and why.  Printed every run:
# a list of what is covered is worth nothing without the list of what is not.
# Keyed by the DRIVER, because these are drivers with no board rather than
# boards with no driver.
#
#   a2060   ARCnet.  Amiberry emulates no ARCnet board -- there is no key for
#           one in tools/amiberry-run.sh:226-241 and none in expansion.cpp.
#   slip    serial-line IP, and rs485 likewise.  No Ethernet board to bridge,
#           and no harness has a serial peer.
# shellcheck disable=SC2034  # read by the sweeps that source this
UNTESTABLE="
a2060.device   no ARCnet board exists in Amiberry
slip.device    serial line, not an Ethernet board
rs485.device   serial line, not an Ethernet board
"

# The rows a sweep should run, filtered by a comma-separated list of boards.
# An empty filter is every card, which is what a gate runs.
cards_rows() { # [board[,board...]]
    _cards_only="${1:-}"
    printf '%s\n' "$CARDS" | while read -r board model addr mac; do
        [ -n "$board" ] || continue
        case ",$_cards_only," in
            ,,) ;;
            *",$board,"*) ;;
            *) continue ;;
        esac
        printf '%s %s %s %s\n' "$board" "$model" "$addr" "$mac"
    done
}
