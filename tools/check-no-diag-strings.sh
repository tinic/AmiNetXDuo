#!/usr/bin/env bash
#
# No diagnostic sentence may be inside a shipped library or device.
#
#   tools/check-no-diag-strings.sh <build-dir>
#
# THE CONSTRAINT THIS HOLDS.  bsdsocket.library is resident for the life of the
# machine, anxnet.device may be the only image of ours on a machine at all, and
# both are size-sensitive to the byte.  That is why AMINETXDUO_LOG is off in
# every shipped build, and why the event ring (aminetxduo/events.h) records
# numbers: the words that go with them live in src/tools/tool_events.c, which
# is a Shell command, loaded to print once and unloaded again.
#
# A rule of that shape erodes.  Somebody adds one message, to one path, in one
# library, because one message is nothing.  This is what says otherwise.  It
# takes every sentence out of the decode table and looks for it in every linked
# image, so a sentence added to the table extends the check by itself and there
# is no second list to keep in step.
#
# EVERY SHIPPED IMAGE IS NAMED, not the one the ring is read from.  A message
# added to anxnet.device would be just as resident, and a check that looked
# only at the library publishing the ring would never see it.
#
# Output is key=value and an exit code: 0 clean, 1 a sentence is in an image,
# 2 nothing to check.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-}"

[ -n "$BUILD" ] || { echo "usage: check-no-diag-strings.sh <build-dir>" >&2; exit 2; }

TABLE="$ROOT/src/tools/tool_events.c"

if [ ! -f "$TABLE" ]; then
    echo "diag_strings=skipped reason=no_table table=$TABLE"
    exit 2
fi

# Every image that ships and stays in memory.  A name not built in this
# configuration is reported absent rather than passed over: the reason is
# normally an option being off (tls.library under -DAMINETXDUO_TLS=OFF), and a
# reader has to be able to tell that from a build where nothing was linked.
IMAGES="
src/bsdsocket/bsdsocket.library
src/netdev/anxnet.device
src/usergroup/usergroup.library
src/tlslib/tls.library
"

# ------------------------------------------------------------- the sentences,
#
# The string literals out of the table, with C's adjacent-literal
# concatenation applied: a sentence too long for one line is written as two and
# the compiler joins them, so the joined form is what would appear in an image.
# The fragments are looked for as well, because half a sentence in a library is
# still prose in a library.
#
# Comments are stripped first, so a quoted phrase inside one is not mistaken
# for a sentence the table ships.
#
# Anything shorter than MINLEN is dropped.  A short byte sequence occurs in a
# 350 KB binary by accident, and a check that cries wolf is a check somebody
# turns off.  Every sentence in the table is far longer.
MINLEN=24

STRINGS=$(MINLEN="$MINLEN" python3 - "$TABLE" <<'PY'
import os, re, sys

src = open(sys.argv[1], encoding='utf-8', errors='replace').read()

# Comments out, string literals kept: one pass so that a /* inside a literal
# and a " inside a comment are both read the way the compiler reads them.
TOKEN = re.compile(r'"(?:[^"\\\n]|\\.)*"' r"|'(?:[^'\\\n]|\\.)*'"
                   r'|/\*.*?\*/|//[^\n]*', re.S)

pieces = []          # (kind, text, start, end)
for m in TOKEN.finditer(src):
    t = m.group(0)
    if t.startswith('"'):
        pieces.append((m.start(), m.end(), t[1:-1]))

# Adjacent literals: two that have only whitespace between them are one string
# to the compiler.
out = set()
run = None
prev_end = None
for start, end, body in pieces:
    if run is not None and src[prev_end:start].strip() == '':
        run += body
    else:
        if run is not None:
            out.add(run)
        run = body
    prev_end = end
    out.add(body)
if run is not None:
    out.add(run)

minlen = int(os.environ['MINLEN'])
for s in sorted(out):
    # \n and friends as the compiler would emit them, so a sentence written
    # with an escape is looked for in the form it takes in the image.
    s = s.encode('utf-8').decode('unicode_escape')
    if len(s) >= minlen:
        print(s)
PY
)

if [ -z "$STRINGS" ]; then
    echo "diag_strings=skipped reason=no_sentences table=$TABLE"
    exit 2
fi

nstrings=$(printf '%s\n' "$STRINGS" | wc -l | tr -d ' ')

# ------------------------------------------------------------------ the scan,

rc=0
checked=0
absent=""
names=""

for rel in $IMAGES; do
    img="$BUILD/$rel"
    name="${rel##*/}"

    if [ ! -f "$img" ]; then
        absent="$absent,$name"
        continue
    fi

    checked=$((checked + 1))
    names="$names,$name"

    while IFS= read -r s; do
        [ -n "$s" ] || continue
        if LC_ALL=C grep -qaF -- "$s" "$img"; then
            echo "diag_strings=FAILED image=$name"
            echo "  a diagnostic sentence is inside a shipped image:"
            echo "    \"$s\""
            echo "  The words belong in src/tools/tool_events.c and the image"
            echo "  belongs to carry a code.  See aminetxduo/events.h."
            rc=1
        fi
    done <<< "$STRINGS"
done

if [ "$checked" = 0 ]; then
    echo "diag_strings=skipped reason=no_images build=$BUILD"
    exit 2
fi

if [ "$rc" = 0 ]; then
    echo "diag_strings=clean images=$checked sentences=$nstrings\
 scanned=${names#,}${absent:+ absent=${absent#,}}"
fi

exit "$rc"
