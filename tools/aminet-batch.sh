#!/usr/bin/env bash
#
# Run tools/aminet-scan.py over a list of Aminet archives.
#
#   tools/aminet-batch.sh <worklist> <out.tsv> [count] [start]
#
# The worklist is one Aminet path per line, `/comm/tcp/Foo.lha`.  Build one
# with, per directory and walking pages until empty:
#
#   curl -s "http://aminet.net/comm/tcp?count=500&page=N" |
#       grep -o 'href="/package/comm/tcp/[^"]*"' |
#       sed 's|href="/package||;s|"||;s|$|.lha|'
#
# Aminet paginates at fifty whatever `count` says, and the .lha links on a
# listing page are only the visible ones -- the /package/ links are the full
# set.  comm/ is about 5,400 archives.
#
# curl USES -f.  Without it a 404 writes an HTML page to a.lha, `lha` exits 0
# having extracted nothing, and the archive is recorded as carrying no Amiga
# executable -- a missing file reads as a clean negative result.  Six archives
# were mis-recorded that way before it was noticed.
#
# WHAT A ROW MEANS.  `attributed` is the only verdict whose vector list may be
# quoted, and even then it is a floor rather than a total.  `ixemul` is not a
# negative result: those binaries make no bsdsocket call of their own because
# the contract lives inside ixemul.library, and they were 11 of the first 60.
# `packed`, `unresolved` and `no bsdsocket binary` are likewise not evidence of
# absence -- see tests/profiles/aminet-evidence.tsv, where a CLEAN claim is
# refused unless it cites an archive the ledger marks `attributed`.
#
# SPDX-License-Identifier: MIT

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIST="${1:?worklist}"
OUT="${2:?output tsv}"
N="${3:-40}"
START="${4:-1}"

[ -r "$LIST" ] || { echo "aminet_batch=FAIL reason=no_worklist path=$LIST"; exit 1; }
command -v lha >/dev/null 2>&1 || command -v lhasa >/dev/null 2>&1 || {
    echo "aminet_batch=FAIL reason=no_lha"; exit 1; }

: > "$OUT"
i=0
while read -r a; do
    [ -n "$a" ] || continue
    [ "$i" -ge "$N" ] && break
    i=$((i + 1))
    w=$(mktemp -d "${TMPDIR:-/tmp}/anxb.XXXXXX") || continue
    if ! curl -fsL --max-time 60 -o "$w/a.lha" "http://aminet.net$a" 2>/dev/null; then
        printf '%s\tDOWNLOAD_FAIL\n' "$a" >> "$OUT"; rm -rf "$w"; continue
    fi
    if ! ( cd "$w" && { lha xq a.lha || lhasa xq a.lha; } >/dev/null 2>&1 ); then
        printf '%s\tEXTRACT_FAIL\n' "$a" >> "$OUT"; rm -rf "$w"; continue
    fi
    res=$(cd "$ROOT" && timeout 120 python3 tools/aminet-scan.py "$w" 2>/dev/null \
          | grep -vE '	no-bsdsocket	|	error	' | head -8)
    if [ -z "$res" ]; then
        printf '%s\tNO_BSDSOCKET_BINARY\n' "$a" >> "$OUT"
    else
        printf '%s\n' "$res" | while IFS= read -r line; do
            printf '%s\t%s\n' "$a" "$line" >> "$OUT"
        done
    fi
    rm -rf "$w"
done < <(sed -n "${START},\$p" "$LIST")

echo "aminet_batch=done archives=$i rows=$(wc -l < "$OUT") out=$OUT"
