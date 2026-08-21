#!/usr/bin/env bash
#
# Do the backlog's citations still point at anything?
#
#   tools/check-backlog.sh
#
# Every row in docs/BACKLOG.md ends with a citation column naming the files the
# row is about, and usually a line inside one of them.  Those rot: a file moves
# and the row goes on naming where it used to be.  `rfbbench` cannot price the
# banded path still cited tests/perf/rfbbench.c months after it became
# src/rfb/host/rfbbench.c, and the row was otherwise correct -- the defect it
# describes is still there.  Somebody reading it would have gone looking in a
# directory that does not have the file.
#
# WHAT THIS CANNOT DO is tell you a row is stale in the way that matters most:
# that the defect was fixed and nobody retired the row.  Four of those turned
# up in one afternoon -- a WebSocket close payload fixed on 10 August, a $VER:
# hash fixed on 17 August, a struct timeval collision that was still keeping
# two files out of the host tier a month after it was resolved, and an
# IPv6-only interface refusal that had a dedicated harness proving otherwise on
# every push.  No script finds those.  A citation that does not resolve is the
# cheap half, and it is worth having because it is free.
#
# key=value and an exit code, like every other gate here.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 2

BACKLOG="docs/BACKLOG.md"
[ -f "$BACKLOG" ] || { echo "no $BACKLOG" >&2; exit 2; }

python3 - "$BACKLOG" <<'PY'
import os, re, sys

path = sys.argv[1]
rows = broken = 0

for line in open(path, encoding='utf-8'):
    if not line.startswith('| ') or line.startswith('| Item') or set(line.strip()) <= set('|- '):
        continue
    parts = line.split('|')
    if len(parts) < 4:
        continue
    rows += 1
    title = parts[1].strip()[:60]

    for cite in re.findall(r'`([^`]+)`', parts[3]):
        cite = cite.strip()
        m = re.match(r'^([\w./+-]+?)(?::(\d+))?$', cite)
        if not m:
            continue
        f, ln = m.group(1), m.group(2)

        # Only things that look like a path in this repo.  A bare identifier is
        # a symbol name, and a path under third_party is inside a submodule
        # whose checkout state is not this script's business.
        if '/' not in f or f.startswith('third_party/'):
            continue
        # A directory is a legitimate citation for "somewhere in here".
        if os.path.isdir(f):
            continue

        if not os.path.exists(f):
            broken += 1
            print("backlog_broken_cite=%s reason=file_gone row=%r" % (cite, title))
            continue

        if ln:
            with open(f, errors='ignore') as fh:
                n = sum(1 for _ in fh)
            if int(ln) > n:
                broken += 1
                print("backlog_broken_cite=%s reason=line_past_eof lines=%d row=%r"
                      % (cite, n, title))

print("backlog_rows=%d" % rows)
print("backlog_broken_cites=%d" % broken)
print("check_backlog=%s" % ("FAIL" if broken else "PASS"))
sys.exit(1 if broken else 0)
PY
