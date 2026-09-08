"""Every vector as USED / NO_CALLER / RESERVED: unused-vectors.tsv.

    tools/aminet-survey/unused.py [data-dir] [destination]

The README called this file derived and nothing regenerated it, so it was
checked by no gate at all -- the same gap that let the other published tables
drift from the ledger, and the reason check-derived.sh exists.

RESERVED is not a survey result.  Those offsets are named `reserved` in
bsdsocket_vectors.c, so no caller is expected and their zero says nothing;
folding them in with NO_CALLER would inflate "nothing calls this" with 18 rows
that were never callable.  That distinction is the whole point of the file.

SPDX-License-Identifier: MIT
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_io

BASE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', '..', 'docs', 'aminet-survey')
DEST = sys.argv[2] if len(sys.argv) > 2 else None

with survey_io.out(DEST) as fh:
    print("offset\tname\tstatus", file=fh)
    for line in survey_io.lines(f'{BASE}/lvo-usage.tsv'):
        p = line.split('\t')
        if p[0] == 'offset':
            continue
        off, name, n = p[0], p[1], int(p[2])
        status = 'RESERVED' if name == 'reserved' else ('USED' if n else 'NO_CALLER')
        print(f"{off}\t{name}\t{status}", file=fh)
