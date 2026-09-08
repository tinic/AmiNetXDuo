#!/bin/bash
# One survey tick: fetch N archives, scan every HUNK executable in each.
# Appends TSV rows; dedups by archive name.
set -u
OUT=${ANXD_SURVEY_OUT:-/tmp/anxd-survey}
LEDGER=/home/turo/anxd-aminet/results.tsv
[ -f "$LEDGER" ] || printf 'archive\tfile\tverdict\tdistinct\tcalls\tlvos\n' > "$LEDGER"
# PICKING NOTHING IS NOT SUCCESS.  The caller used to build the list with
# `grep -vFf done.txt`, and `cut -f1 results.tsv` emits the header and an empty
# line -- an empty pattern in -f matches EVERY line, so the exclusion list
# excluded the whole worklist and the tick reported TICK_RC=0 having scanned
# zero archives.  Same shape as every other silent pass found today.
[ "$#" -gt 0 ] || { echo "tick: no archives given, nothing to do" >&2; exit 2; }

for path in "$@"; do
    name=$(basename "$path")
    if cut -f1 "$LEDGER" | grep -qx "$name"; then
        echo "SKIP already scanned: $name"; continue
    fi
    if ! /home/turo/anxd-aminet/fetch.sh "$path" "$OUT" > /dev/null 2>&1; then
        printf '%s\t-\tFETCH_FAIL\t0\t0\t\n' "$name" >> "$LEDGER"
        echo "FETCH_FAIL $name"; continue
    fi
    found=0
    # -print0 / read -r, NOT `for f in $(find ...)`.  Word splitting broke every
    # path with a space in it -- "Update STFax.info" became two nonexistent
    # arguments, scan.py printed nothing, and the EMPTY output was written to
    # the ledger as a row.  36 blank rows accumulated that way, and a blank row
    # reads as data.  Files with spaces were also never actually scanned.
    while IFS= read -r -d '' f; do
        row=$(python3 scan.py "$f" 2>/dev/null)
        [ -n "$row" ] || { echo "SCAN_ERROR $f" >&2; continue; }
        case "$row" in
            *NO_BSDSOCKET_STRING*|*NO_HUNK*) continue ;;
        esac
        # The path RELATIVE to the archive, not the basename.  AmiVNC ships
        # AmiVNC.060 twice -- Executables/Planar/ and "Executables/RTG &
        # Planar/" -- and two different binaries recorded under one name read
        # as a duplicated row.  (That directory also has a space and an
        # ampersand in it, which is why the null-delimited find matters.)
        rel=${f#"$OUT/$name.d/"}
        row=${row#*$'\t'}
        printf '%s\t%s\t%s\n' "$name" "$rel" "$row" >> "$LEDGER"
        echo "$name | $row" | cut -c1-150
        found=$((found+1))
    done < <(find "$OUT/$name.d" -type f -size +1k -print0)
    [ "$found" = 0 ] && printf '%s\t-\tNO_BSDSOCKET_BINARY\t0\t0\t\n' "$name" >> "$LEDGER"
done

# RUNNING TALLY OF HARNESS CANDIDATES, regenerated every tick so it cannot go
# stale.  tests/HARNESSES drives our own tools, LhA and a Workbench install and
# contains NO third-party network application, so everything here is net-new
# coverage.  Greedy set cover over the 143 vectors: the question a harness asks
# is "which handful covers the most API", not "which program is biggest".
python3 /home/turo/anxd-aminet/candidates.py \
    > /home/turo/anxd-aminet/candidates.tsv 2>/dev/null
echo "candidates: $(tail -1 /home/turo/anxd-aminet/candidates.tsv)"

# Per-LVO usage counts, regenerated every tick alongside the candidates.
python3 - <<'PY' > /home/turo/anxd-aminet/lvo-usage.tsv
import collections
cnt = collections.Counter()
for line in open('/home/turo/anxd-aminet/results.tsv'):
    f = line.rstrip('\n').split('\t')
    if len(f) < 6 or f[0] == 'archive' or not f[2].startswith('OK'):
        continue
    for n in f[5].split(','):
        if n and not n.startswith('?'):
            cnt[n] += 1
print("offset\tlvo\tbinaries")
for line in open('/home/turo/anxd-aminet/lvomap.tsv'):
    p = line.rstrip('\n').split('\t')
    if p[0] != 'offset':
        print(f"{p[1]}\t{p[3]}\t{cnt.get(p[3], 0)}")
PY
echo "lvo-usage: $(awk -F'\t' 'NR>1 && $3>0' /home/turo/anxd-aminet/lvo-usage.tsv | wc -l) of 143 vectors have a caller"

# Vectors with fewer than 10 callers, WITH the callers named -- a count alone
# cannot decide those, and the distinct-program column separates "three
# daemons in three distributions" from "nine unrelated programs".
python3 /home/turo/anxd-aminet/rare.py 10 \
    > /home/turo/anxd-aminet/lvo-rare.tsv 2>/dev/null
echo "lvo-rare: $(awk -F'\t' 'NR>1 && !s[$2]++' /home/turo/anxd-aminet/lvo-rare.tsv | wc -l) vectors under 10 callers"
