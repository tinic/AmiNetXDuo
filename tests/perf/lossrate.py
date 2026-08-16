#!/usr/bin/env python3
"""Inbound loss rate, receiver holes and round trips per chunk, from a
peer-side capture of one FitzBench arm.

    tests/perf/lossrate.py peer.pcap [--ss peer.ss] [--port 17712]

WHY NOT THROUGHPUT
--------------------------------------------------------------------------
Throughput is the last thing in the chain and it moves on its own: the same
library has read 1898 and 1824 KB/s on consecutive boots.  A change that
tripled the peer's retransmission rate therefore shipped invisibly, because
862 -> 796 KB/s reads as rig noise.

The retransmission rate is one number, it is measured on the peer where the
counters are exact, and it is upstream of everything: it sets ssthresh, which
sets cwnd, which sets how many round trips a chunk costs, which is the
throughput.  0.56% -> 1.4% is not ambiguous the way 862 -> 796 is.

WHAT IS COUNTED
--------------------------------------------------------------------------
    inbound loss rate = peer data segments RETRANSMITTED
                        ------------------------------- , read phases only
                        peer data segments SENT

Both counts are of segments carrying payload from the peer to the guest, and
the denominator includes the retransmissions, so this is the same ratio as
`ss`'s bytes_retrans/bytes_sent and the same event as the peer kernel's
TcpRetransSegs.  A segment is a retransmission when its sequence number is
below the highest sequence number the peer has already put on the wire.

READ PHASES ARE ISOLATED, NOT ASSUMED
--------------------------------------------------------------------------
FitzBench interleaves a write and a read per repetition on ONE connection,
and neither direction goes quiet during the other: the guest still sends
request headers during a read and the peer still replies during a write.  A
gap split therefore puts the whole arm in one burst.

What does separate them is segment size.  Only the direction being
transferred carries bulk segments, so the phase boundaries come from those:
bulk segments (payload >= --bulk) are cut into runs wherever the direction
changes or the stream is quiet for longer than --gap, and a run of peer->guest
bulk carrying at least --min-phase bytes is a read phase.  Everything else --
retransmission counting, hole lifetimes, round trips, is then restricted to
those windows.  This is what tests/perf/profsplit.py does to a profile,
except that a capture carries the direction of every segment so the windows
need no arithmetic to place.

RETRANSMISSION IS NOT LOSS
--------------------------------------------------------------------------
This is the trap the metric exists to avoid repeating.  A sender retransmits
when the receiver's acknowledgements stall, and they stall for two different
reasons that cost completely different amounts:

  - the segment never arrived: the hole is filled only by the retransmission,
    a round trip or a retransmission timeout later;
  - the segment arrived late or out of order: the hole is filled by the
    original, typically in well under a millisecond, and any retransmission
    the sender had already sent was wasted.

Both raise the retransmission rate identically.  A gate that reported "1.4%
loss" for a link that in fact reorders and recovers in 100 us would send the
next investigation exactly as far wrong as throughput did.  So this reports
them apart:

  RECEIVER HOLE   a run of --dupacks or more acknowledgements repeating one
                  number with no payload, which is the sender's own
                  fast-retransmit trigger.  It opens at the first repeat and
                  closes at the first acknowledgement past it.  Both ends are
                  observed at the peer and both are delayed by the same half
                  round trip, so the difference IS the hole's lifetime at the
                  guest.

                  The RFC also asks that a duplicate acknowledgement carry no
                  window change.  That test is NOT applied, because this
                  receiver re-advertises a different window on essentially
                  every acknowledgement and applying it finds no holes at all
                  in a capture that demonstrably contains them.  A single
                  repeat is not counted either: on an emulated card the guest
                  acknowledges on a ~13 ms cadence and one repeat is that
                  cadence, not a hole.

  SPURIOUS        a retransmission whose byte range was acknowledged sooner
                  than one round trip after it left the peer.  That
                  acknowledgement was already in flight, so the hole had been
                  filled by something else and the retransmission bought
                  nothing.  The test needs neither TCP timestamps nor DSACK,
                  which is the point: the peer kernel's own TCPFullUndo
                  counter sees the same events but only for a guest whose
                  acknowledgements carry timestamps, so it reports zero for a
                  stack that does not send them and cannot compare two
                  stacks.  Where both are available they agree, on a
                  Roadshow arm whose 156 retransmissions the peer kernel
                  undid all 156, this test called 153 of them spurious.

                  The round trip it compares against is the --rtt-floor
                  percentile of the arm's own measured round trips, NOT the
                  minimum.  The minimum is one sample and moves like one: it
                  came out 2.5 ms on one run of a library and 8.9 ms on the
                  next, which would move the classification of half the
                  retransmissions between two runs of the same build.  A low
                  percentile is the fastest round trip the path delivers
                  repeatedly, and it is stable to a few tenths of a
                  millisecond across runs.

  EFFECTIVE LOSS  retransmissions less the spurious ones, over the same
                  denominator.  This is the figure to gate on when the two
                  disagree; the raw rate is kept beside it because it is the
                  one `ss` and nstat will agree with.

WIRE-ORDER GAPS are counted separately: a peer data segment whose sequence
number is ahead of the highest yet sent, leaving a hole the peer itself fills
later.  On an egress capture that is the peer's own transmission order, so it
is normally zero and a non-zero count means the qdisc or the NIC is
reordering before the frames even leave.  Reordering that happens further
downstream cannot be seen directly in an egress capture at all, it is only
visible through hole lifetimes, which is why those are computed.

ROUND TRIPS PER CHUNK
--------------------------------------------------------------------------
The workload is a request/response exchange per chunk, so the read rate is
set by how many sender bursts each chunk takes rather than by any window
calculation: cwnd x MSS / RTT was 39-165% out across three stacks where
chunk_bytes / (bursts x inter-burst gap) was within 2%.  Bursts are runs of
peer data segments separated by more than --burst-gap, chunks are counted
from the guest's request segments, and the inter-burst gap is measured rather
than assumed.

THE `ss` LOG
--------------------------------------------------------------------------
--ss adds cwnd and ssthresh from an `ss -tim` poll of the peer.  Two things
make a naive read of one wrong.  `ss` keeps a closed socket visible for about
100 seconds, so a log contains the PREVIOUS arm's dead socket as well as this
one's, they are told apart by the ephemeral port, and a dead one is
recognisable anyway as a socket whose byte counters never move.  And the poll
covers writes as well as reads, so samples are kept only where bytes_sent is
climbing at more than --ss-idle of its own peak rate, which is the read
phases.

SPDX-License-Identifier: MIT
"""

