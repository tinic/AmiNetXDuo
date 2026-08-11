#!/usr/bin/env bash
#
# -4 AND -6 ON A GUEST THAT REALLY HAS BOTH.
#
#   tests/tools/run-family.sh [-b BUILDDIR] [-B IFACE] [-m MODEL] [-t SECONDS]
#                             [-N BOARD] [-d NAME] [-p NAME] [-I ARM] [-V]
#
# WHAT IT PROVES
#
#   Every client command carries IPV4=-4/S,IPV6=-6/S, and each of the four arms
#   the pair has:
#
#     -4          resolves the A and connects over IPv4
#     -6          resolves the AAAA and connects over IPv6
#     -6, no AAAA fails saying the NAME has no address of that family, which is
#                 a different sentence from "no such host" and sends the user
#                 somewhere else
#     -4 -6       is an argument error, not a silent preference
#
#   Every row is written so that IGNORING the flag ends quickly.  That is the
#   regression each -6 and each -4 -6 row exists to catch, and a command that
#   would sit there instead turns the regression into a timeout, which this
#   file reports as infrastructure rather than as a failure.
#
#   The third and fourth arms are also swept under SLIRP by
#   tests/tools/run-toolleak.sh, which measures what they cost.  The second one
#   cannot be: SLIRP carries no IPv6 at all, so it is here and only here, and
#   this script is BRIDGED for that reason and has no SLIRP mode to fall back
#   to.
#
# WHY THE -6 ARM MATTERS MORE THAN THE OTHERS
#
#   Without -6, destination-address selection decides, and on a dual stack it
#   has been observed picking the A even where RFC 6724 says the AAAA wins.
#   That is a separate defect and is not what this measures.  -6 has to force
#   the AAAA on its own account, not inherit whatever the preference does, so
#   every -6 arm asserts the ADDRESS THE COMMAND PRINTED, not merely that the
#   command succeeded: a -6 that quietly fell back to IPv4 would pass an
#   exit-code test and fail this one.
#
# TWO GUESTS ON ONE WIRE
#
#   The stack's default host name is `amiga` for every machine and
#   tools/amiberry-run.sh's default MAC is the same for every run, so a second
#   guest on the same segment fights the first for both.  This script sets its
#   own MAC, its own host name and its own run tag, and none of the three may
#   be shared with tools/demo.sh or another harness running at the same time.
#
# OUTPUT
#
#   One `<arm>=pass|fail` line per assertion, then `checks=`, `failures=` and
#   `result=`.  Nothing here is meant to be read as prose.
#
#     0   every arm passed
#     1   an arm failed
#     2   infrastructure: no build, no ROM, no IPv6 on this wire, a timeout
#     3   the GUEST came up without a usable global IPv6 address.  Its own
#         status because it is neither of the other two: nothing is broken and
#         nothing was proved.  See the next section.
#
# PROVING THE ASSERTIONS FIRE
#
#   -I ARM inverts one arm's verdict after the transcript has been parsed, so
#   the check is exercised against the real artifact:
#
#       tests/tools/run-family.sh -V -I ping/v6
#
#   -V re-reads the last run's transcript without booting anything.
#
# THE NAME SERVER IS REACHED OVER IPv4, AND THAT IS NOT A FAILURE
#
#   Nothing here asserts that a lookup TRAVELLED over IPv6.  -4 and -6 choose
#   which record is asked for and which family the command then connects over;
#   the query itself goes to whatever name server the machine has, and on this
#   wire that is the IPv4 one the DHCPv4 lease supplies.  The router does
#   advertise RDNSS, and our side not acting on it is a separate defect owned
#   elsewhere.
#
#   So `dns/aaaa` asking 192.168.1.1 for an AAAA and getting one is the
#   correct shape of a passing -6 lookup, not a half-measure.  Do not rewrite
#   these rows to require an IPv6 name server: no arm here needs one, and one
#   that did could not pass on this network at all.
#
# THE GUEST NEEDS A GLOBAL IPv6 ADDRESS, AND IT IS NOT A GIVEN
#
#   Every -6 arm is asserted against the address the command printed, and that
#   only means anything on a guest that has a global IPv6 address of its own.
#   With nothing but a link-local, a command that answers over IPv4 is
#   OBEYING RFC 6724 rather than ignoring the flag, so the same red row would
#   mean the opposite thing.
#
#   The lab has lost its IPv6 delegation for hours at a time -- WAN_DHCP6 with
#   no gateway, 100% loss -- and a run that straddled that reported the
#   product as broken.  So the guest's own address is a PRECONDITION, checked
#   out of `ShowNetStatus ALL` in the transcript before any -6 arm is judged.
#   Without it those arms are reported `blocked`, which is neither a pass nor a
#   failure, and the run exits 3.  A tentative address does not count: it is
#   one duplicate-address probe short of being usable and nothing may be sent
#   from it.
#
#   This is not a precaution for one bad afternoon.  That link is unreliable
#   for IPv6, so every -6 assertion in this tree is written this way.
#
# A TIMEOUT IS A DEFECT
#
#   A run that burns its ceiling produces a partial transcript and proves
#   nothing.  It exits 2 and names the command the transcript stops at.
#   Raising -t is never the fix.
#
#   That cuts both ways for the rehearsal above: a build with the flag broken
#   on purpose is the case most likely to hang rather than fail, because an
#   ignored flag leaves each command doing what it was asked to do.  Every row
#   carries a deadline for that reason, and the first rehearsal that did not
#   sat on one command until the ceiling and reported nothing at all.
#
# WHAT IT NEEDS
#
#   A 68020 Release build, a2065.device, a Kickstart, and a wire with a router
#   advertising IPv6.  The names it asks for are checked from the host first:
#   if the DUAL name has no AAAA here, or this host cannot reach it over IPv6,
#   the run exits 2 rather than reporting the Amiga as broken.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-build/cm}"
IFACE="${AMINETXDUO_FAMILY_IFACE:-ens18}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
MODEL="${AMINETXDUO_FAMILY_MODEL:-A1200}"
# Measured, not guessed.  The slowest row is telnet's -6 arm on a guest with no
# usable IPv6 source address: telnet has no timeout of its own, so the connect
# runs to the stack's own ceiling, 191 s on the run of 2026-08-10.  That figure
# is worth its own look -- nc reached the same host and gave up in the 20 s its
# -w asked for -- but it is a connect timeout and not a hang, and the whole
# list has to fit inside this one.
TIMEOUT="${AMINETXDUO_FAMILY_TIMEOUT:-900}"
VERDICT_ONLY=0
INJECT=""
GUEST_V6=""

