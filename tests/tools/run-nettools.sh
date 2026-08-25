#!/usr/bin/env bash
# Run nc and telnet against real servers, under FS-UAE on SLIRP.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
MODEL=A1200
TIMEOUT=300
CPU=""
BUILD="${AMINETXDUO_BUILD:-build/cm}"
EXTRA_CONFIG=""

while getopts "m:t:c:b:o:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        c) CPU="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        o) EXTRA_CONFIG="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-c cpu] [-b builddir]" >&2
           exit 2 ;;
    esac
done

ECHO_PORT="${AMINETXDUO_ECHO_PORT:-7001}"
TELNET_PORT="${AMINETXDUO_TELNET_PORT:-7023}"
NC_INBOUND_PORT="${AMINETXDUO_NC_PORT:-7042}"
TFTP_PORT="${AMINETXDUO_TFTP_PORT:-7069}"
WHOIS_PORT="${AMINETXDUO_WHOIS_PORT:-7043}"

SMOKE="$ROOT/$BUILD/src/tools/ToolsSmoke"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
NC="$ROOT/$BUILD/src/tools/nc"
TELNET="$ROOT/$BUILD/src/tools/telnet"
TRACEROUTE="$ROOT/$BUILD/src/tools/traceroute"
TFTP="$ROOT/$BUILD/src/tools/tftp"
WHOIS="$ROOT/$BUILD/src/tools/whois"

for f in "$SMOKE" "$ADDIF" "$BSD" "$NC" "$TELNET" "$TRACEROUTE" \
         "$TFTP" "$WHOIS"; do
    [ -f "$f" ] || { echo "missing $f, build the tree first" >&2; exit 2; }
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


STAGE="$ROOT/build/nettools-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
cp "$ADDIF"  "$STAGE/AddNetInterface"
cp "$NC"     "$STAGE/nc"
cp "$TELNET" "$STAGE/telnet"
cp "$TRACEROUTE" "$STAGE/traceroute"
cp "$TFTP"       "$STAGE/tftp"
cp "$WHOIS"      "$STAGE/whois"

printf 'GET / HTTP/1.0\r\n\r\n' > "$STAGE/request.txt"
printf 'hello from the amiga\n' > "$STAGE/greeting.txt"

cat > "$STAGE/telnetin.txt" <<'EOF'
amiga
quit
EOF



if [ -n "${AMINETXDUO_NETTOOLS_COMMANDS:-}" ]; then
    cp "$AMINETXDUO_NETTOOLS_COMMANDS" "$STAGE/commands.txt"
    echo "==> command list: $AMINETXDUO_NETTOOLS_COMMANDS"
