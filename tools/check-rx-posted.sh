#!/usr/bin/env bash
#
# One writer for the SANA-II reader's `posted` flag.
#
#   tools/check-rx-posted.sh
#
# ami_sana2_rx_post() skips its whole per-drain sweep of the ring when
# rx->unposted is zero.  That is the steady state -- ami_sana2_rx_complete()
# re-posts each slot as it takes the frame out -- and skipping it removes
# AMI_SANA2_RX_MAX_DEPTH calls per drain from the receive path.
#
# The skip is sound only while the count is exact.  A stale nonzero costs one
# wasted sweep and nothing else; A STALE ZERO STOPS THE RING BEING REFILLED and
# receive collapses, with no error anywhere -- the same silent shape as the
# 0.26.0 stall.  So the flag and the count are written together, in the owner
# block in sana2_rx.c, and this gate refuses an assignment to the flag anywhere
# else.  Three sites set it directly before the skip existed; a fourth added
# later would not fail to compile.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OWNER="$ROOT/src/sana2/sana2_rx.c"
BEGIN='---- BEGIN posted-flag owner'
END='---- END posted-flag owner'

if [ ! -f "$OWNER" ]; then
    echo "rx_posted=fail reason=no_owner_file file=src/sana2/sana2_rx.c"
    exit 1
fi

first=$(grep -n -- "$BEGIN" "$OWNER" | head -1 | cut -d: -f1 || true)
last=$(grep -n -- "$END"   "$OWNER" | head -1 | cut -d: -f1 || true)

if [ -z "$first" ] || [ -z "$last" ] || [ "$first" -ge "$last" ]; then
    echo "rx_posted=fail reason=no_owner_block begin=${first:-none} end=${last:-none}"
    exit 1
fi

# Assignment, not comparison: `posted ==` and `posted !=` are reads.
PATTERN='(->|\.)posted[[:space:]]*=[^=]'

rc=0
bad=0
inside=0

# grep's own failure must not read as "no hits": exit 1 is a clean no-match,
# anything above it is a broken invocation and the gate would pass a tree it
# never looked at.  This one did, on an --include placed after the pattern.
set +e
HITS=$(cd "$ROOT" && grep -rnE --include='*.c' --include='*.h' -- "$PATTERN" src)
grc=$?
set -e

if [ "$grc" -gt 1 ]; then
    echo "rx_posted=fail reason=grep_error rc=$grc"
    exit 1
fi

while IFS= read -r hit; do
    [ -n "$hit" ] || continue
    file=${hit%%:*}
    rest=${hit#*:}
    line=${rest%%:*}

    if [ "$file" = "src/sana2/sana2_rx.c" ] &&
       [ "$line" -gt "$first" ] && [ "$line" -lt "$last" ]; then
        inside=$((inside + 1))
        continue
    fi

    echo "rx_posted=fail file=$file line=$line text=$(echo "${rest#*:}" | sed 's/^[[:space:]]*//')"
    bad=$((bad + 1))
    rc=1
done <<EOF
$HITS
EOF

if [ "$inside" -eq 0 ]; then
    echo "rx_posted=fail reason=owner_block_writes_nothing"
    echo "  the block exists but assigns the flag nowhere, so the gate would"
    echo "  pass a tree that had stopped tracking it at all"
    exit 1
fi

if [ "$rc" -ne 0 ]; then
    echo "rx_posted=fail outside=$bad inside=$inside"
    echo "  write the flag through ami_sana2_rx_mark() so rx->unposted moves"
    echo "  with it; see the owner block in src/sana2/sana2_rx.c"
    exit 1
fi

echo "rx_posted=ok inside=$inside outside=0 owner=src/sana2/sana2_rx.c:$first-$last"
exit 0
