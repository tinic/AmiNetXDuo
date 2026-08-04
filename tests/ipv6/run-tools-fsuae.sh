#!/usr/bin/env bash
#
# Run the shipped commands against IPv6, and assert on what they print.
#
#   tests/ipv6/run-tools-fsuae.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR] [-s]
#
# docs/RESEARCH.md 66 measured every command answering an IPv6 literal with
# "cannot resolve", followed by advice to check the spelling and the name
# servers, neither of which has anything to do with a literal.  This is the
# test for the other side of that: a bare literal must never reach the
# resolver, a real name failure must still say so, and the addresses the stack
# configures must be visible through a shipped command.
#
# TWO MODES, PICKED FROM THE BUILD
#
# The build directory's CMakeCache decides which run this is.  Both are real
# configurations and both are tested.
#
#   ipv6   -DAMINETXDUO_IPV6=ON.  ::1 exists, and the commands are asked to
#          use it.  Default, build/v6.
#   ipv4   the shipping default, IPv6 off.  ::1 does not exist, and the
#          commands are asked the same questions anyway.  This is the half
#          that was never run: the tools support IPv6 unconditionally and the
#          library they talk to is built either way, so "what does this
#          command say when the address is valid and the machine cannot use
#          it" is a question only the IPv4-only build asks.  It found three
#          commands giving the wrong answer, `host ::1` blamed the spelling,
#          `nslookup ::1` reported NXDOMAIN for a literal, and `arp ::1`
#          called a valid address "not an address".  This mode used to refuse
#          to start.
#
# THREE GROUPS, AND WHY THE SCRIPT DOES NOT TREAT THEM ALIKE (ipv6 mode)
#
#   control   IPv4 and the machine itself.  A failure here is a defect, and it
#             is the half that says an IPv6 change broke something that worked.
#   v6        IPv6 through the commands.  Asserted since the toolsock
#             conversion; a failure here is a regression.
#   display   66's acceptance test: a configured ADDRESS6 shown by a command.
#             Asserted since the NETSTATUS_ADDRESSES6 work.
#   routes    AddNetRoute/DeleteNetRoute over IPv6, and the neighbour cache in
#             arp.  The last two of 66.4's list.  A route accepted and never
#             used would pass a test that only read the table back, so the
#             assertion that matters is the one on the neighbour cache: adding
#             an on-link prefix changes which address the next packet is
#             solicited for.
#
# ping and traceroute joined the v6 group once bsdsocket.library grew an
# AF_INET6 raw socket (RESEARCH 67); their replies are asserted, not pending.
#
# Exit 0 all clear, 1 a control or -s failure, 3 nothing but pending work.
#
# NO HOST-SIDE PEER, for tests/tools/run-livetools.sh's reason: with several
# agents queued on the emulator lock a Python server's lifetime can expire
# before the guest boots, and every case then fails with "connection refused",
# which looks exactly like a broken command.  Everything here is either inside
# the guest (::1, and nc listening on itself) or answered by SLIRP, which is
# alive for exactly as long as the emulator.  fe80::2 is SLIRP's own router
# and tests/ipv6/ipv6_link_test proves it answers ICMPv6.
#
# The a2065.device driver is not ours to ship: point AMINETXDUO_A2065 at one,
# or drop a copy in build/a2065.device.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
# The ipv6 list spends about seven minutes of it on `wait` lines, unreachable
# addresses and one deliberate 30 s resolver timeout, so 420 left no headroom.
TIMEOUT=540
BUILD="${AMINETXDUO_BUILD:-build/v6}"
STRICT=0

while getopts "m:t:b:s" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        s) STRICT=1 ;;
        *) sed -n '3,5p' "$0" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

STAGED_TOOLS="AddNetInterface ShowNetStatus netstat ping nc telnet whois \
              tftp traceroute NetTrace nslookup arp AddNetRoute DeleteNetRoute \
              host fetch sntp"

