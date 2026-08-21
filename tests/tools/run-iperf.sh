#!/usr/bin/env bash
#
# iperf on a real guest, against a peer that counts what actually arrived.
#
#   tests/tools/run-iperf.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#                            [-B IFACE] [-P PEERHOST] [-a ADDR] [-g GATEWAY]
#                            [-N BOARD] [-V] [-r TRANSCRIPT]
#
# WHICH DRIVER IT MEASURES
#
#   Ours.  -N BOARD stages anxnet.device wherever tools/sana2-stage.sh says it
#   covers the board, and a vendor driver sitting in ~/amiga-assets/devs never
#   displaces it: -V, or AMINETXDUO_SANA2_VENDOR=1, is the only way to ask for
#   one.  The run prints driver_device= and driver_source= before it boots and
#   driver_booted= after, and fails when the two disagree, because bytes cross
#   the wire just as happily under somebody else's driver and every other
#   assertion here would pass on that run.
#
# WHAT IT PROVES
#
#   That the number the command prints is the number of bytes that crossed the
#   wire, and not merely that it printed a number.  The peer is
#   tests/tools/iperfpeer.py, a second implementation of the same wire format
#   in another language, and every throughput assertion compares the guest's
#   own `bytes=` against the peer's `peer_bytes=`, which is what the host
#   kernel's recv() counted.  The rate is then recomputed from the guest's own
#   byte and millisecond counts, so a correct transfer reported at the wrong
#   speed fails too.
#
#   It also checks the refusals, which are the paths a user hits first: no
#   peer, a port that is not a port, a duration that is not a duration, no
#   host, and a host given to -s.
#
# WHY SLIRP IS ENOUGH HERE, AND WHERE IT IS NOT
#
#   These assertions are about BYTE COUNTS, not about speed, and a byte count
#   is exact on any link.  No throughput figure from this script means
#   anything: SLIRP's bandwidth-delay product is nearly zero and the emulator
#   runs in warp mode, so the rate it reports is a property of the host.  For a
#   figure, use tests/perf/run-fitzbench.sh, bridged, which says so at length.
#
#   The two directions where the guest is the SERVER need a peer that can call
#   in, and a SLIRP guest cannot be called.  Those run only with -B, bridged,
#   and -P naming a THIRD machine: a frame the emulator's host sends to the
#   guest leaves its NIC and never returns to that NIC's own pcap, so a peer on
#   the emulator host is unreachable from the guest (docs/RESEARCH.md 63).
#   Without -B the script runs the client directions and the refusals and says
#   which four checks it skipped.
#
# GOOD CASE, MEASURED
#
#   90 s wall on playhouse3 under SLIRP, boot to verdict, of which the guest's
#   own work is three 3-second transfers.  The default -t 240 is between two
#   and three times that.  A run that reaches the timeout is a defect to
#   diagnose and the verdict names the command it stopped in; raising -t is
#   never the fix.
#
# NOTHING HERE WRITES AN UNBOUNDED FILE.  Every peer has a stated lifetime and
# every log is checked against a ceiling at the end, because a server whose
# stdout goes to a file is exactly how 72 GB of interval reports got written on
# this host while the tool was being developed.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=240
BUILD="${AMINETXDUO_BUILD:-build/cm}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
IFACE=""
PEERHOST=""
REPLAY=""

# The bridged arm needs the guest at a known address before it boots: the peer
# has to call IN, and a DHCP lease is not knowable until after.  run-httpd.sh
# takes the same view for the same reason.
ADDRESS="${AMINETXDUO_IPERF_ADDRESS:-192.168.1.240}"
GATEWAY="${AMINETXDUO_IPERF_GATEWAY:-192.168.1.1}"
NETMASK=255.255.255.0

# How long each transfer runs on the guest, and how much the -n arm moves.
#
# Three seconds is what the byte-count assertions need, and they are what this
# script is for.  A THROUGHPUT comparison needs longer -- a 68020 read at 400
# KB/s moves 1.2 MB in three seconds, most of which is slow start -- so
# AMINETXDUO_IPERF_SECS raises it without changing what is asserted.  Both
# directions and the UDP arms take it, so the arms stay the same length as each
# other whatever it is set to.
SECS="${AMINETXDUO_IPERF_SECS:-3}"
SIZE_KB=64

# How long the guest waits when it is the server.  It has to outlast the
# transfer with room for the peer's retry loop to find it, hence the transfer
# plus seventeen seconds rather than a constant: at a raised SECS a constant
# ends the server while the peer is still sending, and the arm reads as a short
# transfer rather than as a truncated one.  The ports are derived from the run
# tag, below, once the tag is known.
SRV_WINDOW=$((SECS + 17))

VENDOR="${AMINETXDUO_SANA2_VENDOR:-}"

while getopts "m:t:b:B:P:a:g:N:r:V" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        P) PEERHOST="$OPTARG" ;;
        a) ADDRESS="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        r) REPLAY="$OPTARG" ;;
        V) VENDOR=1 ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done
[ -z "$VENDOR" ] || export AMINETXDUO_SANA2_VENDOR="$VENDOR"

# Which driver this run is about, filled in by the staging below and asserted
# against the transcript at the end.  Empty means nothing decided it, which is
# the case a replay is in.
WANT_DEVICE=""
WANT_CARD=""
DRIVER_SOURCE=""
DRIVER_PATH=""

