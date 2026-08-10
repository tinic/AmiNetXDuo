#!/usr/bin/env bash
#
# The allocation census, run and asserted.
#
#   tools/alloc-census.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR] [-r ROUNDS]
#                         [-k KNOWNFILE] [-s SCOPE] [--record]
#
# Boots CensusProbe under Amiberry, which opens and closes bsdsocket.library and
# then provokes the expunge, and reads the census the library prints to the
# serial port.  Fails when anything is outstanding that the known set does not
# already account for, which is the whole point: a leak nobody thought to write
# a test for still appears here, named by the file and line that allocated it.
#
# WHAT THE KNOWN SET IS AND IS NOT
#
# tools/alloc-census-known.txt lists sites that are outstanding today, with the
# bytes they hold.  Every line in it is a REPORTED FINDING, not an accepted
# cost: it is there so that a NEW leak is visible against a background that is
# not yet zero, and each one is a bug with an owner.  Growth of a listed site
# fails too -- the count and the bytes are both ceilings.
#
# EXIT STATUS, and the difference between them is the point
#
#   0   the outstanding set is within the known set
#   1   something is outstanding that the known set does not cover, or a known
#       site grew.  A defect in the product.
#   2   usage, or the build is missing.  Nothing was tested.
#   3   NO CENSUS OUTPUT AT ALL.  Infrastructure: the guest did not run, the
#       serial log did not arrive, or the library was never expunged.  This is
#       never a pass, and it is deliberately not 1, because "nothing was
#       outstanding" and "nothing was measured" must not read the same.
#   4   the census disowns its own numbers: lost= or unknown_free= is nonzero,
#       so the table overflowed or saw a free it had no record of.  The run
#       says nothing about leaks either way.
#
# --record prints the outstanding set in known-file format and exits 0 without
# asserting.  Read what it prints before pasting it anywhere.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=120
BUILD="${AMINETXDUO_BUILD:-build/cen}"
ROUNDS=2
KNOWN="$ROOT/tools/alloc-census-known.txt"
SCOPE=bsd-expunge
RECORD=0

while [ $# -gt 0 ]; do
    case "$1" in
        -m) MODEL="$2"; shift 2 ;;
        -t) TIMEOUT="$2"; shift 2 ;;
        -b) BUILD="$2"; shift 2 ;;
        -r) ROUNDS="$2"; shift 2 ;;
        -k) KNOWN="$2"; shift 2 ;;
        -s) SCOPE="$2"; shift 2 ;;
        --record) RECORD=1; shift ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-r rounds]" \
                "[-k known] [-s scope] [--record]" >&2; exit 2 ;;
    esac
done

EXE="$ROOT/$BUILD/src/tools/CensusProbe"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$EXE" "$BSD"; do
    [ -f "$f" ] || {
        echo "missing $f" >&2
        echo "configure with -DAMINETXDUO_ALLOCCENSUS=ON and build" \
             "tool_censusprobe bsdsocket_library" >&2
        exit 2
    }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

# ------------------------------------------------------------------ staging --
#
# tests/netstack/devs, copied and then amended, the same way every harness in
# tests/ stages it.  The amendment is the one thing this run needs that no
# other does: TCP: OFF.
#
# bsd_lib_expunge() declines while the TCP: handler Process is alive, and the
# handler is started by the FIRST open of the library.  With it on, the expunge
# never happens, the census never prints, and the run exits 3 -- which is
# correct, and useless.  The handler's own allocations are therefore outside
# what this run measures; that is a stated gap, not an oversight.

TAG="${AMINETXDUO_RUN_TAG:-alloccensus}"
STAGE="$ROOT/build/census-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
echo "OFF" > "$STAGE/devs/Internet/tcp_handler"

UG="$ROOT/$BUILD/src/usergroup/usergroup.library"
[ -f "$UG" ] && cp "$UG" "$STAGE/libs/usergroup.library"

export AMINETXDUO_RUN_TAG="$TAG"
SERIAL="$ROOT/build/amiberry-serial-$TAG.log"

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
    -a "ROUNDS $ROUNDS" \
    "$EXE" "$STAGE/devs" "$STAGE/libs" > "$ROOT/build/census-run-$TAG.log" 2>&1
RUN_RC=$?
set -e

echo "==> runner rc=$RUN_RC, serial $SERIAL" >&2