for t in $STAGED_TOOLS ToolsSmoke; do
    [ -f "$TOOLS/$t" ] || { echo "missing $TOOLS/$t -- build $BUILD first" >&2
                            exit 2; }
done
[ -f "$BSD" ] || { echo "missing $BSD" >&2; exit 2; }

# Which run this is.  A cache that does not say ON is the shipping default,
# and the ipv4 mode below is written for exactly that library.
MODE=ipv4
TAG=v4
CACHE="$ROOT/$BUILD/CMakeCache.txt"
if [ -f "$CACHE" ] && grep -q "^AMINETXDUO_IPV6:BOOL=ON" "$CACHE"; then
    MODE=ipv6
    TAG=v6
fi

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
    echo "No a2065.device found. Set AMINETXDUO_A2065=<path>." >&2; exit 2; }

# --------------------------------------------------------------- staging ---

STAGE="$ROOT/build/${TAG}tools-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
for t in $STAGED_TOOLS; do cp "$TOOLS/$t" "$STAGE/$t"; done

# DHCP for v4 as every other test has it, plus a static v6 address of our own.
# STATIC rather than AUTO because an address this file names is one the test
# can then look for by name; a SLAAC address depends on the MAC.
#
# The same file in both modes, deliberately: an IPv4-only library reads
# CONFIGURE6 and ADDRESS6 as keys it has no build for, and the interface has
# to come up on IPv4 regardless.
cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=STATIC
ADDRESS6=fd00::10/64
EOF

printf 'hello from the amiga\n' > "$STAGE/greeting.txt"

# A background listener's output must be redirected by its own line: a
# detached process shares no console and its Output() is NIL: (toolssmoke.c).
if [ "$MODE" = ipv6 ]; then
cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
SYS:ShowNetStatus INTERFACES
SYS:ShowNetStatus ALL
SYS:netstat -i
SYS:ping 10.0.2.2 -c 2 -t 20
SYS:ping ::1 -c 2 -t 20
SYS:ping fd00::10 -c 2 -t 20
SYS:ping fe80::2 -c 2 -t 20
SYS:ping 2001:db8::1 -c 1 -t 5
&SYS:nc -l 7099 -v -w 25 -N >DH0:nc-v6srv.txt
wait 4
SYS:nc ::1 7099 -v -w 10 -N <DH0:greeting.txt
wait 4
&SYS:nc -l 7098 -v -w 25 -N >DH0:nc-v4srv.txt
wait 4
SYS:nc 127.0.0.1 7098 -v -w 10 -N <DH0:greeting.txt
wait 4
&SYS:nc -l 7097 -v -w 25 >DH0:nc-telsrv.txt
wait 4
SYS:telnet ::1 7097 QUIET <DH0:greeting.txt
wait 4
&SYS:nc -l 7096 -v -w 25 >DH0:nc-whosrv.txt
wait 4
SYS:whois example.com SERVER ::1 PORT 7096
wait 2
SYS:tftp ::1 GET nosuchfile PORT 7095 TIMEOUT 5
SYS:traceroute ::1 -m 2 -q 1 -w 3 -n
SYS:NetTrace LOOPBACK NOCAPTURE BYTES=65536
SYS:nslookup ::1
SYS:host ::1
SYS:nc no.such.host.invalid 80
SYS:AddNetRoute DEFAULTGATEWAY fd00::99
wait 3
SYS:netstat -r
SYS:DeleteNetRoute DEFAULTGATEWAY fd00::99
SYS:arp
SYS:AddNetRoute DESTINATION fd00:9::/64 GATEWAY fe80::2
SYS:AddNetRoute DESTINATION 2001:db8::/64
SYS:ShowNetStatus ROUTES
&SYS:nc 2001:db8::1 80 -v -w 14 >DH0:nc-onlink.txt
wait 3
SYS:arp 2001:db8::1
wait 14
SYS:DeleteNetRoute DESTINATION 2001:db8::/64
SYS:arp 2001:db8::2
SYS:AddNetRoute DEFAULTGATEWAY fe80::1
EOF
else
# Every command that takes a host, asked for an address this machine has no
# transport for, plus the four that read or write the stack's own tables.
# Nothing here waits on a peer: an address that cannot be reached is refused
# before a socket is made, so the whole run is as fast as its two IPv4 cases.
cat > "$STAGE/commands.txt" <<'EOF'
SYS:AddNetInterface eth0
SYS:ShowNetStatus INTERFACES
SYS:ShowNetStatus ALL
SYS:netstat -i
SYS:ping 10.0.2.2 -c 2 -t 20
&SYS:nc -l 7098 -v -w 25 -N >DH0:nc-v4srv.txt
wait 4
SYS:nc 127.0.0.1 7098 -v -w 10 -N <DH0:greeting.txt
wait 4
SYS:ping ::1 -c 2 -t 20
SYS:ping fe80::2 -c 2 -t 20
SYS:traceroute ::1 -m 2 -q 1 -w 3 -n
SYS:nc ::1 7099 -v -w 10 -N
SYS:telnet ::1 7097 QUIET
SYS:tftp ::1 GET nosuchfile PORT 7095 TIMEOUT 5
SYS:whois example.com SERVER ::1 PORT 7096
SYS:sntp fd00::10 TIMEOUT 5
SYS:fetch http://[::1]/index.html
SYS:host ::1
SYS:nslookup ::1
SYS:nslookup 10.0.2.3
SYS:nc no.such.host.invalid 80
SYS:arp
SYS:arp ::1
SYS:arp fe80::zz
SYS:arp 10.0.2.2
SYS:AddNetRoute DEFAULTGATEWAY fd00::99
SYS:AddNetRoute DESTINATION fd00:9::/64
SYS:DeleteNetRoute DEFAULTGATEWAY fd00::99
SYS:AddNetRoute DESTINATION 192.168.77.0/24 GATEWAY 10.0.2.2
SYS:netstat -r
SYS:ShowNetStatus ROUTES
SYS:DeleteNetRoute DESTINATION 192.168.77.0/24
EOF
fi