# An absolute -b is a tree somewhere else, which is what comparing two builds
# needs.  The case here used to leave $BUILD alone and the two lines below then
# prefixed $ROOT anyway, so `-b /somewhere/build` reported
# "missing /root//somewhere/build/src/tools/ToolsSmoke" and stopped -- with the
# doubled slash the only clue.  tests/tools/run-cardsweep.sh:120-123 has always
# resolved it this way; this file has not.
case "$BUILD" in
    /*) BUILDDIR="$BUILD" ;;
    *)  BUILDDIR="$ROOT/${BUILD#./}" ;;
esac

TOOLS="$BUILDDIR/src/tools"
BSD="$BUILDDIR/src/bsdsocket/bsdsocket.library"

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-iperf}"

# ONE PORT BLOCK PER TAG, and not the fixed 7401..7409 this used to have.
#
# The peers are sized to outlive the run -- PEER_LIFE is TIMEOUT + 120, 420 s
# at the default -- and a run itself is nearer 180 s.  So an arm's peers are
# still holding those ports when the next arm starts.  Same tag and they are
# killed first (the pkill below is tag-scoped); DIFFERENT tag, which is exactly
# what an A/B is, and the second arm dies on "address already in use" before it
# boots anything.  The peer that survives is not even the one under test.
#
# Ten ports per tag in 20000..29000: clear of tools/amiberry-run.sh's 12000
# block, clear of tools/winuae-run.sh's 11000 block, and below the ephemeral
# range on every peer this runs on.  The offsets keep the old layout, so a
# capture filter written against 7401/7402/7403 still reads +1/+2/+3.
#
# Above 1024, one per arm so a peer that is still finishing cannot be mistaken
# for the next one.  DEAD is where nothing ever listens.
PORT_BASE=$((20000 + ($(printf '%s' "$AMINETXDUO_RUN_TAG" | cksum |
                        cut -d' ' -f1) % 900) * 10))
PORT_TCP="${AMINETXDUO_IPERF_PORT_TCP:-$((PORT_BASE + 1))}"
PORT_UDP="${AMINETXDUO_IPERF_PORT_UDP:-$((PORT_BASE + 2))}"
PORT_SIZE="${AMINETXDUO_IPERF_PORT_SIZE:-$((PORT_BASE + 3))}"
PORT_SRV_TCP="${AMINETXDUO_IPERF_PORT_SRVTCP:-$((PORT_BASE + 4))}"
PORT_SRV_UDP="${AMINETXDUO_IPERF_PORT_SRVUDP:-$((PORT_BASE + 5))}"
PORT_DEAD="${AMINETXDUO_IPERF_PORT_DEAD:-$((PORT_BASE + 9))}"

HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"
RUN_RC=0

# Bridged needs a peer that is not this machine; without one the server
# directions cannot be reached and are skipped rather than quietly passing.
SERVER_ARMS=no
if [ -n "$IFACE" ]; then
    if [ -z "$PEERHOST" ]; then
        echo "-B without -P: a bridged guest cannot be reached from the" \
             "machine running the emulator, so -P must name a third one." >&2
        exit 2
    fi
    SERVER_ARMS=yes
elif [ "$BOARD" != a2065 ] && [ -z "$REPLAY" ]; then
    # -N only reaches an emulator on the bridged branch; the SLIRP branch below
    # boots an a2065 and nothing else.  Accepting the key here and ignoring it
    # is how a run reports PASS for a card it never put in the machine.
    echo "-N $BOARD needs -B <iface>: the SLIRP branch here boots an a2065" >&2
    echo "and nothing else, so a board key would be ignored." >&2
    exit 2
fi

# The address the guest calls.  SLIRP's gateway is the emulator host itself.
if [ "$SERVER_ARMS" = yes ]; then
    # -P is an ssh destination and may carry a user, but the guest is given an
    # ADDRESS: it has no DNS of its own here, and "turo@playhouse2" is not a
    # host name to anybody.  Strip the user, then resolve on this side.
    PEERNAME="${PEERHOST#*@}"
    PEERADDR=$(getent ahostsv4 "$PEERNAME" 2>/dev/null | awk 'NR==1{print $1}')
    if [ -z "$PEERADDR" ]; then
        case "$PEERNAME" in
            *[!0-9.]*) echo "cannot resolve $PEERNAME to an address for the" \
                            "guest to call" >&2; exit 2 ;;
            *) PEERADDR="$PEERNAME" ;;
        esac
    fi
    echo "==> the peer is $PEERHOST, which the guest reaches at $PEERADDR"
else
    PEERADDR=10.0.2.2
    ADDRESS=10.0.2.15
    GATEWAY=10.0.2.2
fi

# Tagged, the way tools/amiberry-run.sh:245-251 tags the drive and the
# serial log.  tests/tools/run-cardsweep.sh runs this script nine times
# over, and a shared peer log is one card reading another card's bytes.
PEERLOG="$ROOT/build/iperf-peers-$AMINETXDUO_RUN_TAG"

# --------------------------------------------------------------- preflight ---

if [ -n "$REPLAY" ]; then
    [ -f "$REPLAY" ] || { echo "no such transcript: $REPLAY" >&2; exit 2; }
    REPORT="$REPLAY"
    echo "==> REPLAY of $REPORT: nothing was run, this only checks the checks"
else

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/iperf" "$BSD"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
done

command -v python3 >/dev/null || {
    echo "no python3, which is what runs the peer" >&2; exit 2; }

# The A2065 is the SLIRP board and the default bridged one, and it is the only
# board whose driver is wanted unconditionally.  Asked for another card, this
# used to demand an a2065.device that the run then overwrote the DEVICE= line
# of, so a host with every PCMCIA driver and no LANCE driver could not run a
# PCMCIA arm at all.
A2065=""
if [ "$BOARD" = a2065 ]; then
    A2065="${AMINETXDUO_A2065:-}"
    if [ -z "$A2065" ]; then
        for candidate in \
            "$ROOT/build/a2065.device" \
            "$HOME/amiga-assets/devs/a2065.device"
        do
            [ -f "$candidate" ] && { A2065="$candidate"; break; }
        done
    fi
    [ -n "$A2065" ] && [ -f "$A2065" ] || {
        echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2
        exit 2
    }
fi

# ----------------------------------------------------------------- staging ---

STAGE="$ROOT/build/iperf-stage-$AMINETXDUO_RUN_TAG"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
[ -z "$A2065" ] || cp "$A2065" "$STAGE/devs/a2065.device"

cat > "$STAGE/devs/NetInterfaces/eth0" <<IFEOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=STATIC
ADDRESS=$ADDRESS
NETMASK=$NETMASK
GATEWAY=$GATEWAY
IFEOF

# ------------------------------------------------------- which driver ---
#
# The question this run answers is whether OUR driver carries traffic on this
# card, so anxnet.device is the default wherever tools/sana2-stage.sh says it
# covers the board.  A hand-placed vendor driver in ~/amiga-assets/devs is NOT
# preferred over it and is never a fallback for it: preferring one is how
# `-N ne2000_pcmcia` booted cnet.device, measured cnet.device and printed PASS
# for a run that never loaded anxnet.device.  A vendor driver is a request,
# -V or AMINETXDUO_SANA2_VENDOR=1, and the choice is printed as key=value
# below and asserted against the guest's own transcript at the end.
if [ -n "$IFACE" ]; then
    . "$ROOT/tools/sana2-stage.sh"
    if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ]; then
        sana2_select "$BOARD" "$BUILDDIR"
        if [ -z "$SANA2_SEL_PATH" ]; then
            echo "-N $BOARD wants $SANA2_SEL_DRIVER and this host has not" \
                 "got it." >&2
            if [ "$SANA2_SEL_SOURCE" = anxnet ]; then
                echo "Build the tree, or set AMINETXDUO_ANXNET=<path>." >&2
                echo "This does NOT fall back to the vendor driver: that" >&2
                echo "would measure somebody else's driver and call it ours." >&2
                echo "To test the vendor driver on purpose, pass -V." >&2
            else
                echo "Put it in one of:" >&2
                for _d in ${AMINETXDUO_SANA2_STORE:-} "$HOME/amiga-assets/devs"
                do
                    echo "  $_d" >&2
                done
                echo "or name one with AMINETXDUO_SANA2_DRIVER=<path>." >&2
            fi
            exit 2
        fi
        export AMINETXDUO_SANA2_DRIVER="$SANA2_SEL_PATH"
        export AMINETXDUO_SANA2_DRIVER_NAME="${AMINETXDUO_SANA2_DRIVER_NAME:-$SANA2_SEL_DRIVER}"
        export AMINETXDUO_SANA2_DEVICE="${AMINETXDUO_SANA2_DEVICE:-$SANA2_SEL_DRIVER}"
        [ -z "$SANA2_SEL_CARD" ] ||
            export AMINETXDUO_SANA2_CARD="${AMINETXDUO_SANA2_CARD:-$SANA2_SEL_CARD}"
        DRIVER_SOURCE=$SANA2_SEL_SOURCE
    else
        # The caller spelled it out.  Believe it, and say that is what happened
        # rather than reporting a source this script did not choose.
        DRIVER_SOURCE=given
    fi
    sana2_stage "$BOARD" "$STAGE/devs"
    WANT_DEVICE=$SANA2_DEVICE
    WANT_CARD=$SANA2_CARD
    DRIVER_PATH=${AMINETXDUO_SANA2_DRIVER:-}
else
    # SLIRP boots the A2065 with the vendor LANCE driver.  Said out loud for
    # the same reason as everything above: a reader must not have to infer it.
    WANT_DEVICE=a2065.device
    WANT_CARD=""
    DRIVER_SOURCE=vendor
    DRIVER_PATH=$A2065
fi

printf 'driver_board=%s driver_device=%s driver_card=%s driver_source=%s driver_path=%s\n' \
       "$BOARD" "$WANT_DEVICE" "${WANT_CARD:-none}" "$DRIVER_SOURCE" \
       "${DRIVER_PATH:-none}"

cp "$BSD" "$STAGE/libs/bsdsocket.library"
cp "$TOOLS/AddNetInterface" "$STAGE/AddNetInterface"
cp "$TOOLS/iperf"           "$STAGE/iperf"
# netstat -s is what makes a lost datagram say where it was lost.  See the
# decomposition after the guest-as-server UDP arm.
cp "$TOOLS/netstat"         "$STAGE/netstat"

# The commands, in order.  Every assertion below indexes by position, so this
# list and the checks move together.
{
    echo "SYS:AddNetInterface eth0"
    echo "SYS:iperf -h"
    echo "SYS:iperf $PEERADDR -p $PORT_TCP -t $SECS"
    echo "SYS:iperf -u $PEERADDR -p $PORT_UDP -t $SECS -b 2000"
    echo "SYS:iperf $PEERADDR -p $PORT_SIZE -n $SIZE_KB"
    echo "SYS:iperf $PEERADDR -p $PORT_DEAD -t $SECS"
    echo "SYS:iperf $PEERADDR -p 0 -t $SECS"
    echo "SYS:iperf $PEERADDR -t 0"
    echo "SYS:iperf"
    echo "SYS:iperf -s $PEERADDR"
    if [ "$SERVER_ARMS" = yes ]; then
        echo "SYS:iperf -s -p $PORT_SRV_TCP -t $SRV_WINDOW"
        # These two bracket the ONE arm in this script where the guest receives
        # at whatever rate the peer chooses, so they are the only pair whose
        # difference is a single arm's.  Everything else here is either flow
        # controlled or outbound.
        echo "SYS:netstat -s"
        echo "SYS:iperf -s -u -p $PORT_SRV_UDP -t $SRV_WINDOW"
        echo "SYS:netstat -s"
    fi
} > "$STAGE/commands.txt"

EXPECTED_BLOCKS=$(wc -l < "$STAGE/commands.txt" | tr -d ' ')

# ------------------------------------------------------------------- peers ---
#
# Started before the boot and sized to outlive the whole run: an emulator run
# queues behind whatever else is on this machine, and a peer sized to the
# transfer dies while the run is still waiting, after which every arm fails as
# "connection refused" and names the wrong thing.

rm -rf "$PEERLOG"
mkdir -p "$PEERLOG"

# Where the peers run.  On the bridged arm they must be on the third machine:
# a frame the emulator host sends to its own bridged guest never comes back to
# that NIC's pcap, so a peer here would be unreachable from the guest while
# being reachable from everywhere else.  Locally otherwise, where SLIRP's
# gateway IS this machine.
PEER_PY="$ROOT/tests/tools/iperfpeer.py"
if [ "$SERVER_ARMS" = yes ]; then
    REMOTE_PY="/tmp/iperfpeer-$AMINETXDUO_RUN_TAG.py"
    scp -q "$PEER_PY" "$PEERHOST:$REMOTE_PY" || {
        echo "cannot copy the peer to $PEERHOST" >&2; exit 2; }
    ssh -o ConnectTimeout=10 "$PEERHOST" "python3 $REMOTE_PY --help" \
        >/dev/null 2>&1 || {
        echo "$PEERHOST cannot run the peer; it needs python3" >&2; exit 2; }
    # Under `timeout`, and with any leftover from an earlier run killed first.
    # Killing the local ssh does not kill what it started on the far side, so
    # without a ceiling of its own a peer outlives its run, holds the port, and
    # the next run dies on "address already in use" -- which is how this was
    # found.  The ceiling is the peer's own lifetime and a little slack.
    ssh -o ConnectTimeout=10 "$PEERHOST" \
        "pkill -f '[i]perfpeer-$AMINETXDUO_RUN_TAG' 2>/dev/null; exit 0" || true
    peer_cmd() { ssh -o ConnectTimeout=10 "$PEERHOST" \
                     "timeout $((TIMEOUT + 150)) python3 $REMOTE_PY $*"; }
    echo "==> peers run on $PEERHOST, the guest is $ADDRESS"
else
    peer_cmd() { python3 "$PEER_PY" "$@"; }
fi

PEER_PIDS=()
stop_peers() {
    local p
    for p in "${PEER_PIDS[@]:-}"; do
        [ -n "$p" ] && kill "$p" 2>/dev/null || true
    done
}
trap stop_peers EXIT INT TERM HUP

PEER_LIFE=$((TIMEOUT + 120))

start_peer() { # logname args...
    local name="$1"; shift
    peer_cmd "$@" > "$PEERLOG/$name.out" 2> "$PEERLOG/$name.err" &
    PEER_PIDS+=("$!")
}

# The senders for the guest-as-server arms.  A retry loop, not a sleep: the
# guest reaches its `iperf -s` somewhere inside the run and the host cannot
# know when, so it keeps offering until one attempt gets through.  For TCP that
# is a connect that stops being refused; for UDP it is an attempt that comes
# back with the guest's end-of-test report, since a datagram sent at a guest
# that is not listening yet is simply lost and looks like success.
start_sender() { # logname proto port
    local name="$1" proto="$2" port="$3" kbit=2000
    # The UDP rate the peer offers the guest, raised from outside for a burst
    # measurement.  2000 is what every assertion here is written against: the
    # guest keeps up with it on every card, so a loss at 2000 is a defect.
    # Above it the guest is meant to be overrun, and the arm becomes a
    # frames-delivered measurement rather than a pass.
    [ "$proto" != udp ] || kbit="${AMINETXDUO_IPERF_PEER_UDP_KBIT:-2000}"
    (
        deadline=$(( $(date +%s) + PEER_LIFE ))
        while [ "$(date +%s)" -lt "$deadline" ]; do
            if out=$(peer_cmd send "$proto" "$ADDRESS" --port "$port" \
                        --seconds "$SECS" --kbit "$kbit" \
                        2>>"$PEERLOG/$name.err"); then
                case "$proto" in
                    udp) case "$out" in *peer_report=1*) echo "$out"; exit 0 ;; esac ;;
                    *)   echo "$out"; exit 0 ;;
                esac
            fi
            sleep 1
        done
        exit 1
    ) > "$PEERLOG/$name.out" 2>>"$PEERLOG/$name.err" &
    PEER_PIDS+=("$!")
}

start_peer tcp  serve tcp --port "$PORT_TCP"  --seconds "$PEER_LIFE" --idle 8
start_peer udp  serve udp --port "$PORT_UDP"  --seconds "$PEER_LIFE" --idle 8
start_peer size serve tcp --port "$PORT_SIZE" --seconds "$PEER_LIFE" --idle 8

if [ "$SERVER_ARMS" = yes ]; then
    start_sender srvtcp tcp "$PORT_SRV_TCP"
    start_sender srvudp udp "$PORT_SRV_UDP"
fi

sleep 1
for p in "${PEER_PIDS[@]}"; do
    kill -0 "$p" 2>/dev/null || {
        echo "a peer died before the run started:" >&2
        cat "$PEERLOG"/*.err >&2
        exit 2
    }
done
echo "==> peers up on $PORT_TCP $PORT_UDP $PORT_SIZE, nothing on $PORT_DEAD"

# --------------------------------------------------------------------- run ---

set +e
if [ -n "$IFACE" ]; then
    echo "==> booting $MODEL under Amiberry, $BOARD bridged on $IFACE"
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
        "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/iperf" "$STAGE/netstat"
else
    echo "==> booting $MODEL with the A2065 on SLIRP"
    "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
        "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/iperf" "$STAGE/netstat"
fi
RUN_RC=$?
set -e

sleep 2
stop_peers

fi  # not a replay

if [ ! -f "$REPORT" ]; then
    echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2
    [ "$RUN_RC" = 124 ] &&
        echo "       rc 124 is the ${TIMEOUT}s timeout: the machine never" \
             "got as far as writing one." >&2
    exit 1
fi

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "========================== what the peer saw ========================"
[ -d "$PEERLOG" ] && cat "$PEERLOG"/*.out 2>/dev/null
[ -d "$PEERLOG" ] && cat "$PEERLOG"/*.err 2>/dev/null
echo "====================================================================="
echo

FAILED=0
# An assertion that did not run is counted, not just printed.  See the verdict
# at the end: it is what stops this exiting 0 with half its subject untested.
UNRUN=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }
skip() { echo "SKIP: $*"; UNRUN=$((UNRUN + 1)); }

block() {
    awk -v banner="$1" -v want="$2" '
        index($0, "===== " banner " =====") == 1 { n++; if (n == want) { on = 1; next } }
        on && /^----- rc / { print; exit }
        on { print }
    ' "$REPORT"
}

rc_of() { block "$1" "$2" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'; }
show()  { block "$1" "$2" | sed 's/^/       /' >&2; }

want_rc() { # banner nth expected description
    local got; got=$(rc_of "$1" "$2")
    if [ "$got" = "$3" ]; then pass "$4 (rc $got)"
    else fail "$4: expected rc $3, got '${got:-nothing}'"; show "$1" "$2"
    fi
}

says() { # banner nth ere description
    if block "$1" "$2" | grep -Eq "$3"; then pass "$4"
    else fail "$4"; show "$1" "$2"
    fi
}

# One key out of the guest's key=value result line.
guest_val() { # banner nth key
    block "$1" "$2" | grep -o "[[:space:]]$3=[^[:space:]]*" | tail -1 |
        cut -d= -f2
}

# One key out of a peer's line.
peer_val() { # logname key
    [ -f "$PEERLOG/$1.out" ] || return 0
    grep -o "$2=[^[:space:]]*" "$PEERLOG/$1.out" | tail -1 | cut -d= -f2
}

# ---- the run finished, and finished once --------------------------------

BLOCKS=$(grep -c '^===== SYS:' "$REPORT" || true)
if ! grep -q '^===== done, ' "$REPORT"; then
    LAST=$(grep '^===== SYS:' "$REPORT" | tail -1 | sed 's/^===== //; s/ =====$//')
    fail "THE RUN DID NOT FINISH: it stopped in '${LAST:-nothing ran}'," \
         "block $BLOCKS of $EXPECTED_BLOCKS (emulator rc=$RUN_RC," \
         "timeout ${TIMEOUT}s).  That command did not return."
elif [ "$BLOCKS" -eq "$EXPECTED_BLOCKS" ]; then
    pass "the machine booted once and ran all $EXPECTED_BLOCKS commands"
else
    fail "expected $EXPECTED_BLOCKS command blocks, found $BLOCKS:" \
         "the machine rebooted, or the list was truncated"
fi

# ---- the run measured the driver it was asked to measure -----------------
#
# The gate this file went without.  Nothing else here mentions a driver: every
# assertion below is about bytes, and bytes cross the wire just as happily
# under somebody else's SANA-II driver as under ours.  `-N ne2000_pcmcia` used
# to prefer a hand-placed cnet.device over anxnet.device, boot it, move real
# bytes and report PASS, and the only way to notice was to read the boot log.
#
# AddNetInterface echoes DEVS:NetInterfaces/eth0 as "eth0: <driver> unit N
# [card <card>]" before it starts anything, so the guest itself says which
# driver the rest of the run went through.  Compare it to what was staged.

IFCMD="SYS:AddNetInterface eth0"
BOOTED=$(block "$IFCMD" 1 |
         sed -n 's/^eth0: \([^ ]*\) unit [0-9].*/\1/p' | head -1)
