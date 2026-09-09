#!/usr/bin/env bash
# The cards this project supports, one table, sourced by every sweep.
# SPDX-License-Identifier: MIT

# xsurf IS FLAKY AND THE RATE IS MEASURED: 1 arm in 7 fails, 2026-09-09, on
# playhouse3 against playhouse4, seven back-to-back single-card sweeps.  A
# single passing sweep therefore proves very little about it.
#
# TELL IT BY THE WALL TIME.  A failing arm is ~313 s -- the 300 s emulator
# timeout -- and every passing arm is 51-58 s, so `wall_s` in the cardsweep
# row separates them at a glance.  The shape is always the same: the run stops
# in `SYS:iperf -s -p 74xx`, the peer logs `cannot connect ... timed out`, and
# the guest never answers in SERVER mode.
#
# IT IS NOT THE ADDRESS.  a2065's failure was an occupied address and the
# relocation walk below fixes that one; xsurf's is not.  The same
# 192.168.1.247 passed six arms and failed the seventh in that run.  The card
# transmits either way -- 1,213 frames sourced by its MAC in an earlier
# capture, against a2065's zero.
#
# Repro and next step are in the a2065 backlog row.
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
