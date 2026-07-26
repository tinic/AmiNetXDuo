#!/usr/bin/env bash
#
# Run nc, telnet and ftp against real servers, under FS-UAE on SLIRP.
#
#   tests/tools/run-nettools.sh [-m MODEL] [-t SECONDS] [-c CPU] [-b BUILDDIR]
#                               [-o "line;line;..."]
#
# tools/fsuae-run.sh starts ONE executable with no arguments, so ToolsSmoke
# stands in the middle and runs DH0:commands.txt.  Same shape as
# tests/tls/run-fetch.sh, with two additions ToolsSmoke grew for this run:
# "&<command>" starts something in the background and "wait <secs>" lets it
# settle -- without which a listener and the thing that connects to it can
# never be running at the same time, and `nc -l` could not be tested at all.
#
# WHAT IS ON THE OTHER END
#
#   tests/tools/netpeer.py, on the host: an echo server, a telnet server that
#   negotiates properly, and an FTP server that does both PASV and PORT.  The
#   guest reaches them at 10.0.2.2, which is what SLIRP calls the host.
#
# AND WHAT REACHES THE GUEST
#
#   SLIRP is a NAT, so nothing outside can open a connection INTO the Amiga
#   unless a port is forwarded.  FS-UAE's `slirp_redir` does that -- the
#   option is UAE's, so it goes through the uae_ passthrough -- and this
#   script forwards two:
#
#     NC_INBOUND_PORT    so the host can connect to `nc -l` on the Amiga
#     FTP_DATA_PORT      so the FTP server can call back to ACTIVE mode
#
#   The guest's own address is SLIRP's first DHCP lease, 10.0.2.15.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
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

# Ports.  Deliberately above 1024 and out of the way: the host may well have a
# real ftpd or sshd, and a test that quietly talks to it would be worse than
# one that fails.
ECHO_PORT="${AMINETXDUO_ECHO_PORT:-7001}"
TELNET_PORT="${AMINETXDUO_TELNET_PORT:-7023}"
FTP_PORT="${AMINETXDUO_FTP_PORT:-7021}"
NC_INBOUND_PORT="${AMINETXDUO_NC_PORT:-7042}"
FTP_DATA_PORT="${AMINETXDUO_FTP_DATA_PORT:-7060}"
# TFTP's own port is 69, and binding it needs root on this host -- a test that
# asks for a password is a test nobody runs.  The command takes the port as an
# argument, so the rig uses a high one.
TFTP_PORT="${AMINETXDUO_TFTP_PORT:-7069}"

SMOKE="$ROOT/$BUILD/src/tools/ToolsSmoke"
ADDIF="$ROOT/$BUILD/src/tools/AddNetInterface"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"
NC="$ROOT/$BUILD/src/tools/nc"
TELNET="$ROOT/$BUILD/src/tools/telnet"
FTP="$ROOT/$BUILD/src/tools/ftp"
TRACEROUTE="$ROOT/$BUILD/src/tools/traceroute"
TFTP="$ROOT/$BUILD/src/tools/tftp"

for f in "$SMOKE" "$ADDIF" "$BSD" "$NC" "$TELNET" "$FTP" "$TRACEROUTE" \
         "$TFTP"; do
    [ -f "$f" ] || { echo "missing $f -- build the tree first" >&2; exit 2; }
done

# ------------------------------------------------------------- a2065 -----

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

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/nettools-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"    "$STAGE/libs/bsdsocket.library"
cp "$ADDIF"  "$STAGE/AddNetInterface"
cp "$NC"     "$STAGE/nc"
cp "$TELNET" "$STAGE/telnet"
cp "$FTP"    "$STAGE/ftp"
cp "$TRACEROUTE" "$STAGE/traceroute"
cp "$TFTP"       "$STAGE/tftp"

# What the scripted sessions feed to standard input.
printf 'GET / HTTP/1.0\r\n\r\n' > "$STAGE/request.txt"
printf 'hello from the amiga\n' > "$STAGE/greeting.txt"

cat > "$STAGE/telnetin.txt" <<'EOF'
amiga
quit
EOF

