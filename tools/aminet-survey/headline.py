"""The README's headline figures, regenerated from the ledger.

    tools/aminet-survey/headline.py [data-dir] [destination]

README.md quoted "1,700 archives" and "~55% attribution" long after the corpus
was finished at 5,907 -- numbers written mid-survey and then left, in the one
file a reader trusts for context.  check-derived.sh proves every TABLE matches
the ledger; nothing proved the prose did.

The block this emits is fenced in README.md between the two markers below, so
the gate can regenerate and diff it exactly as it does the tables.

ATTRIBUTION IS PER ARCHIVE AND THE DENOMINATOR IS NOT THE CORPUS.  5,907
archives were scanned but 5,350 hold no file that so much as names
bsdsocket.library, and counting those in the denominator would report 5%
attribution and mean nothing.  The honest figure is archives holding a
bsdsocket binary that we resolved: 312 of 557.

SPDX-License-Identifier: MIT
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_io

BASE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', '..', 'docs', 'aminet-survey')
DEST = sys.argv[2] if len(sys.argv) > 2 else None

allr = list(survey_io.rows(f'{BASE}/results.tsv', want_ok=False))
ok = [f for f in allr if f[2].startswith('OK')]
archives = {f[0] for f in allr}
with_bin = {f[0] for f in allr if not f[2].startswith('NO_BSDSOCKET_BINARY')}
attributed = {f[0] for f in ok}

usage = [l.split('\t') for l in survey_io.lines(f'{BASE}/lvo-usage.tsv')][1:]
called = sum(1 for r in usage if int(r[2]) > 0)
reserved = sum(1 for r in usage if r[1] == 'reserved')
zero = sum(1 for r in usage if int(r[2]) == 0 and r[1] != 'reserved')

try:
    worklist = sum(1 for _ in survey_io.lines(f'{BASE}/worklist.txt'))
except OSError:
    worklist = 0

with survey_io.out(DEST) as fh:
    w = lambda s: print(s, file=fh)
    w(f"| corpus | {worklist} archive paths, {len(archives)} scanned |")
    w("|---|---|")
    w(f"| archives holding a file that names `bsdsocket.library` | {len(with_bin)} |")
    w(f"| of those, archives we ATTRIBUTED | {len(attributed)} "
      f"({100 * len(attributed) // len(with_bin)}%) |")
    w(f"| attributed binaries | {len(ok)} |")
    w(f"| vectors with at least one caller | {called} of {len(usage)} |")
    w(f"| vectors with no caller, excluding reserved | {zero} |")
    w(f"| reserved slots, never callable | {reserved} |")
