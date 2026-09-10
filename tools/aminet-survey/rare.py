"""Vectors with fewer than N callers, WITH the callers named.

A count of 3 is undecidable on its own: three obscure demos and three
widely-used daemons are the same number and opposite decisions.  Below the
threshold the archive and binary are listed so each one can be judged.

Threshold is deliberately 10 and not 5 -- the user set it there so the
borderline cases are visible rather than just the desperate ones.
"""
import collections, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_io

THRESHOLD = int(sys.argv[1]) if len(sys.argv) > 1 else 10
# Data directory, not a hardcoded home.  Defaults to the repo's own copy so the
# tool works from a checkout with no external state.
BASE = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', '..', 'docs', 'aminet-survey')

DEST = sys.argv[3] if len(sys.argv) > 3 else None

callers = collections.defaultdict(list)
for f in survey_io.rows(f'{BASE}/results.tsv'):
    for n in f[5].split(','):
        if n and not n.startswith('?'):
            callers[n].append((f[0], f[1]))

rows = []
for line in survey_io.lines(f'{BASE}/lvo-usage.tsv'):
    p = line.split('\t')
    if p[0] == 'offset' or p[1] == 'reserved':
        continue
    n = int(p[2])
    if 0 < n < THRESHOLD:
        rows.append((n, p[0], p[1]))
rows.sort()

# DISTINCT PROGRAMS, not just caller count.  A count of 9 that is `inetd`,
# `letnet` and `rsh` shipped in three distributions is three programs and the
# inetd family; a count of 9 distinct programs is something else entirely.
# The bare number cannot tell them apart, so both are printed.
with survey_io.out(DEST) as fh:
    print("offset\tlvo\tcallers\tprograms\tarchive\tbinary", file=fh)
    for n, off, name in rows:
        uniq = sorted(set(callers[name]))
        progs = len({b.rsplit('/', 1)[-1] for _a, b in uniq})
        for arc, bin_ in uniq:
            print(f"{off}\t{name}\t{n}\t{progs}\t{arc}\t{bin_}", file=fh)
