"""Yield per Aminet directory: where the remaining survey is worth spending.

    tools/aminet-survey/yield.py [data-dir]

pick.sh weights the draw by directory, and those weights were hand-set from a
sample of 299 archives.  This recomputes them from the whole ledger, so the
picker can be aimed at what the data says rather than at what was true early.

ATTRIBUTION IS PER ARCHIVE, NOT PER ROW.  An archive with nine binaries is one
draw from the picker's point of view, and counting its rows would rank a
directory by how many files its archives happen to contain.

`left` is what the worklist still holds for that directory.  A 0% directory
with hundreds left is the useful output: comm/ambos, comm/maxs and comm/cnet
are BBS and door software with no TCP in them, 0 of 70 sampled, 793 archives
still queued.  Spending ticks there is spending them on NO_BSDSOCKET_BINARY.

SPDX-License-Identifier: MIT
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_io

BASE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', '..', 'docs', 'aminet-survey')

seen, hit = {}, {}
for f in survey_io.rows(f'{BASE}/results.tsv', want_ok=False):
    # The archive's directory comes from its recorded path (column 8); rows
    # written before that column exists fall back to unknown rather than
    # guessing, so they cannot inflate a real directory's denominator.
    path = f[7] if len(f) > 7 else ''
    d = path.rsplit('/', 1)[0] if '/' in path else '(no path recorded)'
    seen.setdefault(d, set()).add(f[0])
    if f[2].startswith('OK'):
        hit.setdefault(d, set()).add(f[0])

total = collections.Counter()
try:
    for line in survey_io.lines(f'{BASE}/worklist.txt'):
        if '/' in line:
            total[line.rsplit('/', 1)[0]] += 1
except OSError:
    pass

print("directory\tscanned\tattributed\tyield_pct\tleft")
for d in sorted(seen, key=lambda x: (-len(hit.get(x, ())), -len(seen[x]))):
    s, a = len(seen[d]), len(hit.get(d, ()))
    print(f"{d}\t{s}\t{a}\t{100 * a // s if s else 0}\t{max(0, total.get(d, 0) - s)}")
