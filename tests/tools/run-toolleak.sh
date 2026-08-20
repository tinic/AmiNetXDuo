#!/usr/bin/env bash
#
# THE LEAK SWEEP FOR THE WHOLE COMMAND SET.
#
#   tests/tools/run-toolleak.sh [-b BUILDDIR] [-g GROUP] [-n RUNS] [-t SECONDS]
#                               [-V] [-I ROWID]
#
# WHAT IT PROVES
#
#   Every command gives back everything it took.  Each row of the table below
#   is run N times inside ONE boot, and by the end of the run an invocation has
#   to cost nothing at all.  The first runs pay whatever a first open costs on
#   this machine -- the library, the pool, the resolver's cache -- and the
#   figure reported is the cost of one more call once that has settled.
#   AmigaOS reclaims nothing when a process exits, so a per-invocation cost of
#   any size is memory that is gone until the machine is switched off.
#
#   That is tests/tools/run-addifleak.sh's shape, generalised: one table, one
#   row per (command, arm), rather than one script per command.  Adding a tool
#   means adding a row.
#
# BOTH ARMS
#
#   A command that leaks only when it refuses is the common case, because the
#   refusal path is the one nobody walks.  Every command that has a meaningful
#   failure gets a row for it: an interface that is not there, a host that does
#   not resolve, a route with no gateway.  `template` rows are the third arm
#   every command has: ReadArgs' own "?", which allocates an RDArgs and an
#   argument buffer and returns before anything else happens.
#
# THE PREMISE COLUMN IS NOT DECORATION
#
#   A row whose command silently stopped reaching the path it names would read
#   flat and pass.  Every row therefore states what has to be true of its own
#   output -- an exit code, or a phrase -- and the row FAILS if it is not,
#   before the arithmetic is even looked at.  A row with `?` in that column has
#   no premise and fails as such: nothing here may pass vacuously.
#
# BOOTS, AND WHY THERE ARE FOUR
#
#   cold    the stack was never started.  Config-only commands and every
#           command's `?`.  Nothing here may open bsdsocket.library, because
#           opening it STARTS the stack and the rest of the boot would no
#           longer be cold.
#   live    one `AddNetInterface eth0` and then everything that reads or uses
#           a running stack without disturbing it.
#   cycle   the commands that take the stack apart: Online/Offline,
#           AddNetInterface/RemoveNetInterface, NetShutdown.  Each is paired
#           with the command that puts back what it took, so the pair repeats.
#   server  `nc -l`, which blocks until its own -w expires and so cannot share
#           a transcript position with anything.
#
#   A boot's command list is CHUNKED to ToolsSmoke's MAX_COMMANDS, which is
#   read out of the source rather than assumed: past it the driver stops
#   reading and the run looks like a list nobody wrote.
#
# PROVING THE ASSERTION FIRES
#
#   -I ROWID subtracts 1024 bytes per run from that row's readings after the
#   real transcript has been parsed, so the assertion is exercised against the
#   real artifact rather than a fabricated one:
#
#       tests/tools/run-toolleak.sh -g live -V -I netstat/all
#
#   -V re-reads the last run's transcripts without booting anything, so the
#   demonstration costs no emulator time.
#
# A TIMEOUT IS A DEFECT
#
#   A run that burns its ceiling produces a partial transcript and proves
#   nothing.  The verdict names the command the transcript stops at, which is
#   the one that hung, and exits 2 -- infrastructure, not a result.  Raising
#   -t is never the fix.
#
# WHAT IT NEEDS
#
#   a2065.device (AMINETXDUO_A2065, or build/a2065.device), a Kickstart, and a
#   68020 Release build:
#
#     cmake -S . -B build/cm -DCMAKE_BUILD_TYPE=Release \
#           -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-m68k-amigaos.cmake
#     cmake --build build/cm --parallel
#     tests/tools/run-toolleak.sh
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

# shellcheck source=tools/test-verdict.sh
. "$ROOT/tools/test-verdict.sh"

BUILD="${AMINETXDUO_BUILD:-build/cm}"
SWEEP_GROUPS="cold live cycle server"
TIMEOUT=0
RUNS_OVERRIDE=0
VERDICT_ONLY=0
INJECT=""
MODEL=A1200

# Host-side peers.  Above 1024 and out of the way of anything real on the box.
ECHO_PORT="${AMINETXDUO_ECHO_PORT:-7301}"
TELNET_PORT="${AMINETXDUO_TELNET_PORT:-7323}"
TFTP_PORT="${AMINETXDUO_TFTP_PORT:-7369}"
WHOIS_PORT="${AMINETXDUO_WHOIS_PORT:-7343}"
HTTP_PORT="${AMINETXDUO_HTTP_PORT:-7380}"
IPERF_PORT="${AMINETXDUO_IPERF_PORT:-7385}"
# Nothing ever listens here.  It is the refusal arm's other end.
DEAD_PORT="${AMINETXDUO_DEAD_PORT:-7399}"

while getopts "b:g:n:t:m:VI:" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        g) SWEEP_GROUPS="$OPTARG" ;;
        n) RUNS_OVERRIDE="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        V) VERDICT_ONLY=1 ;;
        I) INJECT="$OPTARG" ;;
        *) echo "usage: $0 [-b builddir] [-g groups] [-n runs] [-t seconds]" \
                "[-m model] [-V] [-I rowid]" >&2; exit 2 ;;
    esac
done

# ======================================================================= #
#                              THE TABLE                                  #
# ======================================================================= #
#
#   boot | command | arm | runs | premise | prep | command line
#
# premise:  rc=N        the command must have exited N
#           rc!=N       ... must not have exited N
#           re:ERE      ERE must appear in the command's own output
#           ?           nothing stated; the row FAILS.  There is no way to
#                       write a row that passes without saying what it reached.
#
# prep:     `-`, or a command line run before every repetition.  A pair whose
#           two halves undo each other is measured as a pair: if the prep
#           leaks, BOTH rows of the pair go red, which is the truth about a
#           pair and not a defect in the arithmetic.
#
# runs:     at least 4, and 5 unless every run costs seconds.  The first pays
#           the one-off cost; the rest have to be flat.  Four is the floor
#           because the arithmetic reads a two-run window at each end.
#
# The `?` arm exists for every command in the tree.  It is generated rather
# than written out: see template_rows().  Its premise is a distinctive piece
# of that command's own ReadArgs template, read out of the source, so a
# command that stops printing its template cannot pass the row.

