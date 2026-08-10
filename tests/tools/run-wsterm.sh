#!/usr/bin/env bash
#
# THE WEBSOCKET TERMINAL, AND THE WEBDAV DRILL THAT NOTHING USED TO RUN.
#
#   tests/tools/run-wsterm.sh [-t SECONDS] [-p HOSTPORT] [-P GUESTPORT]
#                             [-b BUILDDIR] [-m MODEL] [-c CPU]
#
# WHAT IT PROVES
#
#   An emulated Amiga serves /terminal, upgrades a real WebSocket, and runs a
#   command in an AmigaDOS Shell whose output comes back through the socket.
#   Every check is tests/tools/httpd-drill.py's, at the level of bytes, because
#   a library client would hide exactly the things that must be right: the
#   accept value, the mask, and where a fragment ends.
#
#   It also runs that file's TWELVE EXISTING WebDAV checks, which nothing in
#   this tree invoked -- they were written, committed, and never called by any
#   script or CI stage.  Adding a second suite to the same file and then
#   inventing a second runner for it would have left the first twelve exactly
#   as unreachable as they were, so there is one runner and it runs both.
#
# SLIRP, AND HOW A GUEST BEHIND NAT IS CONNECTED TO
#
#   SLIRP's guest cannot normally be reached from outside, which is why
#   tests/tools/run-httpd.sh is bridged.  It does not have to be: Amiberry
#   carries UAE's `slirp_redir`, so one line of configuration forwards a host
#   port to a guest one and the drill connects to 127.0.0.1.  That keeps this
#   run off the LAN, needs no address nobody else is using, and needs no
#   second machine.
#
#     slirp_redir=tcp:<host port>:<guest port>:10.0.2.15
#
#   The forward is ASSERTED before the drill runs, because a redirection that
#   did not take produces "connection refused" from the drill, which reads
#   exactly like a server that never started.
#
# A TIMEOUT IS A DEFECT
#
#   Two ceilings, and both are measured rather than picked.  BOOT is how long
#   the guest may take to answer its first request; AMINETXDUO_WS_WAIT is how
#   long ONE exchange with the Shell may take.  Both are reported at the end
#   as what they actually cost, so the margin is visible and a run that
#   creeps towards its ceiling is noticed before it burns one.  A run that
#   burns either says which exchange it was and exits 2 -- infrastructure,
#   not a result.  Raising one is never the fix.
#
# WHAT IT PRINTS
#
#   key=value lines and an exit code, and nothing a caller has to read prose
#   out of:
#
#     boot_seconds=NN        how long the guest took to answer at all
#     forward=ok|failed      whether the SLIRP redirection took
#     checks=NN              assertions run
#     failures=NN            assertions that did not hold
#     result=pass|fail|infra
#
#   0 pass, 1 a failed assertion, 2 infrastructure (no forward, no boot, a
#   ceiling burned) -- the last is not a result about the software.
#
# WHAT IT NEEDS
#
#   a2065.device (AMINETXDUO_A2065, or build/a2065.device), a Kickstart, and a
#   68020 Release build:
#
#     cmake -S . -B build/cm -DCMAKE_BUILD_TYPE=Release \
#           -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake
#     cmake --build build/cm --parallel
#     tests/tools/run-wsterm.sh
#
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
PAGE="$ROOT/src/tools/web/terminal.html"

for f in "$TOOLS/httpd" "$BSD" "$PAGE"; do
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

# ------------------------------------------------------------- staging ---
#
# The shape tests/tools/run-httpd.sh stages, plus the terminal's page.  The
# content is deliberately awkward in the same way: a space and a bracket in a
# name is what percent-encoding is for.

STAGE="$ROOT/build/wsterm-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/Public/Docs" "$STAGE/Public/Empty Drawer"

cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
mkdir -p "$STAGE/devs/Networks"
cp "$A2065" "$STAGE/devs/Networks/a2065.device"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$PAGE" "$STAGE/terminal.html"

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

# ------------------------------------------------------------------ run ---
#
# httpd is the only executable: opening bsdsocket.library is what starts the
# network, so the interface comes up underneath the server.  The server never
# exits, so the run ends on the harness timeout and 124 is expected.

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-wsterm}"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

