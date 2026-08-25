#!/usr/bin/env bash
# The cards this project supports, one table, sourced by every sweep.
# SPDX-License-Identifier: MIT

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
