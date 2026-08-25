#!/usr/bin/env bash
#
# Acknowledgement and window behaviour of one machine, measured from off it.
#
#   tests/perf/run-ackscope.sh -A TARGET [-C user@host] [-t SECONDS]
#                              [-p PORT] [-T TAG] [-o DIR] [-g "ARGS"]
#                              [-- command to drive the transfer ...]
#
# WHAT IT IS FOR
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# shellcheck source=tests/perf/peercap.sh
. "$ROOT/tests/perf/peercap.sh"

TARGET="${AMINETXDUO_ACKSCOPE_TARGET:-amiga-1200.local}"
CAPHOST="${AMINETXDUO_ACKSCOPE_CAPHOST:-}"
PEER="${AMINETXDUO_FITZ_PEER:-}"
SECS=0
PORT=""
TAG="${AMINETXDUO_RUN_TAG:-ackscope}"
OUT=""
GATES="${AMINETXDUO_ACKSCOPE_ARGS:-}"
SNAP="${AMINETXDUO_ACKSCOPE_SNAPLEN:-160}"

usage() { sed -n '3,10p' "$0" >&2; }

while getopts "A:C:H:t:p:T:o:g:s:h" opt; do
    case "$opt" in
        A) TARGET="$OPTARG" ;;
        C) CAPHOST="$OPTARG" ;;
        H) PEER="$OPTARG" ;;
        t) SECS="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        o) OUT="$OPTARG" ;;
        g) GATES="$OPTARG" ;;
        s) SNAP="$OPTARG" ;;
        h) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

CAPHOST="${CAPHOST:-$PEER}"
[ -n "$CAPHOST" ] || {
    echo "ackscope: no capture host.  -C names the machine that runs" >&2
    echo "tcpdump; -H alone is enough when the peer is also that machine." >&2
    exit 2; }
[ -n "$TARGET" ] || { usage; exit 2; }
[ "$SECS" != 0 ] || [ "$#" -gt 0 ] || {
    echo "ackscope: nothing would drive a transfer.  Give -t SECONDS to hold" >&2
    echo "the capture open, or -- followed by a command to run while it is." >&2
    exit 2; }

OUT="${OUT:-$ROOT/build/ackscope-$TAG}"
rm -rf "$OUT"; mkdir -p "$OUT"

# ------------------------------------------------------------- the target --
resolve() { # name
    case "$1" in
        [0-9]*.[0-9]*.[0-9]*.[0-9]*) echo "$1"; return 0 ;;
    esac
    ssh -o ConnectTimeout=10 "$CAPHOST" \
        "getent ahostsv4 '$1' 2>/dev/null | awk 'NR==1 { print \$1 }'" 2>/dev/null
}

ADDR=$(resolve "$TARGET" || true)
if [ -z "$ADDR" ]; then
    echo "target_name=$TARGET"
    echo "target_addr=unresolved"
    echo "RESULT=skip"
    echo "ackscope: $CAPHOST cannot resolve '$TARGET'." >&2
    echo "  A name that does not resolve is not a failed measurement, it is" >&2
    echo "  no measurement: the machine is off, or it has not announced" >&2
    echo "  itself yet.  Nothing here waits for it." >&2
    exit 3
fi
echo "target_name=$TARGET"
echo "target_addr=$ADDR"
echo "capture_host=$CAPHOST"
echo "driver_peer=${PEER:-none}"

SELF=$(ssh -o ConnectTimeout=10 "$CAPHOST" \
       "ip -4 -o addr show 2>/dev/null | awk '{ print \$4 }' | cut -d/ -f1" \
       2>/dev/null || true)
for a in $SELF; do
    [ "$a" != "$ADDR" ] || {
        echo "ackscope: $ADDR is $CAPHOST's own address.  '$TARGET' resolved" >&2
        echo "  to the capture host, so this would measure its stack and" >&2
        echo "  file the answer under the Amiga's name." >&2
        exit 2; }
done

# ------------------------------------------------------------ the capture --
export AMINETXDUO_PEERCAP_FILTER="tcp and host $ADDR"
export AMINETXDUO_PEERCAP_SNAPLEN="$SNAP"

CAPTURING=0
stop_capture() {
    [ "$CAPTURING" = 1 ] || return 0
    CAPTURING=0
    local n=0
    until peercap_stop "$CAPHOST" "$OUT" "$TAG"; do
        n=$((n + 1))
        [ "$n" -lt 3 ] || { echo "ackscope: gave up retrieving the capture" \
                                 "after $n attempts" >&2; return 0; }
        echo "==> retrieval attempt $n failed; retrying" >&2
        sleep $((n * 5))
    done
}
trap stop_capture EXIT INT TERM HUP

ATTEMPT=0
until peercap_start "$CAPHOST" "${PORT:-0}" "$OUT" "$TAG"; do
    ATTEMPT=$((ATTEMPT + 1))
    [ "$ATTEMPT" -lt 3 ] || {
        echo "ackscope: could not start the capture on $CAPHOST in $ATTEMPT" >&2
        echo "  attempts.  The diagnosis above is the last one's." >&2
        exit 2; }
    echo "==> attempt $ATTEMPT to reach $CAPHOST failed; retrying" >&2
    sleep $((ATTEMPT * 5))
done
echo "capture_attempts=$((ATTEMPT + 1))"
CAPTURING=1

RC_DRIVE=0
START=$(date +%s)
if [ "$#" -gt 0 ]; then
    echo "==> driving: $*"
    "$@" > "$OUT/drive.txt" 2>&1 || RC_DRIVE=$?
    echo "drive_rc=$RC_DRIVE"
    [ "$RC_DRIVE" = 0 ] || {
        echo "==> the driver exited $RC_DRIVE; its output ends:" >&2
        tail -12 "$OUT/drive.txt" >&2 || true; }
fi
if [ "$SECS" != 0 ]; then
    left=$(( SECS - ( $(date +%s) - START ) ))
    if [ "$left" -gt 0 ]; then
        echo "==> holding the capture open for $left more second(s)"
        while [ "$left" -gt 0 ]; do
            sleep $(( left > 5 ? 5 : left ))
            left=$(( SECS - ( $(date +%s) - START ) ))
        done
    fi
fi

stop_capture
trap - EXIT INT TERM HUP

PCAP="$OUT/$TAG.pcap"
[ -s "$PCAP" ] || {
    echo "RESULT=skip"
    echo "ackscope: no capture came back from $CAPHOST." >&2
    exit 3; }
echo "capture_bytes=$(wc -c < "$PCAP" | tr -d ' ')"

# ------------------------------------------------------------ the reading --
RC=0
# shellcheck disable=SC2086
python3 "$ROOT/tests/perf/ackscope.py" "$PCAP" --guest "$ADDR" \
        --name "$TARGET" ${PORT:+--port "$PORT"} $GATES \
        > "$OUT/ackscope.txt" 2>"$OUT/ackscope.err" || RC=$?
cat "$OUT/ackscope.txt"
if [ "$RC" = 2 ]; then
    sed 's/^/    /' "$OUT/ackscope.err" >&2
    echo "RESULT=skip"
    echo "ackscope: $ADDR ($TARGET) said nothing in $(basename "$PCAP")." >&2
    echo "  The capture is $(wc -c < "$PCAP" | tr -d ' ') bytes, so tcpdump" >&2
    echo "  ran; what is missing is the machine.  Nothing here waits for it." >&2
    exit 3
fi
[ ! -s "$OUT/ackscope.err" ] || sed 's/^/    /' "$OUT/ackscope.err" >&2
echo "out_dir=$OUT"
exit "$RC"
