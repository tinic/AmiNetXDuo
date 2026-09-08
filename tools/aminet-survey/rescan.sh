#!/bin/bash
#
# Re-scan archives whose rows were written by an older scanner.
#
#   tools/aminet-survey/rescan.sh [limit]
#
# THE LEDGER MIXED FIVE SCANNER REVISIONS AND COULD NOT SAY WHICH ROW CAME
# FROM WHICH -- codex's point 6.  Each of those changes altered what a scan
# returns for the same bytes, so an early row is not a weaker result, it is
# possibly a wrong one, and nothing distinguished them.
#
# No network is involved.  Every archive in the ledger is still unpacked under
# $ANXD_SURVEY_OUT, so a rescan is a local walk: 1920 of 1920 archives were
# present when this was written.  Archives missing locally are LEFT ALONE and
# counted out loud rather than dropped -- deleting a row because this machine's
# /tmp was cleared would shrink the survey for a reason that has nothing to do
# with the survey.
#
# SPDX-License-Identifier: MIT

set -u
DIR=${ANXD_SURVEY_DIR:-/home/turo/anxd-aminet}
OUT=${ANXD_SURVEY_OUT:-/tmp/anxd-survey}
LEDGER="$DIR/results.tsv"
LIMIT=${1:-100000}
WANT=$(python3 -c "
import sys; sys.path.insert(0, '$DIR'); import scan; print(scan.SCANNER_VERSION)")

[ -f "$LEDGER" ] || { echo "rescan: no ledger at $LEDGER" >&2; exit 2; }
echo "rescan: current scanner is v$WANT"

# Archives with at least one row not at the current version.
stale=$(awk -F'\t' -v w="scanner=$WANT" '
    NR>1 && $1 != "archive" { if ($7 != w) print $1 }' "$LEDGER" | sort -u | head -n "$LIMIT")
[ -n "$stale" ] || { echo "rescan: every row is at v$WANT"; exit 0; }
n=$(echo "$stale" | wc -l)
echo "rescan: $n archives below v$WANT"

done_n=0; miss=0; changed=0; repathed=0
tmp=$(mktemp) || exit 2
for name in $stale; do
    if [ ! -d "$OUT/$name.d" ]; then miss=$((miss+1)); continue; fi

    old=$(awk -F'\t' -v a="$name" '$1==a' "$LEDGER")
    path=$(printf '%s' "$old" | awk -F'\t' 'NF>7 && $8!=""{print $8; exit}')
    [ -n "$path" ] || path=$(grep -m1 "/$name\$" "$DIR/worklist.txt" || echo "?/$name")
    sha=$(sha256sum "$OUT/$name" 2>/dev/null | cut -d' ' -f1)

    # Build the archive's rows fresh, exactly as tick.sh would.
    : > "$tmp"
    found=0; errs=0
    while IFS= read -r -d '' f; do
        row=$(python3 "$DIR/scan.py" "$f" 2>/dev/null)
        [ -n "$row" ] || { errs=$((errs+1)); continue; }
        case "$row" in *NO_BSDSOCKET_STRING*|*NO_HUNK*) continue ;; esac
        rel=${f#"$OUT/$name.d/"}
        row=${row#*$(printf '\t')}
        printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$rel" "$row" "$path" "$sha" >> "$tmp"
        found=$((found+1))
    done < <(find "$OUT/$name.d" -type f -size +1k -print0)
    if [ "$found" = 0 ]; then
        if [ "$errs" -gt 0 ]; then
            printf '%s\t-\tSCAN_ERROR files=%s\t0\t0\t\tscanner=%s\t%s\t%s\n' \
                "$name" "$errs" "$WANT" "$path" "$sha" >> "$tmp"
        else
            printf '%s\t-\tNO_BSDSOCKET_BINARY\t0\t0\t\tscanner=%s\t%s\t%s\n' \
                "$name" "$WANT" "$path" "$sha" >> "$tmp"
        fi
    fi

    new=$(cat "$tmp")
    # WHAT CHANGED, NOT THAT SOMETHING DID.  Comparing rows as ordered text
    # called AMIGIFT-2.1.lha CHANGED when all eight verdicts were identical:
    # `find` returns files in directory order, which differs between runs, and
    # the old rows carry a bare basename where new ones carry the path relative
    # to the archive.  Both are real upgrades and neither is a verdict change,
    # so reporting them as one number would have read as "the old scanner got
    # 1900 archives wrong".
    #
    # Sorted, and split: the VERDICT set is what says the scanner disagrees
    # with itself; the file column moving from basename to relative path is a
    # separate old fix being applied.
    ov=$(printf '%s' "$old" | cut -f3-6 | sort); nv=$(printf '%s' "$new" | cut -f3-6 | sort)
    of=$(printf '%s' "$old" | cut -f2 | sort);   nf=$(printf '%s' "$new" | cut -f2 | sort)
    if [ "$ov" != "$nv" ]; then
        changed=$((changed+1)); echo "VERDICT_CHANGED $name"
    elif [ "$of" != "$nf" ]; then
        repathed=$((repathed+1))
    fi

    keep=$(mktemp) || exit 2
    awk -F'\t' -v a="$name" '$1!=a' "$LEDGER" > "$keep" && cat "$tmp" >> "$keep" \
        && mv "$keep" "$LEDGER"
    done_n=$((done_n+1))
done
rm -f "$tmp"

echo "rescan: $done_n rescanned, $changed changed VERDICT, $repathed gained a\
 relative file path, $miss not unpacked locally (left as they were)"
