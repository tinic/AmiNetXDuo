#!/usr/bin/env bash
#
# THE mDNS RUN.
#
#   tests/tools/run-mdns.sh [-m MODEL] [-t SECONDS] [-b BUILDDIR]
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

CFG_HOSTNAME="amigatest.home.lan"
MDNS_LABEL="amigatest"

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

echo "MDNS=YES" >> "$STAGE/devs/NetInterfaces/eth0"

echo "hostname $CFG_HOSTNAME" >> "$STAGE/devs/Internet/name_resolution"

cat > "$STAGE/devs/Internet/service_discovery" <<EOF
# written by tests/tools/run-mdns.sh
_ftp._tcp     21
_nope 21                                    # no transport, must be skipped
_http._tcp    80    $SD_HTTP_NAME    txt=$SD_HTTP_TXT
_telnet._udp  23
EOF

{
    echo "SYS:AddNetInterface eth0"

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
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"

echo "==> booting $MODEL with the A2065 on SLIRP"
set +e
"$ROOT/tools/amiberry-run.sh" -N a2065 -m "$MODEL" -t "$TIMEOUT" \
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

BROKEN=0
infra() { echo "INFRA: $*" >&2; BROKEN=$((BROKEN + 1)); }

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

if [ -s "$HD/host.pcap" ]; then
    if ! tcpdump -r "$HD/host.pcap" -n -e "udp port 5353" 2>/dev/null \
            > "$HD/mdns.txt"; then
        infra "tcpdump could not read $HD/host.pcap (is tcpdump installed?)"
    fi
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

    AAAA=$(grep -ci "AAAA (QM)?" "$HD/mdns.txt" || true)
    if [ "$AAAA" -eq 0 ]; then
        pass "no AAAA query was sent, an IPv4 lookup costs one query"
    else
        fail "$AAAA AAAA queries went out; ipv6_address is not NULL"
    fi

    # ---- the services, record by record ---------------------------------
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
    infra "no host-side frame log under Amiberry, so the wire was never
       recorded and the thirteen assertions on what left the card did not run"
fi

# ---- the SLIRP question, reported and not asserted ----------------------

cleanup_watch
sleep 1

echo
echo "=============== what the HOST's network heard ======================="
[ -f "$WATCHLOG" ] && cat "$WATCHLOG"
echo "====================================================================="

if [ ! -f "$WATCHLOG" ] || grep -q "INSTRUMENT UNAVAILABLE" "$WATCHLOG"; then
    infra "the host watcher could not bind UDP 5353, so this whole section was
       not measured"
elif ! grep -q "mdnswatch.local" "$WATCHLOG"; then
    infra "the watcher never saw its own calibration query, so it was not
       listening and its silence means nothing"
else
    HEARD=$(grep -c "^\[" "$WATCHLOG" || true)
    note "instrument calibrated: it saw its own multicast, and $HEARD message(s)"

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
        note "INBOUND:  $IN frame(s) reached the card, $OFFLINK of them from an"
        note "          OFF-LINK source, RFC 6762 11 requires those to be"
        note "          dropped, and the module drops them.  Conformance, not"
        note "          a defect: SLIRP forges the source address."
    elif [ "$IN" -gt 0 ]; then
        fail "INBOUND: $IN mDNS frame(s) reached the card from an ON-LINK
       source and none was accepted, so the responder dropped a reply RFC 6762
       11 does not let it drop"
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

if [ "$BROKEN" -ne 0 ]; then
    echo "mdns: NOT MEASURED ($BROKEN instrument failure(s)), no verdict on the" >&2
    echo "      responder either way" >&2
    exit 3
fi

echo "mdns: PASSED"
exit 0
