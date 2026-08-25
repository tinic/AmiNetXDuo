#!/usr/bin/env bash
# THE WEBSOCKET TERMINAL, AND THE WEBDAV DRILL THAT NOTHING USED TO RUN.
#   a2065.device (AMINETXDUO_A2065, or build/a2065.device), a Kickstart, and a
#   68020 Release build:
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

WINDOW=300
HOSTPORT=18080
GUESTPORT=8080
MODEL=A1200
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
GUEST_IP=10.0.2.15

while getopts "t:p:P:b:m:c:" opt; do
    case "$opt" in
        t) WINDOW="$OPTARG" ;;
        p) HOSTPORT="$OPTARG" ;;
        P) GUESTPORT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        *) echo "usage: $0 [-t seconds] [-p hostport] [-P guestport]" \
                "[-b builddir] [-m model] [-c cpu]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
PAGE="$ROOT/src/tools/web/shell.html"
PAGEGZ="$PAGE.gz"

for f in "$TOOLS/httpd" "$BSD" "$PAGE" "$PAGEGZ"; do
    [ -f "$f" ] || { echo "result=infra"; echo "missing $f, build the tree first" >&2; exit 2; }
done

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
[ -n "$A2065" ] && [ -f "$A2065" ] || {
    echo "result=infra"
    echo "No a2065.device found.  Set AMINETXDUO_A2065=<path>." >&2
    exit 2
}

STAGE="$ROOT/build/wsterm-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/Public/Docs" "$STAGE/Public/Empty Drawer"

cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$PAGE"   "$STAGE/Public/shell.html"
cp "$PAGEGZ" "$STAGE/Public/shell.html.gz"

cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
EOF

echo "Hello from an Amiga." > "$STAGE/Public/readme.txt"
echo "<html><body><h1>Amiga</h1></body></html>" > "$STAGE/Public/index.html"
echo "one two three" > "$STAGE/Public/My File [!].txt"
echo "in a drawer" > "$STAGE/Public/Docs/notes.txt"
: > "$STAGE/Public/empty.dat"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-wsterm}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

export AMINETXDUO_AMIBERRY_EXTRA="slirp_redir=tcp:${HOSTPORT}:${GUESTPORT}:${GUEST_IP}"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

echo "==> httpd on the guest at :${GUESTPORT}, forwarded to 127.0.0.1:${HOSTPORT}"

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -B slirp -m "$MODEL" "${CPUARG[@]}" \
    -t "$WINDOW" \
    -a "DH0:Public $GUESTPORT -T PAGE=DH0:Public/shell.html TRACE" \
    "$TOOLS/httpd" "$STAGE/devs" "$STAGE/libs" "$STAGE/Public" \
    > "$ROOT/build/wsterm-emu.log" 2>&1 &
RUNNER=$!
set -e

cleanup() {
    if kill -0 "$RUNNER" 2>/dev/null; then
        kill "$RUNNER" 2>/dev/null || true
        wait "$RUNNER" 2>/dev/null || true
    fi
}
trap cleanup EXIT

BOOT_MAX=${AMINETXDUO_WSTERM_BOOT:-180}
BOOT_AT=0
FORWARD=failed
STARTED=$(date +%s)

for _ in $(seq 1 "$BOOT_MAX"); do
    sleep 1
    kill -0 "$RUNNER" 2>/dev/null || break
    code=$(curl -s -m 3 -o /dev/null -w '%{http_code}' \
           "http://127.0.0.1:${HOSTPORT}/" 2>/dev/null || true)
    if [ "$code" = "200" ]; then
        FORWARD=ok
        BOOT_AT=$(( $(date +%s) - STARTED ))
        break
    fi
done

echo "forward=$FORWARD"
echo "boot_seconds=$BOOT_AT"

if [ "$FORWARD" != ok ]; then
    echo "checks=0"
    echo "failures=0"
    echo "result=infra"
    echo "!! nothing answered on 127.0.0.1:${HOSTPORT} within ${BOOT_MAX}s." >&2
    echo "!! Either the guest never booted or the SLIRP forward did not take;" >&2
    echo "!! build/wsterm-emu.log and $HD/stdout.txt say which." >&2
    sed -n '1,40p' "$HD/stdout.txt" 2>/dev/null >&2 || true
    exit 2
fi

DRILL_AT=$(date +%s)
set +e
python3 "$ROOT/tests/tools/httpd-drill.py" --terminal \
    --gz-url=/shell.html.gz 127.0.0.1 "$HOSTPORT" \
    > "$ROOT/build/wsterm-drill.txt" 2>&1 &
DRILL=$!
while kill -0 "$DRILL" 2>/dev/null; do
    sleep 5
done
wait "$DRILL"
DRILL_RC=$?
set -e
DRILL_SECS=$(( $(date +%s) - DRILL_AT ))

cat "$ROOT/build/wsterm-drill.txt"

CHECKS=$(sed -n 's/^\([0-9]\{1,\}\) checks.*/\1/p' "$ROOT/build/wsterm-drill.txt" | tail -1)
FAILS=$(sed -n 's/^[0-9]\{1,\} checks, \([0-9]\{1,\}\) failure.*/\1/p' "$ROOT/build/wsterm-drill.txt" | tail -1)

echo
echo "===================== the guest's own log ======================="
if [ -f "$HD/stdout.txt" ]; then
    cat "$HD/stdout.txt"
else
    echo "(the guest wrote no stdout.txt)"
fi
echo "================================================================"
echo

if [ "$DRILL_RC" -eq 0 ] && [ "${FAILS:-1}" -eq 0 ]; then
    set +e
    python3 "$ROOT/tests/tools/wsterm-bench.py" 127.0.0.1 "$HOSTPORT" \
        > "$ROOT/build/wsterm-bench.txt" 2>&1
    set -e
    echo
    echo "===================== what the session costs ===================="
    cat "$ROOT/build/wsterm-bench.txt"
    echo "================================================================"
fi

cleanup
trap - EXIT

echo "drill_seconds=$DRILL_SECS"
echo "checks=${CHECKS:-0}"
echo "failures=${FAILS:-0}"

if [ -z "${CHECKS:-}" ]; then
    echo "result=infra"
    echo "!! the drill printed no tally; it did not reach the end" >&2
    exit 2
fi

if [ "$DRILL_RC" -ne 0 ] || [ "${FAILS:-1}" -ne 0 ]; then
    echo "result=fail"
    exit 1
fi

echo "result=pass"
exit 0