# A name with an A and an AAAA, and a service on port 80 answering over both.
DUAL="${AMINETXDUO_FAMILY_DUAL:-example.com}"
DUAL_V6=""
# A dual-stack HTTP service whose body is the CLIENT's own address.  fetch
# prints no address of its own on a successful transfer, so this is how its two
# arms are told apart: the far end reports which family the request arrived
# over, which is a stronger statement than anything the client could print.
ECHO="${AMINETXDUO_FAMILY_ECHO:-icanhazip.com}"
# The time server for the sntp arms.  2.pool.ntp.org is the pool's IPv6 half.
NTP="${AMINETXDUO_FAMILY_NTP:-2.pool.ntp.org}"
# A name with an A and no AAAA.  Written into the guest's DEVS:Internet/hosts,
# so it needs no DNS at all: the resolver reads that file on the IPv4 side and
# never on the IPv6 side (src/netstack/netstack_dns.c:826, the netdb schema has
# no family field), which is exactly the shape the -6 arm has to report.  .test
# is reserved by RFC 6761, so the AAAA query is an immediate NXDOMAIN.
#
# It points at the guest's own loopback, which is not laziness.  Every -6 row
# below has to end quickly WHEN THE FLAG IS IGNORED, because that is the
# regression the row exists to catch and a row that hangs instead reports a
# timeout, which is infrastructure and not a failure: the broken-build
# rehearsal of this file hung for eight minutes on `whois SERVER v4only.test`
# reaching a router that dropped port 43.  Loopback refuses instantly, so a
# regression is a red row and not a dead boot.
V4ONLY_NAME="v4only.test"
V4ONLY_ADDR="${AMINETXDUO_FAMILY_V4ONLY_ADDR:-127.0.0.1}"