import argparse
import collections
import os
import re
import struct
import sys

BULK_DEFAULT = 512

# --------------------------------------------------------------- the capture --


# The link layers this reads, as (name, header length, where the EtherType is).
#
# THREE OF THEM, NOT ONE.  tests/perf/peercap.sh captures on `any` by default,
# which libpcap 1.10 records as LINUX_SLL2 (276) and older ones as LINUX_SLL
# (113); this only knew EN10MB and raised SystemExit on anything else.  So
# run-fitzbench.sh -w collected a capture, -l and -L asked this to gate on it,
# and the gate died on its first line -- read as "no loss data" rather than as
# a broken rig.
#
# TEACHING THE PARSER rather than pinning AMINETXDUO_PEER_IFACE=ens18: `any`
# is what makes the capture work without knowing the peer's interface names,
# a pinned name is wrong on the next peer and silently captures nothing when
# the traffic takes another interface, and captures already on disk stay
# readable this way.
LINKS = {
    1:   ("EN10MB", 14, 12),        # Ethernet: type at 12, payload at 14
    113: ("LINUX_SLL", 16, 14),     # cooked v1: type at 14, payload at 16
    276: ("LINUX_SLL2", 20, 0),     # cooked v2: type at 0, payload at 20
}


def packets(path):
    """(timestamp, IPv4 header onward) for a classic or nanosecond pcap.

    The link-layer header is stripped here, so everything below sees one
    shape whatever the capture was taken on.  Frames that are not IPv4 --
    ARP, IPv6, anything else on a shared interface -- are dropped.
    """
    with open(path, "rb") as fh:
        head = fh.read(24)
        if len(head) < 24:
            raise SystemExit("%s: too short to be a pcap" % path)
        magic = struct.unpack("<I", head[:4])[0]
        order, div = {
            0xA1B2C3D4: ("<", 1e6),
            0xD4C3B2A1: (">", 1e6),
            0xA1B23C4D: ("<", 1e9),
            0x4D3CB2A1: (">", 1e9),
        }.get(magic, (None, None))
        if order is None:
            raise SystemExit("%s: not a pcap (magic %08x)" % (path, magic))
        link = struct.unpack(order + "I", head[20:24])[0]
        if link not in LINKS:
            raise SystemExit(
                "%s: link type %d; this reads %s"
                % (path, link, ", ".join(
                    "%s (%d)" % (n, k) for k, (n, _, _) in sorted(LINKS.items()))))
        _name, llen, tpos = LINKS[link]
        while True:
            hdr = fh.read(16)
            if len(hdr) < 16:
                return
            sec, frac, incl, _orig = struct.unpack(order + "IIII", hdr)
            data = fh.read(incl)
            if len(data) < incl:
                return
            if len(data) < llen + 20:
                continue
            proto = struct.unpack(">H", data[tpos:tpos + 2])[0]
            off = llen
            if proto == 0x8100:                               # 802.1Q
                if len(data) < off + 4 + 20:
                    continue
                proto = struct.unpack(">H", data[off + 2:off + 4])[0]
                off += 4
            if proto != 0x0800:
                continue
            yield sec + frac / div, data[off:]


