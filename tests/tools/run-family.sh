#!/usr/bin/env bash
#
# -4 AND -6 ON A GUEST THAT REALLY HAS BOTH.
#
#   tests/tools/run-family.sh [-b BUILDDIR] [-B IFACE] [-m MODEL] [-t SECONDS]
#                             [-N BOARD] [-d NAME] [-p NAME] [-I ARM] [-V]
#
# WHAT IT PROVES
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-build/cm}"
IFACE="${AMINETXDUO_FAMILY_IFACE:-ens18}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
MODEL="${AMINETXDUO_FAMILY_MODEL:-A1200}"
TIMEOUT="${AMINETXDUO_FAMILY_TIMEOUT:-900}"
SLAAC_DEADLINE="${AMINETXDUO_FAMILY_SLAAC_DEADLINE:-90}"
VERDICT_ONLY=0
INJECT=""
GUEST_V6=""

# A name with an A and an AAAA, and a service on port 80 answering over both.
DUAL="${AMINETXDUO_FAMILY_DUAL:-example.com}"
DUAL_V6=""
ECHO="${AMINETXDUO_FAMILY_ECHO:-icanhazip.com}"
GOOG="${AMINETXDUO_FAMILY_GOOG:-www.google.com}"
# The time server for the sntp arms.  2.pool.ntp.org is the pool's IPv6 half.
NTP="${AMINETXDUO_FAMILY_NTP:-2.pool.ntp.org}"
V4ONLY_NAME="v4only.test"
V4ONLY_ADDR="${AMINETXDUO_FAMILY_V4ONLY_ADDR:-127.0.0.1}"

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
STAGE="$ROOT/build/family-stage-$AMINETXDUO_RUN_TAG"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"
PARAMS="$ROOT/build/family-params-$AMINETXDUO_RUN_TAG.env"

infra() { echo "error=$*"; echo "result=infra"; exit 2; }

# ======================================================================= #
#                          what the host can see                          #
# ======================================================================= #

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

    getent ahostsv6 "$GOOG" 2>/dev/null |
        awk 'NR==1 && $1 !~ /^::ffff:/ {print $1}' | grep -q . ||
        infra "$GOOG has no AAAA record from this host"

    DUAL_V6="$aaaa"

    mkdir -p "$ROOT/build"
    {
        printf 'DUAL=%s\n'        "$DUAL"
        printf 'DUAL_V6=%s\n'     "$DUAL_V6"
        printf 'ECHO=%s\n'        "$ECHO"
        printf 'NTP=%s\n'         "$NTP"
        printf 'GOOG=%s\n'        "$GOOG"
        printf 'V4ONLY_ADDR=%s\n' "$V4ONLY_ADDR"
    } > "$PARAMS"

    echo "host/dual=$DUAL a=$a aaaa=$aaaa"
    echo "host/v4only=$V4ONLY_NAME addr=$V4ONLY_ADDR"
    echo "host/ntp=$NTP"
    echo "host/echo=$ECHO"
    echo "host/goog=$GOOG"
}

. "$ROOT/tools/sana2-stage.sh"

find_driver() {
    local want="$1" c
    if [ -n "${AMINETXDUO_SANA2_DRIVER:-}" ] &&
       [ -f "${AMINETXDUO_SANA2_DRIVER}" ]; then
        echo "$AMINETXDUO_SANA2_DRIVER"; return 0
    fi
    if [ "$want" = a2065.device ] && [ -n "${AMINETXDUO_A2065:-}" ] &&
       [ -f "${AMINETXDUO_A2065}" ]; then
        echo "$AMINETXDUO_A2065"; return 0
    fi
    for c in "$ROOT/build/$want" "$(sana2_local_driver "$want")"; do
        [ -n "$c" ] && [ -f "$c" ] && { echo "$c"; return 0; }
    done
    return 1
}

# ======================================================================= #
#                                the arms                                 #
# ======================================================================= #
V4RE='[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+'
V6RE='[0-9a-fA-F]*:[0-9a-fA-F]*:'
V6FULL='[0-9a-fA-F:]*:[0-9a-fA-F:]*'