# Distinct from tools/demo.sh (…:77) and from the default (…:01).  The a2065's
# LANCE overwrites the first three octets, so only the last three are the
# machine's identity on the wire.
export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_FAMILY_MAC:-02:41:4d:49:00:46}"
export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-family}"
# Not `amiga`: two guests answering for amiga.local take each other off the air.
GUEST_NAME="${AMINETXDUO_FAMILY_HOSTNAME:-anxd46}"

while getopts "b:B:m:t:N:d:p:I:V" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        d) DUAL="$OPTARG" ;;
        p) NTP="$OPTARG" ;;
        I) INJECT="$OPTARG" ;;
        V) VERDICT_ONLY=1 ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

TOOLS="$BUILD/src/tools"
PROBES="$BUILD/tests/tools"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
STAGE="$ROOT/build/family-stage"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"
PARAMS="$ROOT/build/family-params-$AMINETXDUO_RUN_TAG.env"

infra() { echo "error=$*"; echo "result=infra"; exit 2; }

# ======================================================================= #
#                          what the host can see                          #
# ======================================================================= #
#
# Asked here, before anything boots.  A wire with no IPv6 router, or a DUAL
# name that has lost its AAAA, would make every -6 arm red and the Amiga
# innocent, and a test that cannot tell those apart is worse than none.

host_preflight() {
    local a aaaa

    command -v getent >/dev/null 2>&1 || infra "no getent on this host"

    a=$(getent ahostsv4 "$DUAL" 2>/dev/null | awk 'NR==1{print $1}')
    aaaa=$(getent ahostsv6 "$DUAL" 2>/dev/null |
           awk 'NR==1 && $1 !~ /^::ffff:/ {print $1}')

    [ -n "$a" ]    || infra "$DUAL has no A record from this host"
    [ -n "$aaaa" ] || infra "$DUAL has no AAAA record from this host"

    ip -6 route show default 2>/dev/null | grep -q . ||
        infra "this host has no IPv6 default route, so the wire has no RA"

    ping -6 -c 1 -W 3 "$DUAL" >/dev/null 2>&1 ||
        infra "this host cannot reach $DUAL over IPv6"

    getent ahostsv6 "$NTP" 2>/dev/null |
        awk 'NR==1 && $1 !~ /^::ffff:/ {print $1}' | grep -q . ||
        infra "$NTP has no AAAA record from this host"

    getent ahostsv6 "$ECHO" 2>/dev/null |
        awk 'NR==1 && $1 !~ /^::ffff:/ {print $1}' | grep -q . ||
        infra "$ECHO has no AAAA record from this host"

    DUAL_V6="$aaaa"

    # Written down, because the judge has to build the same command strings the
    # boot did and these are not stable between two lookups: a name behind a
    # round robin answers with a different AAAA a minute later, and -V then
    # looks for a command line that was never run.
    mkdir -p "$ROOT/build"
    {
        printf 'DUAL=%s\n'        "$DUAL"
        printf 'DUAL_V6=%s\n'     "$DUAL_V6"
        printf 'ECHO=%s\n'        "$ECHO"
        printf 'NTP=%s\n'         "$NTP"
        printf 'V4ONLY_ADDR=%s\n' "$V4ONLY_ADDR"
    } > "$PARAMS"

    echo "host/dual=$DUAL a=$a aaaa=$aaaa"
    echo "host/v4only=$V4ONLY_NAME addr=$V4ONLY_ADDR"
    echo "host/ntp=$NTP"
    echo "host/echo=$ECHO"
}

find_a2065() {
    local c
    if [ -n "${AMINETXDUO_A2065:-}" ] && [ -f "${AMINETXDUO_A2065}" ]; then
        echo "$AMINETXDUO_A2065"; return 0
    fi
    for c in "$ROOT/build/a2065.device" \
             "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { echo "$c"; return 0; }
    done
    return 1
}