# ------------------------------------------------------------------ verdict --

[ -s "$SERIAL" ] || {
    echo "alloccensus: result=infrastructure reason=no-serial-log rc=$RUN_RC" >&2
    exit 3
}

CENSUS="$ROOT/build/census-$TAG.txt"
tr -d '\r' < "$SERIAL" | grep '^ALLOCCENSUS ' > "$CENSUS" || true

END_LINE=$(grep "^ALLOCCENSUS end scope=$SCOPE " "$CENSUS" | tail -1 || true)
[ -n "$END_LINE" ] || {
    echo "alloccensus: result=infrastructure reason=no-census scope=$SCOPE rc=$RUN_RC" >&2
    echo "the library was not expunged, or the guest never ran." >&2
    sed -n '1,20p' "$CENSUS" >&2
    exit 3
}

kv() { echo "$END_LINE" | tr ' ' '\n' | awk -F= -v k="$1" '$1==k {print $2}'; }

LIVE=$(kv live);   BYTES=$(kv bytes);  SITES=$(kv sites)
LOST=$(kv lost);   UNKNOWN=$(kv unknown_free); DROPPED=$(kv sites_dropped)

# To stderr, so --record's stdout is the known-file text and nothing else.
echo "alloccensus: scope=$SCOPE live=$LIVE bytes=$BYTES sites=$SITES" \
     "lost=$LOST unknown_free=$UNKNOWN sites_dropped=$DROPPED" >&2

if [ "$LOST" != 0 ] || [ "$DROPPED" != 0 ]; then
    echo "alloccensus: result=unreliable reason=table-overflow lost=$LOST" \
         "sites_dropped=$DROPPED" >&2
    exit 4
fi
if [ "$UNKNOWN" != 0 ]; then
    echo "alloccensus: result=unreliable reason=unknown-free" \
         "unknown_free=$UNKNOWN" >&2
    exit 4
fi

# The site lines belonging to the LAST report of this scope.  A run with more
# than one expunge would otherwise mix them.
OUT="$ROOT/build/census-sites-$TAG.txt"
awk -v scope="$SCOPE" '
    $0 ~ ("^ALLOCCENSUS begin scope=" scope "$") { n = 0; delete rows; next }
    $0 ~ ("^ALLOCCENSUS site=.* scope=" scope "$") { rows[n++] = $0; next }
    $0 ~ ("^ALLOCCENSUS end scope=" scope " ") {
        keep = n; for (i = 0; i < n; i++) last[i] = rows[i]; kept = n; next
    }
    END { for (i = 0; i < kept; i++) print last[i] }
' "$CENSUS" |
sed -E 's/^ALLOCCENSUS site=([^ ]+) bytes=([0-9]+) count=([0-9]+).*/\1 \2 \3/' \
    > "$OUT"

if [ "$RECORD" = 1 ]; then
    echo "# scope site bytes count  -- recorded $(date -u +%Y-%m-%dT%H:%MZ)"
    while read -r site bytes count; do
        [ -n "$site" ] || continue
        echo "$SCOPE $site $bytes $count"
    done < "$OUT"
    exit 0
fi

[ -f "$KNOWN" ] || { echo "missing known set $KNOWN" >&2; exit 2; }

FAILED=0
while read -r site bytes count; do
    [ -n "$site" ] || continue
    entry=$(awk -v s="$SCOPE" -v f="$site" \
            '$1=="#" || $0 ~ /^[[:space:]]*(#|$)/ {next}
             $1==s && $2==f {print $3, $4}' "$KNOWN" | head -1)
    if [ -z "$entry" ]; then
        echo "alloccensus: NEW site=$site bytes=$bytes count=$count scope=$SCOPE" >&2
        FAILED=1
        continue
    fi
    kb=${entry% *}; kc=${entry#* }
    if [ "$bytes" -gt "$kb" ] || [ "$count" -gt "$kc" ]; then
        echo "alloccensus: GREW site=$site bytes=$bytes/$kb count=$count/$kc" \
             "scope=$SCOPE" >&2
        FAILED=1
    fi
done < "$OUT"

if [ "$FAILED" != 0 ]; then
    echo "alloccensus: result=fail scope=$SCOPE" >&2
    exit 1
fi

echo "alloccensus: result=pass scope=$SCOPE live=$LIVE bytes=$BYTES"
exit 0
