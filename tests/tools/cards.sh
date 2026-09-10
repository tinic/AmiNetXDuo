#!/usr/bin/env bash
# The cards this project supports, one table, sourced by every sweep.
# SPDX-License-Identifier: MIT

# xsurf's FLAKINESS HAS A CAUSE AND IT WAS NEVER THE CARD.  Read the whole of
# this before quoting the old rate at anyone.
#
# The peer's UDP server-arm sender retried with no back-off: a UDP send cannot
# fail, so `peer_cmd send udp` returned 0 whether or not the guest was
# listening and the loop went straight round again.  The guest does not open
# that port until stage 14 of 15, so the peer spent the whole run firing full
# payloads at a closed port.  Measured in cardsweep-xs4-xsurf.pcap: 54 bursts
# of 521 datagrams, 28,128 packets, ~41 MB at 1470 bytes each, starting at
# 12:15:44 -- before the guest had booted -- and going on to the 300 s timeout.
# The guest takes and drops every one, and on an emulated A1200 that is the CPU
# its TCP stages needed.  run-iperf.sh now backs off and sequences the UDP
# sender behind the TCP one; tests/tools/peersender-selftest.sh holds it.
#
# WHAT THE FAILING ARM ACTUALLY LOOKED LIKE, since three earlier readings of it
# were wrong:
#   - It is NOT confined to server mode.  The first stage to fail was stage 1,
#     `iperf <peer> -p 7401 -t 3`, with the guest as CLIENT.  The run merely
#     STOPS at the server stage, because that is where the harness blocks.
#   - Inbound was NOT dead.  The guest answered ARP right through the 300 s
#     and the peer's SYN-ACKs went out 8 times for 7401 and 7 for 7403.  What
#     failed was the guest answering them -- and it answered only 7 of the 28
#     ARP requests aimed at it, which is the same starvation seen from L2.
#   - The guest's own UDP client stage PASSED on the failing arm -- 751,170
#     bytes, 510 packets, 0 lost -- which is why "the card transmits" was true
#     and told nobody anything.
#
# VERIFIED ON THE FIXED HARNESS, 2026-09-10.  40 arms, 0 failures:
#
#     xsurf  20 arms (6 slot-indexed, one per SWEEP_SLOT, plus 14)  wall_s 46-48
#     a2065  20 arms                                                wall_s 41-42
#
# Against the rates these two used to fail at -- 1 in 7 for xsurf, about 30%
# for a2065's intermittent hand-run failures -- twenty clean arms each would
# happen 4.6% and 0.08% of the time.  a2065's residue was the SAME flood; its
# .191 address collision was real, separate, and CI-only.  On the wire, a
# fixed arm carries ONE burst of ~523 datagrams and 4,382 frames where the
# failing one carried 54 bursts, 28,128 datagrams and 29,583 frames, and
# srvudp.err fell from 13,030 bytes to 561 while
# `ok: the guest served a UDP receive (rc 0)` still passes -- the sequencing
# cost no coverage.
#
# WHAT SEPARATED THE SIX FROM THE ONE, and it is not the guest being slow:
# AddNetInterface took 9380-9420 ms on all seven arms, the failing one included.
# It is whether the peer's socket ever raised ECONNREFUSED.  A connected UDP
# socket learns a port is closed only from an ICMP port-unreachable, which
# makes the next send() raise; that is a non-zero exit from peer_cmd, which is
# the ONLY path that reached the old back-off.  Count of ECONNREFUSED in
# srvudp.err, by arm:
#
#     xs1 10   xs2 12   xs3 10   xs4 0   xs5 11   xs6 11   xs7 11
#      pass     pass     pass    FAIL     pass     pass     pass
#
# So six arms were rescued by the guest rejecting the datagrams and the seventh
# was not, and once the loop was spinning the guest had no room to start.  The
# fix removes the race rather than betting on which side wins it: the UDP
# sender no longer runs at all until the TCP one is done, and a UDP attempt the
# guest did not answer now backs off whether or not an ICMP came back.
#
# STILL OPEN, and it belongs to the stack rather than to this file: why that
# one arm emitted no ICMP port-unreachable.  It sourced 536 packets all run,
# essentially just its own UDP blast.
#
# AND THE ARMS DID NOT SHARE AN ADDRESS.  SWEEP_SLOT moves both the address and
# the MAC per sweep, base-10*slot and base+32*slot, and only slot 0 is left
# unmoved.  The seven arms drew slots 3,2,0,0,2,1,1 -- four addresses, not one
# -- so "the same 192.168.1.247 passed six arms and failed the seventh" was
# false: exactly two arms ever ran on .247.  Seven ids over six slots cannot be
# seven independent trials either.  The rows carry addr= and mac= now, so this
# is checkable rather than assumed.
#
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


# shellcheck disable=SC2034  # read by the sweeps that source this
UNTESTABLE="
a2060.device   no ARCnet board exists in Amiberry
slip.device    serial line, not an Ethernet board
rs485.device   serial line, not an Ethernet board
"

# CARDS src/netdev/netdev_cards.c SUPPORTS AND NO SWEEP HERE CAN BOOT.
#
# CARDS above is not "every card this project supports", and reading the
# sweep's nine-line table as if it were is the mistake this list exists to
# stop: netdev_cards[] carries twelve, and tools/emu-board.sh can express
# eight of them.  The four below are covered by tests/tools/run-hwcard.sh
# against real hardware, which skips when the board is absent, and their
# rows in netdev_cards.c say as much.
#
# tools/check-card-coverage.sh fails if a card is in netdev_cards[] and in
# neither list, so adding a core cannot quietly shrink what the sweep claims.
# shellcheck disable=SC2034  # read by the sweeps that source this
UNTESTABLE_CARDS="
3c589       no 3Com EtherLink III PCMCIA card in Amiberry; run-hwcard.sh proves it on hardware
3ccfem556   as 3c589, and its CIS tuple is covered by test_netdev_cis.c
3cxem556    as 3c589, and its CIS tuple is covered by test_netdev_cis.c
xsurf500    no ACA500 and no X-Surf 500 is modelled by any emulator (netdev_cards.c)
"

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
