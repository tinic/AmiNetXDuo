#!/bin/sh
#
# Re-attempt the rows that record a failure to look, not a finding.
#
#   tools/aminet-survey/retry.sh [FETCH_FAIL|SCAN_ERROR] [limit]
#
# FETCH_FAIL and SCAN_ERROR are NOT results.  One says the mirror did not
# answer, the other that the scanner died; neither says anything about the
# archive.  But both occupy a row keyed by archive name, and tick.sh skips any
# archive already in the ledger -- so a transient network failure permanently
# removed an archive from the survey and counted it as covered.
#
# 41 of 2413 rows are FETCH_FAIL, which is 1.7% of the ledger silently retired.
#
# The rows are DELETED and the archives re-ticked, so a retry that succeeds
# replaces the failure with the real verdict, and one that fails again just
# writes the failure back.  Deleting is safe precisely because these rows carry
# no information to lose.
#
# SPDX-License-Identifier: MIT

set -u
KIND=${1:-FETCH_FAIL}
LIMIT=${2:-12}
DIR=${ANXD_SURVEY_DIR:-/home/turo/anxd-aminet}
LEDGER="$DIR/results.tsv"

# Every verdict that records a failure to LOOK.  FETCH_UNPACK_FAIL was missing
# and is the one that mattered: tick.sh writes the reason class into the
# verdict, so the rows say FETCH_UNPACK_FAIL, and `$3 ~ "^FETCH_FAIL"` does not
# match that.  A retry pass reported "no FETCH_FAIL rows" with 10 unpack
# failures sitting in the ledger -- a pass that finds nothing because it asked
# the wrong question reads exactly like a pass with nothing to do.
case "$KIND" in
    FETCH_FAIL|FETCH_UNPACK_FAIL|FETCH_NOT_ARCHIVE|SCAN_ERROR|UNPACK_PARTIAL) ;;
    all) KIND='FETCH_|SCAN_ERROR|UNPACK_PARTIAL' ;;
    *) echo "retry.sh: refusing to retry '$KIND' -- only the verdicts that\
 record a failure to look are non-findings; every other verdict is data.\
 Try: FETCH_FAIL FETCH_UNPACK_FAIL FETCH_NOT_ARCHIVE SCAN_ERROR\
 UNPACK_PARTIAL all" >&2; exit 2 ;;
esac

[ -f "$LEDGER" ] || { echo "retry.sh: no ledger at $LEDGER" >&2; exit 2; }

# 404 IS NOT WORTH RETRYING and 000 is.  Rows written before fetch.sh recorded
# the code carry no http= at all; those are retried, because "unknown" is the
# state this whole pass exists to resolve.  ANXD_RETRY_404=1 forces them in.
if [ "${ANXD_RETRY_404:-0}" = "1" ]; then
    names=$(awk -F'\t' -v k="$KIND" '$3 ~ "^"k' "$LEDGER" | cut -f1 | sort -u | head -n "$LIMIT")
else
    names=$(awk -F'\t' -v k="$KIND" '$3 ~ "^"k && $3 !~ /http=404/' "$LEDGER" \
            | cut -f1 | sort -u | head -n "$LIMIT")
fi
[ -n "$names" ] || { echo "retry: no $KIND rows"; exit 0; }
n=$(echo "$names" | wc -l)
echo "retry: $KIND rows to re-attempt: $n"

# Map each name back to a worklist path -- tick.sh takes archive paths, and the
# ledger only keeps the basename.
paths=""
for name in $names; do
    p=$(grep -m1 "/$name\$" "$DIR/worklist.txt" || true)
    if [ -z "$p" ]; then echo "retry: $name is not in worklist.txt, skipped" >&2; continue; fi
    paths="$paths $p"
done
[ -n "$paths" ] || { echo "retry: nothing resolvable"; exit 0; }

# DROP ONLY WHAT IS ABOUT TO BE RETRIED.  The first run dropped every
# FETCH_FAIL row -- 44 of them -- and re-ticked the 20 the limit allowed, so 24
# archives left the ledger without being re-attempted.  Nothing was lost (an
# absent archive is back in the unscanned pool and a later tick can pick it up)
# but the script claimed to retry what it deleted, and a ledger that shrinks by
# 24 rows with no explanation is exactly the kind of number that gets quoted
# later as a survey result.
before=$(wc -l < "$LEDGER")
tmp=$(mktemp) || exit 2
printf '%s\n' "$names" > "$tmp.sel"
awk -F'\t' -v k="$KIND" 'NR==FNR{sel[$0]=1; next} !(sel[$1] && $3 ~ "^"k)' \
    "$tmp.sel" "$LEDGER" > "$tmp" && mv "$tmp" "$LEDGER"
rm -f "$tmp.sel"
kept=$(wc -l < "$LEDGER")
echo "retry: dropped $((before - kept)) $KIND rows for $(echo "$paths" | wc -w) archives, re-ticking"

# shellcheck disable=SC2086
"$DIR/tick.sh" $paths
rc=$?

after=$(wc -l < "$LEDGER")
still=$(awk -F'\t' -v k="$KIND" '$3 ~ "^"k' "$LEDGER" | wc -l)
echo "retry: ledger $before -> $after, $KIND still $still, tick rc=$rc"
exit "$rc"
