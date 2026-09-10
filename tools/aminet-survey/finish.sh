#!/bin/bash
# Scan every archive still missing from the ledger, in parallel batches.
#
#   ./finish.sh [batch]
#
# tick.sh draws a dozen archives because it was built to be run repeatedly by
# a heartbeat.  Finishing the survey is a different job: 3,744 archives, and
# the only reason it was slow is that fetch.sh slept 2 s between them.
# Parallel prefetch is 12.9x on the wire (19.65 s -> 1.52 s for 8), so the
# work is now bounded by scanning, at roughly 27 ms a file.
#
# Batched rather than one enormous list, because every archive is unpacked and
# kept: /tmp/anxd-survey is already 2.3 GB for 1,963 archives, and fetching
# 3,744 more before scanning any of them would need the disk for all of it at
# once.
set -u
cd /home/turo/anxd-aminet
BATCH=${1:-250}
export ANXD_FETCH_JOBS=${ANXD_FETCH_JOBS:-6}
export ANXD_FETCH_PAUSE=0

while :; do
    cut -f1 results.tsv | grep -v '^$\|^archive$' | sort -u > /tmp/fin-done.txt
    # EXACT MATCH ON THE BASENAME, not a substring.  `grep -vFf done.txt` over
    # the full paths excludes any line CONTAINING a scanned basename, so
    # ADMail.lha was dropped because Mail.lha had been scanned and
    # 1ooeMail.lha because of eMail.lha.  100 archives were skipped that way
    # and the run reported "worklist exhausted".  Same shape as the empty
    # -vFf pattern that once excluded the entire worklist.
    left=$(awk -F/ 'NR==FNR{done[$0]=1; next} !($NF in done){print}' \
           /tmp/fin-done.txt worklist.txt | head -n "$BATCH")
    [ -n "$left" ] || { echo "finish: worklist exhausted"; break; }
    n=$(printf '%s\n' "$left" | wc -l)
    echo "=== batch of $n; $(( $(wc -l < worklist.txt) - $(wc -l < /tmp/fin-done.txt) )) archives left"
    # shellcheck disable=SC2086
    ./tick.sh $left 2>&1 | grep -E "^(FETCH|UNPACK|SCAN_ERROR|candidates|lvo-usage|lvo-rare)|\| OK" | tail -12
    echo "=== ledger now $(wc -l < results.tsv) rows, $(cut -f1 results.tsv | sort -u | wc -l) archives"
done