Seg = collections.namedtuple("Seg", "t src dst sport dport seq ack plen flags win")

FIN, SYN, RST, PSH, ACK, URG = 1, 2, 4, 8, 16, 32


def tcp(data):
    """The one TCP segment in an IPv4 packet, or None.

    `data` starts at the IP header: packets() has already stripped whichever
    link layer the capture was taken on.

    The payload length comes from the IP header's total length, not from the
    captured bytes: these captures are taken with a short snaplen, so the
    frame on disk is truncated and len(data) is not the segment's size.  A
    header is never truncated, 20 + 40 is 60 bytes at the very most.
    """
    if len(data) < 20:
        return None
    off = 0
    if (data[off] >> 4) != 4 or data[off + 9] != 6:
        return None
    ihl = (data[off] & 0xF) * 4
    total = struct.unpack(">H", data[off + 2:off + 4])[0]
    src = ".".join(str(b) for b in data[off + 12:off + 16])
    dst = ".".join(str(b) for b in data[off + 16:off + 20])
    t = off + ihl
    if len(data) < t + 20:
        return None
    sport, dport = struct.unpack(">HH", data[t:t + 4])
    seq, ack = struct.unpack(">II", data[t + 4:t + 12])
    doff = (data[t + 12] >> 4) * 4
    flags = data[t + 13]
    win = struct.unpack(">H", data[t + 14:t + 16])[0]
    return sport, dport, seq, ack, max(0, total - ihl - doff), flags, win, src, dst


def seq_lt(a, b):
    """a < b in 32-bit sequence space."""
    return ((a - b) & 0xFFFFFFFF) > 0x7FFFFFFF


def read_capture(path, port, eph):
    """Every segment of the chosen connection, in capture order.

    The connection is chosen by payload volume when it is not named, because a
    capture filtered on a port still holds the previous arm's socket: `ss`
    keeps one visible for ~100 s and a lingering FIN or RST lands in the file.
    The busiest connection is the arm.
    """
    by_conn = collections.defaultdict(list)
    volume = collections.Counter()
    for ts, frame in packets(path):
        parsed = tcp(frame)
        if parsed is None:
            continue
        sport, dport, seq, ack, plen, flags, win, src, dst = parsed
        if port and sport != port and dport != port:
            continue
        peer_out = (sport == port) if port else None
        key = (dport, sport) if peer_out else (sport, dport)
        by_conn[key].append(Seg(ts, src, dst, sport, dport, seq, ack,
                                plen, flags, win))
        volume[key] += plen
    if not by_conn:
        raise SystemExit("%s: no TCP on port %s" % (path, port))
    if eph:
        key = next((k for k in by_conn if eph in k), None)
        if key is None:
            raise SystemExit("%s: no connection with port %d" % (path, eph))
    else:
        key = volume.most_common(1)[0][0]
    return key, by_conn[key], len(by_conn)


# ---------------------------------------------------------------- the phases --