# ======================================================================= #
#                                the arms                                 #
# ======================================================================= #
#
#   id | rc | command line | assertion...
#
# An assertion is `+ERE` (must appear in that command's own block) or `-ERE`
# (must not).  `rc` is the exit code the command must return, or `*`.
#
# The address patterns are the whole point of the -4/-6 rows: a dotted quad and
# a colonned address cannot be mistaken for one another, so asserting which one
# the command printed is asserting which family it actually used.
V4RE='[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+'
V6RE='[0-9a-fA-F]*:[0-9a-fA-F]*:'

arms() {
    # A table of one's own, for putting a single arm to the question without
    # booting the other fifty.  Same seven-and-more fields, same $-expansion;
    # tests/tools/run-toolleak.sh takes its table the same way.
    if [ -n "${AMINETXDUO_FAMILY_TABLE:-}" ]; then
        eval "cat <<EOF
$(cat "$AMINETXDUO_FAMILY_TABLE")
EOF"
        return
    fi
    cat <<EOF
# Can this stack answer a AAAA query at all?  nslookup builds the query itself
# and sends it to a name server, so it reaches no part of the -4/-6 code.  It
# is the first row on purpose: if it is red, every -6 row below it is red for a
# reason that has nothing to do with the flag, and the report says so in one
# line instead of leaving twelve failures to be interpreted.
dns/aaaa|0|SYS:nslookup $DUAL TYPE=AAAA TIMEOUT 15|+$V6RE
dns/a|0|SYS:nslookup $DUAL TYPE=A TIMEOUT 15|+$V4RE
# The interface again, tens of seconds after bring-up.  A router advertisement
# arrives on its own schedule and duplicate-address detection takes a moment
# after it, so this is where a global IPv6 address is expected to be visible if
# this wire is going to produce one at all.
net/settled|0|SYS:ShowNetStatus ALL|+address6
# The picture first: without either flag, whatever selection chooses.  Recorded
# and not asserted -- which family wins there is a separate question, and this
# file is about what happens when the user has said which one they want.
host/none|0|SYS:host $DUAL|+has
host/v4|0|SYS:host $DUAL -4|+has address $V4RE|-has IPv6 address
host/v6|0|SYS:host $DUAL -6|+has IPv6 address $V6RE|-has address $V4RE
host/v4-only-name|0|SYS:host $V4ONLY_NAME -4|+has address $V4ONLY_ADDR
host/no-aaaa|10|SYS:host $V4ONLY_NAME -6|+$V4ONLY_NAME has no IPv6 address, and -6 was given
host/nxdomain|10|SYS:host no.such.host.invalid -6|+cannot resolve|-has no IPv6 address
host/both|10|SYS:host $DUAL -4 -6|+-4 and -6 cannot both be given
# A literal already declares its own family, so a flag that contradicts it is
# an error about the argument and not about any name server.  These two rows
# reach no resolver at all, which is why they belong here: they stay green on a
# wire with no DNS and they are the only rows that isolate the tool half of
# -4/-6 from everything underneath it.
literal/v4-under-6|10|SYS:ping $V4ONLY_ADDR -c 1 -t 10 -6|+$V4ONLY_ADDR is an IPv4 address, and -6 was given
literal/v6-under-4|10|SYS:ping $DUAL_V6 -c 1 -t 10 -4|+$DUAL_V6 is an IPv6 address, and -4 was given
# And the IPv6 datapath itself, by literal, so a -6 arm that fails above can be
# told apart from a machine that cannot send an IPv6 packet at all.
literal/v6-ping|0|SYS:ping $DUAL_V6 -c 2 -t 20|+bytes from|+0% packet loss
ping/v4|0|SYS:ping $DUAL -c 2 -t 25 -4|+bytes from $V4RE:|+0% packet loss
ping/v6|0|SYS:ping $DUAL -c 2 -t 25 -6|+bytes from $V6RE|+0% packet loss
ping/no-aaaa|10|SYS:ping $V4ONLY_NAME -c 1 -t 10 -6|+$V4ONLY_NAME has no IPv6 address, and -6 was given
ping/both|10|SYS:ping $DUAL -c 1 -t 10 -4 -6|+-4 and -6 cannot both be given
traceroute/v4|*|SYS:traceroute $DUAL -m 1 -q 1 -w 5 -n -4|+traceroute to $DUAL \\($V4RE\\)
traceroute/v6|*|SYS:traceroute $DUAL -m 1 -q 1 -w 5 -n -6|+traceroute to $DUAL \\($V6RE\\)
# A HOP HAS TO ANSWER.  The two rows above assert the header line, which
# traceroute prints from the resolved address before it sends anything, so they
# pass on a trace where every single probe times out -- which is exactly the
# state -6 shipped in, and these rows are why nobody noticed.  Asserting the
# first hop is asserting that the probe left with a hop limit on it and that
# the router's ICMP time-exceeded came back to the socket.
traceroute/v4-hop|*|SYS:traceroute $DUAL -m 3 -q 1 -w 5 -n -4|+^ 1 .*$V4RE
traceroute/v6-hop|*|SYS:traceroute $DUAL -m 3 -q 1 -w 5 -n -6|+^ 1 .*$V6RE
traceroute/no-aaaa|10|SYS:traceroute $V4ONLY_NAME -m 1 -q 1 -w 5 -n -6|+has no IPv6 address, and -6 was given
traceroute/both|10|SYS:traceroute $DUAL -m 1 -q 1 -w 5 -n -4 -6|+-4 and -6 cannot both be given
# CTRL-C HAS TO STOP IT.  Over IPv4 on purpose: the break has nothing to do
# with the address family, and running it over IPv4 means it is exercised on
# every wire rather than only on one that has IPv6 -- which is how it went
# unnoticed, since the -6 arms were blocked and nothing else ever interrupted
# this command.  192.0.2.1 is TEST-NET-1 and answers nothing, so the trace is
# still running when the break lands.  The second assertion is the other half
# of the same defect: after an ignored break every remaining hop printed as its
# own number and an empty line.
traceroute/break|0|SYS:TrBreak SECONDS 8 CEILING 3 SYS:traceroute 192.0.2.1 -m 30 -q 3 -w 2 -n -4|+alive_at_break=yes|+result=broke|-^ *[0-9]+ *$
fetch/v4|0|SYS:fetch http://$ECHO/ TIMEOUT 40 TO DH0:f4.txt -4|+HTTP/1.[01] 200
fetch/v6|0|SYS:fetch http://$ECHO/ TIMEOUT 40 TO DH0:f6.txt -6|+HTTP/1.[01] 200
fetch/no-aaaa|10|SYS:fetch http://$V4ONLY_NAME/ TIMEOUT 10 -6|+has no IPv6 address, and -6 was given
fetch/both|10|SYS:fetch http://$ECHO/ TIMEOUT 10 -4 -6|+-4 and -6 cannot both be given
nc/v4|0|SYS:nc -z $DUAL 80 -v -w 20 -4|+$V4RE port 80 open
nc/v6|0|SYS:nc -z $DUAL 80 -v -w 20 -6|+$V6RE.* port 80 open
nc/no-aaaa|10|SYS:nc -z $V4ONLY_NAME 80 -v -w 5 -6|+has no IPv6 address, and -6 was given
nc/both|10|SYS:nc -z $DUAL 80 -v -w 10 -4 -6|+-4 and -6 cannot both be given
telnet/v4|*|SYS:telnet $DUAL 80 -4 <DH0:telnetin.txt|+Trying $V4RE port 80
telnet/v6|*|SYS:telnet $DUAL 80 -6 <DH0:telnetin.txt|+Trying $V6RE.* port 80
telnet/no-aaaa|10|SYS:telnet $V4ONLY_NAME 80 -6|+has no IPv6 address, and -6 was given
telnet/both|10|SYS:telnet $DUAL 80 -4 -6 <DH0:telnetin.txt|+-4 and -6 cannot both be given
tftp/v4|*|SYS:tftp $DUAL GET nosuchfile TIMEOUT 2 -4|+getting nosuchfile from $V4RE
tftp/v6|*|SYS:tftp $DUAL GET nosuchfile TIMEOUT 2 -6|+getting nosuchfile from $V6RE
tftp/no-aaaa|10|SYS:tftp $V4ONLY_NAME GET nosuchfile TIMEOUT 2 -6|+has no IPv6 address, and -6 was given
tftp/both|10|SYS:tftp $DUAL GET nosuchfile TIMEOUT 2 -4 -6|+-4 and -6 cannot both be given
whois/v4|0|SYS:whois example.com -4|+IANA WHOIS server|+domain: *EXAMPLE.COM
whois/v6|0|SYS:whois example.com -6|+IANA WHOIS server|+domain: *EXAMPLE.COM
whois/no-aaaa|10|SYS:whois example.com SERVER $V4ONLY_NAME PORT 43 -6|+has no IPv6 address, and -6 was given
whois/both|10|SYS:whois example.com -4 -6|+-4 and -6 cannot both be given
sntp/v4|0|SYS:sntp $NTP SHOW TIMEOUT 20 -4|+\\($V4RE\\): stratum
sntp/v6|0|SYS:sntp $NTP SHOW TIMEOUT 20 -6|+\\($V6RE.*\\): stratum
sntp/no-aaaa|10|SYS:sntp $V4ONLY_NAME TIMEOUT 5 -6|+has no IPv6 address, and -6 was given
sntp/both|10|SYS:sntp $NTP SHOW TIMEOUT 10 -4 -6|+-4 and -6 cannot both be given
iperf/v4|*|SYS:iperf $DUAL -p 80 -n 32 -4|+TCP to $V4RE port 80
iperf/v6|*|SYS:iperf $DUAL -p 80 -n 32 -6|+TCP to $V6RE.* port 80
iperf/no-aaaa|10|SYS:iperf $V4ONLY_NAME -p 80 -n 32 -6|+has no IPv6 address, and -6 was given
iperf/both|10|SYS:iperf $DUAL -p 80 -n 32 -4 -6|+-4 and -6 cannot both be given
EOF
}

