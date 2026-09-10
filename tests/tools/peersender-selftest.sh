#!/usr/bin/env bash
# start_sender() from run-iperf.sh, driven against a stub peer.
# SPDX-License-Identifier: MIT
#
# WHY THIS EXISTS.  A UDP send cannot fail: there is no connection to refuse,
# so `peer_cmd send udp` exits 0 whether or not anything is listening, and the
# retry loop learns the guest never heard it only afterwards, from the missing
# peer_report in the output.  The `continue` that ends a successful attempt
# used to sit outside that test and catch the failing one too, which skipped
# the one-second back-off and sent the next full payload immediately.
#
# What that cost is measured, not argued: cardsweep-xs4-xsurf.pcap holds 54
# bursts of 521 datagrams -- 28,128 packets, ~41 MB -- fired at a guest that
# was on stage 1 of 15 and would not open that port until stage 14.  The arm
# timed out at 300 s with every TCP stage failed and the UDP one passed: the
# peer's SYN-ACKs went out 8 times for 7401 and 7 for 7403 unanswered, and the
# guest replied to 7 of the 28 ARP requests aimed at it.
#
# THE FUNCTION IS READ OUT OF run-iperf.sh, not copied.  run-iperf.sh has no
# sourcing guard and stages a guest when it runs, so this lifts the function
# text and evaluates that.  An empty extraction is a failure, so renaming the
# function fails this gate rather than quietly testing nothing.

set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SRC="$ROOT/tests/tools/run-iperf.sh"
FAILED=0

ok() { # what cond
    if [ "$2" -eq 1 ]; then
        printf 'ok   %s\n' "$1"
    else
        printf 'FAIL %s\n' "$1"
        FAILED=$((FAILED + 1))
    fi
}

BODY=$(sed -n '/^start_sender() {/,/^}/p' "$SRC")
if [ -z "$BODY" ]; then
    echo "FAIL start_sender() not found in $SRC -- was it renamed?" >&2
    exit 1
fi
eval "$BODY"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PEERLOG="$WORK"
# shellcheck disable=SC2034  # read by the start_sender text eval'd above
ADDRESS=192.0.2.1
# shellcheck disable=SC2034  # ditto -- and 0 keeps a stubbed send instant
SECS=0
declare -a PEER_PIDS=()

# The stub records one line per attempt.  $STUB_REPLY is what it prints, which
# is how a test says "the guest was listening" or "it was not".
STUB_REPLY=""
peer_cmd() {
    echo "$*" >> "$WORK/attempts"
    [ -z "$STUB_REPLY" ] || echo "$STUB_REPLY"
    return 0
}

attempts() { [ -f "$WORK/attempts" ] && wc -l < "$WORK/attempts" || echo 0; }
reset()    { rm -f "$WORK/attempts" "$PEERLOG"/*.out "$PEERLOG"/*.err; PEER_PIDS=(); }

# ---------------------------------------------------------------- back-off
#
# A guest that never listens.  The loop may only retry once a second, so over
# PEER_LIFE seconds it gets PEER_LIFE-ish attempts.  Without the back-off it
# spins as fast as the stub returns, which is thousands.  The bound is
# deliberately loose -- this separates "once a second" from "as fast as it
# can", and nothing finer is worth being flaky over.
reset
PEER_LIFE=4
STUB_REPLY="peer_bytes=0"
start_sender udpnolisten udp 7405 1
wait "${PEER_PIDS[0]}" 2>/dev/null
N=$(attempts)
ok "a UDP send the guest never heard backs off (got $N attempts in ${PEER_LIFE}s, want <= 8)" \
   "$([ "$N" -le 8 ] && echo 1 || echo 0)"
ok "and it does try at least twice (got $N)" "$([ "$N" -ge 2 ] && echo 1 || echo 0)"

# ------------------------------------------------------------ success path
#
# peer_report=1 is the guest answering.  One attempt, counted, exit 0.
reset
PEER_LIFE=4
STUB_REPLY="peer_bytes=751170 peer_report=1"
start_sender udpheard udp 7405 1
wait "${PEER_PIDS[0]}"; rc=$?
ok "a UDP send the guest answered stops after one attempt (got $(attempts))" \
   "$([ "$(attempts)" -eq 1 ] && echo 1 || echo 0)"
ok "and the sender exits 0" "$([ "$rc" -eq 0 ] && echo 1 || echo 0)"
ok "and the peer line is kept" \
   "$(grep -q 'peer_report=1' "$PEERLOG/udpheard.out" && echo 1 || echo 0)"

# ---------------------------------------------------------------- tcp path
#
# TCP is counted on peer_cmd's exit status, with no peer_report needed, and
# that arm must keep its own `continue`: one attempt, not two.
reset
PEER_LIFE=4
STUB_REPLY="peer_bytes=7888896"
start_sender tcpheard tcp 7404 1
wait "${PEER_PIDS[0]}"; rc=$?
# Dropping this arm's own `continue` is INERT and this file does not pretend
# to catch it: after a counted send the while condition is re-tested and, at
# wanted=1, the loop is already done -- so the back-off it would fall into
# never runs.  At wanted>1 it costs one second a round and still sends the
# same number of times.  Catching it means asserting on wall time, which is
# not worth the flakiness.
ok "a TCP send is counted without a peer_report (got $(attempts))" \
   "$([ "$(attempts)" -eq 1 ] && echo 1 || echo 0)"
ok "and the TCP sender exits 0" "$([ "$rc" -eq 0 ] && echo 1 || echo 0)"

# ------------------------------------------------------------ wanted > 1
reset
PEER_LIFE=6
STUB_REPLY="peer_bytes=1 peer_report=1"
start_sender udptwice udp 7405 2
wait "${PEER_PIDS[0]}"
ok "wanted=2 sends twice (got $(attempts))" \
   "$([ "$(attempts)" -eq 2 ] && echo 1 || echo 0)"

# -------------------------------------------------------------- sequencing
#
# The guest opens 7404 before 7405, so the UDP sender waits for the TCP one.
# Nothing may go out while that pid is alive.
reset
PEER_LIFE=8
STUB_REPLY="peer_bytes=1 peer_report=1"
sleep 3 & GATE=$!
start_sender udpafter udp 7405 1 "$GATE"
sleep 1
ok "nothing is sent while the sender it follows is still running (got $(attempts))" \
   "$([ "$(attempts)" -eq 0 ] && echo 1 || echo 0)"
wait "$GATE" 2>/dev/null
wait "${PEER_PIDS[0]}" 2>/dev/null
ok "and it sends once that sender has finished (got $(attempts))" \
   "$([ "$(attempts)" -ge 1 ] && echo 1 || echo 0)"

# An absent pid is not a wait: the common case must not pause at all.
reset
PEER_LIFE=4
STUB_REPLY="peer_bytes=1 peer_report=1"
start_sender udpnogate udp 7405 1 ""
wait "${PEER_PIDS[0]}" 2>/dev/null
ok "no follow-pid means no wait (got $(attempts))" \
   "$([ "$(attempts)" -eq 1 ] && echo 1 || echo 0)"

if [ "$FAILED" -ne 0 ]; then
    echo "peersender-selftest: $FAILED check(s) failed" >&2
    exit 1
fi
echo "peersender-selftest: all checks passed"
