#!/usr/bin/env python3
"""Acknowledgement and window behaviour of one endpoint, from a peer-side
capture.  key=value on stdout, RESULT=pass or RESULT=fail, exit status.

    tests/perf/ackscope.py capture.pcap --guest 192.168.1.240

WHY THIS EXISTS
--------------------------------------------------------------------------
A throughput harness reports bytes and seconds.  It cannot tell a stack that
acknowledges badly from one that is merely slow, and three open questions in
docs/BACKLOG.md are stuck on exactly that distinction: run-fitzbench.sh
measured 1.01 round trips per chunk and 0% rwnd_limited, which is a workload
that never touches the ACK clock or the receive window, so every comparison
taken with it is silent about both.

This reads the wire instead.  Every acknowledgement the guest sends and every
window it advertises is in a capture at the other end, so nothing runs on the
Amiga -- which matters, because the machine in question is a bare A1200 with
2 MB and no Fast RAM where a capture would perturb what it measured.

THE GUEST IS AN ADDRESS, NOT A PORT
--------------------------------------------------------------------------
tests/perf/lossrate.py selects its connection by the FitzBench port, which
ties it to that one workload.  Here the endpoint under test is named by its
IP address and the connection is whichever one carried the most payload.  An
emulated guest in the lab and a real A1200 on the LAN then differ by the
value of --guest and by nothing else, and any workload at all can drive the
transfer: iperf, Fitz, a web fetch, a person typing.

DIRECTION IS NEVER FOLDED TOGETHER
--------------------------------------------------------------------------
Every counter is reported for the guest and for the peer separately, because
the same number means opposite things on the two sides.  guest_retrans is the
Amiga failing to get its own data through; peer_retrans is the Amiga failing
to acknowledge fast enough or losing what arrives.  A tool that added them
would report the 0.16.6 regression -- read down a fifth, write slightly up --
as no change at all, which is the mistake tests/perf/run-lossgate.sh already
exists to avoid at the throughput layer.

WHAT IT MEASURES
--------------------------------------------------------------------------
  retransmissions   per direction, a segment whose sequence number is below
                    the highest that side has already put on the wire.

  duplicate ACKs    runs of repeated acknowledgement numbers, BUCKETED BY RUN
                    LENGTH.  Three is the fast-retransmit trigger and two is
                    not, so a stack that emits pairs and a stack that emits
                    triples behave completely differently at the sender while
                    a single "dupacks" count reports them the same.  Runs of
                    one are counted too and reported apart: on an emulated
                    card the guest acknowledges on a ~13 ms cadence and one
                    repeat is that cadence rather than a hole.

  zero window       every advertisement of zero, and how long each one lasted
                    -- opened at the zero, closed at the next advertisement
                    above zero from the same side.  A zero window is not a
                    fault: a 68020 receiving at line rate is supposed to shut
                    the sender up.  A zero window that STAYS shut is, because
                    it means the window update was lost and only the persist
                    timer will reopen it, so the maximum matters more than
                    the count.  Probes into a shut window are counted beside
                    them.

  silly window      advertisements above zero but below one MSS.  RFC 1122
                    4.2.3.3 says a receiver must not do this, and a stack
                    that dribbles out 200-byte windows makes the sender pay
                    a header per fragment for the rest of the connection.

  ACK delay         the DISTRIBUTION, not a mean.  The pathology is bimodal:
                    a stack that acknowledges every second segment
                    immediately and holds the odd one for a 200 ms timer has
                    a perfectly reasonable mean and a p90 that is the whole
                    problem.  Percentiles and a bucket histogram, both.

                    Measured at the peer, so each sample carries one round
                    trip that is not the guest's fault -- the data segment is
                    timestamped as it leaves and the acknowledgement as it
                    arrives.  ackdelay_excess_* subtracts the round-trip
                    floor and is the guest's own contribution.

  ACK density       acknowledgements per peer data segment, and bytes per
                    acknowledgement.  This is the ACK clock as a single
                    number: 0.5 is the every-other-segment an RFC 1122
                    receiver owes, and a stack starving a sender's clock
                    shows here before it shows anywhere else.

  in flight vs      unacknowledged bytes at each of the sender's own
  advertised        transmissions, against the window the guest advertised at
  window            that moment.  A stack that advertises 32 KB and never has
                    more than 4 KB outstanding is leaving three quarters of
                    the link idle, and no byte count can see it.

  round trip        samples from data and the acknowledgement covering it,
                    per direction, with Karn's rule applied: a range that was
                    retransmitted is ambiguous about which copy is being
                    acknowledged and is dropped rather than averaged in.

WHAT IT DELIBERATELY DOES NOT MEASURE
--------------------------------------------------------------------------
  loss against reordering.  tests/perf/lossrate.py already separates them,
  with a spurious-retransmission test and hole lifetimes, and doing it a
  second way here would give two numbers for one question.  Point that at the
  same capture.

  cwnd, ssthresh and why the SENDER stopped.  Those are not in a capture at
  all -- a capture shows a sender that paused, never whether it paused
  because the window was full or because it had nothing to send.  peercap.sh
  collects `ss -tim` for that and lossrate.py reads it.

  anything from the guest's own counters.  The whole point is that nothing
  runs on the target.

  SACK block contents.  Whether SACK was negotiated is reported, because it
  changes how a sender reacts to the dupacks counted here; what the blocks
  said is a loss-recovery question and belongs with lossrate.py.

WINDOW SCALING IS EITHER KNOWN OR THE WINDOW GATES ARE SKIPPED
--------------------------------------------------------------------------
The scale factor is negotiated in the SYN and appears nowhere else, so a
capture that started mid-connection cannot have its window fields read.
Rather than report a window 64 times too small -- which would call a healthy
stack silly-windowed on every segment -- the shift is reported as unknown and
every gate that depends on it comes back skip.

SPDX-License-Identifier: MIT
"""

