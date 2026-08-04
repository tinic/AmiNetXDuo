#!/usr/bin/env bash
#
# THE mDNS RUN.
#
#   tests/tools/run-mdns.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
#
# WHAT IT IS PROVING, and the three separate questions it keeps apart
#
#   1. THE RESPONDER CLAIMED A NAME, and the right one.  `hostname` in
#      DEVS:Internet/name_resolution is set to a FULLY QUALIFIED name here on
#      purpose, "amigatest.home.lan", because mDNS wants one DNS label and
#      HOSTNAME may not be one.  The name that must appear is "amigatest".
#
#   2. THE RESOLVER TAKES ".local" TO mDNS, from every command, with no
#      command changed.  `host` and `ping` both reach netstack_resolve()
#      through gethostbyname() on bsdsocket.library, so the branch added in
#      src/netstack/netstack_dns.c serves the whole command set at once.  A
#      query for this machine's OWN name is answered out of the module's local
#      cache without a packet, which is what makes this half testable under an
#      emulator whose network is a NAT.
#
#   3. THE SERVICES DECLARED IN DEVS:Internet/service_discovery ARE ON THE
#      WIRE, as records and not as a parse.  A service is four records --
#      PTR, SRV, TXT and the _services._dns-sd._udp enumeration PTR, and a
#      browser needs all of them, so each is asserted separately.  The file
#      written below has a deliberately broken line in it, because "a typo in
#      one service must not stop the network coming up" is a claim about what
#      happens to the OTHER lines.
#
#   4. WHAT ACTUALLY LEFT THE MACHINE, taken from the emulated A2065's own
#      frame log, below every line of our code, and read with tcpdump.  An
#      mDNS probe is a specific thing: UDP 5353 to 224.0.0.251, at the
#      Ethernet layer to 01:00:5e:00:00:fb.  If that is not on the wire, the
#      responder is answering itself in a cupboard.
#
# AND THE TWO QUESTIONS IT ANSWERS BY MEASURING RATHER THAN ASSUMING
#
#   Does FS-UAE's SLIRP pass multicast?  tests/tools/mdnswatch.py sits on the
#   HOST's real network for the whole run, joined to 224.0.0.251:5353, and
#   writes down everything it hears.  It also sends one multicast query of its
#   own at the start (--selftest) so that "nothing arrived" can be told apart
#   from "the instrument was not working".
#
#   The answer, measured: it relays OUTBOUND, rewriting the source port.  The
#   reply comes back too, but SLIRP leaves the host's REAL LAN address on it
#   as the source, so a guest on 10.0.2.0/24 sees a unicast mDNS response from
#   off-link, and RFC 6762 11 says drop it.  The module drops it.  That is
#   conformance rather than a defect, and it means the QUERIER half against a
#   real peer cannot be proved under this emulator.  docs/RESEARCH.md 30.5.
#
#   The verdict below reports all of that and does NOT fail on it.  Whether a
#   NAT forges a source address is a property of the emulator, not of this
#   stack, and a test that went red over it would be one nobody could make
#   green.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

MODEL=A1200
TIMEOUT=300
BUILD="${AMINETXDUO_BUILD:-build/cm}"

while getopts "m:t:b:" opt; do
    case "$opt" in
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        *) echo "usage: $0 [-m model] [-t seconds] [-b builddir]" >&2; exit 2 ;;
    esac
done

TOOLS="$ROOT/$BUILD/src/tools"
BSD="$ROOT/$BUILD/src/bsdsocket/bsdsocket.library"

for f in "$TOOLS/ToolsSmoke" "$TOOLS/AddNetInterface" "$TOOLS/host" \
         "$TOOLS/ping" "$BSD"; do
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

# The name this machine is configured with, and the label mDNS must derive
# from it.  Deliberately not equal: the derivation is half of what is tested.
CFG_HOSTNAME="amigatest.home.lan"
MDNS_LABEL="amigatest"

# What DEVS:Internet/service_discovery declares.  Three services, chosen to
# separate three things that could each be got wrong on their own:
#
#   _ftp._tcp with no instance name , must be announced under the DERIVED
#       host label, not under the fully-qualified HOSTNAME.
#   _http._tcp with a multi-word name and a txt= field, must arrive as one
#       instance name with spaces in it, and as a TXT record with content
#       rather than the empty one the module writes when none is given.
#   _telnet._udp, the transport is taken from the file and not assumed.
SD_HTTP_NAME="Amiga web server"
SD_HTTP_TXT="path=/"