BOOTED_CARD=$(block "$IFCMD" 1 |
              sed -n 's/^eth0: [^ ]* unit [0-9]* card \([^ ]*\).*/\1/p' | head -1)

printf 'driver_asked=%s driver_booted=%s card_asked=%s card_booted=%s\n' \
       "${WANT_DEVICE:-unknown}" "${BOOTED:-none}" \
       "${WANT_CARD:-none}" "${BOOTED_CARD:-none}"

if [ -z "$WANT_DEVICE" ]; then
    skip "which driver this transcript went through: a replay knows what the" \
         "guest printed and not what was staged for it"
elif [ -z "$BOOTED" ]; then
    fail "the guest never named a driver, so nothing here proves which one" \
         "carried the bytes.  Expected '$WANT_DEVICE'."
    show "$IFCMD" 1
elif [ "$BOOTED" != "$WANT_DEVICE" ]; then
    fail "asked for $WANT_DEVICE and the guest opened $BOOTED: every figure" \
         "below is about $BOOTED and this run proves nothing about" \
         "$WANT_DEVICE."
    show "$IFCMD" 1
elif [ "${BOOTED_CARD:-}" != "${WANT_CARD:-}" ]; then
    fail "asked for $WANT_DEVICE CARD=${WANT_CARD:-none} and the guest opened" \
         "it as CARD=${BOOTED_CARD:-none}, which is a different board"
    show "$IFCMD" 1