import argparse
import collections
import os
import struct
import sys

# The pcap reader, the link layers and the sequence comparison come from
# lossrate.py rather than being written again: LINUX_SLL2 versus LINUX_SLL
# versus EN10MB, and the nanosecond magic numbers, are knowledge that was paid
# for once (a whole loss gate died on its first line for want of it) and there
# is no version of it that should be allowed to drift between two files in one
# directory.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lossrate import packets, seq_lt          # noqa: E402

FIN, SYN, RST, PSH, ACK, URG = 1, 2, 4, 8, 16, 32

MSS_DEFAULT = 1460

Seg = collections.namedtuple(
    "Seg", "t src dst sport dport seq ack plen flags win wscale mss sackok ts")


def parse(data):
    """One IPv4 TCP segment, options included, or None.

    `data` starts at the IP header; packets() has stripped the link layer.

    The payload length comes from the IP header's total length rather than
    from the bytes on disk, because these captures are taken with a short
    snaplen and len(data) is not the segment's size.  The OPTIONS are read
    from the bytes on disk and can therefore be truncated -- a header is at
    most 60 bytes and the snaplen this repo captures with reaches them, but a
    file taken elsewhere might not, so a short read yields None for the option
    rather than a wrong value.
    """
    if len(data) < 20 or (data[0] >> 4) != 4 or data[9] != 6:
        return None
    ihl = (data[0] & 0xF) * 4
    total = struct.unpack(">H", data[2:4])[0]
    src = ".".join(str(b) for b in data[12:16])
    dst = ".".join(str(b) for b in data[16:20])
    t = ihl
    if len(data) < t + 20:
        return None
    sport, dport = struct.unpack(">HH", data[t:t + 4])
    seq, ack = struct.unpack(">II", data[t + 4:t + 12])
    doff = (data[t + 12] >> 4) * 4
    flags = data[t + 13]
    win = struct.unpack(">H", data[t + 14:t + 16])[0]
    plen = max(0, total - ihl - doff)

    wscale = mss = None
    sackok = False
    ts = False
    opts = data[t + 20:t + doff]
    i = 0
    while i < len(opts):
        kind = opts[i]
        if kind == 0:                         # EOL
            break
        if kind == 1:                         # NOP
            i += 1
            continue
        if i + 1 >= len(opts):
            break
        ln = opts[i + 1]
        if ln < 2 or i + ln > len(opts):
            break
        if kind == 2 and ln == 4:
            mss = struct.unpack(">H", opts[i + 2:i + 4])[0]
        elif kind == 3 and ln == 3:
            wscale = opts[i + 2]
        elif kind == 4 and ln == 2:
            sackok = True
        elif kind == 8:
            ts = True
        i += ln
    return Seg(0.0, src, dst, sport, dport, seq, ack, plen, flags, win,
               wscale, mss, sackok, ts)


