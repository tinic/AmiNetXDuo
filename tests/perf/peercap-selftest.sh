#!/usr/bin/env bash
# peercap's tcpdump preflight: a missing binary is not a failed ssh transport.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/peercap-selftest.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM HUP

export AMINETXDUO_PEER_TCPDUMP=tcpdump
export AMINETXDUO_PEERCAP_SSH_RETRIES=3
export AMINETXDUO_PEERCAP_SSH_RETRY_DELAY=0

# shellcheck source=tests/perf/peercap.sh
. "$ROOT/tests/perf/peercap.sh"

checks=0
wrong=0
scenario=ok
calls="$WORK/calls"

ssh() {
    local n=0

    [ ! -f "$calls" ] || n=$(sed -n '1p' "$calls")
    n=$((n + 1))
    printf '%s\n' "$n" > "$calls"

    case "$scenario" in
        ok)        echo ok; return 0 ;;
        missing)   echo missing; return 0 ;;
        transient)
            if [ "$n" -lt 3 ]; then
                echo "connection reset by peer" >&2
                return 255
            fi
            echo ok
            return 0 ;;
        down)
            echo "no route to host" >&2
            return 255 ;;
        *) return 99 ;;
    esac
}

check() {
    local ok="$1" what="$2"
    checks=$((checks + 1))
    if [ "$ok" = 1 ]; then
        printf '  ok    %s\n' "$what"
    else
        wrong=$((wrong + 1))
        printf '  WRONG %s\n' "$what"
    fi
}

run_state() {
    : > "$calls"
    state_rc=0
    state_out=$(peercap_tcpdump_state peer.example 2>"$WORK/diag") || state_rc=$?
    state_calls=$(sed -n '1p' "$calls")
}

scenario=ok
run_state
check "$([ "$state_rc" = 0 ] && [ "$state_out" = ok ] &&
         [ "$state_calls" = 1 ] && echo 1 || echo 0)" \
      "a successful preflight is returned after one ssh call"

scenario=missing
run_state
check "$([ "$state_rc" = 0 ] && [ "$state_out" = missing ] &&
         [ "$state_calls" = 1 ] && echo 1 || echo 0)" \
      "a genuinely missing binary is not retried or called transport failure"

scenario=transient
run_state
check "$([ "$state_rc" = 0 ] && [ "$state_out" = ok ] &&
         [ "$state_calls" = 3 ] && echo 1 || echo 0)" \
      "two transient ssh failures are retried and the third answer wins"
check "$([ ! -s "$WORK/diag" ] && echo 1 || echo 0)" \
      "recovered ssh diagnostics do not contaminate the state"

scenario=down
run_state
check "$([ "$state_rc" = 2 ] && [ "$state_out" = transport ] &&
         [ "$state_calls" = 3 ] && echo 1 || echo 0)" \
      "a persistent ssh failure has its own state and bounded retry count"
check "$(grep -q 'no route to host' "$WORK/diag" && echo 1 || echo 0)" \
      "the final ssh diagnostic is preserved"

: > "$calls"
resolve_rc=0
peercap_resolve_tcpdump peer.example >"$WORK/resolve.out" \
    2>"$WORK/resolve.err" || resolve_rc=$?
check "$([ "$resolve_rc" = 1 ] && [ "$(sed -n '1p' "$calls")" = 3 ] &&
         echo 1 || echo 0)" \
      "resolve refuses a transport failure instead of falling back to PATH"

printf 'peercap-selftest: %lu cases, %lu wrong\n' "$checks" "$wrong"
exit "$wrong"
