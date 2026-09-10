"""Which programs call a given vector.

    tools/aminet-survey/callers.py vsyslog [data-dir]

A caller count decides nothing on its own.  `vsyslog` at 155 is the daemon
suite -- inetd, ftpd, in.smtpd, netfs-server -- and that is what makes it a
compatibility gap rather than a statistic; the same 155 spread over unrelated
one-off tools would mean something else entirely.  So the names are
regenerable for any vector, not just the ones someone thought to save.

Emits `program<TAB>archive`, deduplicated, sorted.  Program is the binary's
basename: the same program shipped in several distributions is several rows,
which is deliberate -- who ships it is part of the evidence.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_io

if len(sys.argv) < 2:
    sys.exit("usage: callers.py <vector> [data-dir]")

VECTOR = sys.argv[1]
BASE = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', '..', 'docs', 'aminet-survey')

DEST = sys.argv[3] if len(sys.argv) > 3 else None

rows = set()
for f in survey_io.rows(os.path.join(BASE, 'results.tsv')):
    if VECTOR in f[5].split(','):
        rows.add((f[1].rsplit('/', 1)[-1], f[0]))

with survey_io.out(DEST) as fh:
    for prog, arc in sorted(rows):
        print(f"{prog}\t{arc}", file=fh)