def read_capture(path, guest, port=None, eph=None):
    """(peer address, [Seg]) for the busiest connection the guest is in.

    Busiest by payload, because a capture of a real machine holds whatever
    else it was doing, and on the lab peer it also holds the PREVIOUS arm's
    lingering FIN or RST: `ss` keeps a closed socket visible for about a
    hundred seconds.
    """
    by_conn = collections.defaultdict(list)
    volume = collections.Counter()
    for ts, frame in packets(path):
        s = parse(frame)
        if s is None:
            continue
        if guest not in (s.src, s.dst):
            continue
        if port and port not in (s.sport, s.dport):
            continue
        if eph and eph not in (s.sport, s.dport):
            continue
        if s.src == guest:
            key = (s.dst, s.sport, s.dport)      # peer addr, guest port, peer port
        else:
            key = (s.src, s.dport, s.sport)
        by_conn[key].append(s._replace(t=ts))
        volume[key] += s.plen
    if not by_conn:
        raise SystemExit(2)
    key = volume.most_common(1)[0][0]
    return key, by_conn[key], len(by_conn)


# --------------------------------------------------------------- one direction --


class Side(object):
    """Everything measured about the segments ONE endpoint sent."""

    def __init__(self, name):
        self.name = name
        self.segs = self.data_segs = self.retrans = 0
        self.data_bytes = self.retrans_bytes = 0
        self.pure_acks = 0
        self.rst = self.fin = self.syn = 0
        self.wscale = None          # None until a SYN is seen
        self.mss = None
        self.sackok = False
        self.tstamp = False
        self.wins = []              # advertised windows, scaled, in bytes
        self.zerowin = []           # durations, seconds
        self.zerowin_open = 0
        self.sillywin = 0
        self.probes = 0             # segments sent into a window this side shut
        self.dupruns = collections.Counter()   # run length -> count
        self.rtt = []


def quant(v, f):
    if not v:
        return 0.0
    s = sorted(v)
    return s[min(len(s) - 1, int(len(s) * f))]


