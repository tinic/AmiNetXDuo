#!/bin/bash
# TLS gate runner.  Output is for a program, not a reader.
#
#   tools/tlsgate.sh <builddir> [cpu] [repeat] [slow]
#
# Emits one key=value line per run and one summary line, nothing else.
# The verdict is the EXIT CODE; the text is a convenience, never the contract.
#
#   0  every run passed
#   1  at least one run failed
#   3  at least one run did not reach a verdict (truncated, killed, no boot)
#   2  usage or environment error
#
# Fields: run, result, handshakes, fallbacks, cpu, speed, port, seconds, dir
#
# `repeat` matters: the 68000 gate is intermittent, so a single pass is not
# evidence.  Repetition is the runner's job, not something a caller remembers.
set -u

BUILD="${1:-}"
CPU="${2:-68020}"
REPEAT="${3:-1}"
SPEED="${4:-fast}"

[ -n "$BUILD" ] || { echo "result=usage"; exit 2; }

cd "$(dirname "${BASH_SOURCE[0]}")/.." || { echo "result=environment"; exit 2; }
. ~/amiga-assets/env.sh 2>/dev/null
export AMINETXDUO_TLS13_FETCH_TIMEOUT=900

[ -d "$BUILD" ] || { echo "result=environment reason=nobuild dir=$BUILD"; exit 2; }

case "$CPU" in
    68000) model="-m A600" ;;   # an A1200 ROM cannot boot a 68000
    *)     model="" ;;
esac
throttle=""
[ "$SPEED" = slow ] && throttle="-k 14"

fails=0
incomplete=0

for run in $(seq 1 "$REPEAT"); do
    # Scoped to this checkout by the config path the emulator was started with.
    # A bare `pkill -f amiberry` on the lab machine kills whatever else is
    # booted there -- another agent's run, the demo instance -- and the run
    # that did it looks fine afterwards.
    pkill -f "$PWD/build/amiberry-" 2>/dev/null
    sleep 3

    port=11000
    while ss -ltn 2>/dev/null | grep -qE ":($((port + 1))|$((port + 4)))\b"; do
        port=$((port + 20))
    done

    t0=$(date +%s)
    # shellcheck disable=SC2086
    out=$(tests/tls/run-tls13.sh -b "$BUILD" $model -c "$CPU" $throttle \
            -t 1500 -P "$port" 2>&1)
    secs=$(( $(date +%s) - t0 ))

    hs=$(printf '%s' "$out" | sed -n 's/.*TLS 1\.3 handshakes: *\([0-9]*\).*/\1/p' | tail -1)
    fb=$(printf '%s' "$out" | sed -n 's/.*TLS 1\.2: *\([0-9]*\).*/\1/p' | tail -1)

    if printf '%s' "$out" | grep -qE '^  PASS'; then
        result=pass
    elif [ -n "$hs" ]; then
        result=fail; fails=$((fails + 1))
    else
        result=incomplete; incomplete=$((incomplete + 1))
    fi

    echo "run=$run result=$result handshakes=${hs:--} fallbacks=${fb:--}" \
         "cpu=$CPU speed=$SPEED port=$port seconds=$secs dir=$BUILD"
done

echo "summary runs=$REPEAT failed=$fails incomplete=$incomplete"

[ "$incomplete" -gt 0 ] && exit 3
[ "$fails" -gt 0 ] && exit 1
exit 0
