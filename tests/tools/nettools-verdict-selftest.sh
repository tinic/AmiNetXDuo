#!/usr/bin/env bash
#
# Prove tests/tools/nettools-verdict.sh can fail.
#
#   tests/tools/nettools-verdict-selftest.sh
#
# run-nettools.sh needs a Kickstart, a driver, a peer process and five
# minutes, and until this existed it had no assertion of any kind: its verdict
# was ToolsSmoke's return code, which is the return code of the last line of
# the list.  So each way the five commands can silently do nothing is here as
# a transcript, a DH0: drawer and a peer log, and each one has to come out
# red.
#
# The fixtures are ToolsSmoke's own framing with the commands' own output and
# tests/tools/netpeer.py's own log lines in them.  Needs nothing; under a
# second.
#
# SPDX-License-Identifier: MIT

set -uo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
# shellcheck source=tests/tools/nettools-verdict.sh
. "$ROOT/tests/tools/nettools-verdict.sh"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

PEER=10.0.2.2
pass=0; fail=0

expect() { # name want-rc dir [inbound]
    local name="$1" want="$2" d="$3" got out
    out=$(nettools_verdict "$d/tools.txt" "$d/hd" "$d/peer.log" "$PEER" \
                           "${4:-yes}"); got=$?
    if [ "$got" = "$want" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "SELFTEST FAIL: $name wanted rc $want, got $got" >&2
        printf '%s\n' "$out" | sed 's/^/    /' >&2
    fi
}

# A complete, passing run, built from scratch each time so a case can spoil
# exactly one thing about it.
build_good() { # dir
    local d="$1"
    rm -rf "$d"; mkdir -p "$d/hd"

    cat > "$d/tools.txt" <<EOF

===== SYS:AddNetInterface eth0 =====
eth0: a2065.device unit 0
eth0: online, address 10.0.2.15
----- rc 0, 1760 ms, free 8886288 -----

===== SYS:nc -z $PEER 7001 -v =====
nc: $PEER 7001 open
----- rc 0, 120 ms, free 8886288 -----

===== SYS:nc -z $PEER 1-2 -v -w 5 =====
nc: $PEER 1 refused
nc: $PEER 2 refused
----- rc 10, 340 ms, free 8886288 -----

===== SYS:nc $PEER 7001 -v -w 10 -N =====
nc: $PEER 7001 open
----- rc 0, 620 ms, free 8886288 -----

===== SYS:telnet $PEER 7023 -d =====
telnet: connected to $PEER port 7023
----- rc 0, 900 ms, free 8886288 -----

===== SYS:nc $PEER 1 -v -w 5 =====
nc: $PEER 1 refused
----- rc 10, 100 ms, free 8886288 -----

===== SYS:nc no.such.host.invalid 80 =====
nc: no.such.host.invalid: the name does not resolve
----- rc 10, 5040 ms, free 8886288 -----

===== SYS:traceroute $PEER -m 4 -q 2 -w 3 -n =====
traceroute to $PEER, 4 hops max, 40 byte packets
 1  $PEER  4 ms  3 ms
----- rc 0, 1200 ms, free 8886288 -----

===== SYS:tftp $PEER PORT 7069 GET hello.txt AS DH0:tftp-hello.txt =====
tftp: 49 bytes -> DH0:tftp-hello.txt
----- rc 0, 300 ms, free 8886288 -----

===== SYS:tftp $PEER PORT 7069 GET big.bin AS DH0:tftp-big.bin =====
tftp: 100000 bytes -> DH0:tftp-big.bin
----- rc 0, 9300 ms, free 8886288 -----

===== SYS:tftp $PEER PORT 7069 GET exact.bin AS DH0:tftp-exact.bin =====
tftp: 2048 bytes -> DH0:tftp-exact.bin
----- rc 0, 400 ms, free 8886288 -----

===== SYS:tftp $PEER PORT 7069 GET no.such.file =====
tftp: no.such.file: file not found
----- rc 10, 200 ms, free 8886288 -----

===== SYS:whois plain.test SERVER $PEER PORT 7043 =====
domain:       PLAIN.TEST
organisation: AmiNetXDuo test rig
----- rc 0, 200 ms, free 8886288 -----

===== done, 0 command(s) would not run =====
EOF

    printf 'echo server\r\nhello from the amiga\n' > "$d/hd/nc-echo.txt"
    printf 'hello from the amiga\n' > "$d/hd/nc-loopback.txt"
    printf 'hello from the host\r\n' > "$d/hd/nc-inbound.txt"
    printf 'AmiNetXDuo test telnet server\r\nlogin: you said: amiga\r\n' \
        > "$d/hd/telnet.txt"
    printf 'Hello from the host.\r\nSecond line.\r\nThird line.\r\n' \
        > "$d/hd/tftp-hello.txt"
    head -c 100000 /dev/zero > "$d/hd/tftp-big.bin"
    head -c 2048 /dev/zero > "$d/hd/tftp-exact.bin"

    cat > "$d/peer.log" <<'EOF'
[   1.20] echo   connection from 10.0.2.15:1024
[   1.90] echo   got 21 bytes: b'hello from the amiga\n'
[   3.10] telnet connection from 10.0.2.15:1025
[   3.90] telnet line: 'amiga'
[   4.00] telnet answers: DO ECHO, DO SGA, WONT TERMINAL-TYPE, WONT WINDOW-SIZE
[   9.40] tftp   PUT from-amiga.txt, 21 bytes
[  22.10] dial   connected to 127.0.0.1:7042 after 6 attempts
[  22.60] dial   the Amiga sent back 0 bytes: b''
EOF
}

build_good "$T/good"
expect "a run in which everything worked" 0 "$T/good"

# ---- the transfer that returned 0 and moved nothing -----------------------
build_good "$T/emptybig"; : > "$T/emptybig/hd/tftp-big.bin"
expect "a 100000-byte GET that wrote 0 bytes" 1 "$T/emptybig"

# ---- four blocks without the trailing empty one ---------------------------
build_good "$T/shortexact"; head -c 1536 /dev/zero > "$T/shortexact/hd/tftp-exact.bin"
expect "exact.bin three blocks long" 1 "$T/shortexact"

# ---- the round trip that never happened -----------------------------------
build_good "$T/noecho"; rm -f "$T/noecho/hd/nc-echo.txt"
expect "no echo file at all" 1 "$T/noecho"

build_good "$T/wrongecho"; printf 'echo server\r\n' > "$T/wrongecho/hd/nc-echo.txt"
expect "an echo file with none of the bytes in it" 1 "$T/wrongecho"

# ---- the connection from outside that never arrived -----------------------
build_good "$T/noinbound"; : > "$T/noinbound/hd/nc-inbound.txt"
expect "nothing reached the guest's listener" 1 "$T/noinbound"

# ... and the same run on a runner that cannot forward a port at all, which is
# a HOLE and not a failure.  It must not be silent either: the pass carries
# nettools_nc_inbound=unforwarded.
expect "no forward, so nothing could call in" 0 "$T/noinbound" no

# ---- a scan that found nothing and said it was fine -----------------------
build_good "$T/scan0"
sed -i.bak "s/^nc: $PEER 7001 open\$//" "$T/scan0/tools.txt"
expect "a -z scan that reported no open port" 1 "$T/scan0"

# ---- a port nothing listens on, reported as a success ---------------------
build_good "$T/refused0"
perl -0pi -e "s/(===== SYS:nc -z 10\.0\.2\.2 1-2 -v -w 5 =====\n(?:.*\n)*?----- rc )10/\${1}0/" \
    "$T/refused0/tools.txt"
expect "ports 1-2 reported as open" 1 "$T/refused0"

# ---- a name that does not exist, reported as a success --------------------
build_good "$T/badname0"
perl -0pi -e "s/(===== SYS:nc no\.such\.host\.invalid 80 =====\n(?:.*\n)*?----- rc )10/\${1}0/" \
    "$T/badname0/tools.txt"
expect "an unresolvable name reported as connected" 1 "$T/badname0"

# ---- a file that is not there, reported as fetched ------------------------
build_good "$T/missing0"
perl -0pi -e "s/(GET no\.such\.file =====\n(?:.*\n)*?----- rc )10/\${1}0/" \
    "$T/missing0/tools.txt"
expect "a missing file reported as transferred" 1 "$T/missing0"

# ---- the telnet session that was a connection and nothing else -----------
build_good "$T/nolines"
grep -v "telnet line:" "$T/nolines/peer.log" > "$T/nolines/peer.log.new"
mv "$T/nolines/peer.log.new" "$T/nolines/peer.log"
expect "a telnet server that got no line from the guest" 1 "$T/nolines"

build_good "$T/nosession"
grep -v telnet "$T/nosession/peer.log" > "$T/nosession/peer.log.new"
mv "$T/nosession/peer.log.new" "$T/nosession/peer.log"
expect "a telnet server that saw no session" 1 "$T/nosession"

build_good "$T/noecho2"
printf 'AmiNetXDuo test telnet server\r\nlogin: ' > "$T/noecho2/hd/telnet.txt"
expect "a telnet transcript with no echo of what was sent" 1 "$T/noecho2"

build_good "$T/noanswers"
sed -i.bak 's/answers: DO ECHO.*/answers: (none)/' "$T/noanswers/peer.log"
expect "a telnet client that answered no option negotiation" 1 "$T/noanswers"

# The scripted client can send its input before it reads the server's option
# offer.  `quit` therefore precedes the IAC replies in the byte stream.  The
# peer must drain those replies before closing or its evidence says `(none)`
# even though the client answered every option.
if python3 - "$ROOT/tests/tools/netpeer.py" <<'PY'
import importlib.util
import socket
import sys
import threading

path = sys.argv[1]
spec = importlib.util.spec_from_file_location("netpeer", path)
netpeer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(netpeer)

seen = []
netpeer.log = lambda tag, message: seen.append((tag, message))

server, client = socket.socketpair()
thread = threading.Thread(
    target=netpeer.TelnetHandler,
    args=(server, ("127.0.0.1", 1234), object()),
)
thread.start()

client.recv(4096)  # option offer and prompt
client.sendall(
    b"amiga\r\nquit\r\n"
    + bytes([
        netpeer.IAC, netpeer.DO, netpeer.OPT_ECHO,
        netpeer.IAC, netpeer.DO, netpeer.OPT_SGA,
        netpeer.IAC, netpeer.WONT, netpeer.OPT_TTYPE,
        netpeer.IAC, netpeer.WONT, netpeer.OPT_NAWS,
    ])
)
client.shutdown(socket.SHUT_WR)
thread.join(2)
client.close()
server.close()

want = "closed; answers: DO ECHO, DO SGA, WONT TERMINAL-TYPE, WONT WINDOW-SIZE"
if thread.is_alive() or ("telnet", want) not in seen:
    raise SystemExit("netpeer did not drain negotiation after quit: %r" % (seen,))
PY
then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    echo "SELFTEST FAIL: netpeer stopped reading at quit" >&2
fi

# ---- the PUT that never left -----------------------------------------------
build_good "$T/noput"
grep -v from-amiga "$T/noput/peer.log" > "$T/noput/peer.log.new"
mv "$T/noput/peer.log.new" "$T/noput/peer.log"
expect "a tftp PUT the server never saw" 1 "$T/noput"

# ---- a trace with no answer in it -----------------------------------------
build_good "$T/stars"
sed -i.bak "s/^ 1  $PEER.*/ 1  * * */" "$T/stars/tools.txt"
expect "a traceroute to a host that answers, all stars" 1 "$T/stars"

# ---- the evidence that was not collected ----------------------------------
build_good "$T/nopeerlog"; rm -f "$T/nopeerlog/peer.log"
expect "no peer log at all" 1 "$T/nopeerlog"

build_good "$T/short"
sed -i.bak 's/done, 0 command/done, 3 command/' "$T/short/tools.txt"
expect "three commands never ran" 1 "$T/short"

build_good "$T/notranscript"; : > "$T/notranscript/tools.txt"
expect "an empty transcript" 1 "$T/notranscript"

build_good "$T/noiface"
perl -0pi -e "s/(===== SYS:AddNetInterface eth0 =====\n(?:.*\n)*?----- rc )0/\${1}10/" \
    "$T/noiface/tools.txt"
expect "the interface never came up" 1 "$T/noiface"

echo "nettools-verdict selftest: $pass passed, $fail failed"
[ "$fail" = 0 ] || exit 1
exit 0