cat > "$STAGE/ftppasv.txt" <<'EOF'
status
system
pwd
ls
binary
get hello.txt DH0:got-passive.txt
size hello.txt
put DH0:greeting.txt uploaded-passive.txt
bye
EOF

cat > "$STAGE/ftpactive.txt" <<'EOF'
active
status
ls
binary
get hello.txt DH0:got-active.txt
put DH0:greeting.txt uploaded-active.txt
bye
EOF

if [ -n "${AMINETXDUO_NETTOOLS_COMMANDS:-}" ]; then
    cp "$AMINETXDUO_NETTOOLS_COMMANDS" "$STAGE/commands.txt"
    echo "==> command list: $AMINETXDUO_NETTOOLS_COMMANDS"
else
cat > "$STAGE/commands.txt" <<EOF
# ---- the templates, through ReadArgs' own "?" -------------------------
SYS:nc ?
SYS:telnet ?
SYS:ftp ?
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
# ---- ftp, passive then active -----------------------------------------
SYS:ftp 10.0.2.2 $FTP_PORT USER amiga PASSWORD test -d <DH0:ftppasv.txt >DH0:ftp-passive.txt
SYS:ftp 10.0.2.2 $FTP_PORT USER amiga PASSWORD test DATAPORT $FTP_DATA_PORT TIMEOUT 15 -d <DH0:ftpactive.txt >DH0:ftp-active.txt
# ---- and what the failures look like ----------------------------------
SYS:nc 10.0.2.2 1 -v -w 5
SYS:nc no.such.host.invalid 80
SYS:ftp 10.0.2.2 1
# ---- nc as a server, guest to guest -----------------------------------
# The only listen/accept this emulator can actually drive end to end: SLIRP
# forwards nothing inward (see the section in docs/RESEARCH.md), so the
# Amiga has to be both ends.  This is a full conversation, half-close
# included, over 127.0.0.1.
&SYS:nc -l 7099 -v -w 10 -N >DH0:nc-loopback.txt
wait 4
SYS:nc 127.0.0.1 7099 -v -w 10 -N <DH0:greeting.txt >DH0:nc-loopclient.txt
wait 4
# ... and over this machine's own Ethernet address rather than 127.0.0.1.
&SYS:nc -l 7098 -v -w 10 >DH0:nc-self.txt
wait 4
SYS:nc 10.0.2.15 7098 -v -w 10 <DH0:greeting.txt >DH0:nc-selfclient.txt
# ---- traceroute and tftp ----------------------------------------------
SYS:traceroute ?
SYS:tftp ?
# What SLIRP does with a decrementing TTL is the whole question; the answer
# is in docs/RESEARCH.md 20, and this is one of the runs it came from.
#
#   10.0.2.2   SLIRP answers this one itself, so it is a complete trace.
#   10.0.2.15  our own address, which is one hop by definition.
#   8.8.8.8    proxied by SLIRP, which ignores the TTL and returns replies
#              with the sequence number zeroed.  Every probe is a star, and
#              that is the emulator rather than the command.
#   192.0.2.1  TEST-NET-1, and 10.11.12.13 -- addresses the HOST cannot
#              reach, so SLIRP would answer with an ICMP unreachable quoting
#              the probe.  It does not; see 20.2.
SYS:traceroute 10.0.2.2 -m 4 -q 2 -w 3 -n
SYS:traceroute 10.0.2.15 -m 3 -q 1 -w 3 -n
SYS:traceroute 8.8.8.8 -m 3 -q 1 -w 3 -n -v
SYS:traceroute 192.0.2.1 -m 2 -q 1 -w 3 -n -v
SYS:traceroute 10.11.12.13 -m 2 -q 1 -w 3 -n -v
# tftp against netpeer.py's server: a small file, a big one, one that is an
# exact multiple of the block size -- which ends with an EMPTY data block --
# one going the other way, and one that is not there.
SYS:tftp 10.0.2.2 PORT $TFTP_PORT GET hello.txt AS DH0:tftp-hello.txt
SYS:tftp 10.0.2.2 PORT $TFTP_PORT GET big.bin AS DH0:tftp-big.bin
SYS:tftp 10.0.2.2 PORT $TFTP_PORT GET exact.bin AS DH0:tftp-exact.bin
SYS:tftp 10.0.2.2 PORT $TFTP_PORT PUT DH0:greeting.txt AS from-amiga.txt
SYS:tftp 10.0.2.2 PORT $TFTP_PORT GET no.such.file
# ---- give the inbound connection time to have happened ----------------
wait 15
EOF
fi