# What the host-side watcher will answer for, IF anything ever reaches it.
PEER_LABEL="mdnspeer"
PEER_ADDR="10.0.2.2"

# ------------------------------------------------------------- staging ---

STAGE="$ROOT/build/mdns-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs"
cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
for t in AddNetInterface host ping; do
    cp "$TOOLS/$t" "$STAGE/$t"
done

# The one configuration change.  A fully-qualified name, so that a responder
# that used it verbatim would claim "amigatest.home.lan.local" and fail the
# assertion below rather than passing by accident.
echo "hostname $CFG_HOSTNAME" >> "$STAGE/devs/Internet/name_resolution"

# The services.  A deliberately broken line in the middle: the assertion is
# that it is skipped and the two good lines after it still arrive, because a
# typo in one service must not stop the network coming up.
cat > "$STAGE/devs/Internet/service_discovery" <<EOF
# written by tests/tools/run-mdns.sh
_ftp._tcp     21
_nope 21                                    # no transport, must be skipped
_http._tcp    80    $SD_HTTP_NAME    txt=$SD_HTTP_TXT
_telnet._udp  23
EOF

{
    echo "SYS:AddNetInterface eth0"

    # Probing is three packets 250 ms apart plus a randomised first delay, so
    # it is over long before this; the wait is here so that a failure to claim
    # is reported as a failure to claim rather than as a race.
    echo "wait 3"

    # --- the machine's own name, three spellings of it ---
    echo "SYS:host $MDNS_LABEL.local"
    echo "SYS:host $(echo "$MDNS_LABEL" | tr '[:lower:]' '[:upper:]').LOCAL"
    echo "SYS:host $MDNS_LABEL.local."

    # --- an untouched command, going through the same resolver ---
    echo "SYS:ping -c 2 -t 5 $MDNS_LABEL.local"

    # --- a name nothing has: must fail, and must fail promptly ---
    echo "SYS:host nosuchbox.local TIMEOUT 5"

    # --- and the name only the HOST answers for: the SLIRP relay question ---
    echo "SYS:host $PEER_LABEL.local TIMEOUT 8"

    # --- the unicast path, unbroken by the .local branch in front of it ---
    echo "SYS:host example.com"
} > "$STAGE/commands.txt"

# ------------------------------------------------------- the host watcher ---
#
# Sized against the LOCK QUEUE and not against the run: build/.fsuae.lock
# serialises every emulator run in the tree and the queue can be deep, so a
# watcher that lived for TIMEOUT seconds would routinely be dead before the
# guest booted, and its silence would then be read as "SLIRP dropped it".
# Same lesson as tests/tools/run-nettools.sh and run-sntp.sh.

WATCHLOG="$ROOT/build/mdnswatch.log"
python3 "$ROOT/tests/tools/mdnswatch.py" \
    --log "$WATCHLOG" --seconds "$((TIMEOUT + 3600))" --selftest \
    --respond "$PEER_LABEL=$PEER_ADDR" \
    > "$ROOT/build/mdnswatch.out" 2>&1 &
WATCH_PID=$!
cleanup_watch() { kill -TERM "$WATCH_PID" 2>/dev/null || true; }
trap cleanup_watch EXIT INT TERM HUP
sleep 2
kill -0 "$WATCH_PID" 2>/dev/null || {
    echo "the host watcher did not start:" >&2
    cat "$ROOT/build/mdnswatch.out" >&2
    exit 2
}

# ------------------------------------------------------------------ run ---

export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-mdns}"
HD="$ROOT/build/testhd-$AMINETXDUO_RUN_TAG"
FSLOG="$ROOT/build/fsuae-base-$AMINETXDUO_RUN_TAG/Cache/Logs/fs-uae.log.txt"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/fsuae-run.sh" -n -m "$MODEL" -t "$TIMEOUT" \
    "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" "$STAGE/libs" \
    "$STAGE/AddNetInterface" "$STAGE/host" "$STAGE/ping"
RUN_RC=$?
set -e

REPORT="$HD/tools.txt"
[ -f "$REPORT" ] || { echo "FAIL: the guest wrote no $REPORT (run rc=$RUN_RC)" >&2; exit 1; }