# ------------------------------------------------------------------- run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-${TAG}tools}"
HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"

echo "==> $BUILD ($MODE) on $MODEL, A2065 on SLIRP, eth0 with ADDRESS6=fd00::10/64"
set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/greeting.txt" \
    $(for t in $STAGED_TOOLS; do echo "$STAGE/$t"; done)
RUN_RC=$?
set -e

SERIAL="$ROOT/build/serial-$AMINETXDUO_RUN_TAG.log"
RAW="$HD/tools.txt"
[ -f "$RAW" ] || { echo "FAIL: the guest wrote no $RAW (run rc=$RUN_RC)" >&2
                   exit 1; }

REPORT="$ROOT/build/${TAG}tools-report.txt"
tr -d '\r' < "$RAW" > "$REPORT"

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="
echo

# ------------------------------------------------------------ assertions ---

FAILED=0
PENDING=0

ok()   { echo "  ok:      $*"; }
bad()  { echo "  FAIL:    $*" >&2; FAILED=$((FAILED + 1)); }
soon() {
    if [ "$STRICT" = "1" ]; then bad "$*"
    else echo "  pending: $*"; PENDING=$((PENDING + 1)); fi
}

# One command's own output, by the line ToolsSmoke printed above it.
block() {
    awk -v want="===== $1 =====" '
        index($0, want) == 1 { inb = 1; next }
        inb && /^----- rc /   { exit }
        inb                   { print }
    ' "$REPORT"
}

# And the return code ToolsSmoke printed below it.  "?" when the command never
# ran, which must not read as a pass.
code() {
    awk -v want="===== $1 =====" '
        index($0, want) == 1 { inb = 1; next }
        inb && /^----- rc /   { print $3; found = 1; exit }
        END { if (!found) print "?" }
    ' "$REPORT"
}

have()    { block "$1" | grep -qiF -- "$2"; }
have_re() { block "$1" | grep -qE  -- "$2"; }

