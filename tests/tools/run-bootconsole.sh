#!/usr/bin/env bash
#
# `httpd <drawer> -C` OUT OF S:User-Startup, WITH NO WORKBENCH.
#
#   tests/tools/run-bootconsole.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT IT PROVES
#
#   A user puts `httpd DH0: -C' in S:User-Startup and reboots.  At that point
#   in the boot NOTHING HAS OPENED A SCREEN -- LoadWB is at the END of the
#   Startup-Sequence, so Intuition's screen list is empty -- and -C used to
#   refuse to start over exactly that and take the whole server down with it:
#
#     httpd: -C cannot serve a screen: there are no screens: Intuition's
#     screen list is empty
#     ----- rc 10 -----
#
#   measured on this rig against the tree before the fix, while the same
#   command with -T in the same boot served fine.  That is why the autonomous
#   A1200 rig's own boot line had to drop -C, and it is user-filed
#   (docs/BACKLOG.md, "`httpd -C` dies at boot instead of degrading").
#
#   So this arm boots a machine that has no Workbench on it AT ALL and asserts
#   the whole of the repaired behaviour:
#
#     the server comes up rather than exiting;
#     it SERVES -- a file is fetched off it and compared byte for byte;
#     /console answers with its page, at the size of the page on the disk;
#     it says the screen is missing ONCE, in the banner, and not as an error;
#     and, once a screen does exist, a fresh -C reports that screen's
#     geometry instead -- so the degradation is conditional and the preflight
#     was not simply deleted.
#
#   Four of those six go red on a pristine tree, because there the server is
#   not running at all by the time anything asks it anything.
#
# WHY THERE IS NO WORKBENCH IN IT, AND NO LoadWB
#
#   tools/amiberry-run.sh boots a bare directory hard drive and writes its own
#   Startup-Sequence, which has no LoadWB in it.  That is not a limitation
#   being worked around here: it IS the condition under test, and it is the
#   same condition S:User-Startup runs in on a real machine.  The screen-side
#   half -- a real Workbench 3.1 screen encoded and decoded as pixels -- is
#   tests/tools/run-console.sh's, which needs an asset Workbench and a peer.
#
# WHY IT NEEDS NO PEER, NO BRIDGE AND NOBODY ELSE'S DRIVER
#
#   The whole exchange is on 127.0.0.1, guest to guest: httpd binds, `fetch'
#   connects to it, and the bytes are compared here afterwards.  So there is no
#   second machine in this and nothing has to reach the guest from outside it.
#
#   It does need the stack to START, and that is not free: bsdsocket.library
#   brings the stack up when httpd opens it, and with no interface it can open,
#   it does not come up at all -- measured, "httpd: the network did not start",
#   which is a refusal for a different reason and would have read as this
#   defect.  So one interface is staged, and it names OUR OWN anxnet.device on
#   the emulated A2065, which is the one driver this tree builds and can
#   therefore stage without an asset store.  Nothing is asked of that interface
#   afterwards: no address is used, no lease is waited for, and the arm is
#   green on a machine where DHCP never answers.
#
#   Which is also why the whole ingredient list is a Kickstart.
#
# THE SCREEN THAT ARRIVES is tests/perf/chipscreen.c, which opens a screen of
# its own and holds it in front until CTRL-C.  It is used here for nothing but
# its existence: this arm never looks at a pixel, it only asks what the next
# -C says about the screen it found.
#
# Exit status: 0 pass, 1 something under test failed, 2 an ingredient is
# missing on this machine.
#
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

# Ports above 1024 and out of the way.  They are only ever reached from inside
# the guest, but the emulator's SLIRP does bind on the host for other things
# and a collision reads as a server that would not start.
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

# ------------------------------------------------------------- staging ---

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

# One interface, and it is here so that the STACK starts rather than so that
# anything is sent over it.  CONFIGURE=DHCP because that is what a machine has;
# nothing below waits for the lease, so a rig whose SLIRP never answers is
# still a run of this arm.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=DEVS:Networks/anxnet.device
UNIT=0
CARD=a2065
CONFIGURE=DHCP
EOF

# CONSOLEPAGE= names the page outright rather than leaving httpd to find it.
# Where the page lives is a separate question with a separate answer -- the
# installer's AmiNetXDuo: assign -- and a run that failed because a search path
# came up empty would read as this defect and be a different one.
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

# -------------------------------------------------------------- verdict ---

checks=0
bad=0

say() { printf '  %-6s %s\n' "$1" "$2"; }

ok()   { checks=$((checks + 1)); say ok "$1"; }
nope() { checks=$((checks + 1)); bad=$((bad + 1)); say FAIL "$1"; }

# A file the guest never wrote is not an empty file, it is a phase that did not
# happen, and reading one as the other is how a harness passes vacuously.
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

# 1.  IT DID NOT REFUSE.  This exact sentence is what the pristine tree prints
#     before exiting 10, so it is matched literally.
if grep -q "cannot serve a screen" "$BANNER" 2>/dev/null; then
    nope "boot_refused (-C still takes the server down when there is no screen)"
else
    ok "boot_not_refused"
fi

# 2.  IT CAME UP SERVING.
if grep -q "^Serving " "$BANNER" 2>/dev/null; then
    ok "boot_serving_banner"
else
    nope "boot_serving_banner (httpd never announced a port)"
fi

# 3.  AND IT SAID SO ONCE.  Once, not at all and not on every pass: the count
#     is the assertion, because a line repeated per connection would be a log
#     that fills a disk on a machine nobody is watching.
_said=$(grep -c "no screen is open yet" "$BANNER" 2>/dev/null) || _said=0
if [ "${_said:-0}" = "1" ]; then
    ok "boot_said_once"
else
    nope "boot_said_${_said:-0}_times (it owes exactly one line)"
fi

# 4.  IT SERVES FILES.
if have fetched.txt && cmp -s "$STAGE/greeting.txt" "$HD/fetched.txt"; then
    ok "boot_serves_files"
else
    nope "boot_serves_files (nothing, or not the bytes on the disk)"
fi

# 5.  AND /console ANSWERS, with the page rather than with a refusal.
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

# 6.  THE PREFLIGHT STILL WORKS.  Deleting the check would pass every assertion
#     above and this is what tells that apart from repairing it: with a screen
#     in front, the banner reports its geometry and does not claim there is
#     none.
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

# The emulator's own status is reported and does not decide the verdict: a
# timeout with every file written and every byte right is the run being slow,
# and a clean exit with nothing written is not a pass.
if [ "$bad" != "0" ]; then
    echo "bootconsole=FAIL"
    exit 1
fi
echo "bootconsole=PASS"
exit 0
