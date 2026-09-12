#!/usr/bin/env bash
#
# A NetX Duo or ThreadX status is not thrown away without saying why.
#
# There were 141 bare `(VOID)' casts on these calls across src/ and port/.
# Most were legitimate.  Three were not, and they looked exactly the same:
#
#   a static interface whose address nx_ip_interface_address_set() refused was
#   recorded as resolved anyway, so the stack came up believing in an address
#   the interface had never taken
#
#   a route whose interface changed was deleted and re-added, and
#   nx_ip_static_route_add() updates only the next hop of an entry it finds by
#   (dest, mask) -- so a delete that failed made the add report success with
#   the route still on the old interface
#
# The point is not that a discard is wrong.  It is that a cast cannot be told
# from a defect by reading it.  include/aminetxduo/nxstatus.h names the five
# things a discard can be; this fails the build on one that names none of them.
#
# SPDX-License-Identifier: MIT

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 1

# The call shape: (VOID) in front of an nx_/tx_/_nx_/_tx_ call.  The macros in
# nxstatus.h expand to exactly this, which is why the header is not searched.
# The scan is one script, tools/nx-discard-scan.sh, so this and the baseline
# cannot disagree about what a discard is.  See that file for the two forms and
# what is left out.
SCAN="$ROOT/tools/nx-discard-scan.sh"
BASE="$ROOT/tools/nx-discard-baseline.txt"

for f in "$SCAN" "$BASE"; do
    if [ ! -r "$f" ]; then
        echo "nx_status=FAIL reason=missing file=$f" >&2
        exit 1
    fi
done

found="$(mktemp)"; base="$(mktemp)"
trap 'rm -f "$found" "$base"' EXIT

"$SCAN" > "$found" 2>/dev/null
# LC_ALL=C and a whole-line sort on BOTH sides: comm compares lines, and
# sorting the scan by field 2 while comm expected line order made it warn
# "not in sorted order" and compare nonsense.  The scanner sorts the same way.
grep -v '^#' "$BASE" | grep -v '^$' | LC_ALL=C sort > "$base"

# Prove the scan reached the tree before believing an empty result: a pattern
# that matches nothing because the paths moved looks exactly like a tree with
# no discards left, and only one of those is good news.
if [ "$(grep -c . "$found")" -lt 20 ]; then
    echo "nx_status=FAIL reason=scan_found_nothing rows=$(grep -c . "$found")" >&2
    echo "!! The scan returned almost nothing.  That is not this tree, so this" >&2
    echo "!! gate is looking in the wrong place and fails rather than passing" >&2
    echo "!! on an empty set." >&2
    exit 1
fi

new=$(comm -23 <(LC_ALL=C sort "$found") "$base" || true)
gone=$(comm -13 <(LC_ALL=C sort "$found") "$base" || true)

rc=0
if [ -n "$new" ]; then
    echo "nx_status=FAIL new discards:" >&2
    printf '%s\n' "$new" >&2
    echo "!! A NetX Duo or ThreadX result goes nowhere at a site the baseline" >&2
    echo "!! does not know.  Say which of the seven it is at the call site --" >&2
    echo "!! include/aminetxduo/nxstatus.h defines them -- or handle it." >&2
    rc=1
fi
if [ -n "$gone" ]; then
    echo "nx_status=stale baseline rows that no longer occur:" >&2
    printf '%s\n' "$gone" >&2
    echo "!! Those sites were classified or removed.  Regenerate:" >&2
    echo "!!     tools/nx-discard-scan.sh > tools/nx-discard-baseline.txt" >&2
    rc=1
fi
[ "$rc" != 0 ] && exit 1

known=$(awk -F'\t' '{s+=$1} END{print s+0}' "$base")
echo "nx_status=PASS known=$known discards, 0 new (141 classified sites carry a reason and are not counted here)"