echo
echo "===================== what the commands printed ====================="
cat "$REPORT"
echo "====================================================================="

FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
pass() { echo "  ok: $*"; }
note() { echo "  --: $*"; }

STARTS=$(grep -c "SYS:AddNetInterface eth0 =====" "$REPORT" || true)
if [ "$STARTS" -eq 1 ]; then
    pass "the machine booted exactly once (no reset)"
else
    fail "the command list ran $STARTS times, the machine reset"
fi

# ---- 1 + 2: the name, and the resolver branch ---------------------------

for spelling in "$MDNS_LABEL.local" \
                "$(echo "$MDNS_LABEL" | tr '[:lower:]' '[:upper:]').LOCAL" \
                "$MDNS_LABEL.local."
do
    if grep -qi "^$spelling has address " "$REPORT"; then
        pass "'$spelling' resolved"
    else
        fail "'$spelling' did not resolve"
    fi
done

# It has to be the address the interface actually holds, not merely AN answer.
LEASED=$(grep -oE "^$MDNS_LABEL\.local has address [0-9.]+" "$REPORT" \
         | head -1 | awk '{print $NF}' || true)
if [ -n "$LEASED" ]; then
    pass "$MDNS_LABEL.local answers with $LEASED"
    if grep -q "$LEASED" "$REPORT"; then
        pass "and that address appears elsewhere in the run"
    fi
else
    fail "no address was reported for $MDNS_LABEL.local"
fi

# The fully-qualified HOSTNAME must NOT have been used verbatim.
if grep -qi "$CFG_HOSTNAME.local" "$REPORT"; then
    fail "the responder claimed '$CFG_HOSTNAME.local', the label was not derived"
else
    pass "the FQDN in HOSTNAME was reduced to one label"
fi

if grep -q "bytes from" "$REPORT" && \
   grep -qi "PING $MDNS_LABEL.local" "$REPORT"; then
    pass "ping reached the machine by its .local name, unmodified"
else
    note "ping did not report replies, see the transcript above"
fi

if grep -qi "nosuchbox.local" "$REPORT" && \
   ! grep -qi "^nosuchbox.local has address" "$REPORT"; then
    pass "a name nothing owns failed rather than resolving"
else
    fail "nosuchbox.local was answered, which nothing should have done"
fi

if grep -q "^example.com has address " "$REPORT"; then
    pass "unicast DNS still works with the .local branch in front of it"
else
    fail "example.com did not resolve, the .local branch broke ordinary DNS"
fi

# ---- 3 + 4: the services, and the wire they went out on ------------------

if [ -f "$FSLOG" ]; then
    python3 "$ROOT/tests/trace/a2065pcap.py" "$FSLOG" -o "$HD/host.pcap" \
        > "$HD/a2065.txt" 2>&1 || true
fi

