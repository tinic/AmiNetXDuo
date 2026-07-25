#!/usr/bin/env bash
#
# Reap stray fs-uae processes.
#
#   tools/fsuae-reap.sh [-a MINUTES] [-f] [-n]
#
#   -a  only kill instances older than MINUTES (default 15)
#   -f  kill every instance regardless of age
#   -n  dry run: list what would be killed
#
# The runners kill their own emulator from a trap, but a process can still be
# orphaned if a run is killed with SIGKILL, or if fs-uae wedges before the trap
# is installed. An age threshold is the default so this is safe to run while
# other tests are in flight -- a legitimate run is minutes old at most.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

MAX_AGE_MIN=15
FORCE=0
DRY=0

while getopts "a:fn" opt; do
    case "$opt" in
        a) MAX_AGE_MIN="$OPTARG" ;;
        f) FORCE=1 ;;
        n) DRY=1 ;;
        *) echo "usage: $0 [-a minutes] [-f] [-n]" >&2; exit 2 ;;
    esac
done

# etime is [[dd-]hh:]mm:ss -- normalise to seconds.
etime_to_seconds() {
    local t="$1" days=0 rest="$1"
    case "$t" in
        *-*) days="${t%%-*}"; rest="${t#*-}" ;;
    esac
    local IFS=:
    # shellcheck disable=SC2206
    local parts=($rest)
    local secs=0
    for p in "${parts[@]}"; do
        secs=$((secs * 60 + 10#$p))
    done
    echo $((secs + days * 86400))
}

found=0
killed=0

while read -r pid etime _; do
    [ -n "$pid" ] || continue
    found=$((found + 1))
    age=$(etime_to_seconds "$etime")
    age_min=$((age / 60))

    if [ "$FORCE" = "1" ] || [ "$age" -ge $((MAX_AGE_MIN * 60)) ]; then
        if [ "$DRY" = "1" ]; then
            echo "would kill pid $pid (running ${age_min}m)"
        else
            echo "killing pid $pid (running ${age_min}m)"
            kill -TERM "$pid" 2>/dev/null || true
            sleep 1
            kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
        fi
        killed=$((killed + 1))
    else
        echo "leaving pid $pid alone (running ${age_min}m, under the ${MAX_AGE_MIN}m threshold)"
    fi
done < <(ps -eo pid,etime,comm | awk '$3 ~ /fs-uae$/ {print $1, $2, $3}')

if [ "$found" = "0" ]; then
    echo "no fs-uae instances running"
else
    echo "$found instance(s), $killed actioned"
fi
