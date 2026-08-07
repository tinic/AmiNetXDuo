#!/bin/bash
# One way to run a TLS gate. Everything that has bitten me is handled here
# once instead of being retyped per run.
#
#   gate.sh <builddir> [68000|68020|68060] [slow]
#
#   - kills only real emulator processes, with a pattern that cannot match
#     the shell running this script ([a]miberry, not amiberry)
#   - picks a free port instead of a number I guessed
#   - pairs the model with the CPU, which the harness now enforces anyway
#   - full emulation speed unless "slow" is asked for: the throttle is for
#     measuring speed, not for pass/fail, and it costs 5 minutes a run
#   - prints ONLY the verdict, unbuffered
set -u

BUILD="${1:?usage: gate.sh <builddir> [cpu] [slow]}"
CPU="${2:-68020}"
SPEED="${3:-fast}"

cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 2
. ~/amiga-assets/env.sh 2>/dev/null
export AMINETXDUO_TLS13_FETCH_TIMEOUT=900

pkill -f "[a]miberry" 2>/dev/null
pkill -f "[h]ttppeer" 2>/dev/null
pkill -f "[r]un-tls13" 2>/dev/null
sleep 3

# A port nothing is listening on, plus room for the peer's five sockets.
port=11000
while ss -ltn 2>/dev/null | grep -q ":$((port + 1))\b" ||
      ss -ltn 2>/dev/null | grep -q ":$((port + 4))\b"; do
    port=$((port + 20))
done

case "$CPU" in
    68000) model="-m A600" ;;          # an A1200 ROM will not boot a 68000
    *)     model="" ;;
esac

throttle=""
[ "$SPEED" = slow ] && throttle="-k 14"

echo "== $BUILD  cpu $CPU  port $port  ${SPEED}"
# shellcheck disable=SC2086
out=$(tests/tls/run-tls13.sh -b "$BUILD" $model -c "$CPU" $throttle \
        -t 1500 -P "$port" 2>&1)

echo "$out" | grep -aE "TLS up:|handshakes:|PASS|FAIL:" | tail -5
echo "$out" | grep -aqE "^  PASS" && echo "VERDICT: pass" || echo "VERDICT: fail"
