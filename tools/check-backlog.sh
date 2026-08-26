#!/usr/bin/env bash
#
# Do the backlog's citations still point at anything?
#
#   tools/check-backlog.sh
#
# Cannot tell that a row's defect was fixed and nobody retired the row; a
# citation that does not resolve is only the cheap half.
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

# Lines that carry no subject, so a cite landing on one names nothing.
NOISE = {'{', '}', '};', ')', ');', '*/', '/*', 'fi', 'esac', 'done', 'else',
         'break;', 'continue;', 'return;', 'do', 'then', '#endif', '#else',
         'set -uo pipefail', 'set -eu', 'EOF', 'PY', '"""'}
NOISE_RE = re.compile(
    r'^(\*|//|;)'                       # C block continuation, C++ or asm comment
    r'|^#\s*(SPDX|!)'                   # licence header, shebang
    r'|^#(?!\s*(define|include|if|ifdef|ifndef|elif|pragma|error|undef))\s'
    r'|^\}\s*(else|while)?')            # shell comment, but not a cpp directive

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
                lines = fh.read().split('\n')
            n = len(lines)
            if int(ln) > n:
                broken += 1
                print("backlog_broken_cite=%s reason=line_past_eof lines=%d row=%r"
                      % (cite, n, title))
                continue

            # A cite that slides onto a live but meaningless line passes an
            # end-of-file check silently.  25 of 25 in-range cites were wrong
            # after one comment sweep, every one of them still in range.
            text = lines[int(ln) - 1].strip()
            if not text or text in NOISE or NOISE_RE.match(text):
                broken += 1
                print("backlog_broken_cite=%s reason=no_anchor line=%r row=%r"
                      % (cite, text[:40], title))

print("backlog_rows=%d" % rows)
print("backlog_broken_cites=%d" % broken)
print("check_backlog=%s" % ("FAIL" if broken else "PASS"))
sys.exit(1 if broken else 0)
PY
