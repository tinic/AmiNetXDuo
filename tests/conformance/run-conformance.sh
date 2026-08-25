#!/usr/bin/env bash
#
# Run the bsdsocktest conformance suite against our bsdsocket.library.
#
#   tests/conformance/run-conformance.sh [-m MODEL] [-c CPU] [-t SECONDS]
#                                        [-T TAG] [-a "SUITE ARGS"] [-p] [-b BUILDDIR]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=600
CPU=""
TAG="${AMINETXDUO_RUN_TAG:-conformance}"
ARGS="NOPAGE"
PROBE=0

while getopts "m:c:t:T:a:b:p" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        a) ARGS="$OPTARG" ;;
        b) AMINETXDUO_BUILD="$OPTARG" ;;
        p) PROBE=1 ;;
        *) echo "usage: $0 [-m model] [-c cpu] [-t secs] [-T tag] [-a args]" \
                "[-b builddir] [-p]" >&2
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

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

STAGE="$ROOT/build/conf-stage-$TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$UG"  "$STAGE/libs/usergroup.library"
cp "$SUITE" "$STAGE/bsdsocktest"
printf '%s\n' "$ARGS" > "$STAGE/conf-args"

export AMINETXDUO_RUN_TAG="$TAG"

set +e
if [ "$PROBE" = "1" ]; then
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" ${CPU:+-c "$CPU"} -t "$TIMEOUT" \
        "$ROOT/build/bsdsocktest/conf_probe" "$STAGE/devs" "$STAGE/libs"
    status=$?
    set -e
    exit "$status"
fi
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" ${CPU:+-c "$CPU"} -t "$TIMEOUT" \
    "$LAUNCHER" "$STAGE/devs" "$STAGE/libs" "$STAGE/bsdsocktest" \
    "$STAGE/conf-args"
status=$?
set -e

# ---- which library actually answered -------------------------------------
echo "---- stack under test ----"
ident=$(grep -m1 "^# bsdsocket.library:" \
        "$ROOT/build/amiberry-testhd-$TAG/bsdsocktest.log" 2>/dev/null || true)
case "$ident" in
    *AmiNetXDuo*)
        echo "$ident  (ours)"
        ;;
    "")
        echo "!! no stack identification in the TAP log, so nothing says which" >&2
        echo "!! library answered and the results cannot be attributed." >&2
        echo "conformance: NOT MEASURED (no stack identification)" >&2
        exit 3
        ;;
    *)
        echo "!! $ident" >&2
        echo "!! That is NOT our library. Whatever the suite reported, it" >&2
        echo "!! reported it about somebody else's stack." >&2
        echo "conformance: NOT MEASURED (a foreign bsdsocket.library answered)" >&2
        exit 3
        ;;
esac

# ---- the TAP log is the result -------------------------------------------
. "$ROOT/tests/conformance/tap-verdict.sh"

tap_verdict "$ROOT/build/amiberry-testhd-$TAG/bsdsocktest.log" "$status"
exit $?
