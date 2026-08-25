#!/usr/bin/env bash
# The compiler runtime, on every 68k this ships one binary for.
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

TIMEOUT=200
BUILD="${AMINETXDUO_BUILD:-build/ci/default}"
CLASSES="68000 68020 68030 68040 68060"

while getopts "b:t:c:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CLASSES="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-t seconds] [-c classes]" >&2
           echo "RESULT=refused"; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

BIN="$BUILD/tests/common/rt_test"
[ -f "$BIN" ] || {
    echo "reason=no_binary path=$BIN"
    echo "build it first: cmake --build $BUILD --parallel --target rt_test" >&2
    echo "RESULT=refused"; exit 2; }

[ -n "${AMINETXDUO_KICKSTART:-}${AMINETXDUO_KICKSTART_A600:-}" ] || {
    echo "reason=no_kickstart"
    echo "No Kickstart.  Set AMINETXDUO_KICKSTART=<rom>." >&2
    echo "RESULT=refused"; exit 2; }

model_of() {
    case "$1" in
        68000) echo "A600" ;;
        68020) echo "A1200" ;;
        *)     echo "A3000" ;;
    esac
}
cpu_arg_of() {
    case "$1" in
        68040|68060) echo "-c $1" ;;
        *) echo "" ;;
    esac
}

fails=0
ran=0

for class in $CLASSES; do
    model=$(model_of "$class")
    # shellcheck disable=SC2046
    AMINETXDUO_RUN_TAG="rt$class" "$ROOT/tools/amiberry-run.sh" \
        -t "$TIMEOUT" -m "$model" $(cpu_arg_of "$class") "$BIN" \
        > "$ROOT/build/rt-$class.log" 2>&1 &
    eval "pid_$class=\$!"
done

for class in $CLASSES; do
    model=$(model_of "$class")
    log="$ROOT/build/rt-$class.log"

    eval "wait \$pid_$class"
    rc=$?
    ran=$((ran + 1))

    verdict=$(grep -oE '^(PASS|FAIL)$' "$log" | tail -1)
    counts=$(grep -oE '^checks=[0-9]+ failures=[0-9]+$' "$log" | tail -1)
    attn=$(grep -oE '^attnflags=[0-9A-Fa-f]+' "$log" | tail -1)
    forms=$(grep -oE '^(portable|hardware|no-mulul)=ok' "$log" | sort -u | wc -l | tr -d ' ')

    if [ "$verdict" = PASS ] && [ "$rc" = 0 ]; then
        echo "rt_$class=pass model=$model $attn forms=$forms ${counts:-checks=?}"
    else
        fails=$((fails + 1))
        echo "rt_$class=FAIL model=$model $attn rc=$rc ${counts:-checks=none}"
        grep -E 'ILLEGAL|Illegal instruction|TIMEOUT|^FAIL ' "$log" |
            head -5 | sed 's/^/  /'
        echo "  log=$log"
    fi
done

echo "rt_classes=$ran rt_failed=$fails"
if [ "$fails" = 0 ]; then
    echo "RESULT=pass"
    exit 0
fi
echo "RESULT=fail"
exit 1
