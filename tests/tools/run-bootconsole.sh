#!/usr/bin/env bash
# `httpd <drawer> -C` OUT OF S:User-Startup, WITH NO WORKBENCH.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]" >&2
           exit 2 ;;
    esac
done

case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

PORT_NOSCREEN="${AMINETXDUO_BOOTCONSOLE_PORT:-8891}"
PORT_SCREEN=$((PORT_NOSCREEN + 1))

SMOKE="$BUILD/src/tools/ToolsSmoke"
HTTPD="$BUILD/src/tools/httpd"
FETCH="$BUILD/src/tools/fetch"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
ANXNET="$BUILD/src/netdev/anxnet.device"
CHIPSCREEN="$BUILD/tests/perf/chipscreen"
CONSOLE_HTML="$ROOT/src/tools/web/console.html"

for f in "$SMOKE" "$HTTPD" "$FETCH" "$BSD" "$ANXNET" "$CHIPSCREEN" \
         "$CONSOLE_HTML"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

if [ -z "${AMINETXDUO_KICKSTART:-}" ]; then
    echo "no AMINETXDUO_KICKSTART: this arm needs a ROM and nothing else." >&2
    exit 2
fi

STAGE="$BUILD/bootconsole-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/Networks" "$STAGE/devs/NetInterfaces"
cp "$BSD"          "$STAGE/libs/bsdsocket.library"
cp "$ANXNET"       "$STAGE/devs/Networks/anxnet.device"
cp "$HTTPD"        "$STAGE/httpd"
cp "$FETCH"        "$STAGE/fetch"
cp "$CHIPSCREEN"   "$STAGE/chipscreen"
cp "$CONSOLE_HTML" "$STAGE/console.html"
printf 'hello from the amiga\n' > "$STAGE/greeting.txt"
cp -R "$ROOT/tests/netstack/devs/Internet" "$STAGE/devs/Internet"

cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=DEVS:Networks/anxnet.device
UNIT=0
CARD=a2065
CONFIGURE=DHCP
EOF

cat > "$STAGE/commands.txt" <<EOF
# ---- the boot case: no screen has ever been opened on this machine -----
&SYS:httpd DH0: $PORT_NOSCREEN -C CONSOLEPAGE DH0:console.html >DH0:httpd-noscreen.txt
wait 12
SYS:fetch http://127.0.0.1:$PORT_NOSCREEN/greeting.txt TO DH0:fetched.txt
SYS:fetch http://127.0.0.1:$PORT_NOSCREEN/console TO DH0:consolepage.txt
# ---- and then a screen arrives ----------------------------------------
&SYS:chipscreen 00021800 320 256 6 DH0:chipscreen.txt
wait 8
&SYS:httpd DH0: $PORT_SCREEN -C CONSOLEPAGE DH0:console.html >DH0:httpd-screen.txt
wait 12
SYS:fetch http://127.0.0.1:$PORT_SCREEN/greeting.txt TO DH0:fetched-screen.txt
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-bootconsole}"

echo "==> httpd -C with no Workbench, port $PORT_NOSCREEN;" \
     "with a screen, port $PORT_SCREEN"

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
    "$SMOKE" \
    "$STAGE/devs" "$STAGE/libs" "$STAGE/httpd" "$STAGE/fetch" \
    "$STAGE/chipscreen" "$STAGE/console.html" "$STAGE/greeting.txt" \
    "$STAGE/commands.txt"
RUN_RC=$?
set -e

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

checks=0
bad=0

say() { printf '  %-6s %s\n' "$1" "$2"; }

ok()   { checks=$((checks + 1)); say ok "$1"; }
nope() { checks=$((checks + 1)); bad=$((bad + 1)); say FAIL "$1"; }

have() { [ -f "$HD/$1" ]; }

echo
echo "============================================================"
echo "  httpd -C on a machine with no Workbench"
echo "============================================================"

BANNER="$HD/httpd-noscreen.txt"

if ! have httpd-noscreen.txt; then
    nope "boot_httpd_wrote_nothing (the server never reached its banner)"
    BANNER=/dev/null
fi

echo "---- httpd's own output, no screen open ----"
cat "$BANNER" 2>/dev/null || echo "(none)"
echo

if grep -q "cannot serve a screen" "$BANNER" 2>/dev/null; then
    nope "boot_refused (-C still takes the server down when there is no screen)"
else
    ok "boot_not_refused"
fi

if grep -q "^Serving " "$BANNER" 2>/dev/null; then
    ok "boot_serving_banner"
else
    nope "boot_serving_banner (httpd never announced a port)"
fi

_said=$(grep -c "no screen is open yet" "$BANNER" 2>/dev/null) || _said=0
if [ "${_said:-0}" = "1" ]; then
    ok "boot_said_once"
else
    nope "boot_said_${_said:-0}_times (it owes exactly one line)"
fi

if have fetched.txt && cmp -s "$STAGE/greeting.txt" "$HD/fetched.txt"; then
    ok "boot_serves_files"
else
    nope "boot_serves_files (nothing, or not the bytes on the disk)"
fi

_want=$(wc -c < "$CONSOLE_HTML" | tr -d ' ')
_got=0
have consolepage.txt && _got=$(wc -c < "$HD/consolepage.txt" | tr -d ' ')
if [ "$_want" = "$_got" ]; then
    ok "boot_serves_console_page ($_got bytes)"
else
    nope "boot_serves_console_page (got ${_got} bytes, the page is $_want)"
fi

echo
echo "============================================================"
echo "  and then a screen arrives"
echo "============================================================"

echo "---- chipscreen ----"
cat "$HD/chipscreen.txt" 2>/dev/null || echo "(none)"
echo "---- httpd's own output, a screen in front ----"
cat "$HD/httpd-screen.txt" 2>/dev/null || echo "(none)"
echo

if have httpd-screen.txt &&
   grep -q "the frontmost screen," "$HD/httpd-screen.txt" &&
   ! grep -q "no screen is open yet" "$HD/httpd-screen.txt"; then
    ok "screen_arrives_reported"
else
    nope "screen_arrives_reported (-C no longer names the screen it found)"
fi

if have fetched-screen.txt &&
   cmp -s "$STAGE/greeting.txt" "$HD/fetched-screen.txt"; then
    ok "screen_arrives_serves"
else
    nope "screen_arrives_serves"
fi

echo
echo "bootconsole_checks=$checks bootconsole_fail=$bad run_rc=$RUN_RC"

if [ "$bad" != "0" ]; then
    echo "bootconsole=FAIL"
    exit 1
fi
echo "bootconsole=PASS"
exit 0