# ======================================================================= #
#                                the boot                                 #
# ======================================================================= #

stage_and_boot() {
    local a2065 t

    for t in ToolsSmoke AddNetInterface ShowNetStatus nslookup host ping \
             traceroute fetch nc telnet tftp whois sntp iperf; do
        [ -x "$TOOLS/$t" ] || infra "no $TOOLS/$t; build $BUILD first"
    done
    [ -x "$PROBES/TrBreak" ] || infra "no $PROBES/TrBreak; build $BUILD first"
    [ -f "$BSD" ] || infra "no $BSD; build $BUILD first"

    a2065=$(find_a2065) || infra "no a2065.device; set AMINETXDUO_A2065"

    rm -rf "$STAGE"
    mkdir -p "$STAGE/libs"
    cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
    cp "$a2065" "$STAGE/devs/a2065.device"
    cp "$BSD"   "$STAGE/libs/bsdsocket.library"
    for t in AddNetInterface ShowNetStatus nslookup host ping traceroute \
             fetch nc telnet tftp whois sntp iperf; do
        cp "$TOOLS/$t" "$STAGE/$t"
    done
    cp "$PROBES/TrBreak" "$STAGE/TrBreak"

    # DHCP for IPv4 and, by default, CONFIGURE6=AUTO for IPv6: link-local
    # always, and a global address from the router advertisement this wire
    # carries.  The stock file already says that; it is rewritten here so that
    # a change to the shared one cannot silently move this test to a static
    # address the LAN does not route.
    cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=AUTO
EOF

    printf '%s %s\n' "$V4ONLY_ADDR" "$V4ONLY_NAME" \
        >> "$STAGE/devs/Internet/hosts"

    # Written, not appended to.  The shared file names SLIRP's forwarder,
    # 10.0.2.3, which does not exist on a bridged wire: it goes to the head of
    # the server list ahead of the one DHCP supplies and every lookup pays for
    # it before falling back.  The lease carries a name server; this file only
    # has to carry the host name, which must not be `amiga` while another guest
    # is up.
    printf 'hostname %s\n' "$GUEST_NAME" \
        > "$STAGE/devs/Internet/name_resolution"

    # telnet talks to an HTTP server: two junk lines are a bad request, the
    # server answers 400 and closes, and the session ends by itself.  A telnet
    # arm that needed a person would hang the boot.
    printf 'x\r\n\r\n' > "$STAGE/telnetin.txt"

    arms | grep -v '^#' | cut -d'|' -f3 > "$STAGE/cmds.raw"
    {
        echo "SYS:AddNetInterface eth0"
        echo "SYS:ShowNetStatus ALL"
        cat "$STAGE/cmds.raw"
    } > "$STAGE/commands.txt"

    echo "boot/mac=$AMINETXDUO_AMIBERRY_MAC"
    echo "boot/hostname=$GUEST_NAME"
    echo "boot/tag=$AMINETXDUO_RUN_TAG"
    echo "boot/iface=$IFACE board=$BOARD model=$MODEL"

    set +e
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
        "$STAGE/libs" "$STAGE/telnetin.txt" \
        "$STAGE/AddNetInterface" "$STAGE/ShowNetStatus" "$STAGE/nslookup" \
        "$STAGE/host" \
        "$STAGE/ping" "$STAGE/traceroute" "$STAGE/fetch" "$STAGE/nc" \
        "$STAGE/telnet" "$STAGE/tftp" "$STAGE/whois" "$STAGE/sntp" \
        "$STAGE/iperf" "$STAGE/TrBreak" \
        > "$ROOT/build/family-run.log" 2>&1
    RUN_RC=$?
    set -e

    echo "boot/rc=$RUN_RC"
}