def phases(segs, port, bulk, gap, min_phase):
    """[(t0, t1, peer_is_sender)] for every bulk run in the capture.

    Direction is the discriminator, not silence: both directions carry
    traffic throughout, but only the one being transferred carries bulk.
    """
    marks = [(s.t, s.sport == port) for s in segs if s.plen >= bulk]
    if not marks:
        raise SystemExit("no bulk segments -- is --bulk (%d) above the MSS?" % bulk)
    runs, cur = [], [marks[0]]
    for m in marks[1:]:
        if m[1] != cur[-1][1] or m[0] - cur[-1][0] > gap:
            runs.append(cur)
            cur = [m]
        else:
            cur.append(m)
    runs.append(cur)
    out = []
    for r in runs:
        span = (r[0][0], r[-1][0], r[0][1])
        n = sum(1 for _ in r)
        if n * bulk >= min_phase:
            out.append(span)
    return out


class Windows(object):
    """A set of time windows with an O(log n) membership test."""

    def __init__(self, spans):
        self.spans = sorted((a, b) for a, b, _d in spans)

    def __contains__(self, t):
        import bisect
        i = bisect.bisect_right(self.spans, (t, float("inf"))) - 1
        return i >= 0 and self.spans[i][1] >= t

    def seconds(self):
        return sum(b - a for a, b in self.spans)


# ------------------------------------------------------- loss and reordering --


class Analysis(object):
    pass