else
cat > "$STAGE/commands.txt" <<EOF
# ---- the templates, through ReadArgs' own "?" -------------------------
SYS:nc ?
SYS:telnet ?
# ---- bring the network up once, and leave it up -----------------------
SYS:AddNetInterface eth0
# ---- nc, as a client --------------------------------------------------
SYS:nc -z 10.0.2.2 $ECHO_PORT -v
SYS:nc -z 10.0.2.2 1-2 -v -w 5
SYS:nc 10.0.2.2 $ECHO_PORT -v -w 10 -N <DH0:greeting.txt >DH0:nc-echo.txt
# ---- nc, as a SERVER: bind(), listen(), accept() ----------------------
# The HOST connects to this one, in through the SLIRP forward.  netpeer.py's
# dialer retries until it appears, so it is started here and everything below
# happens while it waits.
&SYS:nc -l $NC_INBOUND_PORT -v -w 120 >DH0:nc-inbound.txt
# ---- telnet -----------------------------------------------------------
SYS:telnet 10.0.2.2 $TELNET_PORT -d <DH0:telnetin.txt >DH0:telnet.txt
# ---- and what the failures look like ----------------------------------
SYS:nc 10.0.2.2 1 -v -w 5
SYS:nc no.such.host.invalid 80
# ---- nc as a server, guest to guest -----------------------------------
# The only listen/accept this emulator can actually drive end to end: SLIRP
# forwards nothing inward, so the Amiga has to be both ends.  This is a full conversation, half-close
# included, over 127.0.0.1.
&SYS:nc -l 7099 -v -w 10 -N >DH0:nc-loopback.txt
wait 4
SYS:nc 127.0.0.1 7099 -v -w 10 -N <DH0:greeting.txt >DH0:nc-loopclient.txt
wait 4
# ... and over this machine's own Ethernet address rather than 127.0.0.1.
&SYS:nc -l 7098 -v -w 10 >DH0:nc-self.txt
wait 4
SYS:nc 10.0.2.15 7098 -v -w 10 <DH0:greeting.txt >DH0:nc-selfclient.txt
# ---- traceroute, tftp and whois ---------------------------------------
SYS:traceroute ?
SYS:tftp ?
SYS:whois ?
# What SLIRP does with a decrementing TTL is the whole question; the answer
# is in docs/RESEARCH.md 20, and this is one of the runs it came from.
#
#   10.0.2.2   SLIRP answers this one itself, so it is a complete trace.
#   10.0.2.15  our own address, which is one hop by definition.
#   8.8.8.8    proxied by SLIRP, which ignores the TTL and returns replies
#              with the sequence number zeroed.  Every probe is a star, and
#              that is the emulator rather than the command.
#   192.0.2.1  TEST-NET-1, and 10.11.12.13, addresses the HOST cannot
#              reach, so SLIRP would answer with an ICMP unreachable quoting
#              the probe.  It does not; see 20.2.
SYS:traceroute 10.0.2.2 -m 4 -q 2 -w 3 -n
SYS:traceroute 10.0.2.15 -m 3 -q 1 -w 3 -n
SYS:traceroute 8.8.8.8 -m 3 -q 1 -w 3 -n -v
SYS:traceroute 192.0.2.1 -m 2 -q 1 -w 3 -n -v
SYS:traceroute 10.11.12.13 -m 2 -q 1 -w 3 -n -v
# tftp against netpeer.py's server: a small file, a big one, one that is an
# exact multiple of the block size, which ends with an EMPTY data block --
# one going the other way, and one that is not there.
SYS:tftp 10.0.2.2 PORT $TFTP_PORT GET hello.txt AS DH0:tftp-hello.txt
SYS:tftp 10.0.2.2 PORT $TFTP_PORT GET big.bin AS DH0:tftp-big.bin
SYS:tftp 10.0.2.2 PORT $TFTP_PORT GET exact.bin AS DH0:tftp-exact.bin
SYS:tftp 10.0.2.2 PORT $TFTP_PORT PUT DH0:greeting.txt AS from-amiga.txt
SYS:tftp 10.0.2.2 PORT $TFTP_PORT GET no.such.file
# whois against netpeer.py's, whose canned records cover the three shapes.
# referral.test refers to the server it came from, which is a loop and has to
# be recognised as one; chain.test refers somewhere ELSE, so without FOLLOW
# the line to type next is printed and with it the client goes there, to
# 127.0.0.1, where nothing is listening, so the second hop demonstrates the
# failure being legible.
SYS:whois plain.test SERVER 10.0.2.2 PORT $WHOIS_PORT
SYS:whois referral.test SERVER 10.0.2.2 PORT $WHOIS_PORT FOLLOW
SYS:whois chain.test SERVER 10.0.2.2 PORT $WHOIS_PORT
SYS:whois chain.test SERVER 10.0.2.2 PORT $WHOIS_PORT FOLLOW
# ... and the default server, which is a real registry over the real internet.
# example.com produces NO referral, IANA administers it and answers for it
# directly, so amiga.com is here as well: IANA refers that one to Verisign,
# and Verisign's record carries the indented "Registrar WHOIS Server:" line
# that a matcher anchored at column zero silently misses.  That is the case
# which found the bug, so it is the case that keeps it fixed.
SYS:whois example.com
SYS:whois amiga.com FOLLOW
# ---- give the inbound connection time to have happened ----------------
wait 15
EOF
fi



