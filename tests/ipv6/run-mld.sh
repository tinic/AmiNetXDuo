#!/usr/bin/env bash
#
# DOES THIS HOST ANNOUNCE THE GROUPS IT LISTENS TO?
#
#   tests/ipv6/run-mld.sh [-B BACKEND] [-b BUILDDIR] [-m MODEL] [-t SECONDS]
#                         [-P user@peer] [-M MAC]
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

BACKEND="${AMINETXDUO_AMIBERRY_BACKEND:-ens18}"
BUILD="${AMINETXDUO_BUILD:-build/v6log}"
MODEL=A1200
TIMEOUT=260
PEER="${AMINETXDUO_MLD_PEER:-}"
MAC="02:41:4d:49:6d:1d"
TAG="mld"

while getopts "B:b:m:t:P:M:T:" opt; do
    case "$opt" in
        B) BACKEND="$OPTARG" ;;
        b) BUILD="$OPTARG" ;;
        m) MODEL="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        P) PEER="$OPTARG" ;;
        M) MAC="$OPTARG" ;;
        T) TAG="$OPTARG" ;;
        *) sed -n '3,6p' "$0" >&2; exit 2 ;;
    esac
done
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac

fail_setup() { echo "reason=$1"; echo "RESULT=refused"; exit 2; }

BSD="$BUILD/src/bsdsocket/bsdsocket.library"
ADDIF="$BUILD/src/tools/AddNetInterface"
SHOW="$BUILD/src/tools/ShowNetStatus"
SMOKE="$BUILD/src/tools/ToolsSmoke"
for f in "$BSD" "$ADDIF" "$SHOW" "$SMOKE"; do
    [ -f "$f" ] || fail_setup "build_missing:$f"
done

. "$ROOT/tools/amiga-toolchain.sh" > /dev/null 2>&1 || true
[ -n "${AMIGA_GCC:-}" ] || fail_setup "no_amiga_gcc"
case "$MODEL" in
    *68000*|*A500*|*A600*|*A2000*) PROBE_ARCH="-m68000" ;;
    *)                             PROBE_ARCH="-m68020" ;;
esac
PROBE="$ROOT/build/$TAG-McastProbe"
"$AMIGA_GCC" -O2 "$PROBE_ARCH" ${AMIGA_NDK:+-I"$AMIGA_NDK"} -o "$PROBE" \
    "$ROOT/tests/tools/mcastprobe.c" || fail_setup "mcastprobe_build_failed"

[ -n "${AMINETXDUO_KICKSTART:-}" ] || fail_setup "no_kickstart"

A2065="${AMINETXDUO_A2065:-}"
if [ -z "$A2065" ]; then
    for c in "$ROOT/build/a2065.device" "$HOME/amiga-assets/devs/a2065.device"; do
        [ -f "$c" ] && { A2065="$c"; break; }
    done
fi
[ -f "${A2065:-/nonexistent}" ] || fail_setup "no_a2065_device"

command -v tcpdump > /dev/null || fail_setup "no_tcpdump"

# ------------------------------------------------- the addresses under test --
TAIL=$(printf '%s' "$MAC" | tr 'A-Z' 'a-z' | cut -d: -f4-6)
GUEST_MAC="00:80:10:$TAIL"
O4=$(printf '%s' "$TAIL" | cut -d: -f1)
O5=$(printf '%s' "$TAIL" | cut -d: -f2)
O6=$(printf '%s' "$TAIL" | cut -d: -f3)
SOLICITED="ff02::1:ff$O4:$O5$O6"
PROBE_GROUP="ff02::c"

echo "guest_mac=$GUEST_MAC solicited=$SOLICITED backend=$BACKEND peer=${PEER:-none}"

# ------------------------------------------------------------------- peer ---

peer_ok=0
if [ -n "$PEER" ]; then
    if ssh -o BatchMode=yes -o ConnectTimeout=10 "$PEER" \
           'test -x ~/python3-cap && test -f ~/mldpeer.py' 2>/dev/null; then
        peer_ok=1
    else
        scp -q -o BatchMode=yes "$ROOT/tests/ipv6/mldpeer.py" "$PEER:~/mldpeer.py" 2>/dev/null
        ssh -o BatchMode=yes "$PEER" 'test -x ~/python3-cap' 2>/dev/null && peer_ok=1
    fi
fi
[ "$peer_ok" = 1 ] || echo "peer=unusable (no ~/python3-cap with CAP_NET_RAW)"

peer_send() {
    [ "$peer_ok" = 1 ] || return 0
    ssh -o BatchMode=yes "$PEER" "~/python3-cap ~/mldpeer.py --iface ${AMINETXDUO_MLD_PEER_IFACE:-ens18} $*" \
        >> "$ROOT/build/$TAG-peer.log" 2>&1
}

