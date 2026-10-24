#!/usr/bin/env bash
#
# forbidrec on a real Amiga, across workload phases, one recorder throughout.
#
#   tools/forbidrec/run-a1200.sh -A AMIGA_IP -P PEER [-s SECS] [-o OUTDIR]
#                                [PHASE...]
#
# PHASE is any of base tcp_in tcp_out udp dns arp dhcp sock tx all (default:
# every one, in that order).  Each runs SECS (60) seconds; CTRL-E ends it.
#
# The Amiga runs httpd (tools/web/remote-control.mjs drives it) and has
# RAM:forbidrec and RAM:frload; build/<cfg>/tools/forbidrec/ has them.  PEER
# is an ssh host on the same LAN with python3 and iperf 2: it runs
# tools/forbidrec/loadhost.py, connects in, and takes the Amiga's connects,
# datagrams and iperf TX.  Bulk RX (iperf -c from the peer) and audio are
# the caller's to start; this script adds to them.  `Run >NIL: RAM:hush 3600`
# streams digital silence through AHI: the USB audio path, nothing to hear.
#
#   arp   datagrams to NET.200..239 (-n NET, default the Amiga's /24), and
#         the peer's ARP entry deleted every second
#   dhcp  ConfigureNetInterface IFACE CONFIGURE DHCP (-i IFACE, genet)
#
# Leaves OUTDIR/forbidrec.log, the per-phase peer logs and tx-peer.log; prints the
# recorder's hist lines and result=.  Exit: the recorder's code, 2 setup.
#
# SPDX-License-Identifier: MIT

set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
AMIGA="" PEER="" SECS=60 OUT="" IFACE=genet NET=""

while getopts "A:P:s:o:i:n:" opt; do
    case "$opt" in
        A) AMIGA="$OPTARG" ;;
        P) PEER="$OPTARG" ;;
        s) SECS="$OPTARG" ;;
        o) OUT="$OPTARG" ;;
        i) IFACE="$OPTARG" ;;
        n) NET="$OPTARG" ;;
        *) sed -n '3,24p' "$0" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ -n "$AMIGA" ] && [ -n "$PEER" ] || { sed -n '3,24p' "$0" >&2; exit 2; }
PHASES=${*:-base tcp_in tcp_out udp dns arp dhcp sock tx all}
OUT=${OUT:-$ROOT/build/forbidrec-a1200}
NET=${NET:-${AMIGA%.*}.}
mkdir -p "$OUT"

PEERADDR=$(ssh -o BatchMode=yes "$PEER" \
    "ip -4 route get $AMIGA | sed -n 's/.* src \([0-9.]*\).*/\1/p'") || exit 2
[ -n "$PEERADDR" ] || { echo "cannot find $PEER's address towards $AMIGA" >&2; exit 2; }
RDIR=ai/scratch/forbidrec-load
ssh "$PEER" "mkdir -p $RDIR/www && head -c 2048 /dev/urandom > $RDIR/www/small" || exit 2
scp -q "$ROOT/tools/forbidrec/loadhost.py" "$PEER:$RDIR/" || exit 2
LH="python3 $RDIR/loadhost.py"
RC="node $ROOT/tools/web/remote-control.mjs --url http://$AMIGA"

say() { echo "$(date +%T) $*"; }

# One Amiga command, retried while the inbound churn crowds httpd out.
amiga() {
    local _ out
    for _ in 1 2 3 4 5; do
        out=$($RC --timeout 20 run "$1" 2>&1)
        if python3 -c 'import json,sys; j=json.loads(sys.argv[1]); sys.exit("error" in j)' "$out" 2>/dev/null; then
            python3 -c 'import json,sys; print(json.loads(sys.argv[1])["output"], end="")' "$out"
            return 0
        fi
        sleep 2
    done
    echo "$out" >&2
    return 1
}

put_script() { # name lines...
    local f="$OUT/$1"; shift
    printf '%s\n' "$@" > "$f"
    $RC put "$f" "/Ram%20Disk/$(basename "$f")" > /dev/null
}

n=0 fetch=() arpd=() dhcp=()
while [ $n -lt $((SECS * 4 / 3)) ]; do
    fetch+=("AmiNetXDuo:C/fetch http://$PEERADDR:47002/small TO NIL: QUIET"
            "AmiNetXDuo:C/nc -z $PEERADDR 47001 >NIL:")
    n=$((n + 1))
