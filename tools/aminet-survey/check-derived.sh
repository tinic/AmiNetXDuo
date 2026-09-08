#!/bin/sh
#
# The published tables really are derivable from the raw ledger.
#
#   tools/aminet-survey/check-derived.sh
#
# docs/aminet-survey/README.md says lvo-usage.tsv and lvo-rare.tsv are derived
# from results.tsv plus lvomap.tsv.  That is a claim about the data, so it is
# checked here rather than believed: both are regenerated into a temporary file
# and compared byte for byte with what is committed.
#
# It catches the two ways the published set goes wrong -- a derived file
# updated while the ledger it came from was not, and a ledger updated while the
# derived files were left behind.  Either makes every number downstream a
# statement about a dataset that no longer exists.
#
# SPDX-License-Identifier: MIT

set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 2
D=docs/aminet-survey
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT

for f in results.tsv lvomap.tsv lvo-usage.tsv lvo-rare.tsv; do
    [ -r "$D/$f" ] || { echo "check_derived=FAIL missing $D/$f"; exit 2; }
done

python3 - "$D" "$T" <<'PY'
import collections, sys
d, t = sys.argv[1], sys.argv[2]
cnt = collections.Counter()
for line in open(f'{d}/results.tsv'):
    f = line.rstrip('\n').split('\t')
    if len(f) < 6 or f[0] == 'archive' or not f[2].startswith('OK'):
        continue
    for n in f[5].split(','):
        if n and not n.startswith('?'):
            cnt[n] += 1
with open(f'{t}/lvo-usage.tsv', 'w') as out:
    out.write("offset\tlvo\tbinaries\n")
    for line in open(f'{d}/lvomap.tsv'):
        p = line.rstrip('\n').split('\t')
        if p[0] != 'offset':
            out.write(f"{p[1]}\t{p[3]}\t{cnt.get(p[3], 0)}\n")
PY
[ $? -eq 0 ] || { echo "check_derived=FAIL regenerating lvo-usage"; exit 2; }

if ! diff -q "$T/lvo-usage.tsv" "$D/lvo-usage.tsv" > /dev/null 2>&1; then
    echo "check_derived=FAIL lvo-usage.tsv does not match results.tsv"
    diff "$D/lvo-usage.tsv" "$T/lvo-usage.tsv" | head -8
    exit 1
fi

# lvo-rare.tsv, regenerated the same way and compared the same way.  Counting
# its rows -- which is all this did at first -- is not checking it: a stale
# lvo-rare.tsv has exactly the row count it always had.  A gate that reports on
# a file it did not regenerate is worse than one that says nothing about it.
python3 tools/aminet-survey/rare.py 10 "$D" > "$T/lvo-rare.tsv" 2>/dev/null \
    || { echo "check_derived=FAIL regenerating lvo-rare"; exit 2; }

if ! diff -q "$T/lvo-rare.tsv" "$D/lvo-rare.tsv" > /dev/null 2>&1; then
    echo "check_derived=FAIL lvo-rare.tsv does not match results.tsv"
    diff "$D/lvo-rare.tsv" "$T/lvo-rare.tsv" | head -8
    exit 1
fi

rows=$(awk -F'\t' 'NR>1' "$D/lvo-usage.tsv" | wc -l)
called=$(awk -F'\t' 'NR>1 && $3>0' "$D/lvo-usage.tsv" | wc -l)
rare=$(awk -F'\t' 'NR>1 && !s[$2]++' "$D/lvo-rare.tsv" | wc -l)
echo "check_derived vectors=$rows called=$called rare_under_10=$rare"
echo "check_derived regenerated=lvo-usage.tsv,lvo-rare.tsv"
echo "check_derived=PASS"