else
    pass "the run went through $BOOTED (${DRIVER_SOURCE:-?})" \
         "${WANT_CARD:+CARD=$WANT_CARD}, which is what was asked for"
fi

# ---- the help says which iperf this is ----------------------------------
#
# The first thing a user does is point this at the iperf3 their distribution
# ships, get nothing, and conclude the stack is broken.  The help has to say
# so, so this asserts it rather than trusting it.

HELPCMD="SYS:iperf -h"
says "$HELPCMD" 1 "iperf 2" "the help says it speaks iperf 2"
says "$HELPCMD" 1 "iperf3|5201" "and names iperf3 as the thing it is not"
says "$HELPCMD" 1 "^  -s  +receive" "an option line has its text at column 34"
want_rc "$HELPCMD" 1 0 "and -h is not an error"

# ---- TCP, guest sending -------------------------------------------------

TCPCMD="SYS:iperf $PEERADDR -p $PORT_TCP -t $SECS"
want_rc "$TCPCMD" 1 0 "a TCP send against a live peer succeeds"
says "$TCPCMD" 1 "dir=tcp-tx" "and reports the direction it ran"

G_BYTES=$(guest_val "$TCPCMD" 1 bytes)
G_MS=$(guest_val "$TCPCMD" 1 ms)
G_BITS=$(guest_val "$TCPCMD" 1 bits_per_sec)
P_BYTES=$(peer_val tcp peer_bytes)

