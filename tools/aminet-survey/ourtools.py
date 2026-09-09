"""Which vectors OUR OWN shipped tools consume: own-tools.tsv.

    tools/aminet-survey/ourtools.py [data-dir] [destination]

The survey answers "does Aminet software call this vector".  That is only half
of what a removal decision needs, and the missing half is not in the ledger at
all: a vector with zero Aminet callers can still be load-bearing for the tools
WE ship.  `getaddrinfo` has no attributed caller in 5,907 archives and
`tools/nc.c`, `tools/host.c`, `tools/censusprobe.c` and `tools/toolsock.c` all
call it.

SCOPE IS src/tools AND src/config ONLY, and that is the whole trick.  Every
vector name appears in `src/bsdsocket/bsdsocket_vectors.c` because that file IS
the table, and each has an implementation beside it -- so grepping `src/`
reports all 143 as "used by us" and means nothing.  A first version of this
check did exactly that and said 52 of 52.  Consumers, not definitions.

The match is a word-boundary grep, so it counts a mention rather than proving
a call: a name in a comment counts.  That is the safe direction for this
question -- over-reporting a dependency delays a removal, under-reporting one
breaks nslookup.

SPDX-License-Identifier: MIT
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import survey_io

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
BASE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'docs', 'aminet-survey')
DEST = sys.argv[2] if len(sys.argv) > 2 else None
DIRS = ['src/tools/', 'src/config/']

rows = [l.split('\t') for l in survey_io.lines(f'{BASE}/lvo-usage.tsv')][1:]

# One grep for every name at once; -o with a name alternation is far cheaper
# than 143 subprocesses and gives file:name pairs directly.
names = [r[1] for r in rows if r[1] != 'reserved']
pat = r'\b(' + '|'.join(re.escape(n) for n in names) + r')\b'
found = {}
for d in DIRS:
    p = os.path.join(ROOT, d)
    if not os.path.isdir(p):
        continue
    # `-h` would hide which file matched, and the file is how a TEST is told
    # from a consumer: RemoveDomainNameServer's only mention under src/config
    # is in src/config/test/test_config.c, and counting that reports a shipped
    # dependency that does not exist.
    out = subprocess.run(['grep', '-rEo', '--include=*.c', pat, p],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        path, _, tok = line.partition(':')
        if '/test/' in path or os.path.basename(path).startswith('test_'):
            continue
        found[tok] = found.get(tok, 0) + 1

with survey_io.out(DEST) as fh:
    print("offset\tlvo\taminet_callers\tour_tool_mentions", file=fh)
    for off, name, n in rows:
        if name == 'reserved':
            continue
        print(f"{off}\t{name}\t{n}\t{found.get(name, 0)}", file=fh)
