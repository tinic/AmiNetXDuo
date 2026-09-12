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
PATTERN='\(VOID\)[[:space:]]*_?(nx|tx)_[a-z0-9_]+[[:space:]]*\('

# Prove the search reaches the tree before believing an empty result: a
# pattern that matches nothing because the paths moved is indistinguishable
# from a tree with no discards left, and only one of those is good news.
reach=$(grep -rlE '_?(nx|tx)_[a-z0-9_]+[[:space:]]*\(' \
            src port --include='*.c' --include='*.h' 2>/dev/null | wc -l)
if [ "$reach" -lt 50 ]; then
    echo "nx_status=FAIL reason=search_found_nothing files=$reach" >&2
    echo "!! Only $reach files under src/ and port/ even call into NetX Duo or" >&2
    echo "!! ThreadX.  That is not this tree, so this gate is looking in the" >&2
    echo "!! wrong place and is failing rather than passing on an empty set." >&2
    exit 1
fi

hits=$(grep -rnE "$PATTERN" src port \
           --include='*.c' --include='*.h' 2>/dev/null |
       grep -v '/test/' || true)

if [ -n "$hits" ]; then
    n=$(printf '%s\n' "$hits" | grep -c .)
    echo "nx_status=FAIL discards=$n" >&2
    printf '%s\n' "$hits" >&2
    echo "!! A NetX Duo or ThreadX status is discarded with a bare (VOID)." >&2
    echo "!! Say which of the seven it is -- Required, Optional, Expected," >&2
    echo "!! Cleanup, OnlySuccess, ByOutput or EitherWay -- at the call site." >&2
    echo "!! include/aminetxduo/nxstatus.h defines them and what each claims." >&2
    exit 1
fi

echo "nx_status=PASS searched=$reach files, 0 unreviewed discards"