def analyse(segs, guest, peer, mss_default, quiet_gap):
    g, p = Side("guest"), Side("peer")
    for s in segs:
        side = g if s.src == guest else p
        side.segs += 1
        if s.flags & SYN:
            side.syn += 1
            # The options are only meaningful on the SYN; a wscale option on
            # anything else is not a renegotiation and must not overwrite it.
            if s.wscale is not None:
                side.wscale = s.wscale
            if s.mss is not None:
                side.mss = s.mss
            side.sackok = side.sackok or s.sackok
        if s.ts:
            side.tstamp = True
        if s.flags & RST:
            side.rst += 1
        if s.flags & FIN:
            side.fin += 1

    # SCALING IS SYMMETRIC OR IT IS OFF.  RFC 7323: the option is only in
    # effect if BOTH sides sent it.  A capture with one SYN in it therefore
    # still cannot scale, and saying so is the whole point of this block.
    scaled = g.wscale is not None and p.wscale is not None
    gsh = g.wscale if scaled else 0
    psh = p.wscale if scaled else 0
    # THE EFFECTIVE PATH MSS, WHICH IS THE SMALLER OF THE TWO.  Silly-window
    # is a receiver advertising less than the segment size its sender will
    # use, so the threshold is the size that actually crosses the wire.
    # Taking the OTHER side's advertisement instead reads a 9000-MTU Linux
    # peer's 8960 against an Amiga window and calls every advertisement under
    # 8960 silly -- which on the first real run inflated 318 events out of a
    # 1460-byte path.  min() of the two is what both ends are bound by.
    path_mss = min(g.mss or mss_default, p.mss or mss_default)
    g_recv_mss = p_recv_mss = path_mss

    # Per direction: highest sequence sent, originals for the round trip, and
    # the acknowledgement stream the other side produced.
    state = {
        guest: dict(side=g, highest=None, sent=[], retrans_ranges=[],
                    shift=gsh, recv_mss=g_recv_mss),
        peer:  dict(side=p, highest=None, sent=[], retrans_ranges=[],
                    shift=psh, recv_mss=p_recv_mss),
    }

    # Window history per side, as (t, window bytes, ack base).  Used for the
    # in-flight comparison and for the zero-window lifetimes.
    zero_at = {guest: None, peer: None}
    winhist = {guest: [], peer: []}

    # ACK bookkeeping for the delayed-ACK measurement: the acknowledgement
    # stream of one side read against the other side's originals.  Every
    # ACK-bearing segment is in it, piggybacked ones included -- an
    # acknowledgement riding on data acknowledges just as hard.
    ackstream = {guest: [], peer: []}      # (t, ack, win_bytes)
    # DUPLICATES ARE COUNTED FROM PURE ACKS ONLY, which is RFC 5681 2 (b) and
    # (c): no payload, no SYN and no FIN.  Reading the ack field of DATA
    # segments instead reports a bulk sender's own stream as a wall of
    # duplicates -- the ack field of a one-way transfer does not move, so a
    # 2438-segment upload came out as a run of 74 and a rate of 876 per
    # thousand segments on a link with nothing wrong with it.  Found by the
    # first capture this was ever pointed at.
    dupstream = {guest: [], peer: []}

    # In-flight samples, BOTH DIRECTIONS.  The peer sending into the guest's
    # window is the read direction and the one the ACK-clock questions are
    # about; the guest sending into the peer's window is the write direction,
    # and a stack that under-fills there is the other half of the same
    # defect.  Keyed by who was sending.
    inflight = {guest: [], peer: []}       # (bytes in flight, window bytes)

    last_ack_from = {guest: None, peer: None}

    for s in segs:
        src, dst = s.src, s.dst
        st = state[src]
        side = st["side"]
        shift = st["shift"]
        win = s.win << shift
        # A SYN carries an unscaled window whatever was negotiated: the scale
        # only applies once the handshake is complete.
        if s.flags & SYN:
            win = s.win

        is_retrans = False
        if s.plen:
            end = (s.seq + s.plen) & 0xFFFFFFFF
            if st["highest"] is None:
                st["highest"] = end
            else:
                if seq_lt(s.seq, st["highest"]):
                    is_retrans = True
                if seq_lt(st["highest"], end):
                    st["highest"] = end
            side.data_segs += 1
            side.data_bytes += s.plen
            if is_retrans:
                side.retrans += 1
                side.retrans_bytes += s.plen
                st["retrans_ranges"].append((s.seq, end))
            else:
                st["sent"].append((s.t, s.seq, end))
            # A segment sent into a window the other side shut is a probe,
            # not ordinary data.  Counted where the shut window is, so it
            # reads beside the zero-window event it belongs to.
            if zero_at[dst] is not None:
                state[dst]["side"].probes += 1
        elif s.flags & ACK and not (s.flags & (SYN | FIN | RST)):
            side.pure_acks += 1
            dupstream[src].append(s.ack)

        if s.flags & ACK:
            ackstream[src].append((s.t, s.ack, win))
            last_ack_from[src] = s.ack

        # WINDOW, and only on segments that carry one.  A RST has no window
        # worth the name and a SYN's is pre-scaling; both would drag the
        # minimum down and invent silly-window events.
        if not (s.flags & (RST | SYN)):
            side.wins.append(win)
            winhist[src].append((s.t, win, s.ack))
            if win == 0:
                if zero_at[src] is None:
                    zero_at[src] = s.t
            else:
                if zero_at[src] is not None:
                    side.zerowin.append(s.t - zero_at[src])
                    zero_at[src] = None
                if win < st["recv_mss"]:
                    side.sillywin += 1

        # BYTES IN FLIGHT AGAINST THE WINDOW THE GUEST ADVERTISED, sampled at
        # the peer's own data transmissions.  Both halves are as the SENDER
        # knew them at that instant: the highest sequence it had put on the
        # wire, less the highest acknowledgement it had received by then.
        if s.plen and last_ack_from[dst] is not None:
            flight = (st["highest"] - last_ack_from[dst]) & 0xFFFFFFFF
            if flight < (1 << 31):                    # not a wrap artefact
                w = state[dst]["side"].wins
                if w and w[-1]:
                    inflight[src].append((flight, w[-1]))

    for addr in (guest, peer):
        if zero_at[addr] is not None:
            # Still shut when the capture ended.  Counted with the time it was
            # known to have lasted rather than dropped: a window that never
            # reopened is the worst case of the thing being measured, and
            # dropping it would report the pathology as absent.
            state[addr]["side"].zerowin.append(segs[-1].t - zero_at[addr])
            state[addr]["side"].zerowin_open = 1

    # ----------------------------------------------------- duplicate ACKs --
    #
    # A run is consecutive acknowledgements repeating one number.  The window
    # is NOT part of the test: this receiver re-advertises a different window
    # on essentially every acknowledgement, and applying the RFC's
    # window-unchanged condition finds no duplicates at all in a capture that
    # demonstrably contains them (lossrate.py:74 found the same thing).
    for addr in (guest, peer):
        side = state[addr]["side"]
        prev = None
        run = 0
        for a in dupstream[addr]:
            if prev is not None and a == prev:
                run += 1
            else:
                if run:
                    side.dupruns[run] += 1
                run = 0
                prev = a
        if run:
            side.dupruns[run] += 1

    # ------------------------------------------------- round trip samples --
    #
    # Karn: an acknowledgement covering a range that was retransmitted cannot
    # be attributed to either copy, so it contributes no sample.
    def overlaps(sq, end, ranges):
        """[sq, end) meets any of [r0, r1), in sequence space."""
        return any(seq_lt(sq, r1) and seq_lt(r0, end) for r0, r1 in ranges)

    karn_dropped = 0
    for sender, acker in ((guest, peer), (peer, guest)):
        st = state[sender]
        side = st["side"]
        rr = st["retrans_ranges"]
        i = 0
        sent = st["sent"]
        for t, a, _w in ackstream[acker]:
            while i < len(sent) and not seq_lt(a, sent[i][2]):
                t0, sq, end = sent[i]
                i += 1
                if overlaps(sq, end, rr):
                    karn_dropped += 1
                    continue
                if t > t0:
                    side.rtt.append(t - t0)

    # ------------------------------------------------------- ACK delay --
    #
    # For every acknowledgement from the guest that advances, the time since
    # the FIRST segment beyond the previous acknowledgement point left the
    # peer.  That segment is the one that started the receiver's delayed-ACK
    # timer.  Ranges the peer retransmitted are skipped: the sample would be
    # measuring a recovery, not an ACK policy.
    #
    # This is measured at the peer, so every sample carries one full round
    # trip that is not the guest's doing.  Both halves are subtracted off in
    # ackdelay_excess_*, using the floor round trip rather than the minimum:
    # the minimum is one sample and moves like one.
    # Sorted by send time, which on a sender's own egress capture is also
    # sequence order for everything that was not retransmitted -- and
    # retransmissions are not in this list.  `prev` only ever advances, so one
    # index walks the whole capture instead of a scan per acknowledgement:
    # the nested form was O(acks x segments) and a four-megabyte arm has
    # thousands of each.
    psent = state[peer]["sent"]
    prr = state[peer]["retrans_ranges"]
    delays = []
    prev = None
    j = 0
    for t, a, _w in ackstream[guest]:
        if prev is None:
            prev = a
            continue
        if a == prev or seq_lt(a, prev):
            continue
        # The first segment whose end is past the previous acknowledgement
        # point: that is the one that started the receiver's timer.
        while j < len(psent) and not seq_lt(prev, psent[j][2]):
            j += 1
        trigger = None
        if j < len(psent):
            t0, sq, end = psent[j]
            if not seq_lt(a, end) and not overlaps(sq, end, prr):
                trigger = t0
        if trigger is not None and t > trigger:
            d = t - trigger
            # A gap longer than the quiet threshold is not an ACK delay: the
            # peer had stopped sending and the guest had nothing to
            # acknowledge until it started again.  Counting those turns every
            # idle period in the capture into a 200 ms ACK.
            if d < quiet_gap:
                delays.append(d)
        prev = a

    return g, p, scaled, delays, inflight, karn_dropped


