#!/usr/bin/env bash
# NO SHIPPED COMMAND MAY SEND A USER TO A LOG IT CANNOT WRITE.
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="${1:-${AMINETXDUO_BUILD:-build/cm}}"
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

TOOLDIR="$BUILD/src/tools"
if [ ! -d "$TOOLDIR" ]; then
    echo "log_advice=skipped reason=no_build dir=$TOOLDIR"
    exit 2
fi

PATTERN='(debug|the|a|error|serial|trace)[[:space:]]+log([[:space:]]|file|s|\.|,|$)|log[[:space:]]*file|check[[:space:]]+the[[:space:]]+log|enable[[:space:]]+logging|turn[[:space:]]+on[[:space:]]+logging|see[[:space:]]+the[[:space:]]+log'

bad=0
checked=0
for f in "$TOOLDIR"/*; do
    [ -f "$f" ] || continue
    case "$f" in
        *.map|*.o|*.a|*.cmake|*Makefile*) continue ;;
    esac
    hits=$(strings -n 6 "$f" 2>/dev/null | grep -inE "$PATTERN" || true)
    checked=$((checked + 1))
    [ -n "$hits" ] || continue

    while IFS= read -r line; do
        [ -n "$line" ] || continue
        echo "log_advice_found=$(basename "$f") text=${line#*:}"
        bad=$((bad + 1))
    done <<EOF
$hits
EOF
done

echo "log_advice_commands_checked=$checked"
echo "log_advice_offences=$bad"

if [ "$bad" = 0 ]; then
    echo "log_advice=clean"
    exit 0
fi

echo "log_advice=FOUND"
echo >&2
echo "A shipped command tells the user to consult a log." >&2
echo >&2
echo "  AMINETXDUO_LOG is off in every shipping drawer, so no such log is" >&2
echo "  ever written.  The advice cannot be followed by the person being" >&2
echo "  given it." >&2
echo >&2
echo "  Replace the sentence with something the user can ACT on: the" >&2
echo "  operation that failed, its code, and the file or the line to change." >&2
echo "  tests/tools/bringupfail-verdict.sh is the contract, and its selftest" >&2
echo "  has a worked example for each of the five causes." >&2
exit 1
