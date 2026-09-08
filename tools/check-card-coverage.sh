#!/usr/bin/env bash
#
# Every card src/netdev/netdev_cards.c supports is either swept or declared.
#
#   tools/check-card-coverage.sh
#
# THE SWEEP'S TABLE IS NOT THE LIST OF SUPPORTED CARDS, and it reads as
# though it were.  tests/tools/run-cardsweep.sh prints nine cards and three
# untestable DRIVERS, and `cardsweep: cards=9 pass=9` looks complete.
# netdev_cards[] carried twelve: 3c589, 3ccfem556, 3cxem556 and xsurf500 were
# in neither list and nothing said so.  Those four are real rows that a
# shared change in src/netdev/netdev_*.c reaches, and the sweep is the only
# cross-core coverage there is.
#
# So: a card in netdev_cards[] must appear in cards.sh's CARDS (via the
# board->card mapping the sweep prints as anxcard=) or in its
# UNTESTABLE_CARDS, with a reason.  Add a core without doing one or the
# other and this goes red, rather than the coverage quietly shrinking.
#
# key=value and an exit code, like every other gate here.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 2

TABLE="src/netdev/netdev_cards.c"
[ -r "$TABLE" ] || { echo "no $TABLE" >&2; exit 2; }

# The table's names, in declaration order.  A row is `{ "name", manid, ...`.
supported=$(grep -oE '^[[:space:]]*\{ "[a-z0-9_]+"' "$TABLE" |
            grep -oE '"[a-z0-9_]+"' | tr -d '"' | sort -u)

[ -n "$supported" ] || { echo "card_coverage=error reason=no_rows_parsed" >&2; exit 2; }

# What the sweep boots, as the card names netdev_cards.c uses.  Taken from the
# harness itself rather than re-derived: -l is a listing and needs no rig.
swept=$(tests/tools/run-cardsweep.sh -l 2>/dev/null |
        grep -oE 'anxcard=[a-z0-9_]+' | cut -d= -f2 | sort -u)

# shellcheck source=/dev/null
. "$ROOT/tests/tools/cards.sh"
declared=$(printf '%s\n' "${UNTESTABLE_CARDS:-}" | awk 'NF {print $1}' | sort -u)

missing=""
for card in $supported; do
    case "
$swept
$declared
" in
        *"
$card
"*) continue ;;
    esac
    missing="$missing $card"
done

printf 'card_coverage_supported=%d\n' "$(printf '%s\n' "$supported" | grep -c .)"
printf 'card_coverage_swept=%d\n'     "$(printf '%s\n' "$swept"     | grep -c .)"
printf 'card_coverage_declared=%d\n'  "$(printf '%s\n' "$declared"  | grep -c .)"

if [ -n "$missing" ]; then
    for card in $missing; do
        echo "card_coverage_undeclared=$card"
    done
    echo "  Each is a row in $TABLE that no sweep boots and no list excuses."
    echo "  Add it to CARDS in tests/tools/cards.sh if a board exists, or to"
    echo "  UNTESTABLE_CARDS with the reason it cannot be booted."
    echo "check_card_coverage=FAIL"
    exit 1
fi

echo "check_card_coverage=PASS"
exit 0
