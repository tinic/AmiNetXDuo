#!/usr/bin/env bash
#
# THIRD-PARTY MAILER, BOTH DIRECTIONS: binkd 1.1a-115 from Aminet, calling a
# binkd of the same version on another machine and moving a file over a real
# FidoNet BinkP session.
#
#   tests/tools/run-binkd.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                            [-N BOARD] [-B BACKEND] [-P user@host]
#
# WHY binkd.  docs/aminet-survey/candidates.tsv ranks it for four vectors that
# real software uses and nothing else we run calls: inet_addr, Inet_NtoA,
# getservbyname and getpeername.  Unlike the resolver arm it is a SESSION --
# authenticated, bidirectional, with a file on the wire -- so it exercises the
# send/recv path under a protocol that fails loudly when a byte is wrong,
# rather than a single request and reply.
#
# THE PEER IS NOT OPTIONAL.  A bridged guest cannot be reached from the
# machine running the emulator, so the other end of the session has to be a
# third machine: -P or AMINETXDUO_PEER, the same rule tests/tools/run-httpd.sh
# and install/test/run-workbench.sh live under.  The peer needs binkd(8);
# Debian and Ubuntu package the identical 1.1a-115 the Amiga binary is built
# from, so both ends speak the same protocol version by construction.
#
# WHAT IS ASSERTED, and where.  On the PEER, because that is the end that
# cannot be fooled by the guest: a file the guest sent has to arrive in the
# peer's inbound with the bytes the guest was given.  The guest's own log is
# read too, for the session verdict, but it is corroboration and not the
# assertion -- a mailer that says OK and moves nothing is the failure this arm
# is for.
#
# The binary is not in this repository.  It lives in the asset store under
# apps/binkd, from Aminet comm/net/binkd_1.1a-115.lha.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT" || exit 2

MODEL=A1200
TIMEOUT=420
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
PEER="${AMINETXDUO_PEER:-}"
PORT="${AMINETXDUO_BINKD_PORT:-24560}"

while getopts "m:t:b:N:B:P:p:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        P) PEER="$OPTARG" ;;
        p) PORT="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir] [-N board]\
 [-B backend] [-P user@host] [-p port]" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "binkd=skipped reason=backend_refused:$IFACE" >&2
        echo "Bridged only: the peer has to reach the guest's own address." >&2
        exit 77 ;;
esac

if [ -z "$PEER" ]; then
    echo "binkd=skipped reason=no_peer set=AMINETXDUO_PEER" >&2
    echo "A bridged guest cannot be reached from the machine running the" >&2
    echo "emulator.  -P <user@host> names a third machine with binkd(8)." >&2
    exit 77
fi

ASSETS="${AMINETXDUO_ASSETS:-$HOME/amiga-assets}"
BINKD="$ASSETS/apps/binkd/binkd"
TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$BINKD" "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$BSD"; do
    [ -f "$f" ] || { echo "binkd=skipped reason=missing file=$f" >&2; exit 77; }
done

if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" 'command -v binkd >/dev/null || [ -x /usr/sbin/binkd ]'; then
    echo "binkd=skipped reason=peer_has_no_binkd peer=$PEER" >&2
    echo "apt-get install binkd on the peer; it packages the same" >&2
    echo "1.1a-115 the Amiga binary is built from." >&2
    exit 77
fi

. "$ROOT/tools/sana2-stage.sh"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { A2065="$candidate"; break; }
    done
fi
if [ "$BOARD" = a2065 ] && { [ -z "$A2065" ] || [ ! -f "$A2065" ]; }; then
    echo "binkd=skipped reason=no_a2065 set=AMINETXDUO_A2065" >&2
    exit 77
fi