# ---------------------------------------------------------------- staging ---

STAGE="$ROOT/build/$TAG-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/libs" "$STAGE/devs/NetInterfaces"
cp "$BSD"   "$STAGE/libs/bsdsocket.library"
cp "$A2065" "$STAGE/devs/a2065.device"
cp "$ADDIF" "$STAGE/AddNetInterface"
cp "$SHOW"  "$STAGE/ShowNetStatus"
cp "$PROBE" "$STAGE/McastProbe"

cat > "$STAGE/devs/NetInterfaces/eth0" <<'EOF'
DEVICE=a2065.device
UNIT=0
CONFIGURE=DHCP
CONFIGURE6=AUTO
EOF

cat > "$STAGE/commands.txt" <<EOF
SYS:AddNetInterface eth0
wait 55
SYS:McastProbe
wait 25
SYS:ShowNetStatus
EOF

# ---------------------------------------------------------------- capture ---

PCAP="$ROOT/build/$TAG.pcap"
CAPPID="$ROOT/build/$TAG-tcpdump.pid"
rm -f "$PCAP" "$ROOT/build/$TAG-peer.log"

setsid nohup tcpdump -i "$BACKEND" -s 256 -U -w "$PCAP" ip6 \
    > "$ROOT/build/$TAG-tcpdump.log" 2>&1 < /dev/null &
echo $! > "$CAPPID"
sleep 2

# Kill by the pid we started and never by pattern: this lab runs more than one
# harness at a time and a bare pkill takes somebody else's capture with it.
stop_capture() {
    [ -f "$CAPPID" ] || return 0
    kill "$(cat "$CAPPID")" 2>/dev/null
    rm -f "$CAPPID"
    sleep 1
}
trap 'stop_capture' EXIT

if ! kill -0 "$(cat "$CAPPID")" 2>/dev/null; then
    cat "$ROOT/build/$TAG-tcpdump.log" >&2
    fail_setup "tcpdump_did_not_start"
fi

# ------------------------------------------------------------------- run ---

SERIAL="$ROOT/build/amiberry-serial-$TAG.log"
rm -f "$SERIAL"

AMINETXDUO_RUN_TAG="$TAG" AMINETXDUO_AMIBERRY_MAC="$MAC" \
    "$ROOT/tools/amiberry-run.sh" \
    -N a2065 -B "$BACKEND" -m "$MODEL" -t "$TIMEOUT" \
    "$SMOKE" "$STAGE/devs" "$STAGE/libs" "$STAGE/AddNetInterface" \
    "$STAGE/McastProbe" "$STAGE/ShowNetStatus" "$STAGE/commands.txt" \
    > "$ROOT/build/$TAG.out" 2>&1 &
RUNPID=$!

# ------------------------------------------------------- the peer schedule --
guest_seen() {
    tcpdump -r "$PCAP" -nne "ether src $GUEST_MAC and ip6" 2>/dev/null | head -1 | grep -q .
}

anchor=0
for _ in $(seq 1 120); do
    if guest_seen; then anchor=1; break; fi
    sleep 1
done

if [ "$anchor" = 0 ]; then
    echo "guest_first_frame=none"
    echo "  (the guest sent no IPv6 at all: read build/$TAG.out and"
    echo "   build/amiberry-serial-$TAG.log before reading anything below)"
else
    echo "guest_first_frame=seen"
fi

if [ "$peer_ok" = 1 ] && [ "$anchor" = 1 ]; then

    # Version 2 first, because that is the version a host starts in.
    sleep 10
    peer_send query-v2 --mrd 4000
    echo "peer=query-v2"

    sleep 14
    peer_send query-v1 --mrd 4000
    echo "peer=query-v1-control"

    sleep 16
    peer_send query-v1 --mrd 8000
    sleep 1
    peer_send report-v1 --group "$SOLICITED"
    echo "peer=query-v1-suppressed"

fi

wait "$RUNPID"
run_rc=$?
sleep 3
stop_capture

echo "run_rc=$run_rc"

[ -s "$PCAP" ] || { echo "RESULT=nocapture"; exit 3; }

# ------------------------------------------------------------------ verdict --

python3 "$ROOT/tests/ipv6/mldcheck.py" --pcap "$PCAP" \
    --guest-mac "$GUEST_MAC" --solicited "$SOLICITED" --group "$PROBE_GROUP"
rc=$?

if [ "$peer_ok" != 1 ]; then
    echo "not_exercised=query_answer_v1,query_answer_v2,suppression (no peer)"
fi

exit "$rc"