table_static() {
    # A table of one's own, for probing a single command without editing this
    # file.  Same seven columns; run-nettools.sh takes its command list the
    # same way.
    if [ -n "${AMINETXDUO_TOOLLEAK_TABLE:-}" ]; then
        eval "cat <<EOF
$(cat "$AMINETXDUO_TOOLLEAK_TABLE")
EOF"
        return
    fi
    cat <<EOF
# ------------------------------------------------------------ cold ------
# The stack was NEVER started.  Nothing here opens bsdsocket.library, which is
# what would start it, so every row is the "the network is not running" arm.
cold|CheckNetConfig|reads-config|5|re:nothing wrong with it|-|SYS:CheckNetConfig
cold|CheckNetConfig|verbose|5|re:Reading the configuration|-|SYS:CheckNetConfig VERBOSE
cold|GetNetStatus|no-stack|5|re:The network is not running|-|SYS:GetNetStatus
cold|ShowNetStatus|no-stack|5|re:Network stack: +not started|-|SYS:ShowNetStatus
cold|netstat|no-stack|5|re:netstat: network not started|-|SYS:netstat -i
cold|arp|no-stack|5|re:arp: network not started|-|SYS:arp
cold|AddNetRoute|no-stack|5|re:not running, so it has no routes|-|SYS:AddNetRoute DST=192.0.2.0 VIA=10.0.2.2
cold|DeleteNetRoute|no-stack|5|re:not running, so it has no routes|-|SYS:DeleteNetRoute DST=198.51.100.0
cold|NetShutdown|no-stack|5|re:nothing to stop|-|SYS:NetShutdown
cold|RemoveNetInterface|unknown-name|5|re:nothing to remove|-|SYS:RemoveNetInterface nosuchif
cold|ConfigureNetInterface|no-stack|5|re:no interface to configure|-|SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.20
cold|ConfigureNetInterface|bad-address|5|re:is not an address|-|SYS:ConfigureNetInterface eth0 ADDRESS notanaddress
cold|ConfigureNetInterface|bad-configure|5|re:CONFIGURE takes DHCP and nothing else|-|SYS:ConfigureNetInterface eth0 CONFIGURE=AUTO
cold|ConfigureNetInterface|bad-mdns|5|re:MDNS is YES or NO|-|SYS:ConfigureNetInterface eth0 MDNS=MAYBE
cold|AddNetInterface|unknown-name|5|re:there is no interface called|-|SYS:AddNetInterface nosuchinterface
# hostname reads the whole configuration into a static AmiConfig to answer with
# the stack down, and ami_config_load() loads the netdb behind it, which is
# where addnetroute.c found 12,616 bytes a run going missing.  The refusal arm
# is the one that returns before any of that, so the pair brackets it.
cold|hostname|no-name|5|re:This machine has no name|-|SYS:hostname
cold|hostname|bad-name|5|re:is not a host name|-|SYS:hostname not_a_name
cold|NetSetup|bad-address|5|re:is not an address|-|SYS:NetSetup eth9 DEVICE=a2065.device UNIT=0 ADDRESS=notanaddress NOONLINE
cold|NetSetup|writes-config|5|re:set up a network interface|-|SYS:NetSetup ethz DEVICE=a2065.device UNIT=0 DHCP NOONLINE FORCE
cold|NetTrace|bad-snap|5|re:SNAP must be between 14 and 65535 bytes|-|SYS:NetTrace SNAP=-1 NOCAPTURE
cold|NetTrace|bad-blen|5|re:BLEN must be between 32 and 32768 bytes|-|SYS:NetTrace BLEN=-1 NOCAPTURE
cold|NetTrace|short-buffer|5|re:BLEN 64 is too small for a snap length of 96|-|SYS:NetTrace BLEN=64 NOCAPTURE
cold|NetTrace|bad-bytes|5|re:BYTES must be at least 1|-|SYS:NetTrace BYTES=-1 NOCAPTURE
cold|NetTrace|bad-port|5|re:-1 is not a port|-|SYS:NetTrace WIRE HOST 10.0.2.2 PORT=-1 NOCAPTURE
# -4 and -6 together, for every command that carries the pair.  A cold row
# because each one refuses BEFORE tool_socket_open(), which is the whole point
# of parsing the pair where it is parsed: an argument error must not start the
# network to say so.  Any of these appearing in the `live` transcript instead
# would mean the check moved below the open.
#
# Each carries a short deadline it never reaches, because a regression here
# would not refuse: it would open bsdsocket.library, start the stack this boot
# is supposed to be without, and then run the command for its default lifetime
# -- thirty hops of traceroute, five TFTP retries -- five times over.  The row
# has to come back red, and a boot that burnt its ceiling is not red, it is
# infrastructure.
cold|ping|family-both|5|re:-4 and -6 cannot both be given|-|SYS:ping 10.0.2.2 -c 1 -t 3 -4 -6
cold|ping|negative-number|5|re:cannot be negative|-|SYS:ping 10.0.2.2 LOAD=-1
cold|traceroute|family-both|5|re:-4 and -6 cannot both be given|-|SYS:traceroute 10.0.2.2 -m 1 -q 1 -w 2 -n -4 -6
cold|traceroute|negative-number|5|re:cannot be negative|-|SYS:traceroute 10.0.2.2 QUERIES=-1
cold|fetch|family-both|5|re:-4 and -6 cannot both be given|-|SYS:fetch http://10.0.2.2/ TIMEOUT 5 -4 -6
cold|fetch|bad-timeout|5|re:TIMEOUT must be between 0 and|-|SYS:fetch http://10.0.2.2/ TIMEOUT=-1
cold|telnet|family-both|5|re:-4 and -6 cannot both be given|-|SYS:telnet 10.0.2.2 23 -4 -6
cold|tftp|family-both|5|re:-4 and -6 cannot both be given|-|SYS:tftp 10.0.2.2 GET x TIMEOUT 1 -4 -6
cold|whois|family-both|5|re:-4 and -6 cannot both be given|-|SYS:whois plain.test -4 -6
cold|whois|bad-port|5|re:-1 is not a port|-|SYS:whois plain.test PORT=-1
cold|sntp|family-both|5|re:-4 and -6 cannot both be given|-|SYS:sntp 10.0.2.2 TIMEOUT 2 -4 -6
cold|host|family-both|5|re:-4 and -6 cannot both be given|-|SYS:host v4only.test -4 -6
cold|nc|family-both|5|re:-4 and -6 cannot both be given|-|SYS:nc -z 10.0.2.2 7301 -w 3 -4 -6
cold|nc|bad-timeout|5|re:a timeout cannot be negative|-|SYS:nc -l 7099 TIMEOUT=-1
cold|nc|bad-local-port|5|re:-1 is not a local port|-|SYS:nc -l LOCALPORT=-1
cold|iperf|family-both|5|re:-4 and -6 cannot both be given|-|SYS:iperf 10.0.2.2 -p 7385 -t 2 -q -4 -6
# ------------------------------------------------------------ live ------
live|CheckNetConfig|stack-up|5|re:nothing wrong with it|-|SYS:CheckNetConfig
live|GetNetStatus|stack-up|5|re:The network is running|-|SYS:GetNetStatus
live|ShowNetStatus|all|5|re:Network stack: +running|-|SYS:ShowNetStatus ALL
live|netstat|all|5|re:Active connections|-|SYS:netstat -a
live|netstat|routes|5|re:Routing table|-|SYS:netstat -r
live|arp|cache|5|re:Hardware address|-|SYS:arp
live|arp|delete-miss|5|re:was not removed from the cache|-|SYS:arp 192.0.2.99 DELETE
live|ping|reply|5|re:0% packet loss|-|SYS:ping 10.0.2.2 -c 1 -t 5
live|ping|no-reply|4|re:100% packet loss|-|SYS:ping 192.0.2.1 -c 1 -t 3
live|host|resolves|5|re:has address|-|SYS:host www.example.com
live|host|nxdomain|5|re:cannot resolve|-|SYS:host no.such.host.invalid TIMEOUT 5
live|nslookup|resolves|5|re:www.example.com from|-|SYS:nslookup www.example.com TYPE=A TIMEOUT 5
live|nslookup|nxdomain|5|re:there is no such name|-|SYS:nslookup no.such.host.invalid TIMEOUT 5
live|fetch|http-200|5|re:HTTP/1.0 200 OK|-|SYS:fetch http://10.0.2.2:${HTTP_PORT}/hello.txt TO DH0:fetched.bin TIMEOUT 10
live|fetch|unresolvable|5|re:cannot resolve|-|SYS:fetch http://no.such.host.invalid/ TIMEOUT 5
live|fetch|refused|5|re:cannot connect to 10.0.2.2 port ${DEAD_PORT}|-|SYS:fetch http://10.0.2.2:${DEAD_PORT}/x TIMEOUT 5
live|nc|scan-open|5|re:port ${ECHO_PORT} open|-|SYS:nc -z 10.0.2.2 ${ECHO_PORT} -v -w 5
live|nc|scan-closed|5|re:port ${DEAD_PORT} connection refused|-|SYS:nc -z 10.0.2.2 ${DEAD_PORT} -v -w 5
live|telnet|session|5|re:Trying 10.0.2.2 port ${TELNET_PORT}|-|SYS:telnet 10.0.2.2 ${TELNET_PORT} -d <DH0:telnetin.txt
live|telnet|refused|5|re:Trying 10.0.2.2 port ${DEAD_PORT}|-|SYS:telnet 10.0.2.2 ${DEAD_PORT}
live|tftp|get|5|re:49 bytes|-|SYS:tftp 10.0.2.2 PORT ${TFTP_PORT} GET hello.txt AS DH0:tftp.txt
live|iperf|measures|5|re:dir=tcp-tx|-|SYS:iperf 10.0.2.2 -p ${IPERF_PORT} -n 32 -q
live|iperf|refused|5|re:nothing is listening|-|SYS:iperf 10.0.2.2 -p ${DEAD_PORT} -t 3 -q
live|tftp|not-found|5|re:there is no such file on the server|-|SYS:tftp 10.0.2.2 PORT ${TFTP_PORT} GET no.such.file
live|whois|answers|5|re:PLAIN.TEST|-|SYS:whois plain.test SERVER 10.0.2.2 PORT ${WHOIS_PORT}
live|whois|refused|5|re:cannot reach 10.0.2.2 port ${DEAD_PORT}|-|SYS:whois plain.test SERVER 10.0.2.2 PORT ${DEAD_PORT}
live|traceroute|reaches|4|re:hops max|-|SYS:traceroute 10.0.2.2 -m 2 -q 1 -w 3 -n
live|traceroute|unresolvable|5|re:cannot resolve|-|SYS:traceroute no.such.host.invalid -m 1 -q 1 -w 2 -n
live|sntp|no-server|4|re:the time server did not answer|-|SYS:sntp 10.0.2.2 TIMEOUT 3
live|sntp|shows-time|4|re:stratum|-|SYS:sntp pool.ntp.org SHOW TIMEOUT 5
# ---------------------------------------------------------- -4 and -6 ---
# Three of the four arms.  The fourth -- -6 reaching a host that really has an
# AAAA -- cannot be here: SLIRP carries no IPv6, so it is asserted on the
# bridged guest by tests/tools/run-family.sh, and a row that pretended to do it
# from here would be measuring the failure path and calling it the success one.
#
# The -6 rows are the leak arm that matters: a lookup that comes back empty
# under a pinned family makes a SECOND getaddrinfo() to find out whether the
# name exists at all (tool_sock_family_absent), so these are the only rows in
# the table where one invocation allocates and frees two addrinfo lists.
live|ping|family-4|5|re:0% packet loss|-|SYS:ping 10.0.2.2 -c 1 -t 5 -4
live|ping|family-6-no-aaaa|5|re:v4only.test has no IPv6 address|-|SYS:ping v4only.test -c 1 -t 5 -6
live|ping|family-6-v4-literal|5|re:10.0.2.2 is an IPv4 address|-|SYS:ping 10.0.2.2 -c 1 -t 5 -6
live|host|family-4|5|re:v4only.test has address 10.0.2.2|-|SYS:host v4only.test -4
live|host|family-6-no-aaaa|5|re:v4only.test has no IPv6 address|-|SYS:host v4only.test -6
live|host|family-6-v4-literal|5|re:10.0.2.2 is an IPv4 address|-|SYS:host 10.0.2.2 -6
live|fetch|family-4|5|re:HTTP/1.0 200 OK|-|SYS:fetch http://10.0.2.2:${HTTP_PORT}/hello.txt TO DH0:fetch4.bin TIMEOUT 10 -4
live|fetch|family-6-no-aaaa|5|re:v4only.test has no IPv6 address|-|SYS:fetch http://v4only.test/ TIMEOUT 5 -6
live|telnet|family-4|5|re:Trying 10.0.2.2 port ${TELNET_PORT}|-|SYS:telnet 10.0.2.2 ${TELNET_PORT} -d -4 <DH0:telnetin.txt
live|telnet|family-6-no-aaaa|5|re:v4only.test has no IPv6 address|-|SYS:telnet v4only.test ${TELNET_PORT} -6
live|tftp|family-4|5|re:49 bytes|-|SYS:tftp 10.0.2.2 PORT ${TFTP_PORT} GET hello.txt AS DH0:tftp4.txt -4
live|tftp|family-6-no-aaaa|5|re:v4only.test has no IPv6 address|-|SYS:tftp v4only.test PORT ${TFTP_PORT} GET hello.txt AS DH0:tftp6.txt -6
live|whois|family-4|5|re:PLAIN.TEST|-|SYS:whois plain.test SERVER 10.0.2.2 PORT ${WHOIS_PORT} -4
live|whois|family-6-no-aaaa|5|re:v4only.test has no IPv6 address|-|SYS:whois plain.test SERVER v4only.test PORT ${WHOIS_PORT} -6
live|traceroute|family-4|4|re:hops max|-|SYS:traceroute 10.0.2.2 -m 2 -q 1 -w 3 -n -4
live|traceroute|family-6-no-aaaa|5|re:v4only.test has no IPv6 address|-|SYS:traceroute v4only.test -m 1 -q 1 -w 2 -n -6
live|sntp|family-4|4|re:the time server did not answer|-|SYS:sntp 10.0.2.2 TIMEOUT 3 -4
live|sntp|family-6-no-aaaa|5|re:v4only.test has no IPv6 address|-|SYS:sntp v4only.test TIMEOUT 3 -6
live|nc|family-4|5|re:port ${ECHO_PORT} open|-|SYS:nc -z 10.0.2.2 ${ECHO_PORT} -v -w 5 -4
live|nc|family-6-no-aaaa|5|re:v4only.test has no IPv6 address|-|SYS:nc -z v4only.test ${ECHO_PORT} -v -w 5 -6
live|iperf|family-4|5|re:dir=tcp-tx|-|SYS:iperf 10.0.2.2 -p ${IPERF_PORT} -n 32 -q -4
live|iperf|family-6-no-aaaa|5|re:v4only.test has no IPv6 address|-|SYS:iperf v4only.test -p ${IPERF_PORT} -t 3 -q -6
live|NetTrace|loopback|4|re:records written|-|SYS:NetTrace LOOPBACK BYTES 4096 OUT DH0:nettrace.pcap
live|NetTrace|wire|4|re:records written|-|SYS:NetTrace WIRE HOST 10.0.2.2 PORT ${HTTP_PORT} PATH /hello.txt OUT DH0:ntwire.pcap
live|NetTrace|unresolvable|5|re:cannot resolve|-|SYS:NetTrace WIRE HOST no.such.host.invalid
live|ShowNetServices|mdns-off|4|re:mDNS is not enabled|-|SYS:ShowNetServices SECONDS=1
live|ShowNetServices|bad-type|5|re:is not a service type|-|SYS:ShowNetServices http
live|httpd|no-root|5|re:required argument missing|-|SYS:httpd
live|httpd|bad-root|5|re:there is no .DH0:nosuchdirectory. to serve|-|SYS:httpd DH0:nosuchdirectory 8099
live|httpd|term-no-page|5|re:to serve the terminal from|-|SYS:httpd DH0: 8099 -T PAGE=DH0:nosuchpage.html
live|httpd|term-page-checked|5|re:there is no .DH0:nosuchdirectory. to serve|-|SYS:httpd DH0:nosuchdirectory 8099 -T PAGE=DH0:shell.html
# Bare -T, both ways.
#
# FOUND: httpd is at SYS:, so PROGDIR: is the stage root and the page staged
# beside it in Terminal/ is the second place the search looks.  The run then
# has to end, and a server that started would not, so -a names something that
# does not resolve: the address is resolved AFTER the page is, which makes the
# "Terminal page:" line the proof that the search ran and found it.
#
# NOT FOUND: the same binary from a drawer with no Terminal beside it and no
# AmiNetXDuo: assign anywhere, which is every place the search looks.  The
# premise is that the refusal NAMES them, because a -T that quietly serves no
# terminal is worse than one that will not start.
live|httpd|term-found|5|re:Terminal page: PROGDIR:Terminal/shell.html|-|SYS:httpd DH0: 8099 -T -a no.such.host.invalid
live|httpd|term-not-found|5|re:Looked for:|-|SYS:nopage/httpd DH0: 8099 -T
live|httpd|term-not-found-names-them|5|re:AmiNetXDuo:Terminal/shell.html|-|SYS:nopage/httpd DH0: 8099 -T
live|httpd|page-without-t|5|re:-T is what turns the terminal on|-|SYS:httpd DH0: 8099 PAGE=DH0:shell.html
live|AddNetRoute|no-gateway|5|re:no GATEWAY was given|-|SYS:AddNetRoute DST=192.0.2.0
live|DeleteNetRoute|no-such-route|5|re:no route to 198.51.100.0|-|SYS:DeleteNetRoute DST=198.51.100.0
live|AddNetRoute|added|5|re:now go through 10.0.2.2|SYS:DeleteNetRoute DST=192.0.2.0|SYS:AddNetRoute DST=192.0.2.0 VIA=10.0.2.2
live|DeleteNetRoute|deleted|5|re:The route to 192.0.2.0/24 is gone|SYS:AddNetRoute DST=192.0.2.0 VIA=10.0.2.2|SYS:DeleteNetRoute DST=192.0.2.0
live|RemoveNetInterface|unknown-name|5|re:there is no interface called|-|SYS:RemoveNetInterface nosuchif
# Both arms of ConfigureNetInterface against a running stack.  The prep moves
# the address away and the MEASURED call puts it back, that way round on
# purpose: every row below this one expects eth0 on 10.0.2.15, and a pair whose
# last step left it somewhere else would fail them and not itself.
live|ConfigureNetInterface|unknown-name|5|re:there is no interface called|-|SYS:ConfigureNetInterface nosuchif ADDRESS 10.0.2.20
live|ConfigureNetInterface|reconfigures|5|re:eth0: 10.0.2.15 netmask 255.255.255.0|SYS:ConfigureNetInterface eth0 QUIET ADDRESS 10.0.2.20/24|SYS:ConfigureNetInterface eth0 ADDRESS 10.0.2.15/24
# The mDNS pair.  The first repetition of the ON row pays for the responder --
# a 4 KB thread stack and the module, which are created the first time any
# interface asks and kept afterwards -- and the flat runs after it are the
# claim: an off/on cycle costs nothing, and in particular does not register the
# services a second time.  eth0's file says nothing about MDNS, so the pair is
# ordered to leave it off, which is where the boot left it.
live|ConfigureNetInterface|mdns-on|5|re:eth0: answering .local here|SYS:ConfigureNetInterface eth0 QUIET MDNS=NO|SYS:ConfigureNetInterface eth0 MDNS=YES
live|ConfigureNetInterface|mdns-off|5|re:eth0: no longer answering .local here|SYS:ConfigureNetInterface eth0 QUIET MDNS=YES|SYS:ConfigureNetInterface eth0 MDNS=NO
live|AddNetInterface|already-up|5|re:online, address 10.0.2.15|-|SYS:AddNetInterface eth0
live|AddNetInterface|unknown-name|5|re:there is no interface called|-|SYS:AddNetInterface nosuchinterface
# Setting a name on a RUNNING stack: the ENV: and ENVARC: writes and the
# NETCTRL_HOSTNAME_SET call, which is the arm that has something to give back.
# Repeating it is not a no-op -- the offer is at ENV:HOSTNAME rank and the
# machine is already named at that rank, so every run takes the name again.
live|hostname|sets|5|re:written to ENV:HOSTNAME|-|SYS:hostname beast
live|Online|unknown-name|5|re:nothing here is called|-|SYS:Online nosuch0
live|Offline|unknown-name|5|re:nothing here is called|-|SYS:Offline nosuch0
# ------------------------------------------------------------ cycle -----
# The commands that take the stack apart, each paired with the one that puts
# it back.  Fewer runs: every repetition here costs a DHCP lease.
# The DHCP half of ConfigureNetInterface.  Both rows are prepped into a bound
# interface, so the measured call is always the same operation on every
# repetition -- CONFIGURE=DHCP on an interface with no lease says "lease taken"
# and on one that has a lease says "lease renewed", and a premise that has to
# hold on every run cannot straddle the two.  The pair ends bound, which is
# what the rows below expect.
cycle|ConfigureNetInterface|dhcp-release|4|re:the lease is released|SYS:ConfigureNetInterface eth0 QUIET CONFIGURE=DHCP TIMEOUT 20|SYS:ConfigureNetInterface eth0 RELEASE
cycle|ConfigureNetInterface|dhcp-renew|4|re:lease renewed|SYS:ConfigureNetInterface eth0 QUIET CONFIGURE=DHCP TIMEOUT 20|SYS:ConfigureNetInterface eth0 CONFIGURE=DHCP TIMEOUT 20
cycle|Offline|takes-down|4|re:eth0 is offline|SYS:Online eth0|SYS:Offline eth0
cycle|Online|brings-up|4|re:eth0 is.*online|SYS:Offline eth0|SYS:Online eth0
cycle|RemoveNetInterface|removes|4|re:eth0: removed|SYS:AddNetInterface eth0|SYS:RemoveNetInterface eth0 FORCE
cycle|AddNetInterface|adds|4|re:online, address 10.0.2.15|SYS:RemoveNetInterface eth0 FORCE|SYS:AddNetInterface eth0
cycle|NetShutdown|stops-all|4|re:eth0: stopped|SYS:Online eth0|SYS:NetShutdown
# ------------------------------------------------------------ server ----
# nc as a listener.  It blocks until its own -w expires, so it gets a boot of
# its own rather than sitting in the middle of somebody else's transcript.
server|nc|listen-timeout|4|re:nobody connected within 3 seconds|-|SYS:nc -l 7099 -v -w 3
# ---------------------------------------------------- and what cannot -----
# A row that could not be measured says so.  It is neither a pass nor a
# failure, and it is here so that the absence is a line in the table rather
# than a command nobody noticed was missing.
live|httpd|serves|0|na:httpd runs until it is killed; a server with no request budget never reaches a second invocation in one transcript|-|SYS:httpd DH0: 8080
live|httpd|terminal-session|0|na:the terminal's success arm is the same server that cannot be measured above, plus a browser; what a session costs is measured in tests/tools/run-wsterm.sh, where the Shell is started and stopped repeatedly inside ONE httpd and the figure is httpd's own free memory, not a per-invocation one|-|SYS:httpd DH0: 8080 -T PAGE=DH0:shell.html
live|telnet|interactive|0|na:a terminal session ends when a person ends it; the scripted arm above is the measurable half|-|SYS:telnet <host>
live|ShowNetServices|browse|0|na:a browse that finds something needs MDNS=YES on the interface and a responder on the LAN; SLIRP carries neither|-|SYS:ShowNetServices SECONDS=1
EOF
}