# The peer's address on the segment the guest is bridged onto.  Asking the
# peer what it thinks its own address is beats guessing from this host's
# routing table, which is a different machine.
PEER_IP=$(ssh -o BatchMode=yes "$PEER" "ip -4 -br addr show scope global | awk '{print \$3}' | cut -d/ -f1 | head -1")
case "$PEER_IP" in
    [0-9]*.[0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "binkd=skipped reason=peer_address_unknown got='$PEER_IP'" >&2; exit 77 ;;
esac
echo "==> peer $PEER is $PEER_IP, binkd on port $PORT"

# --------------------------------------------------------------- the peer ---
#
# Torn down and rebuilt every run: a file left in the inbound by a previous
# run is exactly the thing that would make this arm pass while transferring
# nothing.
PDIR=".aminetxduo-binkd"
PAYLOAD="binkd payload $(date -u +%Y%m%dT%H%M%SZ) $$"
ssh -o BatchMode=yes "$PEER" "
    set -e
    rm -rf ~/$PDIR && mkdir -p ~/$PDIR/{in,out,log}
    : > ~/$PDIR/nodelist
    cat > ~/$PDIR/srv.cfg <<CFG
domain fidonet \$HOME/$PDIR/out 2
address 2:9999/1@fidonet
sysname \"anxd-peer\"
location \"rig\"
sysop \"test\"
nodeinfo 115200,TCP/IP
node 2:9999/2@fidonet - SEKRIT
inbound \$HOME/$PDIR/in
log \$HOME/$PDIR/log/srv.log
loglevel 5
iport $PORT
CFG
    for pid in \$(pgrep -x binkd 2>/dev/null); do
        grep -q '$PDIR' /proc/\$pid/cmdline 2>/dev/null && kill \$pid 2>/dev/null
    done
    sleep 1
    setsid \$(command -v binkd || echo /usr/sbin/binkd) -D ~/$PDIR/srv.cfg </dev/null >/dev/null 2>&1 &
    sleep 2

    # OURS, not merely SOMETHING.  \`ss | grep :PORT' is satisfied by any
    # listener, and a leftover binkd from an earlier rehearsal held this port
    # while our own server failed to bind -- so the guest had a real session
    # with the wrong server, and the assertions read an inbound nobody had
    # written to.  binkd says so in its own log; read that instead.
    if grep -q 'Address already in use' ~/$PDIR/log/srv.log 2>/dev/null; then
        echo 'peer binkd could not bind: something else already holds' >&2
        ss -lntp 2>/dev/null | grep ':$PORT ' >&2
        exit 1
    fi
    grep -q 'servmgr started' ~/$PDIR/log/srv.log 2>/dev/null || {
        echo 'peer binkd never started its server manager' >&2
        tail -5 ~/$PDIR/log/srv.log >&2 2>/dev/null
        exit 1
    }
" || { echo "binkd=skipped reason=peer_setup_failed" >&2; exit 77; }

peer_cleanup() {
    ssh -o BatchMode=yes "$PEER" "
        for pid in \$(pgrep -x binkd 2>/dev/null); do
            grep -q '$PDIR' /proc/\$pid/cmdline 2>/dev/null && kill \$pid 2>/dev/null
        done" >/dev/null 2>&1 || true
}
trap peer_cleanup EXIT

# -------------------------------------------------------------- the guest ---

STAGE="$ROOT/build/binkd-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/bd/in" "$STAGE/bd/out" "$STAGE/bd/log"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$BINKD" "$STAGE/binkd"

# WHAT binkd OPENS BEFORE IT REACHES main().  Read out of the binary rather
# than remembered: see tools/amiga-startup-libs.sh, which exists because three
# harnesses have now rediscovered this from a guest error message.
. "$ROOT/tools/amiga-startup-libs.sh"
MISSING=$(stage_startup_libs "$BINKD" "$STAGE/libs" "$ASSETS")
if [ -n "$MISSING" ]; then
    echo "binkd=skipped reason=missing_startup_libs" >&2
    printf '%s\n' "$MISSING" >&2
    echo "Put them in $ASSETS/libs.  locale.library comes off the Workbench" >&2
    echo "3.1 disks; the maths pair is in the asset store under nglibs." >&2
    exit 77
fi

[ "$BOARD" = a2065 ] && cp "$A2065" "$STAGE/devs/a2065.device"

printf '%s\n' "$PAYLOAD" > "$STAGE/bd/out/payload.txt"

# binkd names an outbound queue <net><node>.flo in hex: 2:9999/1 is net 9999
# (0x270f) node 1 (0x0001).  Getting this wrong is a session that authenticates
# and moves nothing, which is what the first peer-to-peer rehearsal did.
printf 'bd/out/payload.txt\n' > "$STAGE/bd/out/270f0001.flo"

# RELATIVE PATHS, NOT DH0:.
#
# binkd is a Unix program ported with a C library that validates every
# directory in the configuration at parse time, and it rejected DH0:bd/in as
# "incorrect directory" when the same config was fed to the identical version
# on the host.  Its path handling is POSIX; a volume-qualified Amiga path is
# not a path it can stat.  ToolsSmoke runs commands from DH0:, so a relative
# path names the same place and is one it understands.
cat > "$STAGE/bd/binkd.cfg" <<CFG
domain fidonet bd/out 2
address 2:9999/2@fidonet
sysname "anxd-guest"
location "rig"
sysop "test"
nodeinfo 115200,TCP/IP
node 2:9999/1@fidonet $PEER_IP:$PORT SEKRIT
inbound bd/in
log bd/log/cli.log
loglevel 5
CFG

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=$(sana2_driver_for "$BOARD")
UNIT=0
CONFIGURE=DHCP
IFEOF

sana2_stage "$BOARD" "$STAGE/devs"

# binkd -h FIRST, and the real call with its output redirected to a file.
#
# ToolsSmoke gives a child this Shell's Input and Output but cannot give it an
# error stream: SYS_Error is an OS4 tag and this is 3.1.  binkd answers a bad
# configuration with three lines on stderr and rc 1, so from the report that is
# `rc 1' and an empty block -- which is all the first two runs of this arm ever
# said.
#
# `binkd -d' dumps the config as PARSED and exits, on stdout, which is the one
# stream we can capture.  It is the program's own answer to the question, and
# it costs one line.
cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
SYS:binkd -d bd/binkd.cfg >DH0:bd/log/dump.txt
SYS:binkd -c -P 2:9999/1@fidonet bd/binkd.cfg >DH0:bd/log/run.txt
EOF

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-binkd}"

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
echo "==> booting $MODEL under Amiberry, $BOARD on $IFACE"
"$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
    -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/binkd" "$STAGE/bd"
