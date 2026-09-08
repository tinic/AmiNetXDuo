"""Vectors with fewer than N callers, WITH the callers named.

A count of 3 is undecidable on its own: three obscure demos and three
widely-used daemons are the same number and opposite decisions.  Below the
threshold the archive and binary are listed so each one can be judged.

Threshold is deliberately 10 and not 5 -- the user set it there so the
borderline cases are visible rather than just the desperate ones.
"""
import collections, sys

THRESHOLD = int(sys.argv[1]) if len(sys.argv) > 1 else 10
BASE = '/home/turo/anxd-aminet'

callers = collections.defaultdict(list)
for line in open(f'{BASE}/results.tsv'):
    f = line.rstrip('\n').split('\t')
    if len(f) < 6 or f[0] == 'archive' or not f[2].startswith('OK'):
        continue
    for n in f[5].split(','):
        if n and not n.startswith('?'):
            callers[n].append((f[0], f[1]))

rows = []
for line in open(f'{BASE}/lvo-usage.tsv'):
    p = line.rstrip('\n').split('\t')
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
print("offset\tlvo\tcallers\tprograms\tarchive\tbinary")
for n, off, name in rows:
    uniq = sorted(set(callers[name]))
    progs = len({b.rsplit('/', 1)[-1] for _a, b in uniq})
    for arc, bin_ in uniq:
        print(f"{off}\t{name}\t{n}\t{progs}\t{arc}\t{bin_}")