# The commands, from src/tools/CMakeLists.txt rather than from a list somebody
# maintained by hand: a tool added there and not here would otherwise be swept
# by nothing at all.
tool_names() {
    sed -n 's/^aminetxduo_add_tool(\([A-Za-z_0-9]*\)[[:space:]]\{1,\}\([A-Za-z_0-9]*\).*/\2/p' \
        "$ROOT/src/tools/CMakeLists.txt" | sort -u
}

# `<command> ?`: ReadArgs prints the template and reads EOF from NIL:.  The
# premise is a piece of the command's OWN template, taken out of the source it
# was compiled from, so the row cannot pass on a command that printed nothing.
template_premise() {
    local tool="$1" src pat
    src=$(grep -l "TOOL_VERSTAG(\"$tool\")" "$ROOT"/src/tools/*.c 2>/dev/null | head -1)
    [ -n "$src" ] || { echo '?'; return; }
    # The first quoted run of template text on the #define TEMPLATE line(s).
    pat=$(sed -n '/^#define TEMPLATE/,/^$/p' "$src" |
          tr -d '\\\n' | sed -n 's/.*"\([^"]*\)".*/\1/p' | head -1)
    # Anything with a shell or ERE metacharacter in it is no use as a pattern,
    # and `|` cannot travel through this table at all.  Take the first field.
    pat=${pat%%,*}
    case "$pat" in
        ''|*'|'*|*'('*) echo '?' ;;
        *) echo "re:$pat" ;;
    esac
}

