#!/usr/bin/env bash
# A DUAL-STACK NAME WHOSE IPv6 GOES NOWHERE.
#   A 68020 Release build, a2065.device, a Kickstart, and a bridged wire with
#   a router advertising IPv6.
# SPDX-License-Identifier: MIT

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BUILD="${AMINETXDUO_BUILD:-build/cm}"
IFACE="${AMINETXDUO_MULTIADDR_IFACE:-ens18}"
BOARD="${AMINETXDUO_AMIBERRY_BOARD:-a2065}"
MODEL="${AMINETXDUO_MULTIADDR_MODEL:-A1200}"
TIMEOUT="${AMINETXDUO_MULTIADDR_TIMEOUT:-1100}"
VERDICT_ONLY=0

DUAL="${AMINETXDUO_MULTIADDR_DUAL:-example.com}"
BLACKHOLE="${AMINETXDUO_MULTIADDR_BLACKHOLE:-2000::/3}"
DEADV4="${AMINETXDUO_MULTIADDR_DEADV4:-192.0.2.1}"

export AMINETXDUO_AMIBERRY_MAC="${AMINETXDUO_MULTIADDR_MAC:-02:41:4d:49:00:6a}"
export AMINETXDUO_RUN_TAG="${AMINETXDUO_RUN_TAG:-multiaddr}"
GUEST_NAME="${AMINETXDUO_MULTIADDR_HOSTNAME:-anxdma}"

while getopts "b:B:m:t:N:d:V" opt; do
    case "$opt" in
        b) BUILD="$OPTARG" ;;
        B) IFACE="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        N) BOARD="$OPTARG" ;;
        d) DUAL="$OPTARG" ;;
        V) VERDICT_ONLY=1 ;;
        *) sed -n '3,8p' "$0" >&2; exit 2 ;;
    esac
done

case "$IFACE" in
    slirp|slirp_inbound|none)
        echo "error=backend_refused:$IFACE"
        echo "result=infra"
        exit 2
        ;;
esac

TOOLS="$BUILD/src/tools"
BSD="$BUILD/src/bsdsocket/bsdsocket.library"
STAGE="$ROOT/build/multiaddr-stage-$AMINETXDUO_RUN_TAG"
HD="$ROOT/build/amiberry-testhd-$AMINETXDUO_RUN_TAG"
REPORT="$HD/tools.txt"

infra() { echo "error=$*"; echo "result=infra"; exit 2; }

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

    echo "host/dual=$DUAL a=$a aaaa=$aaaa"
    echo "host/blackhole=$BLACKHOLE"
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

arms() {
    cat <<EOF
net/settled|0|SYS:ShowNetStatus ALL|+address6
# IPv6 WORKS HERE.  Asserted before anything is broken, because every arm
# below is about what happens when it stops, and a wire that never carried
# IPv6 would make all of them pass for the wrong reason.  It is also half of
# the -6 proof: this is the flag reaching a service over the family it names.
pin/v6-works|0|SYS:fetch http://$DUAL/ TIMEOUT 30 TO DH0:pre6.txt -6|+HTTP/1.[01] 200
# And now it does not.  Global unicast declared on-link: every IPv6 packet
# waits for a neighbour nobody is, and is dropped without a word.
blackhole|0|SYS:AddNetRoute DST $BLACKHOLE|*
# THE DEFECT.  No flag, so the resolver decides, and it decides AAAA first.
# The AAAA is now a blackhole; the A is the same working web server.
dual/fetch|0|SYS:fetch http://$DUAL/ TIMEOUT 30 TO DH0:dual.txt|+HTTP/1.[01] 200
# The same transfer with the family pinned, as the control.
dual/v4|0|SYS:fetch http://$DUAL/ TIMEOUT 30 TO DH0:v4.txt -4|+HTTP/1.[01] 200
# The other half of the -6 proof.  A user who said -6 and got an IPv4 transfer
# has been lied to, so this MUST fail now, and it is the same command line
# that succeeded above.
pin/v6-blackholed|10|SYS:fetch http://$DUAL/ TIMEOUT 30 TO DH0:v6.txt -6|-HTTP/1.[01] 200
# Nothing answers in either family: the discard port, blackholed over IPv6 and
# unanswered over IPv4.  This is the case that must not get slower.
dead/both|10|SYS:fetch http://$DUAL:9/ TIMEOUT 30|-HTTP/1.[01] 200
# And one address with nothing behind it at all.
dead/one|10|SYS:fetch http://$DEADV4/ TIMEOUT 30|-HTTP/1.[01] 200
EOF
}



stage_and_boot() {
    local a2065 t

    for t in ToolsSmoke AddNetInterface ShowNetStatus AddNetRoute fetch; do
        [ -x "$TOOLS/$t" ] || infra "no $TOOLS/$t; build $BUILD first"
    done
    [ -f "$BSD" ] || infra "no $BSD; build $BUILD first"

    a2065=$(find_a2065) || infra "no a2065.device; set AMINETXDUO_A2065"

    rm -rf "$STAGE"
    mkdir -p "$STAGE/libs"
    cp -R "$ROOT/tests/netstack/devs" "$STAGE/devs"
    cp "$a2065" "$STAGE/devs/a2065.device"
    cp "$BSD"   "$STAGE/libs/bsdsocket.library"
    for t in AddNetInterface ShowNetStatus AddNetRoute fetch; do
        cp "$TOOLS/$t" "$STAGE/$t"
    done

    cat > "$STAGE/devs/NetInterfaces/eth0" <<EOF
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=AUTO
EOF

    . "$ROOT/tools/sana2-stage.sh"
    if [ -z "${AMINETXDUO_SANA2_DRIVER:-}" ] && [ "$BOARD" != a2065 ]; then
        _want=$(sana2_driver_for "$BOARD")
        _have=$(sana2_local_driver "$_want")
        [ -n "$_have" ] && [ -f "$_have" ] &&
            export AMINETXDUO_SANA2_DRIVER="$_have"
    fi
    sana2_stage "$BOARD" "$STAGE/devs"

    printf 'hostname %s\n' "$GUEST_NAME" \
        > "$STAGE/devs/Internet/name_resolution"

    arms | grep -v '^#' | cut -d'|' -f3 > "$STAGE/cmds.raw"
    {
        echo "SYS:AddNetInterface eth0"
        echo "wait 20"
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
        "$STAGE/AddNetRoute" "$STAGE/fetch" \
        > "$ROOT/build/multiaddr-run-$AMINETXDUO_RUN_TAG.log" 2>&1
    RUN_RC=$?
    set -e

    echo "boot/rc=$RUN_RC"
}

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
        echo "error=the guest has no global IPv6 address, so nothing here saw a dual stack"
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