# The same-machine case is NOT in that list, and the reason is a library bug
# rather than a gap in the commands: shutdown(SHUT_WR) on a connection whose
# two ends are on this machine hangs the caller in WaitSelect() forever.
# tests/tools/commands-samehost.txt reproduces it on its own; see the "nc,
# telnet and ftp" section of docs/RESEARCH.md.

# --------------------------------------------------------------- peers ---

PEERLOG="$ROOT/build/netpeer.log"
python3 "$ROOT/tests/tools/netpeer.py" \
    --echo-port "$ECHO_PORT" --telnet-port "$TELNET_PORT" \
    --ftp-port "$FTP_PORT" --tftp-port "$TFTP_PORT" \
    --advertise 10.0.2.2 --active-via-loopback \
    --dial "127.0.0.1:$NC_INBOUND_PORT" --dial-for "$TIMEOUT" \
    --log "$PEERLOG" --seconds "$((TIMEOUT + 120))" \
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
echo "==> netpeer.py: echo $ECHO_PORT, telnet $TELNET_PORT, ftp $FTP_PORT," \
     "tftp $TFTP_PORT"

# --------------------------------------------------------------- slirp ---
#
# Inbound.  Without these two the guest can call out and nothing can call in,
# which is exactly the half `nc -l` and active FTP exist to test.
REDIR="uae_slirp_redir = tcp:$NC_INBOUND_PORT:$NC_INBOUND_PORT"
REDIR="$REDIR;uae_slirp_redir = tcp:$FTP_DATA_PORT:$FTP_DATA_PORT"
[ -z "$EXTRA_CONFIG" ] || REDIR="$REDIR;$EXTRA_CONFIG"

export AMINETXDUO_FSUAE_EXTRA="$REDIR"
export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-nettools}"

CPUARG=()
[ -z "$CPU" ] || CPUARG=(-c "$CPU")

# Did the forward actually happen?  `slirp_redir` is accepted by the config
# parser whether or not the SLIRP build honours it, so the only proof is a
# host socket in LISTEN.  Checked from the side, while the run is going on.
(
    # Wait for the emulator itself, not for the clock: the run may sit in the
    # queue behind another one for minutes, and a probe that fires while it is
    # still queued proves nothing.
    waited=0
    while [ "$waited" -lt 600 ]; do
        pgrep -f "test-${AMINETXDUO_RUN_TAG:-nettools}.fs-uae" >/dev/null 2>&1 \
            && break
        sleep 2
        waited=$((waited + 2))
    done
    sleep 25
    if lsof -nP -iTCP:"$NC_INBOUND_PORT" -sTCP:LISTEN >/dev/null 2>&1; then
        echo "==> slirp_redir: host port $NC_INBOUND_PORT is LISTENING"
    else
        echo "!! slirp_redir: host port $NC_INBOUND_PORT is NOT listening;" \
             "nothing outside can reach the guest"
    fi
) &

set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" "${CPUARG[@]}" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/nc" "$STAGE/telnet" \
    "$STAGE/ftp" "$STAGE/traceroute" "$STAGE/tftp" \
    "$STAGE/AddNetInterface" "$STAGE/commands.txt" \
    "$STAGE/request.txt" "$STAGE/greeting.txt" "$STAGE/telnetin.txt" \
    "$STAGE/ftppasv.txt" "$STAGE/ftpactive.txt"
RC=$?
set -e

echo
echo "================ what the host servers saw ================"
cat "$PEERLOG" 2>/dev/null || true

exit "$RC"
