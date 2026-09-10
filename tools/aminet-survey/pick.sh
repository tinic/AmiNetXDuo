#!/bin/bash
# Pick a dozen unscanned archives, AIMED BY MEASURED YIELD.
#
#   tools/aminet-survey/pick.sh <seed>
#
# The old weights were hand-set from a sample of 299 archives, back when
# comm/tcp was the goldmine.  comm/tcp (18%, 739 scanned) and comm/net (9%)
# are now EXHAUSTED -- 19 archives left between them -- so those weights aim
# at nothing.  yield.py recomputes the table from the whole ledger; this draws
# from it.
#
# THREE TIERS, and the middle one is the point:
#
#   1. PROVEN YIELD.  A directory with a measured hit rate and archives left.
#      comm/mail (4%, 54 left) and comm/irc (5%, 31 left) finish first.
#
#   2. UNDERSAMPLED.  A directory with fewer than MINSAMPLE archives scanned
#      is not a 0% directory, it is an unmeasured one.  comm/dlg is 0 of 4
#      with 329 archives queued; comm/term 0 of 4 with 105.  Writing off 1,036
#      archives on four samples would be a conclusion the data cannot carry,
#      so every directory gets sampled to MINSAMPLE before its rate is
#      believed.
#
#   3. MEASURED ZERO.  comm/ambos (0/32), comm/maxs (0/20) and comm/cnet
#      (0/18) are BBS and door software with no TCP in them.  793 archives,
#      drawn last and never dropped -- a zero over 70 samples is evidence, not
#      proof, and the tier still gets a share so it can be disproved.
#
# SPDX-License-Identifier: MIT
set -u
DIR=${ANXD_SURVEY_DIR:-/home/turo/anxd-aminet}
cd "$DIR" || exit 2
SEED=${1:-1}
N=${ANXD_PICK_N:-12}
MINSAMPLE=${ANXD_PICK_MINSAMPLE:-25}

cut -f1 results.tsv | grep -v '^$\|^archive$' | sort -u > /tmp/done-$$.txt
trap 'rm -f /tmp/done-$$.txt /tmp/tier-$$.*' EXIT

Y=$(python3 "$(dirname "$0")/yield.py" "$DIR" 2>/dev/null)
[ -n "$Y" ] || { echo "pick: yield.py produced nothing" >&2; exit 2; }

# Split the directories into the three tiers.
# TIER BY RATE, NOT BY "SOMETHING WAS FOUND".  A first cut put every
# directory with one attributed archive into tier 1, which swept comm/misc,
# comm/www and comm/bbs (1-3%, 1,655 archives queued) in beside comm/mail and
# comm/irc (4-5%, 85 left) -- and since the draw is a shuffle over the pooled
# paths, sheer volume meant the two directories worth finishing were not drawn
# at all.  The rate is the thing being aimed at, so the rate sets the tier.
printf '%s\n' "$Y" | awk -F'\t' -v m="$MINSAMPLE" '
    NR==1 || $1 ~ /^\(|^\?$/ { next }
    $5 == 0                  { next }                 # nothing left to draw
    $4 >= 4                  { print "1\t"$1; next }  # good rate
    $3 >  0                  { print "2\t"$1; next }  # thin but non-zero
    $2 <  m                  { print "3\t"$1; next }  # undersampled
                             { print "4\t"$1 }        # measured zero
' > /tmp/tier-$$.all

draw() {  # tier count
    local t=$1 c=$2 dirs
    dirs=$(awk -F'\t' -v t="$t" '$1==t{print $2}' /tmp/tier-$$.all)
    [ -n "$dirs" ] || return 0
    for d in $dirs; do grep "^$d/" worklist.txt; done \
        | grep -vFf /tmp/done-$$.txt \
        | shuf -n "$c" --random-source=<(yes "$SEED-$t")
}

# 5 good-rate, 3 thin, 3 undersampled, 1 measured-zero.  Any tier that cannot
# fill its share leaves the pick short rather than stealing from another, so a
# run that thins out says so in the count instead of quietly redistributing --
# and comm/mail and comm/irc running dry is exactly the signal that the survey
# has finished the part of Aminet where sockets actually live.
{ draw 1 5; draw 2 3; draw 3 3; draw 4 1; } | sort -u
