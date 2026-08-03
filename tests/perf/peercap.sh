#!/usr/bin/env bash
#
# Capture one FitzBench arm at the PEER, so tests/perf/lossrate.py has
# something to read.  Sourced by run-fitzbench.sh and run-stackprof.sh; not
# useful on its own.
#
#   peercap_start  <user@host> <port> <outdir> <tag>
#   peercap_stop   <user@host> <outdir> <tag>
#   peercap_report <outdir> <tag> [--max-loss PCT ...]
#
# WHY THE PEER AND NOT THE GUEST
#
#   The number wanted is how many of the peer's segments it had to send twice,
#   and the peer is where that is exact: it is the sender, so its own egress
#   is the whole population, no inference and nothing on the Amiga.  A capture
#   at the emulator's bridge would answer a different question -- what
#   arrived -- and would need the guest to be instrumented to answer this one.
#
# WHAT IS COLLECTED
#
#   - a pcap of the fitz port.  -s 96 is deliberate: every IPv4 TCP header
#     fits in 74 bytes, the payload is not read, and a full-length capture of
#     a 4 MB transfer is 40 MB of disk for nothing.  Payload LENGTHS come from
#     the IP header, which is not truncated -- a short snaplen has previously
#     been misread as short segments and sent an investigation the wrong way.
#   - an `ss -tim` poll of the same socket, because rwnd_limited,
#     sndbuf_limited and app_limited cannot be inferred from a capture at all.
#
# BOTH NEED PRIVILEGE ON THE PEER.  tcpdump needs CAP_NET_RAW.  Where sudo is
# not available the working pattern is a copy of the binary with the
# capability set on the copy, which leaves the packaged one alone; point
# AMINETXDUO_PEER_TCPDUMP at it.
#
#   AMINETXDUO_PEER_TCPDUMP   tcpdump on the peer          (default tcpdump)
#   AMINETXDUO_PEER_SS        ss on the peer               (default ss)
#   AMINETXDUO_PEER_IFACE     interface to capture         (default any)
#   AMINETXDUO_PEER_TMP       scratch directory on the peer (default /tmp)
#
# SPDX-License-Identifier: MIT

PEERCAP_TCPDUMP="${AMINETXDUO_PEER_TCPDUMP:-tcpdump}"
PEERCAP_SS="${AMINETXDUO_PEER_SS:-ss}"
PEERCAP_IFACE="${AMINETXDUO_PEER_IFACE:-any}"
PEERCAP_TMP="${AMINETXDUO_PEER_TMP:-/tmp}"

peercap_start() {
    local peer="$1" port="$2" outdir="$3" tag="$4"
    [ -n "$peer" ] || { echo "peercap: no peer, not capturing" >&2; return 1; }
    mkdir -p "$outdir"
    # The poll interval is 50 ms.  It is not a sampling rate for anything
    # timed -- the capture carries the timing -- it just has to be short
    # enough that the read phases contain samples at all.
    ssh "$peer" "
        rm -f $PEERCAP_TMP/peercap-$tag.pcap $PEERCAP_TMP/peercap-$tag.ss
        nohup $PEERCAP_TCPDUMP -i $PEERCAP_IFACE -s 96 -w \
            $PEERCAP_TMP/peercap-$tag.pcap 'tcp port $port' \
            > $PEERCAP_TMP/peercap-$tag.tcpdump.log 2>&1 &
        echo \$! > $PEERCAP_TMP/peercap-$tag.tcpdump.pid
        nohup sh -c 'while :; do
                         date +\"T %s.%N\"
                         $PEERCAP_SS -tim \"sport = :$port\"
                         sleep 0.05
                     done' > $PEERCAP_TMP/peercap-$tag.ss 2>/dev/null &
        echo \$! > $PEERCAP_TMP/peercap-$tag.ss.pid
        sleep 1
    " >/dev/null 2>&1 || { echo "peercap: could not start on $peer" >&2; return 1; }
    echo "==> capturing at the peer into $PEERCAP_TMP/peercap-$tag.{pcap,ss}"
    return 0
}

peercap_stop() {
    local peer="$1" outdir="$2" tag="$3"
    [ -n "$peer" ] || return 1
    # By PID, from the file written at start.  `pkill -f` matches the remote
    # shell's own command line and has killed other people's runs; `pkill -x
    # tcpdump` kills a capture somebody else started on the same machine.
    ssh "$peer" "
        for p in tcpdump ss; do
            f=$PEERCAP_TMP/peercap-$tag.\$p.pid
            [ -f \$f ] && kill \$(cat \$f) 2>/dev/null
            rm -f \$f
        done
        sleep 1
    " >/dev/null 2>&1 || true
    scp -q "$peer:$PEERCAP_TMP/peercap-$tag.pcap" "$outdir/$tag.pcap" \
        2>/dev/null || { echo "peercap: no capture came back" >&2; return 1; }
    scp -q "$peer:$PEERCAP_TMP/peercap-$tag.ss" "$outdir/$tag.ss" \
        2>/dev/null || true
    return 0
}

peercap_report() {
    local outdir="$1" tag="$2"
    shift 2
    local pcap="$outdir/$tag.pcap"
    [ -f "$pcap" ] || { echo "peercap: no $pcap to read" >&2; return 0; }
    local args=("$pcap")
    [ -f "$outdir/$tag.ss" ] && args+=(--ss "$outdir/$tag.ss")
    echo
    echo "==> inbound loss (tests/perf/lossrate.py, peer-side capture)"
    python3 "$(dirname "${BASH_SOURCE[0]}")/lossrate.py" "${args[@]}" "$@" \
        | sed 's/^/    /'
    return "${PIPESTATUS[0]}"
}