# want CASE PHRASE WHY / deny CASE PHRASE WHY, and the _soon pair for the
# work that is not in yet.
want()      { if have "$1" "$2"; then ok   "$3"; else bad  "$3"; fi; }
deny()      { if have "$1" "$2"; then bad  "$3"; else ok   "$3"; fi; }
want_soon() { if have "$1" "$2"; then ok   "$3"; else soon "$3"; fi; }
# The same, matching a set rather than one string: a neighbour's state depends
# on how long ago it last answered, and every one of them is a pass.
want_re()   { if have_re "$1" "$2"; then ok "$3"; else bad "$3"; fi; }

# failed CASE WHY / worked CASE WHY, the return code, for the cases where a
# command must refuse rather than appear to have an answer.
failed() { if [ "$(code "$1")" != "0" ]; then ok "$2"; else bad "$2"; fi; }
worked() { if [ "$(code "$1")" =  "0" ]; then ok "$2"; else bad "$2"; fi; }

# ---- control: one boot, not a reset dressed up as a hang (RESEARCH 25) ----
if [ -s "$SERIAL" ]; then
    BOOTS=$(grep -c "netstack: starting ThreadX" "$SERIAL" || true)
    BOOT_SRC="the serial log"
else
    BOOTS=$(grep -c "^===== SYS:AddNetInterface eth0 =====" "$REPORT" || true)
    BOOT_SRC="the transcript (no serial output on this host)"
fi
if [ "$BOOTS" -gt 1 ]; then
    bad "THE MACHINE REBOOTED: $BOOT_SRC shows $BOOTS starts -- a command
           crashed hard enough to reset the Amiga, which is not a hang"
elif [ "$BOOTS" -eq 1 ]; then
    ok "the machine booted exactly once, per $BOOT_SRC"
else
    bad "no start found in $BOOT_SRC -- the run did not get far enough to judge"
fi

# ---- control: IPv4 still works, in both modes ----------------------------
want "SYS:AddNetInterface eth0" "10.0.2.15"       "the interface leased 10.0.2.15"
want "SYS:ping 10.0.2.2 -c 2 -t 20" "2 received"  "ping 10.0.2.2 got both replies"
if [ -f "$HD/nc-v4srv.txt" ] &&
   tr -d '\r' < "$HD/nc-v4srv.txt" | grep -qF "hello from the amiga"; then
    ok "nc over 127.0.0.1 delivered the greeting"
else
    bad "nc over 127.0.0.1 did not deliver the greeting -- IPv4 loopback broke"
fi

# ---- control: a real name failure must still name the resolver -----------
#
# The negative assertions below delete a message; this one proves it was only
# deleted where it was wrong.  A command that answers every failure with
# "connection refused" is not an improvement.
want "SYS:nc no.such.host.invalid 80" "cannot resolve" \
     "a name that does not exist is still reported as a name failure"

# ---- both modes: host refuses a literal, for the reason that is true ------
#
# `host` answers through the machine's resolver, and no build has a call that
# reverses an IPv6 address, so the message is the same on both, and both
# assert it.  It used to say "cannot resolve", then blame the spelling.
failed "SYS:host ::1" "host ::1 fails"
want "SYS:host ::1" "is an address, not a name" \
     "host says ::1 is an address rather than looking it up as a name"
want "SYS:host ::1" "nslookup" \
     "host names the command that can ask the question"
deny "SYS:host ::1" "cannot resolve" \
     "host does not hand the literal to the resolver"
deny "SYS:host ::1" "name servers" \
     "host does not blame the name servers for a literal"
deny "SYS:host ::1" "Check the spelling" \
     "host does not blame the spelling of a literal"

# ---- both modes: nslookup asks ip6.arpa, whatever carries the query -------
#
# A PTR question about an IPv6 address is an ordinary DNS question and needs no
# IPv6 to carry it.  Telling the literal from a name used to go through the
# library's inet_pton(), which answers EAFNOSUPPORT on an IPv4-only library --
# so this passed in one build and reported NXDOMAIN in the other.
deny "SYS:nslookup ::1" "there is no such name" \
     "nslookup treats ::1 as an address, not a name"