# ======================================================================= #
#                               the verdict                               #
# ======================================================================= #

# One command's own block out of the transcript.
block() {
    awk -v want="===== $1 =====" '
        $0 == want { on = 1; next }
        on && /^----- / { exit }
        on { print }
    ' "$REPORT"
}

rc_of() {
    awk -v want="===== $1 =====" '
        $0 == want { on = 1; next }
        on && /^----- rc / { print; exit }
    ' "$REPORT" | sed -n 's/^----- rc \([0-9-]*\),.*/\1/p'
}

CHECKS=0
FAILURES=0
BLOCKED=0

# The guest's own global IPv6 address.  Not a link-local, and not one still
# finishing duplicate-address detection: nothing may be sent from a tentative
# address, so a command that answers over IPv4 from one is right to.
#
# The WHOLE transcript, not one block.  A global address arrives when a router
# advertisement does, which is after bring-up and not during it, and the
# command list therefore samples the interface a second time further down.
# Reading only the first sample called a guest address-less that had one twenty
# seconds later.
guest_global_v6() {
    awk '$1 == "address6" && $2 !~ /^fe80:/ && $0 !~ /\(tentative\)/ \
         { print $2; exit }' "$REPORT"
}

# An arm that cannot mean anything without that address.
v6_dependent() {
    case "$1" in
        */v6|literal/v6-ping) return 0 ;;
        *)                    return 1 ;;
    esac
}

