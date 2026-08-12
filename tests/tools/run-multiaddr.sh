#!/usr/bin/env bash
#
# A DUAL-STACK NAME WHOSE IPv6 GOES NOWHERE.
#
#   tests/tools/run-multiaddr.sh [-b BUILDDIR] [-B IFACE] [-m MODEL] [-t SECS]
#                                [-N BOARD] [-d NAME] [-g GATEWAY6] [-a ADDR6]
#                                [-V]
#
# WHAT IT PROVES
#
#   A name with an AAAA and an A, on a machine whose IPv6 reaches nothing, is
#   still fetched.  The resolver puts the AAAA first, so before this the whole
#   connect went to an address that could not answer, cost the stack's entire
#   SYN schedule -- 191 s -- and then failed with the A never tried.
#
#   Alongside it, the two cases that must not get worse: a name where nothing
#   answers in either family, and one address that answers nothing at all.
#
# HOW THE IPv6 IS BROKEN, AND WHY THAT WAY
#
#   The guest is given a static global address on this wire and a default
#   router that is a machine on the same segment which is NOT a router.  It
#   answers neighbour discovery, so the route is real and every send succeeds;
#   it forwards nothing, so every packet is dropped in silence.  That is a
#   blackhole and not an error, which is the case that costs 191 s -- an ICMP
#   unreachable would fail in milliseconds and prove nothing.
#
#   -g takes that machine's address.  It must be ON THIS SEGMENT and must not
#   forward.  The emulator's host is not a candidate: frames the guest sends
#   never enter its receive path, so it would not answer the solicitation and
#   the route would be dead rather than silent.
#
# OUTPUT
#
#   `<arm>=pass|fail`, then `<arm>/ms=` for every arm, `checks=`, `failures=`
#   and `result=`.  The milliseconds are the point of the file: this is a
#   measurement, and the pass rows only say the measurement was of the right
#   thing.
#
#     0   every arm passed
#     1   an arm failed
#     2   infrastructure: no build, no ROM, no IPv6 on this wire, a timeout
#     3   the guest came up without the static IPv6 address it was given, so
#         nothing here was measured against a dual stack at all
#
# WHAT IT NEEDS
#
#   A 68020 Release build, a2065.device, a Kickstart, a bridged wire, and a
#   second machine on it to play the silent router.
#
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-build/cm}"
IFACE="${AMINETXDUO_MULTIADDR_IFACE:-ens18}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
MODEL="${AMINETXDUO_MULTIADDR_MODEL:-A1200}"
# Room for four connects that may each run to the stack's own ceiling, plus a
# boot.  A run that burns this proves nothing and says so.
TIMEOUT="${AMINETXDUO_MULTIADDR_TIMEOUT:-1100}"
VERDICT_ONLY=0

# A name with an A and an AAAA and a web server answering over both.
DUAL="${AMINETXDUO_MULTIADDR_DUAL:-example.com}"
# The silent router, and the address the guest gives itself.  Both belong to
# the wire this runs on and have no defaults worth guessing.
GATEWAY6="${AMINETXDUO_MULTIADDR_GATEWAY6:-}"
ADDRESS6="${AMINETXDUO_MULTIADDR_ADDRESS6:-}"
# One address, nothing behind it.  RFC 5737 TEST-NET-1: routed nowhere.
DEADV4="${AMINETXDUO_MULTIADDR_DEADV4:-192.0.2.1}"

export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_MULTIADDR_MAC:-02:41:4d:49:00:6a}"
export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-multiaddr}"
GUEST_NAME="${AMINETXDUO_MULTIADDR_HOSTNAME:-anxdma}"

while getopts "b:B:m:t:N:d:g:a:V" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        d) DUAL="$OPTARG" ;;
        g) GATEWAY6="$OPTARG" ;;
        a) ADDRESS6="$OPTARG" ;;
        V) VERDICT_ONLY=1 ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

TOOLS="$BUILD/src/tools"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
STAGE="$ROOT/build/multiaddr-stage-$AMINETXDUO_RUN_TAG"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"

infra() { echo "error=$*"; echo "result=infra"; exit 2; }

