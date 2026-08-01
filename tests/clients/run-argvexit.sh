#!/usr/bin/env bash
#
# Run ArgvExit several times under the emulator and check the 256 KB client
# stack comes back each time.
#
# The leak it guards was 256 KB per invocation of every ported client -- see
# tests/clients/argvexit.c and the note above __wrap__exit() in
# clients/compat/amiga_argv.c. It is here rather than in tests/tools because it
# links the client shim, not the library.
#
#   tests/clients/run-argvexit.sh [-m model] [-t seconds] [-n runs] [-A]
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=240
RUNS=5
# FS-UAE needs an X server; on a headless Linux box it dies in GLAD before the
# guest boots, so -A picks Amiberry, which runs genuinely headless.
RUNNER="${AMINETXDUO_RUNNER:-fsuae}"
BOARD=a2065
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-slirp}"

while getopts "m:t:n:AN:B:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        n) RUNS="$OPTARG" ;;
        A) RUNNER=amiberry ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-n runs] [-A]" >&2; exit 2 ;;
    esac
done

# Built the way a client is built, because that is what is under test: the same
# compiler, the same crt0, and the same three --wrap flags.
. "$ROOT/clients/amiga-client.sh"
amiga_client_prepare || { echo "no cross toolchain" >&2; exit 2; }

OUT="$ROOT/build/argvexit"
mkdir -p "$(dirname "$OUT")"
# shellcheck disable=SC2086
$AMIGA_CLIENT_CC $AMIGA_CLIENT_CFLAGS \
    "$ROOT/tests/clients/argvexit.c" \
    "$ROOT/clients/compat/amiga_argv.c" \
    -o "$OUT" $AMIGA_CLIENT_LDFLAGS

TOOLS="$ROOT/${AMINETXDUO_BUILD:-build/cm}/src/tools"
[ -f "$TOOLS/ToolsSmoke" ] || { echo "build ${AMINETXDUO_BUILD:-build/cm} first" >&2; exit 2; }

STAGE="$ROOT/build/argvexit-stage"
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp "$OUT" "$STAGE/ArgvExit"

: > "$STAGE/commands.txt"
for _ in $(seq 1 "$RUNS"); do echo "SYS:ArgvExit" >> "$STAGE/commands.txt"; done

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-argvexit}"

set +e
if [ "$RUNNER" = "amiberry" ]; then
    HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/ArgvExit"
else
    HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"
    "$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/ArgvExit"
fi
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the guest printed ====================="
grep "ArgvExit: free" "$REPORT" || true
echo "================================================================="

mapfile -t FREE < <(sed -n 's/.*ArgvExit: free \([0-9]*\).*/\1/p' "$REPORT")
fail=0
if [ "${#FREE[@]}" -ne "$RUNS" ]; then
    echo "FAIL: $RUNS runs asked for, ${#FREE[@]} reported -- the probe did not run" >&2
    exit 1
fi

# The stack is 256 KB. Anything approaching that per run is the leak back; a
# few hundred bytes of drift is the Shell's own churn between invocations.
BUDGET=8192
first=${FREE[0]}
last=${FREE[$((${#FREE[@]} - 1))]}
drop=$(( first - last ))
per=$(( drop / (RUNS - 1) ))

echo "  first $first, last $last, $per bytes per run (budget $BUDGET)"
if [ "$per" -gt "$BUDGET" ]; then
    echo "FAIL: a client invocation loses $per bytes -- the 256 KB stack is not coming back" >&2
    fail=1
else
    echo "  ok: the client stack comes back on exit()"
fi

[ "$fail" -eq 0 ] && echo "argvexit: PASSED" || { echo "argvexit: FAILED"; exit 1; }