verdict() {
    local id="$1" verdict="$2" why="$3"

    if [ "$id" = "$INJECT" ]; then
        # -I: invert this one, so the assertion is exercised against the real
        # transcript rather than a fabricated one.
        [ "$verdict" = pass ] && { verdict=fail; why="injected"; } \
                              || { verdict=pass; why=""; }
    fi

    CHECKS=$((CHECKS + 1))
    if [ "$verdict" = pass ]; then
        echo "$id=pass"
    elif [ "$verdict" = blocked ]; then
        BLOCKED=$((BLOCKED + 1))
        echo "$id=blocked why=$why"
    else
        FAILURES=$((FAILURES + 1))
        echo "$id=fail why=$why"
    fi
}

judge() {
    local line id want cmd rest got body ok why field

    [ -f "$REPORT" ] || infra "the guest wrote no $REPORT"

    GUEST_V6=$(guest_global_v6)
    if [ -n "$GUEST_V6" ]; then
        echo "guest/global6=$GUEST_V6"
    else
        echo "guest/global6=none"
        block "SYS:ShowNetStatus ALL" | sed -n 's/^ *address6/guest\/address6=/p'
    fi

    # A transcript that stops early is a timeout, not a set of failures.
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue ;; esac
        cmd=$(printf '%s' "$line" | cut -d'|' -f3)
        if ! grep -qF -- "===== $cmd =====" "$REPORT"; then
            echo "stopped_at=$cmd"
            infra "the transcript stops before this command ran"
        fi
    done < <(arms)

    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue ;; esac

        id=$(printf   '%s' "$line" | cut -d'|' -f1)
        want=$(printf '%s' "$line" | cut -d'|' -f2)
        cmd=$(printf  '%s' "$line" | cut -d'|' -f3)
        rest=$(printf '%s' "$line" | cut -d'|' -f4-)

        body=$(block "$cmd")
        got=$(rc_of "$cmd")
        ok=pass
        why=""

        # The precondition, ahead of the assertion rather than after it: with
        # no global address of its own the guest answering over IPv4 is
        # correct, so this row has nothing to say either way.
        if [ -z "$GUEST_V6" ] && v6_dependent "$id"; then
            verdict "$id" blocked "guest has no global IPv6 address"
            continue
        fi

        if [ "$want" != '*' ] && [ "$got" != "$want" ]; then
            ok=fail; why="rc=$got want=$want"
        fi

        while [ -n "$rest" ]; do
            field=${rest%%|*}
            if [ "$field" = "$rest" ]; then rest=""; else rest=${rest#*|}; fi
            [ -n "$field" ] || continue

            case "$field" in
                +*) printf '%s\n' "$body" |
                        grep -Eq -- "${field#+}" ||
                        { ok=fail; why="missing:${field#+}"; } ;;
                -*) printf '%s\n' "$body" |
                        grep -Eq -- "${field#-}" &&
                        { ok=fail; why="present:${field#-}"; } || true ;;
            esac
        done

        verdict "$id" "$ok" "$why"
    done < <(arms)
}

