#!/usr/bin/env bash
#
# The mDNS responder must still give the machine back between resource records.
#
#   tools/check-mdns-yield.sh
#
# The helper thread drained its receive queue in one mutex-held pass, and each
# record it processed was up to four walks of the whole peer cache.  Measured on
# a 14 MHz 68020 with a populated cache: 100-500 ms passes, and no TCP
# acknowledgment left the machine for the duration -- about one transfer in six
# crossed a chatter burst and lost 5-10 per cent of its read rate to that dead
# air.  Fixed in 0.25.3 (tinic/netxduo d66ae716 and 4d983c2c) by dropping the
# mutex and relinquishing once per record.
#
# THE BOUND IS THE RECORD, NOT THE PACKET, and it must be at the TOP of each
# section loop: several paths through a record body continue past the bottom, so
# a yield placed beside the data_ptr update is skipped by them.  That placement
# is the whole content of 4d983c2c, and it is exactly what an innocent tidy-up
# of the loop would undo.
#
# There is no unit test for this: the property is a bound on how long one thread
# holds a mutex, which needs a scheduler and a slow CPU to observe.  What is
# mechanically checkable is that the call sites are still there and still shaped
# the way the measurement was taken against, and that is what this does.
#
# Output is key=value and an exit code: 0 the yields are in place, 1 one is
# gone or has moved out of the loop head, 2 the source is not there to check.
#
# SPDX-License-Identifier: MIT

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/third_party/netxduo/addons/mdns/nxd_mdns.c"

[ -r "$SRC" ] || {
    echo "mdns_yield=skipped reason=no_nxd_mdns_c" >&2
    exit 2
}

bad=0

# 1. The yield itself: put, relinquish, get, in that order and in one function.
#    A relinquish that keeps the mutex hands the processor over with the lock
#    held, so whoever wants it inherits the wait and nothing has been gained.
body=$(awk '/^static VOID _nx_mdns_yield\(/ { on = 1 }
            on { print }
            on && /^}/ { exit }' "$SRC")

order=$(printf '%s\n' "$body" |
        sed -E -n 's/.*(tx_mutex_put|tx_thread_relinquish|tx_mutex_get).*/\1/p' |
        tr '\n' ' ')

case "$order" in
    "tx_mutex_put tx_thread_relinquish tx_mutex_get ") ;;
    "")
        echo "mdns_yield=missing what=_nx_mdns_yield" >&2
        bad=1 ;;
    *)
        echo "mdns_yield=reshaped what=_nx_mdns_yield order=\"$order\"" >&2
        echo "  wanted: tx_mutex_put tx_thread_relinquish tx_mutex_get" >&2
        bad=1 ;;
esac

# 2. Both section loops of _nx_mdns_packet_process yield at the head of the
#    body.  Counted against the loops themselves, so a loop that lost its yield
#    is a failure and not a silently smaller number.
loops=$(awk '
    /^static UINT +_nx_mdns_packet_process\(.*[^;]$/ { on = 1 }
    !on { next }
    /for \(index = 0; index < question_count; index\+\+\)/ { seen = "question"; n = 0; next }
    /for \(index = 0; index < answer_count; index\+\+\)/   { seen = "answer";   n = 0; next }
    seen && /_nx_mdns_yield\(/ && n < 12 { print seen; seen = "" }
    seen { n++ ; if (n >= 12) seen = "" }
    on && /^}/ { exit }
' "$SRC" | sort -u | tr '\n' ' ')

case "$loops" in
    "answer question ") ;;
    *)
        echo "mdns_yield=out_of_loop_head loops=\"$loops\"" >&2
        echo "  both the question and the answer section loop must call" >&2
        echo "  _nx_mdns_yield() within the first few lines of the body," >&2
        echo "  because several paths through a record continue past the" >&2
        echo "  bottom of it.  See tinic/netxduo 4d983c2c." >&2
        bad=1 ;;
esac

# 3. And between packets, which is where d66ae716 put the first one.
if ! grep -q '_nx_mdns_yield(mdns_ptr);' "$SRC"; then
    echo "mdns_yield=missing what=between_packets" >&2
    bad=1
fi

sites=$(grep -c '_nx_mdns_yield(mdns_ptr);' "$SRC" || true)

if [ "$bad" = 0 ]; then
    echo "mdns_yield=ok sites=$sites loops=question,answer"
    exit 0
fi

echo "mdns_yield=failed sites=$sites" >&2
exit 1
