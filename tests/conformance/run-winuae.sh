#!/usr/bin/env bash
# Run the bsdsocktest conformance suite against our bsdsocket.library, under
# WinUAE on a remote Windows host.  The WinUAE counterpart of run-winuae.sh.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=600
CPU=""
TAG="${AMINETXDUO_RUN_TAG:-conformance}"
ARGS="NOPAGE"
PROBE=0

BOARD=a2065

while getopts "m:c:t:T:a:b:N:p" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        a) ARGS="$OPTARG" ;;
        b) AMINETXDUO_BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        p) PROBE=1 ;;
        *) echo "usage: $0 [-m model] [-c cpu] [-t secs] [-T tag] [-a args]" \
                "[-b builddir] [-N board] [-p]" >&2
           exit 2 ;;
    esac
done

SUITE="$ROOT/build/bsdsocktest/bsdsocktest"
LAUNCHER="$ROOT/build/bsdsocktest/conf_launcher"
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
UG="$ROOT/$BUILD/src/usergroup/usergroup.library"

for f in "$SUITE" "$LAUNCHER"; do
    [ -f "$f" ] || { echo "missing $f, run tests/conformance/build.sh" >&2; exit 2; }
done
for f in "$BSD" "$UG"; do
    [ -f "$f" ] || { echo "missing $f, build bsdsocket_library usergroup_library" >&2; exit 2; }
done

. "$ROOT/tools/sana2-stage.sh"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ] && [ "$BOARD" = a2065 ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
    [ -n "$A2065" ] && [ -f "$A2065" ] || {
        echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
        exit 2
    }
fi

STAGE="$ROOT/build/conf-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
[ -z "$A2065" ] || cp "$A2065" "$STAGE/devs/a2065.device"
sana2_stage "$BOARD" "$STAGE/devs"
echo "==> $BOARD: $SANA2_DRIVER, opened as '$SANA2_DEVICE'"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$UG"  "$STAGE/libs/usergroup.library"
cp "$SUITE" "$STAGE/bsdsocktest"
printf '%s\n' "$ARGS" > "$STAGE/conf-args"

if [ -n "${AMINETXDUO_CONF_HELPER:-}" ]; then
    HELPER_SRC="$ROOT/third_party/bsdsocktest/host/bsdsocktest_helper.py"
    [ -f "$HELPER_SRC" ] || { echo "missing $HELPER_SRC" >&2; exit 2; }
    HELPER_SH="$ROOT/build/conf-helper-start.sh"
    cat > "$HELPER_SH" <<'EOF'
#!/bin/sh
pkill -f bsdsocktest_helper 2>/dev/null
sleep 1
setsid nohup python3 /tmp/bsdsocktest_helper.py -v --bind 0.0.0.0 \
    > /tmp/helper.log 2>&1 < /dev/null &
sleep 2
pgrep -f bsdsocktest_helper > /dev/null
EOF
    echo "==> restarting the helper on $AMINETXDUO_CONF_HELPER"
    scp -q "$HELPER_SRC" "$AMINETXDUO_CONF_HELPER:/tmp/bsdsocktest_helper.py"
    scp -q "$HELPER_SH" "$AMINETXDUO_CONF_HELPER:/tmp/anxd-conf-helper.sh"
    ssh "$AMINETXDUO_CONF_HELPER" 'sh /tmp/anxd-conf-helper.sh' \
        || { echo "the helper did not start on $AMINETXDUO_CONF_HELPER" >&2
             exit 2; }
fi

export AMINETXDUO_RUN_TAG="$TAG"

set +e
if [ "$PROBE" = "1" ]; then
    "$ROOT/tools/winuae-run.sh" -N "$BOARD" -m "$MODEL" ${CPU:+-c "$CPU"} -t "$TIMEOUT" \
        "$ROOT/build/bsdsocktest/conf_probe" "$STAGE/devs" "$STAGE/libs"
    status=$?
    set -e
    exit "$status"
fi
"$ROOT/tools/winuae-run.sh" -N "$BOARD" -m "$MODEL" ${CPU:+-c "$CPU"} -t "$TIMEOUT" \
    "$LAUNCHER" "$STAGE/devs" "$STAGE/libs" "$STAGE/bsdsocktest" \
    "$STAGE/conf-args"
status=$?
set -e

echo "---- stack under test ----"
ident=$(grep -m1 "^# bsdsocket.library:" \
        "$ROOT/build/winuae-testhd-$TAG/bsdsocktest.log" 2>/dev/null || true)
case "$ident" in
    *AmiNetXDuo*) echo "$ident  (ours)" ;;
    "")           echo "!! no stack identification in the TAP log" >&2
                  echo "conformance: NOT MEASURED (no stack identification)" >&2
                  exit 3 ;;
    *)            echo "!! $ident, NOT our library" >&2
                  echo "conformance: NOT MEASURED (a foreign library answered)" >&2
                  exit 3 ;;
esac

. "$ROOT/tests/conformance/tap-verdict.sh"

tap_verdict "$ROOT/build/winuae-testhd-$TAG/bsdsocktest.log" "$status"
exit $?