template_rows() {
    local tool premise
    for tool in $(tool_names); do
        premise=$(template_premise "$tool")
        echo "cold|$tool|template|5|$premise|-|SYS:$tool ?"
    done
}

table() {
    table_static | grep -v '^#'
    template_rows
}

# ======================================================================= #
#                            the verdict half                             #
# ======================================================================= #

WORK="$ROOT/build/toolleak"
FAILED=0
INFRA=0
REPORT_ROWS=""

note_fail()  { echo "FAIL: $*" >&2; FAILED=$((FAILED + 1)); }
note_infra() { echo "INFRA: $*" >&2; INFRA=$((INFRA + 1)); }

# Split a ToolsSmoke transcript into one file per command, and print an index
# of `idx|banner|kind|rc|free`.  Written against the format toolssmoke.c
# actually prints -- `----- rc N, M ms, free K -----` -- and a line that does
# not match becomes `badformat` rather than vanishing, because a pattern that
# silently matches nothing is how tests/tools/run-addifleak.sh spent months
# measuring an empty array.
split_transcript() {
    local transcript="$1" dir="$2"
    rm -rf "$dir"; mkdir -p "$dir"
    awk -v dir="$dir" '
        /^===== .+ =====$/ {
            if (rec != "") close(rec)
            idx++
            banner = $0
            sub(/^===== /, "", banner)
            sub(/ =====$/, "", banner)
            rec = sprintf("%s/rec-%04d.txt", dir, idx)
            printf "" > rec
            next
        }
        idx == 0 { next }
        { print $0 >> rec }
        /^----- rc / {
            if (match($0, /rc -?[0-9]+, [0-9]+ ms, free [0-9]+/)) {
                s = substr($0, RSTART, RLENGTH)
                gsub(/[^0-9-]/, " ", s)
                split(s, f, " ")
                printf "%d|%s|ok|%s|%s|%s\n", idx, banner, f[1], f[3], f[2]
            } else {
                printf "%d|%s|badformat|0|0|0\n", idx, banner
            }
        }
        /^----- could not run/          { printf "%d|%s|norun|0|0|0\n", idx, banner }
        /^----- started in the back/    { printf "%d|%s|async|0|0|0\n", idx, banner }
        /^----- waited/                 { printf "%d|%s|wait|0|0\n",  idx, banner }
        END { if (rec != "") close(rec) }
    ' "$transcript"
}