want "SYS:nslookup ::1" "ip6.arpa" \
     "nslookup asked ip6.arpa for ::1"

if [ "$MODE" = ipv6 ]; then

want "SYS:NetTrace LOOPBACK NOCAPTURE BYTES=65536" "65536" \
     "NetTrace moved 65536 bytes over loopback"

# ---- v6: a literal must not be handed to the resolver -------------------
#
# 66's exact defect.  The address is a literal in every one of these, so
# nothing here should mention names, spelling or name servers.
for c in "SYS:ping ::1 -c 2 -t 20" \
         "SYS:ping fd00::10 -c 2 -t 20" \
         "SYS:ping fe80::2 -c 2 -t 20" \
         "SYS:ping 2001:db8::1 -c 1 -t 5" \
         "SYS:nc ::1 7099 -v -w 10 -N <DH0:greeting.txt" \
         "SYS:telnet ::1 7097 QUIET <DH0:greeting.txt" \
         "SYS:whois example.com SERVER ::1 PORT 7096" \
         "SYS:tftp ::1 GET nosuchfile PORT 7095 TIMEOUT 5" \
         "SYS:traceroute ::1 -m 2 -q 1 -w 3 -n"
do
    short=${c#SYS:}
    short=${short%% *}
    addr=$(printf '%s\n' "$c" | awk '{ for (i = 1; i <= NF; i++)
                                           if ($i ~ /:.*:/) { print $i; exit } }')
    deny "$c" "cannot resolve"     "$short does not try to resolve $addr"
    deny "$c" "name servers"       "$short does not blame the name servers for $addr"
    deny "$c" "Check the spelling" "$short does not blame the spelling of $addr"
done

# ---- v6: and the traffic actually moved ----------------------------------
if [ -f "$HD/nc-v6srv.txt" ] &&
   tr -d '\r' < "$HD/nc-v6srv.txt" | grep -qF "hello from the amiga"; then
    ok "nc over ::1 delivered the greeting"
else
    bad "nc over ::1 did not deliver the greeting"
fi

want "SYS:tftp ::1 GET nosuchfile PORT 7095 TIMEOUT 5" "from ::1" \
     "tftp reached ::1 and reported the transfer, not the name"

# ---- v6: ping and traceroute answer -------------------------------------
want "SYS:ping ::1 -c 2 -t 20"        "2 received" "ping ::1 got both replies"
want "SYS:ping fd00::10 -c 2 -t 20"   "2 received" \
     "ping fd00::10 answered -- the configured ADDRESS6 is live"
want "SYS:ping fe80::2 -c 2 -t 20"    "2 received" \
     "ping fe80::2 crossed the wire to SLIRP's router"

want "SYS:traceroute ::1 -m 2 -q 1 -w 3 -n" " ms" \
     "traceroute ::1 timed a hop"
deny "SYS:traceroute ::1 -m 2 -q 1 -w 3 -n" "did not answer" \
     "traceroute ::1 reached its destination"

# ---- display: 66's acceptance test --------------------------------------
#
# The stack really does configure this, an INFO-level library prints
# "netstack: eth0 fd00::10/64" in the same boot, and until NETSTATUS_ADDRESSES6
# no shipped command showed it, so a user could not tell whether ADDRESS6 took.
want "SYS:ShowNetStatus ALL"        "fd00::10" \
     "ShowNetStatus ALL shows the configured fd00::10"
want "SYS:ShowNetStatus INTERFACES" "fd00::"   \
     "ShowNetStatus INTERFACES shows an IPv6 address"
want "SYS:netstat -i"               "fd00::"   \
     "netstat -i shows an IPv6 address"
want "SYS:ShowNetStatus ALL"        "fe80::"   \
     "ShowNetStatus ALL shows the link-local address"

# ---- routes: AddNetRoute / DeleteNetRoute take an IPv6 prefix ------------
#
# 66.4's last item.  Two mechanisms and no third: NetX Duo has no
# destination-to-next-hop table for IPv6, so a default router and an on-link
# prefix are what there is to write, and a prefix given a next hop must be
# refused with the reason rather than accepted and dropped.

deny "SYS:AddNetRoute DEFAULTGATEWAY fd00::99" "four numbers with dots" \
     "AddNetRoute no longer answers an IPv6 literal with the IPv4 advice"
want "SYS:AddNetRoute DEFAULTGATEWAY fd00::99" "fd00::99" \
     "AddNetRoute set an IPv6 default router"
# Three seconds after it was added, which is the point: the once-a-second tick
# in nxd_ipv6_prefix_router_timer_tick.c reads lifetime 0 as expired and drops
# the entry, so a router asked for with the wrong constant is gone before any
# command can print it.  This is the same call CONFIGURE6's GATEWAY6 makes.
want "SYS:netstat -r" "fd00::99" \
     "the IPv6 default route is still there three seconds later"
want "SYS:netstat -r" "::/0" \
     "netstat -r writes an IPv6 default route as ::/0"
want "SYS:netstat -r" "fd00::/64" \
     "netstat -r shows the on-link IPv6 prefix"
want "SYS:netstat -r" "10.0.2.0" \
     "netstat -r still shows the IPv4 table beside it"
want "SYS:DeleteNetRoute DEFAULTGATEWAY fd00::99" "is gone" \
     "DeleteNetRoute removed the IPv6 default router"

want "SYS:AddNetRoute DESTINATION fd00:9::/64 GATEWAY fe80::2" \
     "no table that maps a prefix to a next hop" \
     "AddNetRoute says why a per-prefix IPv6 next hop cannot be stored"

want "SYS:AddNetRoute DESTINATION 2001:db8::/64" "go straight there" \
     "AddNetRoute added an on-link IPv6 prefix"
want "SYS:ShowNetStatus ROUTES" "2001:db8::/64" \
     "ShowNetStatus ROUTES shows the prefix that was added"
want "SYS:DeleteNetRoute DESTINATION 2001:db8::/64" "is gone" \
     "DeleteNetRoute removed the on-link prefix"

want "SYS:AddNetRoute DEFAULTGATEWAY fe80::1" "fe80::1" \
     "AddNetRoute took a link-local router with one interface up"

# ---- the route changed where the packet went ----------------------------
#
# This is the assertion the rest of the route work is for.  2001:db8::1 is not
# on this link until the prefix above says it is, so packets for it are handed
# to a router and neighbour discovery never asks about the address itself.
# Once it is on link the stack solicits 2001:db8::1 directly, nothing answers,
# and the cache gains an entry that did not exist before.
#
# Two things make this a test of the route rather than of the cache.  The
# earlier `ping 2001:db8::1` sent to that address long before the route
# existed, so a stack that did not discard its IPv6 destination cache would
# still be handing the packets to the router and no entry would ever appear.
# And the sample is taken while `nc` is still trying: an unanswered neighbour
# entry is deleted after its third solicitation, so the connection attempt is
# left running across the `arp` that reads it.
deny "SYS:arp" "2001:db8::1" \
     "before the route there is no neighbour entry for 2001:db8::1"
want "SYS:arp 2001:db8::1" "INCOMPLETE" \
     "after the route the stack solicited 2001:db8::1 itself"
want "SYS:arp 2001:db8::1" "no reply" \
     "and nothing answered, which is what an unreachable neighbour looks like"

# ---- arp: the neighbour cache -------------------------------------------
want "SYS:arp" "Neighbour" "arp has a neighbour section"
want "SYS:arp" "fe80::2"   "arp lists SLIRP's router as a neighbour"
want "SYS:arp" "router"    "arp says which neighbour is a router"
want_re "SYS:arp" "(INCOMPLETE|REACHABLE|STALE|DELAY|PROBE|CREATED)" \
     "arp names the neighbour discovery state"
want_re "SYS:arp" "^  (INCOMPLETE|REACHABLE|STALE|DELAY|PROBE|CREATED) +[a-z]" \
     "arp spells out what that state means"
want "SYS:arp" "Address          Hardware address" \
     "arp still prints the ARP cache above it"
want "SYS:arp" "10.0.2.2"  "arp still lists the IPv4 entries"
want "SYS:arp 2001:db8::2" "reached through a router" \
     "arp explains an address neighbour discovery can never reach"

else # ------------------------------------------------------ ipv4 mode ----

# ---- the shape every degraded answer has to have -------------------------
#
# Three things at once, and the first is what stops a command passing by going
# quiet: it must refuse, it must give IPv6 as the reason, and it must not
# reach for the resolver's vocabulary.  A command that printed nothing, or
# that failed for a reason it kept to itself, fails here.
degrades() {
    failed "$1" "$2 refuses rather than appearing to have an answer"
    want "$1" "IPv6"              "$2 gives IPv6 as the reason"
    deny "$1" "cannot resolve"    "$2 does not hand the literal to the resolver"
    deny "$1" "name servers"      "$2 does not blame the name servers"
    deny "$1" "Check the spelling" "$2 does not blame the spelling"
}

# ---- every command that takes a host ------------------------------------
degrades "SYS:ping ::1 -c 2 -t 20"                       "ping ::1"
degrades "SYS:ping fe80::2 -c 2 -t 20"                   "ping fe80::2"
degrades "SYS:traceroute ::1 -m 2 -q 1 -w 3 -n"          "traceroute ::1"
degrades "SYS:nc ::1 7099 -v -w 10 -N"                   "nc ::1"
degrades "SYS:telnet ::1 7097 QUIET"                     "telnet ::1"
degrades "SYS:tftp ::1 GET nosuchfile PORT 7095 TIMEOUT 5" "tftp ::1"
degrades "SYS:whois example.com SERVER ::1 PORT 7096"    "whois SERVER ::1"
degrades "SYS:sntp fd00::10 TIMEOUT 5"                   "sntp fd00::10"
degrades "SYS:fetch http://[::1]/index.html"             "fetch of an IPv6 URL"

# The eight above share tool_sock_resolve(), so one of them proves the exact
# words rather than only their shape.
want "SYS:ping ::1 -c 2 -t 20" "this machine's network has no IPv6" \
     "the shared refusal names the machine's network, not the address"

# ---- nslookup: the lookup this machine can still do ----------------------
#
# An ip6.arpa PTR travels over IPv4 like any other question, so this is not a
# degradation case at all, it is the same answer the IPv6 build gives, and
# the assertions above are shared with it.  Asserted here as well: it fails
# for a wholly different reason than the commands above, and that is the point.
# The dotted quad beside it, so the two reverse forms are asserted together.
# Whether the server has a PTR for 10.0.2.3 is SLIRP's business and not this
# test's: what is asserted is the question, which the command builds itself.
want "SYS:nslookup 10.0.2.3" "in-addr.arpa" \
     "nslookup asked in-addr.arpa for 10.0.2.3"
deny "SYS:nslookup 10.0.2.3" "is not a name the DNS can be asked about" \
     "nslookup read 10.0.2.3 as an address too"

# ---- arp: a valid address the stack has no cache for ---------------------
failed "SYS:arp ::1" "arp ::1 refuses"
want "SYS:arp ::1" "the running stack has no IPv6" \
     "arp says the stack has no IPv6, in AddNetRoute's words"
want "SYS:arp ::1" "well-formed IPv6 address" \
     "arp grants that the address itself is good"
deny "SYS:arp ::1" "is not an address" \
     "arp no longer calls a valid IPv6 address invalid"
deny "SYS:arp ::1" "cannot resolve" \
     "arp does not reach for the resolver"

# A typo is still a typo, and must not be excused by the build.
failed "SYS:arp fe80::zz" "arp fe80::zz refuses"
want "SYS:arp fe80::zz" "is not an address" \
     "arp still rejects an IPv6 address that is not one"
deny "SYS:arp fe80::zz" "the running stack has no IPv6" \
     "arp does not blame the build for a malformed address"

worked "SYS:arp 10.0.2.2" "arp still answers about an IPv4 address"
want "SYS:arp" "Address          Hardware address" \
     "arp prints the ARP cache"
want "SYS:arp" "10.0.2.3" "arp lists the IPv4 entries"
deny "SYS:arp" "Neighbour" \
     "arp prints no empty neighbour section on a machine with no neighbours"

# ---- the tables: IPv4 alone, with no hole where IPv6 would be ------------
want "SYS:netstat -i" "10.0.2.15" "netstat -i shows the leased address"
deny "SYS:netstat -i" "fd00::"    "netstat -i shows no IPv6 address"
deny "SYS:netstat -i" "fe80::"    "netstat -i shows no link-local address"
want "SYS:netstat -r" "10.0.2.0"  "netstat -r shows the IPv4 table"
deny "SYS:netstat -r" "::/0"      "netstat -r shows no IPv6 default route"
deny "SYS:netstat -r" "fd00::/64" "netstat -r shows no IPv6 prefix"

want "SYS:ShowNetStatus ALL" "10.0.2.15" \
     "ShowNetStatus ALL shows the leased address"
want "SYS:ShowNetStatus ALL" "No problems found" \
     "ShowNetStatus ALL still finds nothing wrong"
deny "SYS:ShowNetStatus ALL" "fd00::" \
     "ShowNetStatus ALL claims no address the machine does not have"
deny "SYS:ShowNetStatus INTERFACES" "fe80::" \
     "ShowNetStatus INTERFACES claims no link-local address"
# The interface file asks for ADDRESS6 and the interface comes up anyway.
want "SYS:AddNetInterface eth0" "online" \
     "AddNetInterface brings eth0 up despite an ADDRESS6 it cannot honour"

# ---- AddNetRoute / DeleteNetRoute: the standard the other three match ----
for c in "SYS:AddNetRoute DEFAULTGATEWAY fd00::99" \
         "SYS:AddNetRoute DESTINATION fd00:9::/64" \
         "SYS:DeleteNetRoute DEFAULTGATEWAY fd00::99"
do
    short=${c#SYS:}
    short=${short%% *}
    failed "$c" "$short refuses an IPv6 route"
    want "$c" "the running stack has no IPv6" \
         "$short says the stack has no IPv6"
    want "$c" "can be switched on" \
         "$short says it is a build option and not a setting"
    want "$c" "ShowNetStatus INTERFACES" \
         "$short says where to look for the addresses this machine has"
    deny "$c" "four numbers with dots" \
         "$short does not answer an IPv6 literal with the IPv4 advice"
    deny "$c" "cannot resolve" \
         "$short does not hand the literal to the resolver"
done

# ---- control: the IPv4 half of the same commands still works ------------
worked "SYS:AddNetRoute DESTINATION 192.168.77.0/24 GATEWAY 10.0.2.2" \
       "AddNetRoute still adds an IPv4 route"
want "SYS:netstat -r" "192.168.77.0" \
     "netstat -r shows the IPv4 route that was added"
want "SYS:ShowNetStatus ROUTES" "192.168.77.0" \
     "ShowNetStatus ROUTES shows it too"
want "SYS:DeleteNetRoute DESTINATION 192.168.77.0/24" "is gone" \
     "DeleteNetRoute removed the IPv4 route"

fi

# --------------------------------------------------------------- verdict ---

echo
if [ "$FAILED" -gt 0 ]; then
    echo "FAILED: $FAILED assertion(s), $PENDING pending"
    exit 1
fi
if [ "$PENDING" -gt 0 ]; then
    echo "PENDING: $PENDING assertion(s) waiting on the tools work; nothing failed"
    exit 3
fi
echo "PASS: everything asserted, nothing pending"
exit 0