done
for ((n = 0; n < SECS; n++)); do arpd+=("AmiNetXDuo:C/arp -d $PEERADDR >NIL:" "Wait 1"); done
for ((n = 0; n < SECS / 10 + 1; n++)); do dhcp+=("AmiNetXDuo:C/ConfigureNetInterface $IFACE CONFIGURE DHCP" "Wait 9"); done
put_script fr-fetch.scr "${fetch[@]}"
put_script fr-arpd.scr "${arpd[@]}"
put_script fr-dhcp.scr "${dhcp[@]}"

peer() { ssh -o BatchMode=yes "$PEER" "$@"; }

ph_base()    { :; }
ph_tcp_in()  { peer "$LH churn $AMIGA 80 $SECS 64" & peer "$LH churn $AMIGA 80 $SECS 4 get" & }
ph_tcp_out() { peer "cd $RDIR/www && timeout $((SECS + 15)) python3 -m http.server --bind $PEERADDR 47002 >/dev/null 2>&1" &
               peer "$LH acceptclose $PEERADDR 47001 $((SECS + 10))" &
               sleep 2
               amiga "run >NIL: RAM:frload MODE=connect HOST=$PEERADDR PORT=47001 SECS=$SECS TO=RAM:fr-conn1.log"
               amiga "run >NIL: RAM:frload MODE=connect HOST=$PEERADDR PORT=47001 SECS=$SECS TO=RAM:fr-conn2.log"
               amiga "run >NIL: execute RAM:fr-fetch.scr"; }
ph_udp()     { peer "$LH udpecho $PEERADDR 47100 200 $((SECS + 10))" & peer "$LH udpin $AMIGA $SECS" &
               sleep 2
               amiga "run >NIL: RAM:frload MODE=udp HOST=$PEERADDR PORT=47100 COUNT=200 SECS=$SECS TO=RAM:fr-udp.log"; }
ph_dns()     { amiga "run >NIL: RAM:frload MODE=dns SECS=$SECS TO=RAM:fr-dns1.log"
               amiga "run >NIL: RAM:frload MODE=dns SECS=$SECS TO=RAM:fr-dns2.log"; }
ph_arp()     { amiga "run >NIL: RAM:frload MODE=arp NET=$NET FIRST=200 COUNT=40 SECS=$SECS TO=RAM:fr-arp.log"
               amiga "run >NIL: execute RAM:fr-arpd.scr"; }
ph_dhcp()    { amiga "run >NIL: execute RAM:fr-dhcp.scr"; }
ph_sock()    { amiga "run >NIL: RAM:frload MODE=sock SECS=$SECS TO=RAM:fr-sock1.log"
               amiga "run >NIL: RAM:frload MODE=sock SECS=$SECS TO=RAM:fr-sock2.log"; }
ph_tx()      { peer "timeout $((SECS + 15)) iperf -s -p 5003 -f m" > "$OUT/tx-peer.log" 2>&1 &
               sleep 2
               amiga "run >NIL: AmiNetXDuo:C/iperf $PEERADDR -p 5003 -t $SECS"; }
ph_all()     { ph_tcp_in; ph_tcp_out; ph_udp; ph_dns; ph_arp; ph_dhcp; ph_sock; ph_tx; }

say "phases=\"$PHASES\" secs=$SECS amiga=$AMIGA peer=$PEER/$PEERADDR"
amiga "run >NIL: RAM:forbidrec TO=RAM:forbidrec.log" > /dev/null || exit 2
sleep 3
CLI=$(amiga "status COMMAND=RAM:forbidrec" | tr -dc 0-9)
[ -n "$CLI" ] || { echo "forbidrec did not start" >&2; exit 2; }
n=1
for p in $PHASES; do
    say "phase=$n name=$p begin"
    "ph_$p" > "$OUT/phase$n-$p.log" 2>&1
    sleep "$SECS"
    wait
    sleep 3
    amiga "Break $CLI E" > /dev/null
    say "phase=$n name=$p end"
    n=$((n + 1))
    sleep 4
done
amiga "Break $CLI C" > /dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
    sleep 2
    [ -z "$(amiga "status COMMAND=RAM:forbidrec" | tr -dc 0-9)" ] && break
done
$RC get "/Ram%20Disk/forbidrec.log" "$OUT/forbidrec.log" > /dev/null || exit 2
for f in conn1 conn2 udp dns1 dns2 arp sock1 sock2; do
    amiga "type RAM:fr-$f.log" 2>/dev/null | grep -E '^frload' | sed "s/^/load=$f /"
done
grep -E '^(phase=|hist |result=)' "$OUT/forbidrec.log"
rc=$(sed -n 's/^result=\([a-z_]*\).*/\1/p' "$OUT/forbidrec.log")
case "$rc" in clean) exit 0 ;; holds_found) exit 5 ;; *) exit 1 ;; esac