# premise_holds PREMISE RC RECFILE
premise_holds() {
    local premise="$1" rc="$2" rec="$3"
    case "$premise" in
        '?')      return 2 ;;
        rc=*)     [ "$rc" = "${premise#rc=}" ] ;;
        'rc!='*)  [ "$rc" != "${premise#rc!=}" ] ;;
        re:*)     grep -Eq -- "${premise#re:}" "$rec" ;;
        *)        return 2 ;;
    esac
}

# ======================================================================= #
#                               staging                                   #
# ======================================================================= #

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

require_binaries() {
    local t missing=0
    [ -f "$TOOLS/ToolsSmoke" ] || { echo "missing $TOOLS/ToolsSmoke" >&2; missing=1; }
    [ -f "$BSD" ] || { echo "missing $BSD" >&2; missing=1; }
    for t in $(tool_names); do
        [ -f "$TOOLS/$t" ] || { echo "missing $TOOLS/$t" >&2; missing=1; }
    done
    [ "$missing" = 0 ] || { echo "build $BUILD first" >&2; exit 2; }
}

find_a2065() {
    local candidate
    if [ -n "${AMINETXDUO_A2065:-}" ] && [ -f "${AMINETXDUO_A2065}" ]; then
        echo "$AMINETXDUO_A2065"; return 0
    fi
    for candidate in \
        "$ROOT/build/a2065.device" \
        "$HOME/amiga-assets/devs/a2065.device" \
        "$HOME/amiga-os-src/os-source/other_networking/sana2/bin/devs/a2065.device"
    do
        [ -f "$candidate" ] && { echo "$candidate"; return 0; }
    done
    return 1
}