host_preflight() {
    local a aaaa

    [ -n "$GATEWAY6" ] || infra "no -g: this needs a machine on this wire that answers ND and forwards nothing"
    [ -n "$ADDRESS6" ] || infra "no -a: the guest needs a static global address on this wire"

    command -v getent >/dev/null 2>&1 || infra "no getent on this host"

    a=$(getent ahostsv4 "$DUAL" 2>/dev/null | awk 'NR==1{print $1}')
    aaaa=$(getent ahostsv6 "$DUAL" 2>/dev/null |
           awk 'NR==1 && $1 !~ /^::ffff:/ {print $1}')

    [ -n "$a" ]    || infra "$DUAL has no A record from this host"
    [ -n "$aaaa" ] || infra "$DUAL has no AAAA record from this host"

    echo "host/dual=$DUAL a=$a aaaa=$aaaa"
    echo "host/gateway6=$GATEWAY6 address6=$ADDRESS6"
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
# TIMEOUT is on every fetch and is deliberately small.  Before this change it
# bounded reads and the handshake and never the connect, so the same line
# takes the stack's 191 s on an old binary and the number it asks for on a
# new one: the two transcripts are directly comparable.
arms() {
    cat <<EOF
net/settled|0|SYS:ShowNetStatus ALL|+address6
# THE DEFECT.  No flag, so the resolver decides, and it decides AAAA first.
# The AAAA is blackholed; the A is a working web server.
dual/fetch|0|SYS:fetch http://$DUAL/ TIMEOUT 30 TO DH0:dual.txt|+HTTP/1.[01] 200
# The same transfer with the family pinned, as the control.
dual/v4|0|SYS:fetch http://$DUAL/ TIMEOUT 30 TO DH0:v4.txt -4|+HTTP/1.[01] 200
# -6 MUST NOT FALL BACK.  A user who said -6 and got an IPv4 transfer has been
# lied to, so this arm has to FAIL on a machine whose IPv6 goes nowhere.
dual/v6|10|SYS:fetch http://$DUAL/ TIMEOUT 30 TO DH0:v6.txt -6|-HTTP/1.[01] 200
# Nothing answers in either family: the discard port, blackholed over IPv6 and
# unanswered over IPv4.  This is the case that must not get slower.
dead/both|10|SYS:fetch http://$DUAL:9/ TIMEOUT 30|-HTTP/1.[01] 200
# And one address with nothing behind it at all.
dead/one|10|SYS:fetch http://$DEADV4/ TIMEOUT 30|-HTTP/1.[01] 200
EOF
}

# ======================================================================= #
#                                the boot                                 #
# ======================================================================= #

stage_and_boot() {
    local a2065 t

    for t in ToolsSmoke AddNetInterface ShowNetStatus fetch; do
        [ -x "$TOOLS/$t" ] || infra "no $TOOLS/$t; build $BUILD first"
    done
    [ -f "$BSD" ] || infra "no $BSD; build $BUILD first"

    a2065=$(find_a2065) || infra "no a2065.device; set AMINETXDUO_A2065"

    rm -rf "$STAGE"
    mkdir -p "$STAGE/libs"
    cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
    cp "$a2065" "$STAGE/devs/a2065.device"
    cp "$BSD"   "$STAGE/libs/bsdsocket.library"
    for t in AddNetInterface ShowNetStatus fetch; do
        cp "$TOOLS/$t" "$STAGE/$t"
    done

    # DHCP for IPv4, and an IPv6 configuration that is deliberately useless:
    # a real address on this wire so the machine believes it has IPv6 and
    # source selection offers it, behind a router that swallows everything.
    cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=STATIC
ADDRESS6=$ADDRESS6
GATEWAY6=$GATEWAY6
EOF

    # The shared file names SLIRP's forwarder, which does not exist here and
    # would be asked first on every lookup.  The lease carries a name server.
    printf 'hostname %s\n' "$GUEST_NAME" \
        > "$STAGE/devs/Internet/name_resolution"

    arms | grep -v '^#' | cut -d'|' -f3 > "$STAGE/cmds.raw"
    # Duplicate address detection has to finish before the static address is
    # anything but tentative, and nothing may be sent from a tentative one:
    # without the wait every IPv6 arm fails instantly with EADDRNOTAVAIL,
    # which looks like a blackhole and is not one.
    {
        echo "SYS:AddNetInterface eth0"
        echo "wait 15"
        cat "$STAGE/cmds.raw"
    } > "$STAGE/commands.txt"

    echo "boot/mac=$AMINETXDUO_AMIBERRY_MAC"
    echo "boot/hostname=$GUEST_NAME"
    echo "boot/tag=$AMINETXDUO_RUN_TAG"
    echo "boot/iface=$IFACE board=$BOARD model=$MODEL build=$BUILD"

    set +e
    "$ROOT/tools/amiberry-run.sh" -N "$BOARD" -B "$IFACE" -m "$MODEL" \
        -t "$TIMEOUT" \
        "$TOOLS/ToolsSmoke" "$STAGE/commands.txt" "$STAGE/devs" \
        "$STAGE/libs" "$STAGE/AddNetInterface" "$STAGE/ShowNetStatus" \
        "$STAGE/fetch" \
        > "$ROOT/build/multiaddr-run-$AMINETXDUO_RUN_TAG.log" 2>&1
    RUN_RC=$?
    set -e

    echo "boot/rc=$RUN_RC"
}

# ======================================================================= #
#                               the verdict                               #
# ======================================================================= #

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

ms_of() {
    awk -v want="===== $1 =====" '
        $0 == want { on = 1; next }
        on && /^----- rc / { print; exit }
    ' "$REPORT" | sed -n 's/^----- rc [0-9-]*, \([0-9]*\) ms.*/\1/p'
}

CHECKS=0
FAILURES=0

guest_global_v6() {
    awk '$1 == "address6" && $2 !~ /^fe80:/ && $0 !~ /\(tentative\)/ \
         { print $2; exit }' "$REPORT"
}

verdict() {
    local id="$1" v="$2" why="$3"

    CHECKS=$((CHECKS + 1))
    [ "$v" = pass ] || FAILURES=$((FAILURES + 1))

    if [ -n "$why" ]; then
        echo "$id=$v ($why)"
    else
        echo "$id=$v"
    fi
}

judge() {
    local line id want cmd rest text ok why pat

    [ -f "$REPORT" ] || infra "no transcript at $REPORT"

    if [ -z "$(guest_global_v6)" ]; then
        echo "guest/address6=none"
        echo "error=the guest never took the static IPv6 address, so nothing here saw a dual stack"
        echo "result=noipv6"
        exit 3
    fi
    echo "guest/address6=$(guest_global_v6)"

    arms | grep -v '^#' | while IFS= read -r line; do
        id=${line%%|*};  rest=${line#*|}
        want=${rest%%|*}; rest=${rest#*|}
        cmd=${rest%%|*};  rest=${rest#*|}

        text=$(block "$cmd")
        ok=pass; why=""

        if [ "$want" != '*' ] && [ "$(rc_of "$cmd")" != "$want" ]; then
            ok=fail; why="rc $(rc_of "$cmd"), wanted $want"
        fi

        while [ -n "$rest" ] && [ "$ok" = pass ]; do
            pat=${rest%%|*}
            case "$pat" in
                +*) printf '%s\n' "$text" | grep -Eq -- "${pat#+}" ||
                        { ok=fail; why="no /${pat#+}/"; } ;;
                -*) printf '%s\n' "$text" | grep -Eq -- "${pat#-}" &&
                        { ok=fail; why="matched /${pat#-}/"; } ;;
            esac
            [ "$rest" = "${rest#*|}" ] && break
            rest=${rest#*|}
        done

        printf '%s|%s|%s|%s\n' "$id" "$ok" "$why" "$(ms_of "$cmd")"
    done > "$ROOT/build/multiaddr-verdicts-$AMINETXDUO_RUN_TAG.txt"

    while IFS='|' read -r id ok why ms; do
        verdict "$id" "$ok" "$why"
        echo "$id/ms=${ms:-?}"
    done < "$ROOT/build/multiaddr-verdicts-$AMINETXDUO_RUN_TAG.txt"

    echo "checks=$CHECKS"
    echo "failures=$FAILURES"
    [ "$FAILURES" -eq 0 ] && { echo "result=ok"; exit 0; }
    echo "result=fail"
    exit 1
}

host_preflight
[ "$VERDICT_ONLY" -eq 1 ] || stage_and_boot
judge