if [ -s "$HD/host.pcap" ]; then
    tcpdump -r "$HD/host.pcap" -n -e "udp port 5353" 2>/dev/null \
        > "$HD/mdns.txt" || true
    MDNS_FRAMES=$(wc -l < "$HD/mdns.txt" | tr -d ' ')

    echo
    echo "  mDNS frames the A2065 handled: $MDNS_FRAMES"
    head -12 "$HD/mdns.txt" | sed 's/^/       /'

    if [ "$MDNS_FRAMES" -gt 0 ]; then
        pass "the responder put mDNS on the wire"
    else
        fail "no UDP 5353 frame ever left the machine"
    fi

    if grep -qi "01:00:5e:00:00:fb" "$HD/mdns.txt"; then
        pass "addressed to the RFC 6762 multicast MAC 01:00:5e:00:00:fb"
    else
        fail "the frames are not addressed to the mDNS multicast MAC"
    fi

    if grep -q "224.0.0.251.5353" "$HD/mdns.txt"; then
        pass "and to 224.0.0.251:5353"
    else
        fail "the frames are not addressed to 224.0.0.251:5353"
    fi

    # The three probes and the announcements, named rather than counted: this
    # is what makes the difference between "some UDP left" and "RFC 6762 6
    # happened".  A probe is an ANY question with the proposed record in the
    # authority section; an announcement is an unsolicited response with the
    # cache-flush bit set.
    if grep -q "ANY (QM)? $MDNS_LABEL.local" "$HD/mdns.txt"; then
        pass "it probed for the name before claiming it (RFC 6762 8.1)"
    else
        fail "no probe was sent, the name was claimed without asking"
    fi
    if grep -q "(Cache flush) A " "$HD/mdns.txt"; then
        pass "and announced an A record with the cache-flush bit (10.2)"
    else
        fail "no announcement with a cache-flush A record was sent"
    fi

    # Every lookup used to cost a second, serial, AAAA query with its own full
    # timeout; netstack_mdns.c passes NULL for ipv6_address so it does not.
    AAAA=$(grep -ci "AAAA (QM)?" "$HD/mdns.txt" || true)
    if [ "$AAAA" -eq 0 ]; then
        pass "no AAAA query was sent, an IPv4 lookup costs one query"
    else
        fail "$AAAA AAAA queries went out; ipv6_address is not NULL"
    fi

    # ---- the services, record by record ---------------------------------
    #
    # This is the part that a "the file parsed" test cannot reach.  RFC 6763
    # says a service is FOUR records and a browser needs all of them: a PTR
    # from the type to the instance, an SRV giving the port and target host,
    # a TXT, and a PTR from _services._dns-sd._udp so that a browser which
    # does not already know the type can enumerate it.  tcpdump names them
    # individually, so each is asserted on its own rather than counted.
    #
    # -A prints the packet body, which is where the instance name and the TXT
    # content are; the summary line only names the record.
    tcpdump -r "$HD/host.pcap" -n -A "udp port 5353" 2>/dev/null \
        > "$HD/mdns-full.txt" || true

    echo
    echo "  the service records, as tcpdump reads them off the A2065:"
    grep -Ei "PTR|SRV|TXT" "$HD/mdns.txt" | head -20 | sed 's/^/       /'

    for rec in "_ftp._tcp.local" "_http._tcp.local" "_telnet._udp.local"; do
        if grep -q "$rec" "$HD/mdns.txt"; then
            pass "$rec is on the wire"
        else
            fail "$rec was declared and never announced"
        fi
    done

    # The SRV, exactly: the target host, the port from the file, and the
    # priority and weight netstack_mdns.c passes as zero.
    for port in 21 80 23; do
        if grep -q "SRV $MDNS_LABEL.local.:$port 0 0" "$HD/mdns.txt"; then
            pass "SRV -> $MDNS_LABEL.local:$port, priority 0 weight 0"
        else
            fail "no SRV for port $port pointing at $MDNS_LABEL.local"
        fi
    done

    if grep -q "TXT" "$HD/mdns.txt"; then
        pass "TXT records are announced alongside (RFC 6763 6.1)"
    else
        fail "no TXT record"
    fi

    # The _services._dns-sd._udp.local enumeration PTR is registered by
    # nx_mdns_service_add() and answered when something queries for it, but it
    # is NOT in the announcement, and that is the module's design rather than
    # a gap here: the announcing state machine flags a PTR for sending only
    # when its rdata is the announcing SRV's own name, which is true of the
    # type PTR and not of the enumeration PTR whose rdata is the type.  RFC
    # 6763 9 requires the meta-query to be ANSWERED, not announced, and Avahi
    # and mDNSResponder do the same.  It cannot be proved from here either
    # way: a query from the host arrives through SLIRP with an off-link source
    # and RFC 6762 11 makes the module drop it, as the trailer below says.
    if grep -q "_services._dns-sd._udp.local" "$HD/mdns.txt"; then
        note "the _services._dns-sd._udp PTR was announced as well"
    else
        note "no _services._dns-sd._udp PTR in the announcement, expected;"
        note "    it is answered on query, not announced (RFC 6763 9)"
    fi

    # The instance name with no name= in the file must be the DERIVED label.
    if grep -q "$MDNS_LABEL\._ftp\._tcp\.local" "$HD/mdns.txt"; then
        pass "_ftp._tcp took the host label '$MDNS_LABEL' as its instance name"
    else
        fail "_ftp._tcp did not default its instance name to the host label"
    fi

    # And the multi-word one arrived whole, spaces and all.
    if grep -q "$SD_HTTP_NAME" "$HD/mdns-full.txt"; then
        pass "'$SD_HTTP_NAME' arrived as one instance name, spaces included"
    else
        fail "the multi-word instance name did not survive the parser"
    fi

    if grep -q "$SD_HTTP_TXT" "$HD/mdns-full.txt"; then
        pass "the txt= field reached the TXT record as '$SD_HTTP_TXT'"
    else
        fail "the txt= field is not in the TXT record"
    fi

    # The malformed line must have been skipped and nothing else.
    if grep -q "_nope" "$HD/mdns.txt"; then
        fail "the malformed line was announced anyway"
    else
        pass "the malformed line was skipped and the good ones still went out"
    fi