stage_group() {
    local stage="$1" t a2065
    a2065=$(find_a2065) || {
        echo "No a2065.device.  Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

    rm -rf "$stage"
    mkdir -p "$stage/libs"
    cp -R "$ROOT/tests/netstack/devs" "$stage/devs"
    cp "$a2065" "$stage/devs/a2065.device"
    cp "$BSD"   "$stage/libs/bsdsocket.library"
    for t in $(tool_names); do
        cp "$TOOLS/$t" "$stage/$t"
    done
    # A name with an A record and no AAAA anywhere, for the -6 arms.  The
    # resolver reads DEVS:Internet/hosts on the IPv4 side and never on the
    # IPv6 side (src/netstack/netstack_dns.c:1051, the netdb schema has no
    # family), so this name resolves under -4, does not resolve under -6, and
    # naming which of those happened is what the -6 arms assert.  .test is
    # reserved (RFC 6761), so the AAAA query is an immediate NXDOMAIN rather
    # than a wait.
    printf '10.0.2.2 v4only.test\n' >> "$stage/devs/Internet/hosts"
    printf 'amiga\r\nquit\r\n' > "$stage/telnetin.txt"
    printf 'hello from the amiga\n' > "$stage/greeting.txt"
    # httpd's bare -T searches PROGDIR:Terminal/shell.html, and PROGDIR: for
    # SYS:httpd is the stage root.  A stub, not the real 400 KB page: nothing
    # here ever serves it, the search only opens it.  The second copy of httpd
    # is the other arm -- a drawer with no Terminal beside it, so the search
    # runs out and has to say where it looked.
    mkdir -p "$stage/Terminal" "$stage/nopage"
    printf '<html><body>stub</body></html>\n' > "$stage/Terminal/shell.html"
    cp "$TOOLS/httpd" "$stage/nopage/httpd"
}

# ======================================================================= #
#                             host-side peers                             #
# ======================================================================= #

PEER_PID=""
HTTP_PID=""
IPERF_PID=""
HTTPROOT="$WORK/httproot"

# Reached through the EXIT trap below, not by name.
# shellcheck disable=SC2329
stop_peers() {
    [ -z "$PEER_PID" ] || kill -TERM "$PEER_PID" 2>/dev/null || true
    [ -z "$HTTP_PID" ] || kill -TERM "$HTTP_PID" 2>/dev/null || true
    [ -z "$IPERF_PID" ] || kill -TERM "$IPERF_PID" 2>/dev/null || true
    PEER_PID=""
    HTTP_PID=""
    IPERF_PID=""
}
trap stop_peers EXIT INT TERM HUP

start_peers() {
    local lifetime="$1"
    mkdir -p "$HTTPROOT"
    printf 'hello from the host\n' > "$HTTPROOT/hello.txt"

    python3 "$ROOT/tests/tools/netpeer.py" \
        --echo-port "$ECHO_PORT" --telnet-port "$TELNET_PORT" \
        --tftp-port "$TFTP_PORT" --whois-port "$WHOIS_PORT" \
        --advertise 10.0.2.2 --log "$WORK/netpeer.log" \
        --seconds "$lifetime" > "$WORK/netpeer.out" 2>&1 &
    PEER_PID=$!

    ( cd "$HTTPROOT" && exec python3 -m http.server "$HTTP_PORT" --bind 0.0.0.0 ) \
        > "$WORK/httpd-host.log" 2>&1 &
    HTTP_PID=$!

    # --keep, because the sweep runs each row several times against one peer
    # and a peer that served once would make every repetition after the first
    # fail as "connection refused" and blame the tool for it.
    python3 "$ROOT/tests/tools/iperfpeer.py" serve tcp \
        --port "$IPERF_PORT" --seconds "$lifetime" --idle 6 --keep \
        > "$WORK/iperfpeer.out" 2>&1 &
    IPERF_PID=$!

    sleep 1
    kill -0 "$PEER_PID" 2>/dev/null || {
        echo "netpeer.py did not start:" >&2; cat "$WORK/netpeer.out" >&2; exit 2; }
    kill -0 "$HTTP_PID" 2>/dev/null || {
        echo "python3 -m http.server did not start:" >&2
        cat "$WORK/httpd-host.log" >&2; exit 2; }
    kill -0 "$IPERF_PID" 2>/dev/null || {
        echo "iperfpeer.py did not start:" >&2
        cat "$WORK/iperfpeer.out" >&2; exit 2; }
    echo "==> peers: echo $ECHO_PORT, telnet $TELNET_PORT, tftp $TFTP_PORT," \
         "whois $WHOIS_PORT, http $HTTP_PORT, iperf $IPERF_PORT"
}

# ======================================================================= #
#                                chunking                                 #
# ======================================================================= #

# ToolsSmoke stops reading at MAX_COMMANDS and says nothing, so a list past it
# is a list whose tail was never run.  Read the ceiling out of the source.
smoke_max() {
    local n
    n=$(sed -n 's/^#define MAX_COMMANDS[[:space:]]*\([0-9]*\).*/\1/p' \
        "$ROOT/src/tools/toolssmoke.c" | head -1)
    [ -n "$n" ] || {
        echo "cannot read MAX_COMMANDS out of src/tools/toolssmoke.c" >&2
        exit 2; }
    echo "$n"
}

group_prologue() {
    case "$1" in
        live|cycle|server) echo "SYS:AddNetInterface eth0" ;;
        *) ;;
    esac
}

# ======================================================================= #
#                                the run                                  #
# ======================================================================= #

mkdir -p "$WORK"

SMOKE_MAX=$(smoke_max)
echo "==> ToolsSmoke reads at most $SMOKE_MAX commands a boot"

if [ "$VERDICT_ONLY" = 0 ]; then
    require_binaries
fi

# The whole run's rows, one file, so the report can be assembled across boots.
ALLROWS="$WORK/rows.psv"
table > "$ALLROWS"

# A row is addressed by boot, command and arm, so two rows that share all
# three are one row as far as every lookup below is concerned, and the second
# would silently take the first's premise.  That happened.
DUPES=$(awk -F'|' '{print $1 "|" $2 "|" $3}' "$ALLROWS" | sort | uniq -d)
if [ -n "$DUPES" ]; then
    echo "INFRA: two rows share a boot, a command and an arm:" >&2
    printf '  %s\n' "$DUPES" >&2
    exit 2
fi

TOTAL_ROWS=$(wc -l < "$ALLROWS" | tr -d ' ')
echo "==> $TOTAL_ROWS rows over $(tool_names | wc -l | tr -d ' ') commands"

run_group() {
    local group="$1"
    local stage="$WORK/stage-$group"
    local rows="$WORK/rows-$group.psv"
    local prologue chunk cost limit
    local cmdfile expectfile

    # One report file per group: two groups swept at once must not write over
    # each other, and the combined table is assembled from all of them.
    REPORT_ROWS="$WORK/report-$group.psv"
    : > "$REPORT_ROWS"

    awk -F'|' -v g="$group" '$1 == g' "$ALLROWS" > "$rows"
    if [ ! -s "$rows" ]; then
        echo "==> $group: no rows"
        return 0
    fi

    prologue=$(group_prologue "$group")
    limit=$((SMOKE_MAX - 2))

    # --- pack the rows into as few boots as the driver's ceiling allows ---
    chunk=1
    cost=0
    cmdfile="$WORK/$group-1.commands.txt"
    expectfile="$WORK/$group-1.expect.psv"
    : > "$cmdfile"; : > "$expectfile"
    [ -z "$prologue" ] || printf '%s\n' "$prologue" >> "$cmdfile"
    [ -z "$prologue" ] || printf '0|-prologue-|prep|%s\n' "$prologue" >> "$expectfile"

    local boot command arm runs premise prep cmdline rowcost i
    while IFS='|' read -r boot command arm runs premise prep cmdline; do
        [ -n "$boot" ] || continue
        # A row that states it cannot be measured is reported, not run.
        if [ "${premise#na:}" != "$premise" ]; then
            printf '%s|%s/%s|not-measurable|-|-|-|%s\n' \
                "$group" "$command" "$arm" "${premise#na:}" >> "$REPORT_ROWS"
            continue
        fi
        [ "$runs" -ge 4 ] || {
            note_infra "$command/$arm asks for $runs runs; 4 is the floor"
            continue
        }
        rowcost=$runs
        [ "$prep" = "-" ] || rowcost=$((runs * 2))
        if [ $((cost + rowcost)) -gt "$limit" ]; then
            chunk=$((chunk + 1))
            cost=0
            cmdfile="$WORK/$group-$chunk.commands.txt"
            expectfile="$WORK/$group-$chunk.expect.psv"
            : > "$cmdfile"; : > "$expectfile"
            [ -z "$prologue" ] || printf '%s\n' "$prologue" >> "$cmdfile"
            [ -z "$prologue" ] || printf '0|-prologue-|prep|%s\n' "$prologue" >> "$expectfile"
        fi
        for ((i = 0; i < runs; i++)); do
            if [ "$prep" != "-" ]; then
                printf '%s\n' "$prep" >> "$cmdfile"
                printf '%s/%s|%s|prep|%s\n' "$command" "$arm" "$chunk" "$prep" >> "$expectfile"
            fi
            printf '%s\n' "$cmdline" >> "$cmdfile"
            printf '%s/%s|%s|measure|%s\n' "$command" "$arm" "$chunk" "$cmdline" >> "$expectfile"
        done
        cost=$((cost + rowcost))
    done < "$rows"

    local chunks="$chunk"
    echo "==> $group: $(wc -l < "$rows" | tr -d ' ') rows in $chunks boot(s)"

    # --------------- boot each chunk, and read it back before the next -----
    #
    # Read back immediately rather than after the last boot: the milliseconds
    # this chunk measured are what sizes the NEXT chunk's ceiling, and a group
    # that boots three times would otherwise guess three times over.
    local c tag hd rc
    for ((c = 1; c <= chunks; c++)); do
        tag="toolleak-$group-$c"
        hd="$ROOT/build/amiberry-testhd-$tag"
        if [ "$VERDICT_ONLY" = 0 ]; then
            stage_group "$stage"
            cp "$WORK/$group-$c.commands.txt" "$stage/commands.txt"

            local extras=()
            while IFS= read -r e; do extras+=("$e"); done \
                < <(find "$stage" -mindepth 1 -maxdepth 1)

            local t="$TIMEOUT"
            [ "$t" != 0 ] || t=$(chunk_timeout "$WORK/$group-$c.commands.txt")

            echo "==> $tag: $(wc -l < "$stage/commands.txt" | tr -d ' ') commands," \
                 "ceiling ${t}s"
            AMINETXDUO_RUN_TAG="$tag" \
            "$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$t" \
                "$TOOLS/ToolsSmoke" "${extras[@]}" \
                > "$WORK/$tag.out" 2>&1 && rc=0 || rc=$?
            echo "    emulator exit $rc, $(wc -l < "$WORK/$tag.out" | tr -d ' ') lines of log"
            printf '%s\n' "$rc" > "$WORK/$tag.rc"
        else
            echo "==> $tag: reusing $hd/tools.txt"
        fi
        rc=$(cat "$WORK/$tag.rc" 2>/dev/null || echo "?")
        read_chunk "$group" "$c" "$tag" "$hd" "$rc" || true
    done
}