export AMINETXDUO_AMIBERRY_EXTRA="slirp_redir=tcp:${HOSTPORT}:${GUESTPORT}:${GUEST_IP}"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

echo "==> httpd on the guest at :${GUESTPORT}, forwarded to 127.0.0.1:${HOSTPORT}"

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -B slirp -m "$MODEL" "${CPUARG[@]}" \
    -t "$WINDOW" \
    -a "DH0:Public $GUESTPORT TERMINAL=DH0:terminal.html TRACE" \
    "$TOOLS/httpd" "$STAGE/devs" "$STAGE/libs" "$STAGE/Public" \
    "$STAGE/terminal.html" > "$ROOT/build/wsterm-emu.log" 2>&1 &
RUNNER=$!
set -e

cleanup() {
    if kill -0 "$RUNNER" 2>/dev/null; then
        kill "$RUNNER" 2>/dev/null || true
        wait "$RUNNER" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ------------------------------------------------------- is it there yet ---
#
# Polled rather than slept: the boot cost is one of the numbers this run
# reports, and a fixed sleep would hide it.  BOOT_MAX is the ceiling, and
# what a good boot actually costs is printed beside it.

BOOT_MAX=${AMINETXDUO_WSTERM_BOOT:-180}
BOOT_AT=0
FORWARD=failed
STARTED=$(date +%s)

# The emulator's own log grows at roughly a megabyte a second and playhouse3
# is shared; a five-minute run left 313 MB of it and filled the disk twice,
# which reads as "cat: write error" and "unpack-objects failed" rather than as
# anything about this feature.  Truncated in place while the run is going, and
# what is kept is the tail, which is what a person reads when something went
# wrong.  The backend assertion inside amiberry-run.sh has already read the
# line it needs by the time the first poll happens.
EMULOG="$ROOT/build/amiberry-$AMINETXDUO_RUN_TAG.log"
EMULOG_MAX=${AMINETXDUO_WSTERM_LOGMAX:-33554432}

trim_emulog() {
    [ -f "$EMULOG" ] || return 0
    local size
    size=$(wc -c < "$EMULOG" 2>/dev/null || echo 0)
    [ "$size" -gt "$EMULOG_MAX" ] || return 0
    tail -c 4194304 "$EMULOG" > "$EMULOG.tail" 2>/dev/null || return 0
    mv "$EMULOG.tail" "$EMULOG"
}

for _ in $(seq 1 "$BOOT_MAX"); do
    sleep 1
    trim_emulog
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

# ---------------------------------------------------------------- the drill --

DRILL_AT=$(date +%s)
set +e
python3 "$ROOT/tests/tools/httpd-drill.py" --terminal 127.0.0.1 "$HOSTPORT" \
    > "$ROOT/build/wsterm-drill.txt" 2>&1 &
DRILL=$!
while kill -0 "$DRILL" 2>/dev/null; do
    sleep 5
    trim_emulog
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

# ------------------------------------------------------------ the numbers --
#
# What the session actually costs, once the assertions have held.  It runs
# after the drill and not instead of it: this measures and does not assert,
# because "0.4 seconds to echo a line" is neither right nor wrong -- it is the
# answer to whether the thing is pleasant to use, and a person reads it.
#
# Skipped when the drill failed, since a number taken from a broken session is
# a number about nothing.
if [ "$DRILL_RC" -eq 0 ] && [ "${FAILS:-1}" -eq 0 ]; then
    set +e
    python3 "$ROOT/tests/tools/wsterm-bench.py" 127.0.0.1 "$HOSTPORT" \
        > "$ROOT/build/wsterm-bench.txt" 2>&1
    set -e
    trim_emulog
    echo
    echo "===================== what the session costs ===================="
    cat "$ROOT/build/wsterm-bench.txt"
    echo "================================================================"
fi

cleanup
trap - EXIT

# And the last of it, once nothing is writing any more.
if [ -f "$EMULOG" ]; then
    tail -400 "$EMULOG" > "$EMULOG.tail" && mv "$EMULOG.tail" "$EMULOG"
fi

echo "drill_seconds=$DRILL_SECS"
echo "checks=${CHECKS:-0}"
echo "failures=${FAILS:-0}"

# A drill that produced no tally at all did not finish, which is not the same
# thing as a drill whose assertions held.
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