# What the FAR END saw.  fetch prints no address on a successful transfer, so
# the only honest way to say which family carried it is to read the body: the
# echo service answers with the client address it was reached from, and that
# file is on the guest's drive, which is a directory on this host.
judge_files() {
    local four six ok

    four="$HD/f4.txt"
    six="$HD/f6.txt"

    ok=pass
    if [ ! -s "$four" ]; then ok=fail
    elif ! grep -Eq "^$V4RE" "$four"; then ok=fail; fi
    verdict "fetch/v4-body" "$ok" "$(head -c 64 "$four" 2>/dev/null | tr -d '\r\n')"

    if [ -z "$GUEST_V6" ]; then
        verdict "fetch/v6-body" blocked "guest has no global IPv6 address"
        return
    fi

    ok=pass
    if [ ! -s "$six" ]; then ok=fail
    elif ! grep -Eq "^$V6RE" "$six"; then ok=fail; fi
    verdict "fetch/v6-body" "$ok" "$(head -c 64 "$six" 2>/dev/null | tr -d '\r\n')"
}

# ======================================================================= #

if [ "$VERDICT_ONLY" = 0 ]; then
    host_preflight
    stage_and_boot
elif [ -f "$PARAMS" ]; then
    # The names the last boot actually used, not what they resolve to now.
    # shellcheck source=/dev/null
    . "$PARAMS"
else
    infra "no $PARAMS; -V needs a run to re-read"
fi

judge
judge_files

echo "checks=$CHECKS"
echo "failures=$FAILURES"
echo "blocked=$BLOCKED"

if [ "$FAILURES" -ne 0 ]; then
    echo "result=fail"
    exit 1
fi

if [ "$BLOCKED" -ne 0 ]; then
    # Nothing failed, but the -6 arms were never put to the question.  Not a
    # pass: a pass here would be read as "IPv6 works".
    echo "result=infra-no-guest-ipv6"
    exit 3
fi

echo "result=pass"
exit 0
