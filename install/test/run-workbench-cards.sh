#!/usr/bin/env bash
#
# THE RELEASE GATE, ON EVERY CARD.
#
#   install/test/run-workbench-cards.sh -a ARCHIVE.lha [-c BOARD[,BOARD...]]
#                                       [-l LEVEL] [-t SECONDS] [-T SECONDS]
#
# install/test/run-workbench.sh installs the release archive on a real
# Workbench 3.1, power cycles the machine and then uses it.  It booted the
# A2065 and nothing else -- the driver was staged by name and the emulator
# config held two literal a2065 lines -- so every release this project has cut
# was gated on one network card.  The standing rule is that every card appears
# in the end-to-end, and one-card coverage has already let a regression reach a
# user once.
#
# The cards are tests/tools/cards.sh, the same table the two card sweeps read.
# One run per card, in series: each one boots a whole Workbench twice, and the
# lab host is shared.
#
# EVERY RUN GETS ITS OWN MAC, because AMINETXDUO_RUN_TAG is per card and
# tools/emu-mac.sh derives the address from the tag.  Nine guests on the LAN
# under one hardware address is not a collision the emulator reports: the
# frames arrive, a peer's neighbour cache keeps whichever answered last, and
# what fails is an assertion somewhere else.
#
# WHAT A GREEN CARD MEANS, AND IT IS NOT ONE THING.  The Installer asks which
# card the machine has and installdrive.c cannot answer an askchoice, so the
# card is selected only when the script's own detection loop finds the staged
# driver -- which needs the driver's FILE NAME to be one of the eight in
# Install-AmiNetXDuo:524-533.  For the rest, run-workbench.sh rewrites the
# interface file after the install and says so.  Both are reported, never
# merged:
#
#   card_config=installer      the installer chose this card
#   card_config=post-install   the stack drives this card; the installer did
#                              not choose it and this run does not claim it did
#
# Output is key=value plus an exit code, one line per card and one at the end.
#
# Exit status: 0 every card that ran passed, 1 a card failed, 2 no card could
# run at all.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

# shellcheck source=../../tests/tools/cards.sh
. "$ROOT/tests/tools/cards.sh"

ARCHIVE=""
ONLY=""
LEVEL=NOVICE
INSTALL_TIMEOUT=420
BOOT_TIMEOUT=720
TAGBASE="${AMINETXDUO_RUN_TAG:-wbcards}"

while getopts "a:c:l:t:T:" opt; do
    case "$opt" in
        a) ARCHIVE="$OPTARG" ;;
        c) ONLY="$OPTARG" ;;
        l) LEVEL="$OPTARG" ;;
        t) INSTALL_TIMEOUT="$OPTARG" ;;
        T) BOOT_TIMEOUT="$OPTARG" ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