if [ -z "${G_BYTES:-}" ] || [ -z "${P_BYTES:-}" ]; then
    fail "no byte count to compare: guest '${G_BYTES:-}' peer '${P_BYTES:-}'"
elif [ "$G_BYTES" = "$P_BYTES" ]; then
    pass "every byte the guest counted arrived: $G_BYTES = $P_BYTES"
else
    fail "the guest says it sent $G_BYTES bytes and the peer received" \
         "$P_BYTES.  On TCP with a clean close those are the same number."
fi

# The rate, recomputed from the guest's own two numbers.  A transfer that
# moved the right bytes and reported the wrong speed fails here.
if [ -n "${G_BYTES:-}" ] && [ -n "${G_MS:-}" ] && [ "${G_MS:-0}" -gt 0 ]; then
    WANT=$(( (G_BYTES * 8000 + G_MS / 2) / G_MS ))
    LO=$(( WANT - WANT / 100 - 1 ))
    HI=$(( WANT + WANT / 100 + 1 ))
    if [ "$G_BITS" -ge "$LO" ] && [ "$G_BITS" -le "$HI" ]; then
        pass "the rate matches its own bytes and milliseconds ($G_BITS)"
    else
        fail "$G_BYTES bytes in $G_MS ms is $WANT bit/s," \
             "and it reported $G_BITS"
    fi
    # The bug in the vendored engine, asserted rather than described: it
    # divides before scaling and reports whole Mbit/s, so anything under a
    # megabit reads as zero.
    if [ "$G_BITS" -gt 0 ]; then
        pass "a sub-megabit rate would not be reported as zero"
    else
        fail "the rate came out as 0 bit/s for $G_BYTES bytes in $G_MS ms"
    fi