RUN_RC=$?

REPORT="$HD/tools.txt"
if [ ! -f "$REPORT" ]; then
    echo "binkd=FAIL reason=no_transcript rc=$RUN_RC" >&2
    exit 1
fi

echo
echo "==================== what the commands printed ====================="
cat "$REPORT"
echo "==================================================================="
echo "------------------------- the guest's log -------------------------"
for f in dump.txt run.txt cli.log; do
    if [ -s "$HD/bd/log/$f" ]; then
        echo "---- $f ----"
        cat "$HD/bd/log/$f"
    else
        echo "---- $f: empty or absent ----"
    fi
done
echo "==================================================================="
echo

FAILED=0
CHECKS=0
fail() { echo "FAIL: $*" >&2; FAILED=$((FAILED + 1)); CHECKS=$((CHECKS + 1)); }
pass() { echo "  ok: $*"; CHECKS=$((CHECKS + 1)); }

block()    { awk -v c="===== SYS:$1 =====" '
                 $0 == c            { inb = 1; next }
                 inb && /^----- rc / { exit }
                 inb                { print }' "$REPORT"; }
block_rc() { awk -v c="===== SYS:$1 =====" '
                 $0 == c            { inb = 1; next }
                 inb && /^----- rc / { sub(/^----- rc /, ""); sub(/,.*/, ""); print; exit }
             ' "$REPORT"; }

IF_RC=$(block_rc "AddNetInterface eth0")
[ "${IF_RC:-x}" = 0 ] && pass "AddNetInterface brought eth0 up" \
    || fail "AddNetInterface returned rc ${IF_RC:-none}: $(block "AddNetInterface eth0" | tr '\n' ' ')"

# THE ASSERTION, made on the peer.  The guest cannot fake this one.
GOT=$(ssh -o BatchMode=yes "$PEER" "cat ~/$PDIR/in/payload.txt 2>/dev/null" || true)
if [ "$GOT" = "$PAYLOAD" ]; then
    pass "the file the guest sent arrived on $PEER with the bytes it was given"
else
    fail "the peer's inbound does not hold what the guest was given" \
         "(wanted '$PAYLOAD', got '${GOT:-<nothing>}')"
fi

# Corroboration from both logs.
CLI="$HD/bd/log/cli.log"
if grep -qE 'done \(to .*OK, S/R: [1-9]' "$CLI" 2>/dev/null; then
    pass "the guest's own log reports a completed session with a file sent"
else
    fail "the guest's log has no successful send:" \
         "$(grep -E 'done \(to' "$CLI" 2>/dev/null | tail -1 || echo none)"
fi

if grep -q 'pwd protected session' "$CLI" 2>/dev/null; then
    pass "the session was password protected (MD5), not a plain greet"
else
    fail "no authenticated session in the guest's log"
fi

SRV=$(ssh -o BatchMode=yes "$PEER" "grep -cE 'rcvd file|done \(from' ~/$PDIR/log/srv.log 2>/dev/null || true" | tail -1)
SRV=${SRV:-0}
[ "${SRV:-0}" -gt 0 ] && pass "the peer's own log records the inbound session" \
                      || fail "the peer's log records no session"

echo
echo "binkd_checks=$CHECKS failures=$FAILED"
if [ "$FAILED" != 0 ]; then
    echo "binkd=FAIL" >&2
    exit 1
fi
echo "binkd=PASS"