PEERLOG="$ROOT/build/netpeer.log"
python3 "$ROOT/tests/tools/netpeer.py" \
    --echo-port "$ECHO_PORT" --telnet-port "$TELNET_PORT" \
    --tftp-port "$TFTP_PORT" \
    --whois-port "$WHOIS_PORT" \
    --advertise 10.0.2.2 \
    --dial "127.0.0.1:$NC_INBOUND_PORT" --dial-for "$TIMEOUT" \
    --log "$PEERLOG" --seconds "$((TIMEOUT + 3600))" \
    > "$ROOT/build/netpeer.out" 2>&1 &
PEER_PID=$!

cleanup_peer() {
    kill -TERM "$PEER_PID" 2>/dev/null || true
}
trap cleanup_peer EXIT INT TERM HUP

sleep 1
kill -0 "$PEER_PID" 2>/dev/null || {
    echo "netpeer.py did not start:" >&2
    cat "$ROOT/build/netpeer.out" >&2
    exit 2
}
echo "==> netpeer.py: echo $ECHO_PORT, telnet $TELNET_PORT," \
     "tftp $TFTP_PORT, whois $WHOIS_PORT"

INBOUND_FLAG="$ROOT/build/nettools-inbound.flag"
echo unknown > "$INBOUND_FLAG"

REDIR="uae_slirp_redir = tcp:$NC_INBOUND_PORT:$NC_INBOUND_PORT"
[ -z "$EXTRA_CONFIG" ] || REDIR="$REDIR;$EXTRA_CONFIG"

export AMINETXDUO_FSUAE_EXTRA="$REDIR"
export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-nettools}"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

(
    # Wait for the emulator itself, not for the clock: the run may sit in the
    waited=0
    while [ "$waited" -lt 600 ]; do
        pgrep -f "amiberry-${AMINETXDUO_RUN_TAG:-nettools}.uae" >/dev/null 2>&1 \
            && break
        sleep 2
        waited=$((waited + 2))
    done
    sleep 25
    if lsof -nP -iTCP:"$NC_INBOUND_PORT" -sTCP:LISTEN >/dev/null 2>&1; then
        echo "==> slirp_redir: host port $NC_INBOUND_PORT is LISTENING"
        echo yes > "$INBOUND_FLAG"
    else
        echo "!! slirp_redir: host port $NC_INBOUND_PORT is NOT listening;" \
             "nothing outside can reach the guest"
        echo no > "$INBOUND_FLAG"
    fi
) &

set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/nc" "$STAGE/telnet" \
    "$STAGE/traceroute" "$STAGE/tftp" "$STAGE/whois" \
    "$STAGE/AddNetInterface" "$STAGE/commands.txt" \
    "$STAGE/request.txt" "$STAGE/greeting.txt" "$STAGE/telnetin.txt"
RC=$?
set -e

echo
echo "================ what the host servers saw ================"
cat "$PEERLOG" 2>/dev/null || true

echo
echo "---- the verdict ----"
# shellcheck source=tests/tools/nettools-verdict.sh
. "$ROOT/tests/tools/nettools-verdict.sh"

HD="$ROOT/build/amiberry-testhd-${AMINETXDUO_RUN_TAG:-nettools}"

printf 'run_rc=%s\n' "$RC"
if [ "$RC" != 0 ]; then
    printf 'reason=%s\n' "the guest did not come back (124 is the timeout)"
    printf 'RESULT=broken\n'
    exit 3
fi

if [ -n "${AMINETXDUO_NETTOOLS_COMMANDS:-}" ]; then
    printf 'reason=%s\n' "custom_command_list"
    printf 'RESULT=skip\n'
    exit 77
fi

if nettools_verdict "$HD/tools.txt" "$HD" "$PEERLOG" 10.0.2.2 \
                    "$(cat "$INBOUND_FLAG" 2>/dev/null || echo unknown)"; then
    printf 'RESULT=pass\n'
    exit 0
fi
printf 'RESULT=fail\n'
exit 1