else
    fail "the guest reported no duration for the TCP send"
fi

# ---- a byte target rather than a duration -------------------------------

SIZECMD="SYS:iperf $PEERADDR -p $PORT_SIZE -n $SIZE_KB"
want_rc "$SIZECMD" 1 0 "a run bounded by size succeeds"

S_BYTES=$(guest_val "$SIZECMD" 1 bytes)
WANT_BYTES=$(( SIZE_KB * 1024 ))
if [ -z "${S_BYTES:-}" ]; then
    fail "the -n run reported no byte count"
elif [ "$S_BYTES" -ge "$WANT_BYTES" ] &&
     [ "$S_BYTES" -lt $(( WANT_BYTES + 8192 )) ]; then
    pass "-n $SIZE_KB moved $S_BYTES bytes, at least the $WANT_BYTES asked" \
         "for and less than one buffer over"
else
    fail "-n $SIZE_KB should move $WANT_BYTES bytes and moved $S_BYTES"
fi

SP_BYTES=$(peer_val size peer_bytes)
if [ -n "${SP_BYTES:-}" ] && [ "$S_BYTES" = "$SP_BYTES" ]; then
    pass "and the peer received exactly those $SP_BYTES bytes"
else
    fail "the -n run sent $S_BYTES and the peer received '${SP_BYTES:-nothing}'"
fi

# ---- UDP, guest sending -------------------------------------------------

UDPCMD="SYS:iperf -u $PEERADDR -p $PORT_UDP -t $SECS -b 2000"
want_rc "$UDPCMD" 1 0 "a UDP send against a live peer succeeds"
says "$UDPCMD" 1 "dir=udp-tx" "and reports the direction it ran"

# The strong one: the guest parsed the peer's end-of-test report.  This is the
# whole reason the FIN handshake exists, and it is where a wrong report offset
# or a garbage client_hdr shows up as silence rather than as a wrong number.
if [ "$(guest_val "$UDPCMD" 1 peerreport)" = "1" ]; then
    pass "the guest read the peer's end-of-test report"
else
    fail "the guest got no end-of-test report from the peer, so the" \
         "figure is its own send rate and not the delivered one"
    show "$UDPCMD" 1
fi

U_BYTES=$(guest_val "$UDPCMD" 1 bytes)
UP_BYTES=$(peer_val udp peer_bytes)
UP_LOST=$(peer_val udp peer_lost)
UP_OOO=$(peer_val udp peer_outoforder)

# With nothing lost the two totals must agree EXACTLY, end marker included.
# iperf 2's receiver counts that marker into the total it reports back, so a
# sender that leaves it out disagrees with every peer by one datagram -- which
# is what this caught on the first run.
if [ -z "${U_BYTES:-}" ] || [ -z "${UP_BYTES:-}" ]; then
    fail "no UDP byte count to compare: guest '${U_BYTES:-}' peer '${UP_BYTES:-}'"
elif [ "$UP_BYTES" -gt "$U_BYTES" ]; then
    fail "the peer received $UP_BYTES bytes and the guest only sent $U_BYTES"
elif [ "${UP_LOST:-0}" = "0" ] && [ "$UP_BYTES" = "$U_BYTES" ]; then
    pass "nothing was lost and both ends counted $U_BYTES bytes, end marker" \
         "included"
