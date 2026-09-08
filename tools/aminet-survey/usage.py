"""Per-vector caller counts: lvo-usage.tsv.

    tools/aminet-survey/usage.py [data-dir] [destination]

ONE generator, called by both the tick that publishes the table and the gate
that checks it.  It used to be two copies of the same fifteen lines -- an
inline heredoc in tick.sh and another in check-derived.sh -- which is a gate
that can only catch a stale file, never a wrong derivation: change the rule in
one copy and the gate agrees with the tick that the wrong answer is correct.

Every vector in lvomap.tsv gets a row, including the ones nobody calls.  A zero
is a finding here -- it is the whole basis of the micro-build question -- so it
has to be a printed row and not an absence.

SPDX-License-Identifier: MIT
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_io

BASE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', '..', 'docs', 'aminet-survey')
DEST = sys.argv[2] if len(sys.argv) > 2 else None

cnt = collections.Counter()
for f in survey_io.rows(f'{BASE}/results.tsv'):
    for n in f[5].split(','):
        # `?` prefixes an offset the scanner could not resolve to a name.  It
        # is evidence that a call happened, not evidence about which vector,
        # so it must not land on any vector's count.
        if n and not n.startswith('?'):
            cnt[n] += 1

with survey_io.out(DEST) as fh:
    print("offset\tlvo\tbinaries", file=fh)
    for line in survey_io.lines(f'{BASE}/lvomap.tsv'):
        p = line.split('\t')
        if p[0] != 'offset':
            print(f"{p[1]}\t{p[3]}\t{cnt.get(p[3], 0)}", file=fh)