# A ceiling taken from what the list ACTUALLY COST last time, not from a round
# number.  ToolsSmoke prints the milliseconds every command took, so the good
# case is an artifact rather than an estimate: build/toolleak/costs.psv carries
# the worst time each command line has ever taken on this machine, and the
# ceiling is a boot plus twice the sum.
#
# A line nobody has timed yet falls back to the arithmetic below, which reads
# the command's own TIMEOUT out of it.  The first run of a new row is
# therefore the only one working from a guess, and it writes down what it
# learned.
COSTS="$WORK/costs.psv"

chunk_timeout() {
    local file="$1" line
    # One pass: measured milliseconds for the lines that have been timed here
    # before, and the command's own TIMEOUT for the ones that have not.  The
    # estimate covers ONLY the unknown lines, so a chunk in which one command
    # line changed does not fall back to estimating all ninety-five.
    line=$(awk -F'\t' -v have="$([ -s "$COSTS" ] && echo 1 || echo 0)" '
        NR == FNR && have == 1 { cost[$1] = $2; next }
        {
            if ($0 in cost) { known++; ms += cost[$0]; next }
            unknown++
            t = 1
            if (match($0, /-w [0-9]+/))         { t = substr($0, RSTART + 3, RLENGTH - 3) + 1 }
            if (match($0, /TIMEOUT[= ][0-9]+/)) { t = substr($0, RSTART + 8, RLENGTH - 8) + 1 }
            if (match($0, /-t [0-9]+/))         { t = substr($0, RSTART + 3, RLENGTH - 3) + 1 }
            if ($0 ~ /AddNetInterface eth0/)    { t = 12 }
            if ($0 ~ /NetTrace/)                { t = 8 }
            if ($0 ~ /SECONDS=/)                { t = 3 }
            est += t
        }
        END { printf "%d %d %d %d\n", (ms + 999) / 1000, est + 0, known + 0, unknown + 0 }' \
        "$COSTS" "$file" 2>/dev/null || echo "0 0 0 0")

    local measured estimated known unknown
    read -r measured estimated known unknown <<< "$line"

    # A boot of its own, then twice the work: an emulator that costs more than
    # double what it has already been seen to cost is hung, and the ceiling is
    # what says so rather than something to raise.
    echo "==> good case: ${measured}s measured over $known command(s)," \
         "${estimated}s estimated over $unknown never timed here" >&2
    echo $((60 + (measured + estimated) * 2))
}

# Every command line's worst measured time, so the next ceiling is an artifact
# and not a guess.  Worst rather than mean: a ceiling sized to the average is
# one the slowest run walks straight through.
record_costs() {
    local index="$1"
    [ -s "$index" ] || return 0
    { [ ! -f "$COSTS" ] || cat "$COSTS"
      awk -F'|' -v OFS='\t' '$3 == "ok" { print $2, $6 }' "$index"
    } | awk -F'\t' -v OFS='\t' '
        { if (!($1 in worst) || $2 + 0 > worst[$1]) worst[$1] = $2 + 0 }
        END { for (c in worst) print c, worst[c] }' > "$COSTS.tmp"
    mv -f "$COSTS.tmp" "$COSTS"
}

read_chunk() {
    local group="$1" chunk="$2" tag="$3" hd="$4" runrc="$5"
    local transcript index dir
    transcript="$hd/tools.txt"

    verdict_find "$tag" "$runrc" '===== ' "$transcript" > /dev/null || {
        note_infra "$tag produced no transcript; nothing in this boot was measured"
        awk -F'|' -v c="$chunk" '$2 == c && $3 == "measure" {print $1}' \
            "$WORK/$group-$chunk.expect.psv" | sort -u |
            while read -r rid; do
                printf '%s|%s|no-transcript|0|0|0|-\n' "$group" "$rid" >> "$REPORT_ROWS"
            done
        return 1
    }

    dir="$WORK/$tag.rec"
    index="$WORK/$tag.index.psv"
    split_transcript "$transcript" "$dir" > "$index"
    record_costs "$index"
    : > "$WORK/$tag.readings.psv"

    # ---- alignment.  The transcript's Nth command must be the Nth command
    # the list asked for; anything else means the driver ran something other
    # than what was staged, and no arithmetic on it means anything.
    local expected="$WORK/$group-$chunk.expect.psv"
    local nexp nrec
    nexp=$(wc -l < "$expected" | tr -d ' ')
    nrec=$(wc -l < "$index" | tr -d ' ')

    local i=0 mismatch=0
    local rid _ck role want idx banner kind rc free ms
    exec 3< "$expected"
    exec 4< "$index"
    while IFS='|' read -r rid _ck role want <&3; do
        i=$((i + 1))
        if ! IFS='|' read -r idx banner kind rc free ms <&4; then
            note_infra "$tag: the transcript stops after $((i - 1)) of $nexp" \
                       "commands; the one it never finished is: $want"
            mismatch=1
            break
        fi
        if [ "$banner" != "$want" ]; then
            note_infra "$tag: command $i is '$banner', the list asked for '$want'"
            mismatch=1
            break
        fi
        case "$kind" in
            ok) ;;
            norun) note_infra "$tag: '$want' could not be started at all" ;;
            badformat)
                note_infra "$tag: '$want' reported in a format this script cannot" \
                           "read; toolssmoke.c's report line has changed" ;;
            *) note_infra "$tag: '$want' reported '$kind'" ;;
        esac
        [ "$role" = "measure" ] || continue
        printf '%s|%s|%s|%s|%s\n' "$rid" "$rc" "$free" \
            "$dir/rec-$(printf '%04d' "$idx").txt" "$ms" \
            >> "$WORK/$tag.readings.psv"
    done
    exec 3<&-
    exec 4<&-

    if [ "$mismatch" = 0 ] && [ "$nrec" -gt "$nexp" ]; then
        note_infra "$tag: $nrec commands ran and only $nexp were staged"
    fi

    verdict_chunk "$group" "$chunk" "$tag"
}

verdict_chunk() {
    local group="$1" chunk="$2" tag="$3"
    local readings="$WORK/$tag.readings.psv"
    local expected="$WORK/$group-$chunk.expect.psv"

    local rid
    while read -r rid; do
        verdict_row "$group" "$rid" "$readings"
    done < <(awk -F'|' -v c="$chunk" '$2 == c && $3 == "measure" {print $1}' \
             "$expected" | awk '!seen[$0]++')
}