elif [ "${UP_LOST:-0}" = "0" ]; then
    fail "nothing was lost, yet the guest counted $U_BYTES bytes and the" \
         "peer $UP_BYTES.  A difference of one datagram is the end marker."
elif [ "$UP_BYTES" -ge $(( U_BYTES - U_BYTES / 20 )) ]; then
    pass "the peer received $UP_BYTES of $U_BYTES bytes sent, within 5%"
else
    fail "the peer received $UP_BYTES of $U_BYTES bytes sent," \
         "which is more than 5% lost on a link that should lose nothing"
fi

# The datagram accounting has to be self-consistent: the peer counts what the
# ids said, and a sender that never restarts its sequence loses nothing here.
if [ "${UP_LOST:-0}" = "0" ] && [ "${UP_OOO:-0}" = "0" ]; then
    pass "the peer saw no gap and no reordering in the datagram ids"
else
    fail "the peer saw $UP_LOST lost and $UP_OOO out of order, which on this" \
         "link means the ids are wrong rather than that datagrams were dropped"
fi

# ---- the refusals -------------------------------------------------------

DEADCMD="SYS:iperf $PEERADDR -p $PORT_DEAD -t $SECS"
want_rc "$DEADCMD" 1 10 "a port with nothing listening is refused"
says "$DEADCMD" 1 "nothing is listening|did not answer|no route" \
     "and says so in words rather than an errno"
if block "$DEADCMD" 1 | grep -Eq '(^| )dir='; then
    fail "the refused run still printed a result line, which a script" \
         "would read as a measurement"
else
    pass "and prints no result line, so a script cannot read it as a figure"
fi

want_rc "SYS:iperf $PEERADDR -p 0 -t $SECS" 1 10 "port 0 is refused"
says "SYS:iperf $PEERADDR -p 0 -t $SECS" 1 "not a port" "and says why"

want_rc "SYS:iperf $PEERADDR -t 0" 1 10 "a zero-second run is refused"
says "SYS:iperf $PEERADDR -t 0" 1 "not a measurement" \
     "and says a run has to be bounded"

want_rc "SYS:iperf" 1 10 "a client with no host is refused"
says "SYS:iperf" 1 "no host was given" "and asks for one"

want_rc "SYS:iperf -s $PEERADDR" 1 10 "-s with a host is refused"
says "SYS:iperf -s $PEERADDR" 1 "takes no host" "and says why"

# ---- the guest as the server --------------------------------------------