# ------------------------------------------------------------------- output --


def kv(k, v):
    print("%s=%s" % (k, v))


def rate(b, secs):
    return (b / secs / 1024.0) if secs > 0 else 0.0


def pct(n, d):
    return (100.0 * n / d) if d else 0.0


def main():
    ap = argparse.ArgumentParser(
        description="acknowledgement and window behaviour of one endpoint, "
                    "from a capture taken at the other one",
        epilog="the endpoint under test is named by --guest, so the same "
               "command reads an emulated guest and a real machine on the LAN")
    ap.add_argument("pcap")
    ap.add_argument("--guest", required=True,
                    help="the address of the machine under test")
    # THE ADDRESS IS NOT THE MACHINE.  The real A1200 takes a DHCP lease and
    # its address changes every reboot, so a figure filed under an address
    # cannot be matched to the machine that produced it a week later -- and
    # two guests on this LAN, one emulated and one real, would file numbers
    # under addresses that had swapped.  The caller passes the name it
    # resolved and it is echoed into the output beside the address.
    ap.add_argument("--name", default="",
                    help="the name the target was asked for, echoed into the "
                         "output so a figure carries the machine and not just "
                         "an address that will be reissued")
    ap.add_argument("--port", type=int,
                    help="restrict to this port, when the capture holds more "
                         "than the transfer of interest")
    ap.add_argument("--eph", type=int, help="the guest's ephemeral port")
    ap.add_argument("--mss", type=int, default=MSS_DEFAULT,
                    help="MSS to assume when the handshake is not in the "
                         "capture (default %d)" % MSS_DEFAULT)
    ap.add_argument("--rtt-floor", type=float, default=0.05,
                    help="percentile of the measured round trips taken as the "
                         "fastest the path repeats, subtracted from the ACK "
                         "delays (default 0.05); the minimum is one sample "
                         "and moves like one")
    ap.add_argument("--quiet-gap", type=float, default=1.0,
                    help="seconds after which a wait for data is idleness "
                         "rather than a delayed acknowledgement (default 1.0)")

    ap.add_argument("--max-retrans-pct", type=float, default=1.0,
                    help="fail if either side retransmitted more than this "
                         "percentage of its data segments (default 1.0)")
    ap.add_argument("--max-dup3-per-kseg", type=float, default=5.0,
                    help="fail above this many runs of three or more "
                         "duplicate acknowledgements per thousand data "
                         "segments (default 5.0); three is the fast "
                         "retransmit trigger and two is not")
    ap.add_argument("--max-zerowin-ms", type=float, default=250.0,
                    help="fail if any single zero window lasted longer than "
                         "this (default 250); a zero window is normal, one "
                         "that stays shut means the update was lost")
    ap.add_argument("--max-sillywin", type=int, default=0,
                    help="fail above this many advertisements below one MSS "
                         "(default 0, which is what RFC 1122 4.2.3.3 asks)")
    ap.add_argument("--max-ackdelay-p90-ms", type=float, default=250.0,
                    help="fail if the 90th percentile ACK delay is above this "
                         "(default 250; RFC 1122 4.2.3.2 caps the delay at "
                         "500 ms and asks for 200)")
    ap.add_argument("--min-fill-pct", type=float,
                    help="fail if the median bytes in flight is below this "
                         "percentage of the advertised window; off by "
                         "default, because a request/response workload is "
                         "legitimately application limited")
    ap.add_argument("--min-acks-per-seg", type=float,
                    help="fail if the guest acknowledged fewer times than "
                         "this per peer data segment; off by default, 0.5 is "
                         "what RFC 1122 4.2.3.2 asks of a receiver")
    ap.add_argument("--no-gates", action="store_true",
                    help="report the counters and pass whatever they say")
    args = ap.parse_args()

    if not os.path.exists(args.pcap):
        print("ackscope: no such capture: %s" % args.pcap, file=sys.stderr)
        return 2
    try:
        key, segs, nconn = read_capture(args.pcap, args.guest, args.port,
                                        args.eph)
    except SystemExit:
        print("ackscope: no TCP involving %s in %s"
              % (args.guest, args.pcap), file=sys.stderr)
        return 2
    peer, gport, pport = key
    if len(segs) < 4:
        print("ackscope: only %d segments for %s; nothing to read"
              % (len(segs), args.guest), file=sys.stderr)
        return 2

    g, p, scaled, delays, inflight, karn = analyse(
        segs, args.guest, peer, args.mss, args.quiet_gap)
    wall = segs[-1].t - segs[0].t

    kv("capture", args.pcap)
    kv("target_name", args.name or "-")
    kv("guest", args.guest)
    kv("peer", peer)
    kv("conn", "%s:%d-%s:%d" % (args.guest, gport, peer, pport))
    kv("conns_seen", nconn)
    kv("segments", len(segs))
    kv("wall_s", "%.3f" % wall)
    kv("handshake_seen", 1 if (g.syn and p.syn) else 0)
    kv("wscale_known", 1 if scaled else 0)
    kv("guest_wscale", g.wscale if g.wscale is not None else -1)
    kv("peer_wscale", p.wscale if p.wscale is not None else -1)
    kv("guest_mss", g.mss if g.mss else 0)
    kv("peer_mss", p.mss if p.mss else 0)
    kv("path_mss", min(g.mss or args.mss, p.mss or args.mss))
    kv("mss_source", "syn" if (g.mss and p.mss) else "default")
    kv("guest_sack_ok", 1 if g.sackok else 0)
    kv("guest_timestamps", 1 if g.tstamp else 0)

    for side, tag in ((g, "guest"), (p, "peer")):
        kv("%s_data_segs" % tag, side.data_segs)
        kv("%s_data_bytes" % tag, side.data_bytes)
        kv("%s_kbs" % tag, "%.1f" % rate(side.data_bytes, wall))
        kv("%s_retrans" % tag, side.retrans)
        kv("%s_retrans_pct" % tag, "%.3f" % pct(side.retrans, side.data_segs))
        kv("%s_pure_acks" % tag, side.pure_acks)
        kv("%s_rst" % tag, side.rst)

    # ------------------------------------------------------ duplicate ACKs --
    for side, tag in ((g, "guest"), (p, "peer")):
        d = side.dupruns
        three = sum(n for r, n in d.items() if r >= 3)
        kv("%s_dupack_runs" % tag, sum(d.values()))
        kv("%s_dupack_run1" % tag, d.get(1, 0))
        kv("%s_dupack_run2" % tag, d.get(2, 0))
        kv("%s_dupack_run3plus" % tag, three)
        kv("%s_dupack_max_run" % tag, max(d) if d else 0)
    # The denominator is the OTHER side's data segments: a duplicate
    # acknowledgement from the guest is a comment on the peer's stream.
    dup3_g = sum(n for r, n in g.dupruns.items() if r >= 3)
    dup3_p = sum(n for r, n in p.dupruns.items() if r >= 3)
    rate_g = 1000.0 * dup3_g / p.data_segs if p.data_segs else 0.0
    rate_p = 1000.0 * dup3_p / g.data_segs if g.data_segs else 0.0
    kv("guest_dup3_per_kseg", "%.2f" % rate_g)
    kv("peer_dup3_per_kseg", "%.2f" % rate_p)

    # -------------------------------------------------- windows advertised --
    for side, tag in ((g, "guest"), (p, "peer")):
        w = side.wins
        kv("%s_win_n" % tag, len(w))
        kv("%s_win_min" % tag, min(w) if w else 0)
        kv("%s_win_p50" % tag, int(quant(w, 0.5)) if w else 0)
        kv("%s_win_max" % tag, max(w) if w else 0)
        kv("%s_zerowin_events" % tag, len(side.zerowin))
        kv("%s_zerowin_ms_total" % tag, "%.1f" % (sum(side.zerowin) * 1e3))
        kv("%s_zerowin_ms_max" % tag,
           "%.1f" % (max(side.zerowin) * 1e3 if side.zerowin else 0.0))
        kv("%s_zerowin_at_end" % tag, side.zerowin_open)
        kv("%s_zerowin_probes" % tag, side.probes)
        kv("%s_sillywin_events" % tag, side.sillywin)

    # -------------------------------------------------------- round trips --
    for side, tag in ((g, "guest"), (p, "peer")):
        r = side.rtt
        kv("%s_rtt_n" % tag, len(r))
        kv("%s_rtt_min_ms" % tag, "%.2f" % (min(r) * 1e3 if r else 0.0))
        for f in (0.5, 0.9):
            kv("%s_rtt_p%d_ms" % (tag, int(f * 100)),
               "%.2f" % (quant(r, f) * 1e3))
        kv("%s_rtt_max_ms" % tag, "%.2f" % (max(r) * 1e3 if r else 0.0))
    kv("rtt_karn_dropped", karn)

    # ---------------------------------------------------------- ACK delay --
    floor = quant(p.rtt, args.rtt_floor) if p.rtt else 0.0
    kv("rtt_floor_ms", "%.2f" % (floor * 1e3))
    kv("ackdelay_n", len(delays))
    for f in (0.1, 0.5, 0.9, 0.99):
        kv("ackdelay_p%g_ms" % (f * 100), "%.2f" % (quant(delays, f) * 1e3))
    kv("ackdelay_max_ms", "%.2f" % (max(delays) * 1e3 if delays else 0.0))
    kv("ackdelay_excess_p50_ms",
       "%.2f" % (max(0.0, quant(delays, 0.5) - floor) * 1e3))
    kv("ackdelay_excess_p90_ms",
       "%.2f" % (max(0.0, quant(delays, 0.9) - floor) * 1e3))
    # THE HISTOGRAM, because the pathology is bimodal and percentiles of a
    # bimodal distribution describe neither mode.  A stack that acknowledges
    # half its segments in 1 ms and holds the other half for 200 has a p50 in
    # a bucket that contains nothing.
    buckets = ((1e-3, "u1ms"), (5e-3, "1_5ms"), (2e-2, "5_20ms"),
               (5e-2, "20_50ms"), (2e-1, "50_200ms"), (float("inf"), "o200ms"))
    counts = collections.Counter()
    for d in delays:
        for lim, name in buckets:
            if d < lim:
                counts[name] += 1
                break
    for _lim, name in buckets:
        kv("ackdelay_%s" % name, counts[name])

    # ------------------------------------------------------- ACK density --
    #
    # The ACK clock as one number.  RFC 1122 4.2.3.2 asks a receiver to
    # acknowledge at least every second full-sized segment, which is 0.5.
    apd = (float(g.pure_acks) / p.data_segs) if p.data_segs else 0.0
    kv("guest_acks_per_data_seg", "%.3f" % apd)
    kv("guest_bytes_per_ack",
       "%.0f" % (float(p.data_bytes) / g.pure_acks if g.pure_acks else 0.0))

    # ------------------------------------- bytes in flight vs the window --
    #
    # Named by WHOSE WINDOW is being filled, not by who was sending, because
    # the window is the thing under test: read_fill is the peer filling the
    # guest's advertised window, write_fill is the guest filling the peer's.
    fills = {}
    for sender, tag in ((peer, "read"), (args.guest, "write")):
        samples = inflight[sender]
        fills[tag] = [100.0 * a / b for a, b in samples if b]
        kv("%s_inflight_n" % tag, len(samples))
        f = [x[0] for x in samples]
        w = [x[1] for x in samples]
        kv("%s_inflight_p50" % tag, int(quant(f, 0.5)))
        kv("%s_inflight_max" % tag, max(f) if f else 0)
        kv("%s_advwin_p50" % tag, int(quant(w, 0.5)))
        kv("%s_fill_p50_pct" % tag, "%.1f" % quant(fills[tag], 0.5))
        kv("%s_fill_p90_pct" % tag, "%.1f" % quant(fills[tag], 0.9))
        # How often the sender was actually up against the window.  This is
        # the difference between "advertises 32 KB and fills it" and
        # "advertises 32 KB and never uses it".
        kv("%s_win_limited_pct" % tag,
           "%.1f" % pct(sum(1 for x in fills[tag] if x >= 90.0),
                        len(fills[tag])))

    # ------------------------------------------------------------ verdict --
    #
    # A gate whose input is missing comes back skip, never pass.  A capture
    # with no handshake cannot scale a window, and calling that a pass is how
    # a green run over an empty measurement happens.
    gates = []

    def gate(name, ok, detail):
        gates.append((name, "pass" if ok else "fail", detail))

    def skip(name, why):
        gates.append((name, "skip", why))

    if args.no_gates:
        pass
    else:
        for side, tag in ((g, "guest"), (p, "peer")):
            if side.data_segs:
                gate("%s_retrans" % tag,
                     pct(side.retrans, side.data_segs) <= args.max_retrans_pct,
                     "%.3f%%" % pct(side.retrans, side.data_segs))
            else:
                skip("%s_retrans" % tag, "sent no data")
        gate("dupack3", max(rate_g, rate_p) <= args.max_dup3_per_kseg,
             "%.2f/kseg" % max(rate_g, rate_p))
        worst_zero = max(
            [max(s.zerowin) * 1e3 if s.zerowin else 0.0 for s in (g, p)])
        gate("zerowin", worst_zero <= args.max_zerowin_ms,
             "%.1fms" % worst_zero)
        if scaled or (not g.wscale and not p.wscale and g.syn and p.syn):
            gate("sillywin", (g.sillywin + p.sillywin) <= args.max_sillywin,
                 "%d" % (g.sillywin + p.sillywin))
        else:
            skip("sillywin", "no handshake, window scale unknown")
        if delays:
            gate("ackdelay", quant(delays, 0.9) * 1e3 <= args.max_ackdelay_p90_ms,
                 "p90 %.1fms" % (quant(delays, 0.9) * 1e3))
        else:
            skip("ackdelay", "no acknowledgement advanced over new data")
        if args.min_fill_pct is not None:
            if fills["read"]:
                gate("read_fill", quant(fills["read"], 0.5) >= args.min_fill_pct,
                     "%.1f%%" % quant(fills["read"], 0.5))
            else:
                skip("read_fill", "the peer sent no data")
        if args.min_acks_per_seg is not None:
            if p.data_segs:
                gate("ackdensity", apd >= args.min_acks_per_seg, "%.3f" % apd)
            else:
                skip("ackdensity", "the peer sent no data")

    failed = [n for n, v, _d in gates if v == "fail"]
    for n, v, d in gates:
        kv("gate_%s" % n, "%s(%s)" % (v, d))
    kv("gates_run", sum(1 for _n, v, _d in gates if v != "skip"))
    kv("gates_failed", len(failed))
    kv("failed", ",".join(failed) if failed else "none")
    kv("RESULT", "fail" if failed else "pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