verdict_row() {
    local group="$1" rid="$2" readings="$3"
    local want_runs premise cmdline
    want_runs=$(awk -F'|' -v g="$group" -v r="$rid" \
                    '$1 == g && $2 "/" $3 == r {print $4; exit}' "$ALLROWS")
    premise=$(awk -F'|' -v g="$group" -v r="$rid" \
                  '$1 == g && $2 "/" $3 == r {print $5; exit}' "$ALLROWS")
    cmdline=$(awk -F'|' -v g="$group" -v r="$rid" \
                  '$1 == g && $2 "/" $3 == r {print $7; exit}' "$ALLROWS")

    local frees=() rcs=() recs=()
    local f_rid rc free rec ms
    local mss=()
    while IFS='|' read -r f_rid rc free rec ms; do
        [ "$f_rid" = "$rid" ] || continue
        rcs+=("$rc"); frees+=("$free"); recs+=("$rec"); mss+=("$ms")
    done < "$readings"

    local slowest=0 m
    for m in "${mss[@]:-0}"; do [ "$m" -le "$slowest" ] || slowest=$m; done

    local n=${#frees[@]}
    if [ "$n" -ne "$want_runs" ]; then
        note_infra "$rid: $n of $want_runs runs reported free memory"
        printf '%s|%s|not-measured|0|0|0|%s\n' "$group" "$rid" "$cmdline" >> "$REPORT_ROWS"
        return
    fi

    # ---- the premise, on every run, before any arithmetic ----------------
    local k st bad_premise=""
    for ((k = 0; k < n; k++)); do
        st=0
        premise_holds "$premise" "${rcs[$k]}" "${recs[$k]}" || st=$?
        if [ "$st" = 0 ]; then
            continue
        fi
        if [ "$st" = 2 ]; then
            bad_premise="no premise stated ('$premise')"
        else
            bad_premise="run $((k + 1)) did not satisfy '$premise' (rc ${rcs[$k]})"
        fi
        break
    done
    if [ -n "$bad_premise" ]; then
        note_fail "$rid: $bad_premise"
        printf '%s|%s|no-premise|0|%s|%s|%s\n' "$group" "$rid" "${rcs[0]}" \
            "$slowest" "$cmdline" >> "$REPORT_ROWS"
        return
    fi

    # ---- the injection, for proving this assertion fires -----------------
    if [ -n "$INJECT" ] && [ "$INJECT" = "$rid" ]; then
        for ((k = 0; k < n; k++)); do
            frees[k]=$(( frees[k] - 1024 * k ))
        done
        echo "  (-I $rid: 1024 bytes a run subtracted from the real readings)"
    fi

    # ---- the arithmetic -------------------------------------------------
    #
    # A leak is a cost paid on EVERY invocation, so the question is whether it
    # is still being paid at the end of the run, and the answer is in the last
    # three readings:
    #
    #     EARLY = max(r[n-3], r[n-2])
    #     LATE  = max(r[n-2], r[n-1])
    #
    # one invocation apart, so their difference is the cost of one call.
    #
    # NOT r[1] - r[n-1] over the whole run.  Two things break that.  Free
    # memory JITTERS once a stack is running: a timer thread holds a buffer at
    # the instant of the sample and gives it back afterwards, which is a
    # reading 3-4 KB below the true level and never above it -- measured, and a
    # plain difference called that a 1828-byte leak in a command that leaks
    # nothing.  Jitter only ever subtracts, so the maximum of a two-run window
    # is the true level.  And some one-off costs are not paid in one run:
    # `NetTrace ?` reads 9989504, 9989480, 9989456, 9989456, 9989456 -- it
    # settles after three and is flat forever after, which a window anchored at
    # run 2 reports as 12 bytes a call and this reports as the 0 it is.
    local early late delta per
    early=${frees[$((n - 3))]}
    [ "${frees[$((n - 2))]}" -le "$early" ] || early=${frees[$((n - 2))]}
    late=${frees[$((n - 2))]}
    [ "${frees[$((n - 1))]}" -le "$late" ] || late=${frees[$((n - 1))]}
    delta=$((early - late))
    per=$delta

    # What the whole run cost, so a one-off that takes three runs to settle is
    # SAID rather than merely not failed.
    local settle=$(( frees[1] - frees[n - 1] ))
    if [ "$delta" -eq 0 ] && [ "$settle" -gt 0 ]; then
        echo "  note: $rid settled after the first runs" \
             "($settle bytes between run 2 and run $n, then flat)"
    fi

    # Free memory that went UP is not a leak: something else on the machine
    # gave memory back between the two windows.  Reported as the number it is
    # rather than hidden, and not counted against the row.
    if [ "$delta" -lt 0 ]; then
        printf '%s|%s|ok|%s|%s|%s|%s\n' "$group" "$rid" "$per" "${rcs[0]}" \
            "$slowest" "$cmdline" >> "$REPORT_ROWS"
        return
    fi

    if [ "$delta" -eq 0 ]; then
        printf '%s|%s|ok|0|%s|%s|%s\n' "$group" "$rid" "${rcs[0]}" "$slowest" \
            "$cmdline" >> "$REPORT_ROWS"
    else
        note_fail "$rid: $per bytes lost per invocation, and never returned" \
                  "(free after run $((n - 2)) $early, after run $n $late)"
        printf '%s|%s|LEAK|%s|%s|%s|%s\n' "$group" "$rid" "$per" "${rcs[0]}" \
            "$slowest" "$cmdline" >> "$REPORT_ROWS"
    fi
}

# ======================================================================= #
#                                 drive                                   #
# ======================================================================= #

if [ "$RUNS_OVERRIDE" != 0 ]; then
    awk -F'|' -v OFS='|' -v n="$RUNS_OVERRIDE" '{$4 = n; print}' "$ALLROWS" \
        > "$ALLROWS.tmp" && mv "$ALLROWS.tmp" "$ALLROWS"
fi

if [ "$VERDICT_ONLY" = 0 ]; then
    # The peers have to outlive the whole sweep, not one boot.
    start_peers 5400
fi

for g in $SWEEP_GROUPS; do
    run_group "$g"
done

# ======================================================================= #
#                                 report                                  #
# ======================================================================= #

echo
echo "=================== bytes lost per invocation ======================"
printf '%-20s %-16s %10s %5s %8s  %s\n' COMMAND ARM BYTES/RUN RC MS VERDICT
for g in $SWEEP_GROUPS; do cat "$WORK/report-$g.psv" 2>/dev/null || true; done |
sort -t'|' -k1,1 -k2,2 |
while IFS='|' read -r _g rid verdictword bytes rc ms _cmd; do
    printf '%-20s %-16s %10s %5s %8s  %s\n' \
        "${rid%%/*}" "${rid#*/}" "$bytes" "$rc" "$ms" "$verdictword"
done
echo "===================================================================="

ALLREPORT="$WORK/report-all.psv"
: > "$ALLREPORT"
for g in $SWEEP_GROUPS; do cat "$WORK/report-$g.psv" >> "$ALLREPORT" 2>/dev/null || true; done
MEASURED=$(grep -c '|ok|' "$ALLREPORT" 2>/dev/null || true)
LEAKS=$(grep -c '|LEAK|' "$ALLREPORT" 2>/dev/null || true)
echo "$MEASURED rows flat, $LEAKS leaking, $FAILED failures, $INFRA infrastructure faults"

if [ "$INFRA" -ne 0 ]; then
    echo "toolleak: INFRASTRUCTURE FAILURE -- these rows measured nothing" >&2
    exit 2
fi
if [ "$FAILED" -ne 0 ]; then
    echo "toolleak: FAILED" >&2
    exit 1
fi
echo "toolleak: PASSED"
exit 0
