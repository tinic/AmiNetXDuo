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

PORT="${AMINETXDUO_BOOTCONSOLE_PORT:-8891}"

# What the screen that arrives late is, and what the session must therefore
# report.  Fixed here rather than spelled out three times below.
SCREEN_MODE=00021800
SCREEN_W=320
SCREEN_H=256
SCREEN_D=6

SMOKE="$BUILD/src/tools/ToolsSmoke"
HTTPD="$BUILD/src/tools/httpd"
FETCH="$BUILD/src/tools/fetch"
NC="$BUILD/src/tools/nc"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
ANXNET="$BUILD/src/netdev/anxnet.device"
CHIPSCREEN="$BUILD/tests/perf/chipscreen"
CONSOLE_HTML="$ROOT/src/tools/web/console.html"

for f in "$SMOKE" "$HTTPD" "$FETCH" "$NC" "$BSD" "$ANXNET" "$CHIPSCREEN" \
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
cp "$NC"           "$STAGE/nc"
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

# THE CONSOLE SESSION, ASKED FOR BY nc.  A browser is the only other client
# this has, and there is none on the guest: the handshake is a fixed request
# and `nc CRLF` turns each line ending into the CRLF the protocol wants.  The
# key is RFC 6455's own example, which is what the accept is computed from --
# the server checks that it is 24 base64 characters and nothing else.
#
# WHY IT MATTERS THAT THIS IS THE SAME httpd.  http_fb_open() ran before any
# screen existed and remembered that; http_fb_start() reads the front screen
# AGAIN, per session, and only that second read can name the screen that
# arrived in between.  A second server started after the screen proves the
# first read, not this one.
cat > "$STAGE/wsreq.txt" <<EOF
GET /console HTTP/1.1
Host: 127.0.0.1:$PORT
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
Sec-WebSocket-Version: 13

EOF

cat > "$STAGE/commands.txt" <<EOF
# ---- the boot case: no screen has ever been opened on this machine -----
&SYS:httpd DH0: $PORT -C CONSOLEPAGE DH0:console.html >DH0:httpd.txt
wait 12
SYS:fetch http://127.0.0.1:$PORT/greeting.txt TO DH0:fetched.txt
SYS:fetch http://127.0.0.1:$PORT/console TO DH0:consolepage.txt
# ---- and then a screen arrives, to the server that is ALREADY serving ----
&SYS:chipscreen $SCREEN_MODE $SCREEN_W $SCREEN_H $SCREEN_D DH0:chipscreen.txt
wait 8
&SYS:nc 127.0.0.1 $PORT CRLF <DH0:wsreq.txt >DH0:session.bin
wait 20
SYS:fetch http://127.0.0.1:$PORT/greeting.txt TO DH0:fetched-screen.txt
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-bootconsole}"

echo "==> one httpd -C with no Workbench on port $PORT, and a screen after it"

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
    "$SMOKE" \
    "$STAGE/devs" "$STAGE/libs" "$STAGE/httpd" "$STAGE/fetch" "$STAGE/nc" \
    "$STAGE/chipscreen" "$STAGE/console.html" "$STAGE/greeting.txt" \
    "$STAGE/wsreq.txt" "$STAGE/commands.txt"
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

BANNER="$HD/httpd.txt"

if ! have httpd.txt; then
    nope "boot_httpd_wrote_nothing (the server never reached its banner)"
    BANNER=/dev/null
fi

echo "---- httpd's own output, the whole session ----"
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
echo "  and then a screen arrives, to the server already running"
echo "============================================================"

echo "---- chipscreen ----"
cat "$HD/chipscreen.txt" 2>/dev/null || echo "(none)"
echo

if grep -q "^result=open" "$HD/chipscreen.txt" 2>/dev/null; then
    ok "screen_opened"
else
    nope "screen_opened (no screen arrived, so nothing below is a console test)"
fi

# THE ASSERTION THIS FILE EXISTS FOR.  The line is httpd_log_console_start()'s,
# it is written when a session starts and not before, and the numbers in it are
# the ones http_fb_start() read for THIS session.  The startup banner above
# said there was no screen, so a size here can only have come from the re-read.
WANT="console started: frontmost screen ${SCREEN_W}x${SCREEN_H}x${SCREEN_D}"

if grep -q "console did not start" "$BANNER" 2>/dev/null; then
    nope "session_started ($(grep -m1 'console did not start' "$BANNER"))"
elif grep -q "$WANT" "$BANNER" 2>/dev/null; then
    ok "session_picked_up_the_late_screen ($WANT)"
elif grep -q "console started" "$BANNER" 2>/dev/null; then
    nope "session_picked_up_the_late_screen (it started on \
$(grep -m1 'console started' "$BANNER" | sed 's/.*screen //'), not \
${SCREEN_W}x${SCREEN_H}x${SCREEN_D})"
else
    nope "session_picked_up_the_late_screen (no session ever started)"
fi

_bytes=0
have session.bin && _bytes=$(wc -c < "$HD/session.bin" | tr -d ' ')
if [ "${_bytes:-0}" -ge 4096 ]; then
    ok "session_sent_pixels ($_bytes bytes down the socket)"
else
    nope "session_sent_pixels (only ${_bytes:-0} bytes came back)"
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
