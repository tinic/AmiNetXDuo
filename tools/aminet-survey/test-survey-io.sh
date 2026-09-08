#!/bin/sh
#
# THE LEDGER IS NOT UTF-8.  Regression fixture for the byte that proved it.
#
#   tools/aminet-survey/test-survey-io.sh
#
# Spitfire2.lha ships a drawer named `Spitfire<b2> Install` -- 0xb2, superscript
# two in Latin-1 -- and that single byte raised UnicodeDecodeError in every
# generator that read the ledger.  Because each was invoked as
# `python3 gen.py > table.tsv`, the shell had already truncated the published
# table to nothing before python failed, and the tick printed TICK_RC=0.
#
# The fixture carries the byte itself rather than a description of it, so this
# stays a test of the decoder and not of a comment.  It also asserts the
# NEGATIVE -- that a plain utf-8 read still fails on the same fixture -- so the
# test cannot quietly stop testing anything if the data is ever sanitised.
#
# SPDX-License-Identifier: MIT

set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
fail=0

# The fixture: header, one row whose path holds the raw 0xb2, one clean row.
printf 'archive\tfile\tverdict\tdistinct\tcalls\tlvos\n' > "$T/results.tsv"
printf 'Spitfire2.lha\tSpitfire\262 Install/x.library\tOK\tdistinct=1\tcalls=1\tvsyslog\n' >> "$T/results.tsv"
printf 'clean.lha\tclean/bin/foo\tOK\tdistinct=1\tcalls=1\tvsyslog,socket\n' >> "$T/results.tsv"
cp "$ROOT/docs/aminet-survey/lvomap.tsv" "$T/lvomap.tsv"

# The negative: a default utf-8 read must still blow up on this fixture.
if python3 -c "
import sys
for _ in open(sys.argv[1]): pass
" "$T/results.tsv" 2>/dev/null; then
    echo "survey_io=FAIL fixture no longer contains a non-utf-8 byte"
    fail=1
fi

# The positive: every generator reads it, and the byte survives round trip.
got=$(python3 "$ROOT/tools/aminet-survey/callers.py" vsyslog "$T" | wc -l)
[ "$got" = "2" ] || { echo "survey_io=FAIL callers.py read $got rows, want 2"; fail=1; }

python3 "$ROOT/tools/aminet-survey/usage.py" "$T" "$T/lvo-usage.tsv" \
    || { echo "survey_io=FAIL usage.py died on the fixture"; fail=1; }
n=$(awk -F'\t' '$2=="vsyslog"{print $3}' "$T/lvo-usage.tsv")
[ "$n" = "2" ] || { echo "survey_io=FAIL usage.py counted vsyslog=$n, want 2"; fail=1; }

python3 "$ROOT/tools/aminet-survey/rare.py" 10 "$T" "$T/lvo-rare.tsv" \
    || { echo "survey_io=FAIL rare.py died on the fixture"; fail=1; }
grep -q 'Spitfire' "$T/lvo-rare.tsv" \
    || { echo "survey_io=FAIL rare.py lost the latin-1 row"; fail=1; }
python3 -c "
import sys
b = open(sys.argv[1], 'rb').read()
sys.exit(0 if b'Spitfire\xb2 Install' in b else 1)
" "$T/lvo-rare.tsv" || { echo "survey_io=FAIL the 0xb2 did not round trip"; fail=1; }

python3 "$ROOT/tools/aminet-survey/candidates.py" "$T" "$T/candidates.tsv" \
    || { echo "survey_io=FAIL candidates.py died on the fixture"; fail=1; }

# A generator that dies must leave the previous table alone, never truncate it.
printf 'previous good table\n' > "$T/keep.tsv"
python3 -c "
import os, sys
sys.path.insert(0, os.path.join(sys.argv[1], 'tools', 'aminet-survey'))
import survey_io
try:
    with survey_io.out(sys.argv[2]) as fh:
        print('half a table', file=fh)
        raise RuntimeError('died mid-write')
except RuntimeError:
    pass
" "$ROOT" "$T/keep.tsv"
[ "$(cat "$T/keep.tsv")" = "previous good table" ] \
    || { echo "survey_io=FAIL a dying generator clobbered the published table"; fail=1; }
[ -z "$(find "$T" -name '*.tmp' -print -quit)" ] \
    || { echo "survey_io=FAIL temporary file left behind"; fail=1; }

[ "$fail" -eq 0 ] || exit 1
echo "survey_io=PASS latin-1 ledger, atomic tables"
