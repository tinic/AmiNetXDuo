#!/usr/bin/env bash
#
# The two diagnostic mechanisms are where they belong.
#
#   tools/check-no-diag-strings.sh <build-dir>
#
# THE EVENT RING'S sentences must be OUTSIDE every shipped library and device.
# The ring is a code, an interface and a value; the words are in the command
# that prints them, src/tools/tool_events.c.  A user with no serial capture
# reads it with ShowNetStatus, which is why it may not depend on anything the
# library says.  The first scan below fails on a table sentence found in an
# image.
#
# THE ami_log() TIER must match AMINETXDUO_LOG as the build was configured.
# Off -- every shipping build -- bsdsocket.library must hold NONE of those
# sentences: it stays resident and they are 27,948 bytes of it.  On, it must
# hold some, or the bug-report build captures nothing.  The second scan reads
# the answer out of CMakeCache.txt and fails either way round.
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

# ----------------------------------------- and the tier that ships on request
#
# Every AMI_ERROR/AMI_WARN/AMI_INFO format string in the tree, and how many are
# in bsdsocket.library.  The assertion is against AMINETXDUO_LOG as the build
# was CONFIGURED, read from the cache rather than assumed, because both answers
# are a defect in the other build: sentences in a shipping library are 27,948
# resident bytes nobody asked for, and none in a -DAMINETXDUO_LOG=ON build is
# the option silently doing nothing.
#
# The count is not the assertion either way -- a sentence belongs to whichever
# component it is written in, and -Os and LTO drop the ones on paths the linker
# proved unreachable.  Zero against not-zero is.

LIB="$BUILD/src/bsdsocket/bsdsocket.library"
CACHE="$BUILD/CMakeCache.txt"

WANT=OFF
if [ -r "$CACHE" ]; then
    WANT=$(sed -n 's/^AMINETXDUO_LOG:BOOL=//p' "$CACHE" | head -1)
    WANT=${WANT:-OFF}
fi

if [ ! -f "$LIB" ]; then
    echo "diag_tier=skipped reason=no_library image=$LIB"
    exit "$rc"
fi

TIER=$(cd "$ROOT" && MINLEN="$MINLEN" python3 - <<'TIERPY'
import os, re, subprocess

# The first argument of every AMI_ERROR/AMI_WARN/AMI_INFO call, with C's
# adjacent-literal concatenation applied.  A call whose format is not a literal
# is skipped; there are none, and one would carry no sentence anyway.
CALL = re.compile(r'\bAMI_(?:ERROR|WARN|INFO)\s*\(\s*'
                  r'((?:"(?:[^"\\\n]|\\.)*"\s*)+)')
LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')

out = set()
listed = subprocess.run(["git", "ls-files", "src"],
                        capture_output=True, text=True).stdout.split()
for path in listed:
    if not path.endswith(".c"):
        continue
    try:
        src = open(path, encoding='utf-8', errors='replace').read()
    except OSError:
        continue
    for m in CALL.finditer(src):
        body = "".join(LIT.findall(m.group(1)))
        out.add(body.encode('utf-8').decode('unicode_escape'))

minlen = int(os.environ['MINLEN'])
for s in sorted(out):
    if len(s) >= minlen:
        print(s)
TIERPY
)

total=0
found=0
while IFS= read -r s; do
    [ -n "$s" ] || continue
    total=$((total + 1))
    if LC_ALL=C grep -qaF -- "$s" "$LIB"; then
        found=$((found + 1))
    fi
done <<< "$TIER"

if [ "$total" = 0 ]; then
    echo "diag_tier=skipped reason=no_sentences_in_source"
    exit "$rc"
fi

case "$WANT" in
    ON|on|1|TRUE|true|YES|yes)
        if [ "$found" = 0 ]; then
            echo "diag_tier=MISSING found=0 of=$total image=bsdsocket.library"
            echo "  AMINETXDUO_LOG=ON and the library holds not one sentence,"
            echo "  so the option compiled nothing in and a bug-report build"
            echo "  would capture nothing."
            rc=1
        else
            echo "diag_tier=present log=ON found=$found of=$total image=bsdsocket.library"
        fi
        ;;
    *)
        if [ "$found" != 0 ]; then
            echo "diag_tier=LEAKED found=$found of=$total image=bsdsocket.library"
            echo "  AMINETXDUO_LOG is off and the library still carries"
            echo "  $found sentence(s).  bsdsocket.library stays resident, so"
            echo "  these are bytes held for the life of the machine.  A call"
            echo "  reaching ami_log() outside AMI_ERROR/AMI_WARN/AMI_INFO is"
            echo "  what does it."
            rc=1
        else
            echo "diag_tier=absent log=OFF of=$total image=bsdsocket.library"
        fi
        ;;
esac

exit "$rc"
