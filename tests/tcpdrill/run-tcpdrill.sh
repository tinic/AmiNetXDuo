#!/usr/bin/env bash
#
# Run the tcpdrill scripts under an emulator.
#
#   tests/tcpdrill/run-tcpdrill.sh [-m MODEL] [-t SECS] [-b BUILD] [-T TAG]
#                                  [-s SCRIPT|all]
#
# Amiberry, and only Amiberry.  This carried RUNNER=${AMINETXDUO_RUNNER:-fsuae}
# and a -A to flip it, and neither ever selected anything: the run below has
# always been tools/amiberry-run.sh.  All the default did was name the fs-uae
# serial log, build/serial-<tag>.log, for a transcript amiberry-run.sh writes
# to build/amiberry-serial-<tag>.log -- so every failing case sent the reader
# to a file that does not exist.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$ROOT/tools/test-verdict.sh"
MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"
TAG="tcpdrill"
SCRIPT="$ROOT/tests/tcpdrill/scripts/tcp.drill"

while getopts "m:t:b:T:s:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        s) SCRIPT="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t secs] [-b build] [-T tag] [-s script]" >&2
           exit 2 ;;
    esac
done

BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
DRILL="$ROOT/$BUILD/tests/tcpdrill/TcpDrill"

if [ "$SCRIPT" = all ]; then
    rc=0
    nskip=0

    for s in "$ROOT"/tests/tcpdrill/scripts/*.drill; do
        name=$(basename "$s" .drill)
        out="$ROOT/build/tcpdrill-$TAG-$name.out"

        if [ "$name" = keepalive ] &&
           ! grep -q '^AMINETXDUO_TCP_KEEPALIVE_INITIAL:STRING=[0-9]' \
                 "$ROOT/$BUILD/CMakeCache.txt" 2>/dev/null; then
            echo "$name: SKIP, needs -DAMINETXDUO_TCP_KEEPALIVE_INITIAL=5"
            nskip=$((nskip + 1))
            continue
        fi


        arm=0
        "$0" -m "$MODEL" -t "$TIMEOUT" -b "$BUILD" -T "$TAG-$name" -s "$s" \
            > "$out" 2>&1 || arm=$?

        verdict=$(grep -E 'case\(s\)' "$out" | tail -1)
        echo "$name: ${verdict:-no verdict} rc=$arm"
        [ "$arm" -eq 0 ] || rc=1
    done

    if [ "$rc" -ne 0 ]; then
        echo "tcpdrill_all=FAIL skipped=$nskip"
        exit "$rc"
    fi
    if [ "$nskip" -ne 0 ]; then
        echo "tcpdrill_all=PARTIAL skipped=$nskip"
        echo "  $nskip script(s) did not run; the SKIP lines above say why." >&2
        exit 77
    fi
    echo "tcpdrill_all=PASS skipped=0"
    exit 0
fi

for f in "$BSD" "$DRILL" "$SCRIPT"; do
    [ -f "$f" ] || { echo "missing $f, build it first" >&2; exit 2; }
done

STAGE="$ROOT/build/tcpdrill-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/tcpdrill/devs" "$STAGE/devs"
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
cp "$DRILL"  "$STAGE/TcpDrill"
cp "$SCRIPT" "$STAGE/drill.txt"

echo "==> stack:  $BUILD"
EXPECTED_CASES=$(grep -c '^case ' "$SCRIPT")
echo "==> script: $SCRIPT ($EXPECTED_CASES cases)"

export AMINETXDUO_RUN_TAG="$TAG"

RUN=("$ROOT/tools/amiberry-run.sh")
HD="$ROOT/build/amiberry-testhd-$TAG"

set +e
"${RUN[@]}" -m "$MODEL" -t "$TIMEOUT" \
    "$DRILL" "$STAGE/devs" "$STAGE/libs" "$STAGE/drill.txt" \
    > "$ROOT/build/tcpdrill-$TAG.log" 2>&1
RC=$?
set -e

echo
echo "================ tcpdrill ================"
if [ -f "$HD/tcpdrill.txt" ]; then
    cat "$HD/tcpdrill.txt"
else
    echo "(no tcpdrill.txt, the run did not get that far)"
    [ -f "$HD/stdout.txt" ] && { echo "--- stdout ---"; cat "$HD/stdout.txt"; }
fi

echo
echo "emulator log: build/tcpdrill-$TAG.log"
echo "serial log:   build/amiberry-serial-$TAG.log"
echo "guest files:  $HD"

verdict_guest_cases "tcpdrill-$(basename "$SCRIPT" .drill)" \
    "$EXPECTED_CASES" "$EXPECTED_CASES" "$RC" "$HD/tcpdrill.txt"