arms() {
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
traceroute/v6|*|SYS:traceroute $DUAL -m 1 -q 1 -w 5 -n -6|+traceroute to $DUAL \\($V6FULL\\)
# A HOP HAS TO ANSWER.  The two rows above assert the header line, which
# traceroute prints from the resolved address before it sends anything, so they
# pass on a trace where every single probe times out -- which is exactly the
# state -6 shipped in, and these rows are why nobody noticed.  Asserting the
# first hop is asserting that the probe left with a hop limit on it and that
# the router's ICMP time-exceeded came back to the socket.
traceroute/v4-hop|*|SYS:traceroute $DUAL -m 3 -q 1 -w 5 -n -4|+^ 1 .*$V4RE
traceroute/v6-hop|*|SYS:traceroute $DUAL -m 3 -q 1 -w 5 -n -6|+^ 1 .*$V6RE
# AND IT HAS TO STOP.  rc 0 is the whole assertion: traceroute returns 0 only
# when a hop was the destination, and 5 when it used up MAXTTL without
# arriving.  This is the row for the report that -6 "never finishes" against
# Google.  Measured 2026-08-10: hops 1-7 answer, 8-12 are stars because that
# network does not send ICMPv6 time-exceeded, and hop 13 is the destination --
# 20 s at -q 2 -w 2, about 75 s at the defaults, which is what "never
# finishes" was.  The stars are not ours to fix; stopping at the destination
# is, and that is what this measures.
traceroute/v6-arrives|0|SYS:traceroute $GOOG -m 30 -q 2 -w 2 -n -6|+^ 1 .*$V6RE
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
traceroute/break|0|SYS:TrBreak SECONDS 8 CEILING 3 SYS:traceroute 192.0.2.1 -m 30 -q 3 -w 2 -n -4|+alive_at_break=yes|+result=broke|+child_rc=5|-^ *[0-9]+ *$
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
    local driver drvpath t

    for t in ToolsSmoke AddNetInterface ShowNetStatus nslookup host ping \
             traceroute fetch nc telnet tftp whois sntp iperf; do
        [ -x "$TOOLS/$t" ] || infra "no $TOOLS/$t; build $BUILD first"
    done
    [ -x "$PROBES/TrBreak" ] || infra "no $PROBES/TrBreak; build $BUILD first"
    [ -f "$BSD" ] || infra "no $BSD; build $BUILD first"

    driver=$(sana2_driver_for "$BOARD")
    drvpath=$(find_driver "$driver") ||
        infra "$BOARD wants $driver and there is none; put one in the driver\
 store or set AMINETXDUO_SANA2_DRIVER"

    rm -rf "$STAGE"
    mkdir -p "$STAGE/libs"
    cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
    cp "$BSD"   "$STAGE/libs/bsdsocket.library"
    for t in AddNetInterface ShowNetStatus nslookup host ping traceroute \
             fetch nc telnet tftp whois sntp iperf; do
        cp "$TOOLS/$t" "$STAGE/$t"
    done
    cp "$PROBES/TrBreak" "$STAGE/TrBreak"

    cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=$driver
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=AUTO
EOF

    export AMINETXDUO_SANA2_DRIVER="$drvpath"
    sana2_stage "$BOARD" "$STAGE/devs"

    printf '%s %s\n' "$V4ONLY_ADDR" "$V4ONLY_NAME" \
        >> "$STAGE/devs/Internet/hosts"

    printf 'hostname %s\n' "$GUEST_NAME" \
        > "$STAGE/devs/Internet/name_resolution"

    printf 'x\r\n\r\n' > "$STAGE/telnetin.txt"

    arms | grep -v '^#' | cut -d'|' -f3 > "$STAGE/cmds.raw"
    {
        echo "SYS:AddNetInterface eth0"
        echo "until $SLAAC_DEADLINE address6 fe80,tentative SYS:ShowNetStatus ALL"
        cat "$STAGE/cmds.raw"
    } > "$STAGE/commands.txt"

    echo "boot/mac=$AMINETXDUO_AMIBERRY_MAC"
    echo "boot/hostname=$GUEST_NAME"
    echo "boot/tag=$AMINETXDUO_RUN_TAG"
    echo "boot/iface=$IFACE board=$BOARD model=$MODEL"
    echo "boot/driver=$SANA2_DRIVER device=$SANA2_DEVICE path=$drvpath"

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

guest_global_v6() {
    awk '$1 == "address6" && $2 !~ /^fe80:/ && $0 !~ /\(tentative\)/ \
         { print $2; exit }' "$REPORT"
}

# What the bounded poll above concluded, verbatim.
slaac_wait_outcome() {
    sed -n 's/^----- until address6: \(.*\) -----$/\1/p' "$REPORT" | head -1
}

v6_dependent() {
    case "$1" in
        */v6-hop)                          return 1 ;;
        */v6|*/v6-arrives|literal/v6-ping) return 0 ;;
        *)                                 return 1 ;;
    esac
}

verdict() {
    local id="$1" verdict="$2" why="$3"

    if [ "$id" = "$INJECT" ]; then
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
    echo "guest/slaac-wait=$(slaac_wait_outcome) deadline=${SLAAC_DEADLINE}s"
    if [ -n "$GUEST_V6" ]; then
        echo "guest/global6=$GUEST_V6"
    else
        echo "guest/global6=none"
        sed -n 's/^ *address6 */guest\/address6=/p' "$REPORT" | sort -u
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

judge_files() {
    local four six ok

    if [ -n "${AMINETXDUO_FAMILY_TABLE:-}" ]; then
        return
    fi

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
    echo "error=no global IPv6 address on the guest after ${SLAAC_DEADLINE}s of\
 asking; the wire carries no usable router advertisement, or the guest did not\
 act on one.  Raising the deadline is not the fix unless the poll shows it\
 arriving late"
    echo "result=infra-no-guest-ipv6"
    exit 3
fi

echo "result=pass"
exit 0