if [ "$SERVER_ARMS" = yes ]; then
    # The mirror of the client arms: the guest counted what it received, the
    # host counted what it sent, and on TCP with a clean close those are the
    # same number.  This is the only coverage of accept() and of the guest
    # generating the UDP end-of-test report rather than parsing one.
    SRVTCP="SYS:iperf -s -p $PORT_SRV_TCP -t $SRV_WINDOW"
    want_rc "$SRVTCP" 1 0 "the guest served a TCP receive"
    says "$SRVTCP" 1 "dir=tcp-rx" "and reports the direction it ran"

    R_BYTES=$(guest_val "$SRVTCP" 1 bytes)
    RP_BYTES=$(peer_val srvtcp peer_bytes)
    if [ -z "${R_BYTES:-}" ] || [ -z "${RP_BYTES:-}" ]; then
        fail "no TCP receive count to compare: guest '${R_BYTES:-}'" \
             "peer '${RP_BYTES:-}'"
    elif [ "$R_BYTES" = "$RP_BYTES" ]; then
        pass "the guest received every byte the peer sent: $R_BYTES"
    else
        fail "the peer sent $RP_BYTES bytes and the guest counted $R_BYTES"
    fi

    SRVUDP="SYS:iperf -s -u -p $PORT_SRV_UDP -t $SRV_WINDOW"
    want_rc "$SRVUDP" 1 0 "the guest served a UDP receive"
    says "$SRVUDP" 1 "dir=udp-rx" "and reports the direction it ran"

    # The peer only exits 0 on peer_report=1, so reaching here at all means
    # the guest built a report the peer could parse.  Assert the numbers in it
    # rather than merely that one arrived.
    U_FAR=$(peer_val srvudp peer_far_bytes)
    UR_BYTES=$(guest_val "$SRVUDP" 1 bytes)
    if [ -z "${U_FAR:-}" ]; then
        fail "the guest sent no end-of-test report the peer could read, so" \
             "nothing on the far end would print a Server Report for it"
    elif [ "$U_FAR" = "$UR_BYTES" ]; then
        pass "the report the guest sent says $U_FAR bytes, which is what it" \
             "counted"
    else
        fail "the guest counted $UR_BYTES bytes and told the peer $U_FAR"
    fi

    # ---- and where the ones that did not arrive were counted -------------
    #
    # A UDP receive can lose datagrams and that is not a failure here: the
    # peer sends at a rate it chose and a slow machine cannot be made to keep
    # up by asserting that it does.  What IS worth having is WHERE.  A loss
    # figure with no location kept `the DP8390 PIO cards lose a third of a UDP
    # stream with no Fast RAM` open with `nobody has looked at where the frames
    # go` as its last sentence, and answering it needed nothing this script did
    # not already boot -- only two `netstat -s` either side of the arm.
    #
    # The five places a received datagram can be counted, outermost first:
    #
    #   overruns          the chip's receive ring or FIFO filled while the CPU
    #                     was copying the frame before it
    #   receive errors    the frame reached the driver damaged or unusable
    #   unknown types     the frame reached anxnet.device and no CMD_READ was
    #                     outstanding for its EtherType, so nothing took it
    #   buffer failures   a reader could not get an NX_PACKET to re-arm with,
    #                     which is the packet pool starving
    #   udp dropped       the datagram reached UDP and the socket's receive
    #                     queue was full, or nothing was bound to the port
    #
    # Only the last one means the machine did all the work and then threw the
    # result away, and it is the one every arm measured so far lands on.  A
    # difference the five do not account for is a frame that vanished between
    # the wire and the driver, which no counter on the guest can see and which
    # is the answer that would need a capture.
    #
    # Buffer failures are printed and NOT added into that sum. A reader that
    # could not get a packet is a read that was not posted, and the frame it
    # would have caught is then counted as an unknown type when it arrives:
    # summing both would count one frame twice. It is reported beside them
    # because it says which of the two the machine ran out of.
    #
    # One counter out of the nth `netstat -s`.  Named by the label netstat
    # prints beside it, because the two of them move together: a column that
    # is renamed there stops matching here rather than reporting the wrong
    # number.  `netstat -s` prints "buffer failures" only when a driver is
    # attached, so a missing line reads as 0 and not as an empty string.
    ns_val() { # nth sed-expression
        # `|| true` because `set -o pipefail` is on and head -1 can close the
        # pipe under sed.
        _v=$(block "SYS:netstat -s" "$1" | sed -n "$2" | head -1 || true)
        printf '%s' "${_v:-0}"
    }
    ns_delta() { # nth-before nth-after sed-expression
        printf '%s' "$(( $(ns_val "$2" "$3") - $(ns_val "$1" "$3") ))"
    }

    NS_OVR='s/.*overruns  *\([0-9][0-9]*\).*/\1/p'
    NS_ERR='s/^  receive errors  *\([0-9][0-9]*\).*/\1/p'
    NS_UNK='s/^  unknown types  *\([0-9][0-9]*\).*/\1/p'
    NS_BUF='s/^  buffer failures  *\([0-9][0-9]*\).*/\1/p'
    NS_UDP='s/.*bad datagrams, .*, \([0-9][0-9]*\) dropped.*/\1/p'

    U_OFFERED=$(peer_val srvudp peer_packets)
    U_GOT=$(guest_val "$SRVUDP" 1 packets)

    D_OVR=$(ns_delta 1 2 "$NS_OVR")
    D_ERR=$(ns_delta 1 2 "$NS_ERR")
    D_UNK=$(ns_delta 1 2 "$NS_UNK")
    D_BUF=$(ns_delta 1 2 "$NS_BUF")
    D_UDP=$(ns_delta 1 2 "$NS_UDP")

    printf 'udp_rx_offered=%s udp_rx_delivered=%s dev_overruns=%s dev_rx_errors=%s dev_unknown_types=%s pool_buffer_failures=%s udp_socket_dropped=%s\n' \
           "${U_OFFERED:-0}" "${U_GOT:-0}" "$D_OVR" "$D_ERR" "$D_UNK" \
           "$D_BUF" "$D_UDP"

    if [ "${U_OFFERED:-0}" -gt 0 ] &&
       [ "${U_GOT:-0}" -lt "${U_OFFERED:-0}" ]; then
        _short=$(( U_OFFERED - U_GOT ))
        _seen=$(( D_OVR + D_ERR + D_UNK + D_UDP ))
        if [ "$_seen" -ge "$_short" ]; then
            pass "the $_short datagram(s) that did not reach the" \
                 "application are accounted for: $D_UDP at the socket's" \
                 "receive queue, $D_UNK with no read outstanding, $D_OVR" \
                 "chip overruns, $D_ERR receive errors"
        else
            pass "of $_short datagram(s) short, $_seen are accounted for on" \
                 "the guest ($D_UDP socket queue, $D_UNK no read, $D_OVR" \
                 "overrun, $D_ERR error); the rest never reached the driver"
        fi
    fi
else
    # SLIRP has no way in, so these two arms cannot run here -- and they are
    # the ONLY coverage of accept() and of the guest generating a UDP
    # end-of-test report rather than parsing one.  The nightly runs this
    # harness with no -B and no -P, so that half has never been measured by
    # anything, under a step that reported a tick.  Counted, and the verdict
    # exits 77 for it.
    skip "the two directions with the guest as the server: a SLIRP guest" \
         "cannot be called in to.  Re-run with -B <iface> -P <third-machine>."
fi

# ---- nothing wrote an unbounded file ------------------------------------
#
# The gate, not a tidiness check.  A peer or a guest that spun would show up
# here as a log that grew without limit, which is the failure this whole
# arrangement is arranged to make impossible.

LOG_CEILING=$(( 1024 * 1024 ))
BIG=""
for f in "$PEERLOG"/*.out "$PEERLOG"/*.err "$HD"/*.txt; do
    [ -f "$f" ] || continue
    sz=$(wc -c < "$f" | tr -d ' ')
    [ "$sz" -gt "$LOG_CEILING" ] && BIG="$BIG $f($sz)"
done
if [ -z "$BIG" ]; then
    pass "every log stayed under $LOG_CEILING bytes"
else
    fail "these grew past $LOG_CEILING bytes, which means something spun:$BIG"
fi

# ------------------------------------------------------------------ verdict --

echo
# Failures first.  A run with both a failure and an unrun arm is a failure: the
# skip is the less urgent of the two facts and reporting it first sends
# somebody to look at the rig instead of at the defect.
if [ "$FAILED" != 0 ]; then
    echo "the transcript above is the whole run" >&2
    exit 1
fi

if [ "$UNRUN" != 0 ]; then
    echo "SKIPPED: iperf measured what it could, and $UNRUN arm(s) did not run" >&2
    echo "  Everything that DID run passed.  This is not a pass: an arm that" >&2
    echo "  cannot be reached proves nothing about the code it would have" >&2
    echo "  exercised.  The SKIP lines above say which and what to re-run with." >&2
    exit 77
fi

echo "PASS: iperf measured what crossed the wire, and refused what it should"
exit 0