# -a IS NOT OPTIONAL HERE, and that is the difference between this and one run
# of run-workbench.sh.  Without it that script builds an archive from the tree
# it is in, and nine cards would then build nine archives -- each one a few
# minutes, all of them identical, and none of them the artefact a user
# downloads.  A sweep of the release gate gates THE RELEASE ARCHIVE.
[ -n "$ARCHIVE" ] || {
    echo "-a <archive.lha> is required: this sweep gates one archive on every" >&2
    echo "card, and it must be the archive that will be released." >&2
    exit 2
}
case "$ARCHIVE" in /*) ;; *) ARCHIVE="$PWD/$ARCHIVE" ;; esac
[ -f "$ARCHIVE" ] || { echo "no such archive: $ARCHIVE" >&2; exit 2; }

LOGDIR="$ROOT/build/wbcards"
mkdir -p "$LOGDIR"

echo "archive=$ARCHIVE bytes=$(wc -c < "$ARCHIVE" | tr -d ' ') level=$LEVEL"
echo "cards=$(cards_rows "$ONLY" | awk '{ printf "%s ", $1 }')"

# WHAT IS NOT COVERED, PRINTED EVERY RUN.  A list of what was tested is worth
# nothing without the list of what was not, and these are drivers with no board
# rather than boards with no driver -- Amiberry emulates no ARCnet card, and
# neither serial line has an Ethernet board to bridge.
printf '%s\n' "$UNTESTABLE" |
    awk 'NF { printf "untestable=%s reason=%s\n", $1, substr($0, index($0, $2)) }'

RUN=0
PASSED=0
FAILED=0
SKIPPED=0
BY_INSTALLER=0
FAILED_CARDS=""
SKIPPED_CARDS=""

while read -r board model _addr _mac; do
    [ -n "$board" ] || continue
    RUN=$((RUN + 1))
    tag="$TAGBASE-$board"
    log="$LOGDIR/$board.log"
    started=$(date +%s)

    echo
    echo "=== $board ($model) -> $log"

    # AMINETXDUO_MODEL is deliberately NOT set: run-workbench.sh takes the
    # model from the same cards.sh row, and setting it here would mean two
    # places deciding it.  AMINETXDUO_A2065 is left alone for the same reason.
    AMINETXDUO_RUN_TAG="$tag" \
    install/test/run-workbench.sh -a "$ARCHIVE" -N "$board" -l "$LEVEL" \
        -t "$INSTALL_TIMEOUT" -T "$BOOT_TIMEOUT" > "$log" 2>&1
    rc=$?
    elapsed=$(( $(date +%s) - started ))

    verdict=$(sed -n 's/^workbench_e2e=\([A-Z]*\).*/\1/p' "$log" | tail -1)
    config=$(sed -n 's/.*card_config=\([a-z-]*\).*/\1/p' "$log" | tail -1)
    selected=$(sed -n 's/^installer_card_selected=//p' "$log" | tail -1)
    device=$(sed -n 's/^installer_device=//p' "$log" | tail -1)
    driver=$(sed -n 's/^card_driver=//p' "$log" | tail -1)
    boot=$(sed -n 's/.*boot_status=\([^ ]*\).*/\1/p' "$log" | tail -1)

    if [ "$rc" = "2" ]; then
        # An ingredient this host has not got.  Not a failure of the product,
        # and it must not read as one -- most of these drivers cannot be
        # fetched, so they arrive by somebody putting the file there.
        SKIPPED=$((SKIPPED + 1))
        SKIPPED_CARDS="$SKIPPED_CARDS $board"
        # The first thing the run refused on.  `reason=unknown` on all eight
        # of these is how a harness bug reads as a missing asset, so this
        # takes the first complaint of any shape rather than two prefixes.
        why=$(grep -m1 -e '^No ' -e '^!! ' -e '^-N ' -e 'not exist' "$log")
        printf 'card=%s model=%s rc=2 verdict=SKIP elapsed=%s reason=%s\n' \
               "$board" "$model" "$elapsed" "${why:-unknown}"
        continue
    fi

    [ "$rc" = "0" ] && PASSED=$((PASSED + 1)) || {
        FAILED=$((FAILED + 1)); FAILED_CARDS="$FAILED_CARDS $board"; }
    [ "$config" = "installer" ] && BY_INSTALLER=$((BY_INSTALLER + 1))

    printf 'card=%s model=%s driver=%s rc=%s verdict=%s card_config=%s installer_card_selected=%s installer_device=%s boot_status=%s elapsed=%s log=%s\n' \
           "$board" "$model" "${driver:-unknown}" "$rc" \
           "${verdict:-NONE}" "${config:-unknown}" "${selected:-unknown}" \
           "${device:-unknown}" "${boot:-unknown}" "$elapsed" "$log"
done <<EOF
$(cards_rows "$ONLY")
EOF

echo
printf 'cards_run=%s cards_passed=%s cards_failed=%s cards_skipped=%s installer_selected=%s\n' \
       "$RUN" "$PASSED" "$FAILED" "$SKIPPED" "$BY_INSTALLER"
[ -z "$FAILED_CARDS" ]  || echo "failed_cards=${FAILED_CARDS# }"
[ -z "$SKIPPED_CARDS" ] || echo "skipped_cards=${SKIPPED_CARDS# }"

if [ "$RUN" = "0" ] || [ "$((PASSED + FAILED))" = "0" ]; then
    echo "workbench_cards=NONE"
    exit 2
fi
if [ "$FAILED" != "0" ]; then
    echo "workbench_cards=FAIL"
    exit 1
fi

# A SKIP IS NOT A PASS, and this said otherwise once: one card ran, eight were
# skipped for a driver the harness had already found and then failed to hand
# on, and the sweep printed workbench_cards=PASS.  That is the vacuous green
# this whole file exists to prevent -- "every card" cannot mean "the one card
# whose ingredients happened to be in place".  The gate is only green when
# every card in the table ran.
if [ "$SKIPPED" != "0" ]; then
    echo "workbench_cards=INCOMPLETE"
    exit 1
fi
echo "workbench_cards=PASS"
exit 0