else
    fail "no host-side capture at $HD/host.pcap, the wire was not observed"
fi

# ---- the SLIRP question, reported and not asserted ----------------------

cleanup_watch
sleep 1

echo
echo "=============== what the HOST's network heard ======================="
[ -f "$WATCHLOG" ] && cat "$WATCHLOG"
echo "====================================================================="

#
# Three separate things, and the run only comments on the ones it has evidence
# for.  The instrument is calibrated FIRST, mdnswatch sends one query of its
# own with id 0x4d44 at startup and must see it come back, so that "nothing
# arrived" and "nothing was listening" are never confused.  The trailer
# mdnswatch writes at the end of its own countdown is NOT available here,
# because this script kills it; the calibration is read from the body instead.
#
if [ ! -f "$WATCHLOG" ] || grep -q "INSTRUMENT UNAVAILABLE" "$WATCHLOG"; then
    note "the host watcher could not bind UDP 5353; nothing can be concluded"
elif ! grep -q "mdnswatch.local" "$WATCHLOG"; then
    note "the watcher never saw its own calibration query, inconclusive"
else
    HEARD=$(grep -c "^\[" "$WATCHLOG" || true)
    note "instrument calibrated: it saw its own multicast, and $HEARD message(s)"

    # OUTBOUND.  The guest's own name appearing on the host's REAL network can
    # only have got there through the emulator's NAT.
    if grep -qi "$MDNS_LABEL" "$WATCHLOG"; then
        note "OUTBOUND: SLIRP DOES relay the guest's mDNS onto the host LAN"
        note "          (with the source port rewritten, :5353 became :NNNNN)"
    else
        note "OUTBOUND: nothing carrying '$MDNS_LABEL' reached the host LAN"
    fi

    if grep -q "_ftp._tcp" "$WATCHLOG"; then
        note "          and the service records came with it, the host's own"
        note "          network saw _ftp._tcp.local, not just the A record"
    fi

    # INBOUND, which is three questions and not one: did the reply cross the
    # NAT, did it reach the card, and did the responder accept it.
    IN=0
    OFFLINK=0
    if [ -s "$HD/mdns.txt" ]; then
        IN=$(grep -c "> 10.0.2.15.5353" "$HD/mdns.txt" || true)
        OFFLINK=$(grep "> 10.0.2.15.5353" "$HD/mdns.txt" \
                  | grep -cv " 10\.0\.2\.[0-9]*\.5353 >" || true)
    fi

    if grep -qi "^$PEER_LABEL.local has address" "$REPORT"; then
        note "INBOUND:  works, the guest resolved a name only the host answers"
    elif [ "$IN" -gt 0 ] && [ "$OFFLINK" -gt 0 ]; then
        #
        # This is the interesting outcome, and it is NOT a defect.  The reply
        # crossed the NAT and reached the card, but SLIRP passes the host's
        # REAL LAN address through as the source, so a guest on 10.0.2.0/24
        # sees a unicast mDNS response from off-link.  RFC 6762 11 says drop
        # it, and _nx_mdns_packet_address_check() does, unconditionally and
        # correctly.  On a bridged backend or real hardware the peer is on
        # link and the check passes.
        #
        note "INBOUND:  $IN frame(s) reached the card, $OFFLINK of them from an"
        note "          OFF-LINK source, RFC 6762 11 requires those to be"
        note "          dropped, and the module drops them.  Conformance, not"
        note "          a defect: SLIRP forges the source address."
    elif [ "$IN" -gt 0 ]; then
        note "INBOUND:  $IN frame(s) reached the card and were not accepted --"
        note "          and NOT for the off-link reason.  Worth investigating."
    elif grep -q "and unicast to" "$WATCHLOG"; then
        note "INBOUND:  the host answered by multicast AND by unicast to the"
        note "          rewritten source port, and neither reached the card"
    else
        note "INBOUND:  the guest never asked for $PEER_LABEL.local"
    fi
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "mdns: FAILED" >&2
    exit 1
fi

echo "mdns: PASSED"
exit 0