def analyse(segs, port, reads, rtt_floor_pct, dupacks):
    """Retransmissions, receiver holes and round-trip samples, read phases only."""
    a = Analysis()
    a.data_segs = a.retrans = a.wire_gaps = 0
    a.data_bytes = a.retrans_bytes = 0
    retrans = []          # (t, seq, end)
    sent = []             # (t, end) for originals, for the round-trip estimate
    highest = None
    acks = []             # (t, ack, win) guest -> peer, pure acknowledgements

    for s in segs:
        peer_out = s.sport == port
        if peer_out and s.plen:
            end = (s.seq + s.plen) & 0xFFFFFFFF
            is_retrans = False
            if highest is None:
                highest = end
            else:
                if seq_lt(s.seq, highest):
                    is_retrans = True
                elif seq_lt(highest, s.seq) and s.t in reads:
                    a.wire_gaps += 1
                if seq_lt(highest, end):
                    highest = end
            if s.t in reads:
                a.data_segs += 1
                a.data_bytes += s.plen
                if is_retrans:
                    a.retrans += 1
                    a.retrans_bytes += s.plen
                    retrans.append((s.t, s.seq, end))
                else:
                    sent.append((s.t, end))
        elif not peer_out and s.plen == 0 and (s.flags & ACK):
            acks.append((s.t, s.ack, s.win))

    # Minimum round trip, from originals only: an acknowledgement covering a
    # retransmitted range is ambiguous about which copy it answers (Karn).
    samples = []
    i = 0
    for t, ack, _w in acks:
        while i < len(sent) and not seq_lt(ack, sent[i][1]):
            samples.append(t - sent[i][0])
            i += 1
    samples = sorted(s for s in samples if s > 0)
    a.rtt_samples = len(samples)
    a.minrtt = samples[0] if samples else 0.0
    a.medrtt = samples[len(samples) // 2] if samples else None
    a.rtt_floor = (samples[min(len(samples) - 1, int(len(samples) * rtt_floor_pct))]
                   if samples else 0.0)

    # Receiver holes.  A run of repeated acknowledgement numbers with no
    # payload; see the header for why the window is not part of the test and
    # why a single repeat does not count.
    a.holes = []
    a.stalls = 0
    prev_ack = None
    dups = 0
    opened = None
    for t, ack, _win in acks:
        if prev_ack is None:
            prev_ack = ack
            continue
        if ack == prev_ack:
            dups += 1
            if dups == 1:
                opened = t
        elif seq_lt(prev_ack, ack):
            if dups and opened is not None and opened in reads:
                a.stalls += 1
                if dups >= dupacks:
                    a.holes.append((opened, prev_ack, t - opened, dups))
            prev_ack = ack
            dups, opened = 0, None
    a.dupacks = dupacks

    # A retransmission is spurious when its range was acknowledged sooner than
    # one round trip after it was sent: that acknowledgement was already in
    # flight and the hole had been filled by something else.
    ack_times = [(t, ack) for t, ack, _w in acks]
    a.spurious = 0
    a.retrans_ack_gap = []
    j = 0
    for t, _seq, end in retrans:
        while j < len(ack_times) and ack_times[j][0] <= t:
            j += 1
        k = j
        while k < len(ack_times) and seq_lt(ack_times[k][1], end):
            k += 1
        if k < len(ack_times):
            gap = ack_times[k][0] - t
            a.retrans_ack_gap.append(gap)
            if gap < a.rtt_floor:
                a.spurious += 1

    # Holes that no retransmission covered were filled by an original arriving
    # late: reordering, with no sender involvement at all.
    a.holes_reordered = 0
    for opened, at, life, _d in a.holes:
        covered = any(opened - a.rtt_floor <= r[0] <= opened + life
                      and not seq_lt(at, r[1]) and seq_lt(at, r[2])
                      for r in retrans)
        if not covered:
            a.holes_reordered += 1
    return a


# ------------------------------------------------------- round trips a chunk --


def breakdown(segs, port, spans, step=1 << 20):
    """Loss per read phase and per `step` bytes into a phase.

    A rate that climbs with the bytes already transferred would mean a short
    transfer never sees it, and that a measurement taken at one transfer size
    says nothing about another.  A flat one means the size is not the
    variable.
    """
    reads = [s for s in spans if s[2]]
    highest = None
    per_phase = [[0, 0, 0] for _ in reads]          # segments, retrans, bytes
    per_step = collections.defaultdict(lambda: [0, 0])
    for s in segs:
        if not (s.sport == port and s.plen):
            continue
        end = (s.seq + s.plen) & 0xFFFFFFFF
        is_retrans = False
        if highest is None:
            highest = end
        else:
            if seq_lt(s.seq, highest):
                is_retrans = True
            if seq_lt(highest, end):
                highest = end
        for i, (t0, t1, _d) in enumerate(reads):
            if t0 <= s.t <= t1:
                p = per_phase[i]
                p[0] += 1
                p[2] += s.plen
                k = per_step[p[2] // step]
                k[0] += 1
                if is_retrans:
                    p[1] += 1
                    k[1] += 1
                break
    return per_phase, per_step


def bursts(segs, port, reads, burst_gap):
    """(bursts, chunks, [inter-burst gaps]) over the read phases.

    A chunk is one request/response exchange, counted from the guest's own
    request segments; a burst is a run of peer data segments with no pause
    longer than --burst-gap in it.
    """
    times = [s.t for s in segs if s.sport == port and s.plen and s.t in reads]
    reqs = [s.t for s in segs if s.dport == port and s.plen and s.t in reads]
    if not times:
        return 0, 0, []
    n, gaps = 1, []
    for i in range(1, len(times)):
        d = times[i] - times[i - 1]
        if d > burst_gap:
            n += 1
            gaps.append(d)
    # FitzBench sends more than one request segment per chunk (a fixed header
    # and a variable one), so the chunk count is the number of DISTINCT
    # request instants, collapsed at the same gap the bursts use.
    chunks = 1
    for i in range(1, len(reqs)):
        if reqs[i] - reqs[i - 1] > burst_gap:
            chunks += 1
    return n, chunks, gaps


# ------------------------------------------------------------------ the `ss` --

SS_FIELDS = ("cwnd", "ssthresh", "bytes_sent", "bytes_retrans", "rtt")


def read_ss(path, eph):
    """{port: [(t, {field: value})]} from an `ss -tim` poll log."""
    out = collections.defaultdict(list)
    now = None
    cur = None
    for line in open(path, errors="replace"):
        if line.startswith("T "):
            try:
                now = float(line.split()[1])
            except (IndexError, ValueError):
                now = None
            continue
        # `ss -tim` prints the state column only with -a or -H; the polls this
        # reads start straight at Recv-Q, so the state is optional here.
        m = re.match(r"\s*(?:[A-Z-]+\s+)?\d+\s+\d+\s+\S+:\d+\s+\S+:(\d+)\s*$",
                     line)
        if m:
            cur = int(m.group(1))
            continue
        if cur is None or now is None or not line.startswith("\t"):
            continue
        vals = {}
        for f in SS_FIELDS:
            mm = re.search(r"(?:^|\s)" + f + r":([0-9.]+)", line)
            if mm:
                vals[f] = float(mm.group(1))
        vals["app_limited"] = 1.0 if re.search(r"(?:^|\s)app_limited(?:\s|$)",
                                               line) else 0.0
        out[cur].append((now, vals))
        cur = None
    if eph:
        return {eph: out.get(eph, [])}
    return out


def ss_report(path, eph, idle_frac):
    """cwnd and ssthresh medians over the samples where bytes_sent is climbing."""
    socks = read_ss(path, eph)
    lines = []
    for portno, samples in sorted(socks.items()):
        if len(samples) < 5:
            continue
        slopes = []
        for i in range(1, len(samples)):
            dt = samples[i][0] - samples[i - 1][0]
            db = samples[i][1].get("bytes_sent", 0) - \
                samples[i - 1][1].get("bytes_sent", 0)
            slopes.append(db / dt if dt > 0 else 0.0)
        peak = max(slopes) if slopes else 0.0
        if peak <= 0:
            lines.append("  port %d: %d samples, bytes_sent never moved, this "
                         "is the previous arm's closed socket" % (portno, len(samples)))
            continue
        keep = [samples[i + 1][1] for i, s in enumerate(slopes)
                if s > idle_frac * peak]
        if not keep:
            continue

        def med(f):
            v = sorted(s[f] for s in keep if f in s)
            return v[len(v) // 2] if v else float("nan")

        lines.append("  port %d: %d of %d samples in the sending phases"
                     % (portno, len(keep), len(samples)))
        lines.append("    cwnd median %.0f, ssthresh median %.0f, rtt median "
                     "%.1f ms" % (med("cwnd"), med("ssthresh"), med("rtt")))
        applim = sum(1 for s in keep if s.get("app_limited"))
        lines.append("    app_limited in %d of them" % applim)
        if "bytes_sent" in keep[-1] and "bytes_retrans" in keep[-1]:
            bs = keep[-1]["bytes_sent"] - keep[0].get("bytes_sent", 0)
            br = keep[-1]["bytes_retrans"] - keep[0].get("bytes_retrans", 0)
            if bs > 0:
                lines.append("    bytes_retrans/bytes_sent over the same "
                             "samples %.3f %%" % (100.0 * br / bs))
    return lines


# ------------------------------------------------------------------- reports --


def pct(n, d):
    return 100.0 * n / d if d else float("nan")


def main():
    ap = argparse.ArgumentParser(
        description="inbound loss rate and receiver-hole lifetimes from a "
                    "peer-side capture of a FitzBench arm",
        epilog="the loss rate is peer retransmissions over peer data segments "
               "sent, read phases only; see the header comment for what "
               "separates a lost segment from a reordered one")
    ap.add_argument("pcap", help="peer-side capture of the transfer")
    ap.add_argument("--ss", help="`ss -tim` poll log from the same arm")
    ap.add_argument("--port", type=int, default=17712,
                    help="the peer's listening port (default 17712); 0 to "
                         "take the busiest connection in the file")
    ap.add_argument("--eph", type=int,
                    help="the arm's ephemeral port, when a stale socket from "
                         "the previous arm is in the same file")
    ap.add_argument("--bulk", type=int, default=BULK_DEFAULT,
                    help="payload at or above which a segment marks the "
                         "direction being transferred (default 512)")
    ap.add_argument("--gap", type=float, default=0.25,
                    help="seconds of quiet that end a phase (default 0.25)")
    ap.add_argument("--min-phase", type=int, default=1 << 18,
                    help="bytes a bulk run must carry to count as a phase "
                         "(default 256 KB)")
    ap.add_argument("--burst-gap", type=float, default=0.005,
                    help="seconds between peer data segments that start a new "
                         "burst (default 0.005)")
    ap.add_argument("--rtt-floor", type=float, default=0.01,
                    help="percentile of the arm's own round trips taken as "
                         "the fastest one the path repeats (default 0.01); a "
                         "retransmission acknowledged sooner than this is "
                         "spurious")
    ap.add_argument("--dupacks", type=int, default=3,
                    help="repeated acknowledgements that make a receiver hole "
                         "(default 3, the fast-retransmit trigger)")
    ap.add_argument("--ss-idle", type=float, default=0.05,
                    help="fraction of its own peak bytes_sent slope below "
                         "which an `ss` sample is not a read phase")
    ap.add_argument("--max-loss", type=float,
                    help="fail with status 1 if the loss rate is above this "
                         "percentage")
    ap.add_argument("--max-effective-loss", type=float,
                    help="the same, against the rate with spurious "
                         "retransmissions removed")
    ap.add_argument("--max-rt-per-chunk", type=float,
                    help="fail if the read costs more round trips a chunk "
                         "than this; the tightest of these numbers, and the "
                         "one that predicts the rate")
    ap.add_argument("--per-phase", action="store_true",
                    help="also break the loss rate down by read phase and by "
                         "megabyte into each phase, which is what says "
                         "whether a short transfer would have shown it")
    ap.add_argument("--brief", action="store_true",
                    help="one line: file, loss %%, effective loss %%, holes")
    args = ap.parse_args()

    port = args.port
    key, segs, nconn = read_capture(args.pcap, port, args.eph)
    if not port:
        port = min(key)                       # the listening side is the lower
    spans = phases(segs, port, args.bulk, args.gap, args.min_phase)
    reads = Windows([s for s in spans if s[2]])
    writes = Windows([s for s in spans if not s[2]])
    if not reads.spans:
        raise SystemExit("%s: no read phase found, the peer sent no bulk "
                         "run of %d bytes" % (args.pcap, args.min_phase))

    a = analyse(segs, port, reads, args.rtt_floor, args.dupacks)
    loss = pct(a.retrans, a.data_segs)
    eff = pct(a.retrans - a.spurious, a.data_segs)

    if args.brief:
        lives = sorted(h[2] for h in a.holes)
        nb, nc, gaps = bursts(segs, port, reads, args.burst_gap)
        print("%-26s loss %6.3f %%  effective %6.3f %%  holes %4d  "
              "hole p50 %6.2f ms  rtc %4.2f  %6.0f KB/s"
              % (os.path.basename(args.pcap), loss, eff, len(a.holes),
                 (lives[len(lives) // 2] * 1e3) if lives else 0.0,
                 (float(nb) / nc) if nc else 0.0,
                 a.data_bytes / reads.seconds() / 1024))
    else:
        print("capture      %s" % args.pcap)
        print("connection   peer port %d, guest port %d%s"
              % (port, max(key) if max(key) != port else min(key),
                 "" if nconn == 1 else
                 "   (%d connections in the file; took the busiest)" % nconn))
        print("phases       %d read, %d write; %.2f s of reading out of %.2f s"
              % (len(reads.spans), len(writes.spans), reads.seconds(),
                 segs[-1].t - segs[0].t))
        print()
        print("INBOUND LOSS RATE   %.3f %%    %d retransmitted of %d data "
              "segments sent" % (loss, a.retrans, a.data_segs))
        print("                              %d B of %d B"
              % (a.retrans_bytes, a.data_bytes))
        print("  spurious                    %d of those were acknowledged "
              "sooner than %.2f ms" % (a.spurious, a.rtt_floor * 1e3))
        print("                              after being sent, so the "
              "acknowledgement was")
        print("                              already in flight and they "
              "filled nothing")
        print("EFFECTIVE LOSS      %.3f %%    the rest" % eff)
        if a.retrans_ack_gap:
            gaps = sorted(a.retrans_ack_gap)

            def qg(f):
                return gaps[min(len(gaps) - 1, int(len(gaps) * f))] * 1e3
            print("  retransmission to the acknowledgement that covered it, "
                  "ms:")
            print("    p10 %.2f   p50 %.2f   p90 %.2f   max %.2f"
                  % (qg(0.1), qg(0.5), qg(0.9), gaps[-1] * 1e3))
        print()
        print("receiver holes      %d runs of %d or more repeated "
              "acknowledgements" % (len(a.holes), a.dupacks))
        print("                    (%d acknowledgement stalls in all, most of "
              "them one" % a.stalls)
        print("                     repeat, which is the guest's "
              "acknowledgement cadence)")
        if a.holes:
            lives = sorted(h[2] for h in a.holes)

            def q(f):
                return lives[min(len(lives) - 1, int(len(lives) * f))] * 1e3
            print("  filled with no retransmission covering them  %d "
                  "(reordering)" % a.holes_reordered)
            print("  filled after a retransmission                %d"
                  % (len(a.holes) - a.holes_reordered))
            print("  lifetime ms   p50 %.3f   p90 %.3f   max %.3f"
                  % (q(0.5), q(0.9), lives[-1] * 1e3))
            buckets = collections.Counter()
            for l in lives:
                buckets["under 1 ms" if l < 1e-3 else
                        "1 to 5 ms" if l < 5e-3 else
                        "5 to 15 ms" if l < 15e-3 else "15 ms or more"] += 1
            for name in ("under 1 ms", "1 to 5 ms", "5 to 15 ms",
                         "15 ms or more"):
                print("    %-14s %5d" % (name, buckets[name]))
            # A hole that closed inside a round trip was filled by something
            # already in flight, which is the same statement the spurious
            # test makes about a retransmission.  The two count different
            # populations, one hole can attract several retransmissions,
            # and a hole the sender never reacted to attracts none, so
            # these are not equal, but they should move together, and a
            # reading where one says reordering and the other says loss is a
            # reading not to quote.
            short = sum(1 for l in lives if l < a.rtt_floor)
            print("  closed inside the %.2f ms round-trip floor  %d of %d "
                  "holes (%.0f %%)," % (a.rtt_floor * 1e3, short, len(lives),
                                        pct(short, len(lives))))
            print("                    against %d of %d retransmissions "
                  "spurious (%.0f %%)"
                  % (a.spurious, a.retrans, pct(a.spurious, a.retrans)))
        print("wire-order gaps     %d  (peer data sent ahead of its own "
              "highest sequence;" % a.wire_gaps)
        print("                     this is an egress capture, so reordering "
              "further down")
        print("                     the path shows up in hole lifetimes and "
              "not here)")
        print("round trip          min %.2f ms, %g%% floor %.2f ms, median %s,"
              " %d samples"
              % (a.minrtt * 1e3, args.rtt_floor * 100, a.rtt_floor * 1e3,
                 "%.2f ms" % (a.medrtt * 1e3) if a.medrtt else "n/a",
                 a.rtt_samples))
        print()
        nb, nc, gaps = bursts(segs, port, reads, args.burst_gap)
        if nc:
            gaps = sorted(gaps)
            g = gaps[len(gaps) // 2] if gaps else 0.0
            per_chunk = float(a.data_bytes) / nc
            measured = reads.seconds() / nc
            print("round trips a chunk %.2f    %d bursts over %d chunks of "
                  "%.0f B" % (float(nb) / nc, nb, nc, per_chunk))
            print("  inter-burst gap   %.2f ms median" % (g * 1e3))
            print("  bursts x gap      %.1f ms a chunk, against %.1f ms "
                  "measured" % (nb / float(nc) * g * 1e3, measured * 1e3))
            print("  read rate         %.0f KB/s over the read phases"
                  % (a.data_bytes / reads.seconds() / 1024))

        if args.per_phase:
            per_phase, per_step = breakdown(segs, port, spans)
            print()
            print("loss by read phase")
            for i, (n, r, by) in enumerate(per_phase):
                print("  phase %d   %5d segments  %4d retransmitted  %6.3f %%"
                      "  %d B" % (i, n, r, pct(r, n), by))
            print("loss by megabyte into a phase, flat means a shorter "
                  "transfer would")
            print("have shown the same rate, climbing means it would not")
            for m in sorted(per_step):
                n, r = per_step[m]
                print("  MB %-3d    %5d segments  %4d retransmitted  %6.3f %%"
                      % (m, n, r, pct(r, n)))

        if args.ss:
            print()
            print("peer socket (`ss -tim`, sending samples only)")
            for line in ss_report(args.ss, args.eph, args.ss_idle):
                print(line)

    rc = 0
    if args.max_rt_per_chunk is not None:
        nb, nc, _g = bursts(segs, port, reads, args.burst_gap)
        rtc = (float(nb) / nc) if nc else 0.0
        if rtc > args.max_rt_per_chunk:
            print("FAIL: %.2f round trips a chunk is above the %.2f gate"
                  % (rtc, args.max_rt_per_chunk), file=sys.stderr)
            rc = 1
    if args.max_loss is not None and loss > args.max_loss:
        print("FAIL: loss %.3f %% is above the %.3f %% gate"
              % (loss, args.max_loss), file=sys.stderr)
        rc = 1
    if args.max_effective_loss is not None and eff > args.max_effective_loss:
        print("FAIL: effective loss %.3f %% is above the %.3f %% gate"
              % (eff, args.max_effective_loss), file=sys.stderr)
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
