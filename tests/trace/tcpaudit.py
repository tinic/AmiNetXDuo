#!/usr/bin/env python3
"""Audit the TCP in a pcap file: loss, windows, ACK delay, segment sizes.

Reads a classic pcap (either endianness, DLT_EN10MB) and answers the questions
docs/RESEARCH.md keeps having to guess at:

  retransmissions   a segment carrying a sequence range already seen from the
                    same side.  This is the direct successor to the
                    AMI_SANA2_RX_DEPTH_IPV4 defect, where TCP hid the loss and
                    only the throughput showed it.
  duplicate ACKs    three or more in a row is fast retransmit; even one says
                    something arrived out of order.
  window            what each side ADVERTISED, minimum and maximum, and
                    whether it ever reached zero.  A window that is small
                    relative to bandwidth x delay is the ceiling, and no
                    amount of assembly moves it.
  ACK delay         for each pure ACK, the time since the newest data it
                    covers arrived.  A cluster at 200 ms is the delayed-ACK
                    timer; a cluster at 20 ms is the periodic tick.
  segments          the size histogram, and the MSS each side offered in its
                    SYN.  Undersized segments mean Nagle is off and the
                    application is writing small, or the MSS is wrong.

Nothing here needs scapy or libpcap: the format is eight fields and a header.

SPDX-License-Identifier: MIT
"""

import argparse
import struct
import sys
from collections import defaultdict

ETH_HDR = 14
ETHERTYPE_IPV4 = 0x0800


def read_pcap(path):
    with open(path, "rb") as fh:
        blob = fh.read()

    if len(blob) < 24:
        raise SystemExit("%s: too short to be a pcap" % path)

    magic = struct.unpack(">I", blob[0:4])[0]
    if magic == 0xA1B2C3D4:
        end, usec_scale = ">", 1
    elif magic == 0xD4C3B2A1:
        end, usec_scale = "<", 1
    elif magic == 0xA1B23C4D:
        end, usec_scale = ">", 1000        # nanosecond pcap
    elif magic == 0x4D3CB2A1:
        end, usec_scale = "<", 1000
    else:
        raise SystemExit("%s: not a pcap (magic %08x)" % (path, magic))

    _, _, _, _, snap, dlt = struct.unpack(end + "HHiIII", blob[4:24])
    pos = 24
    out = []
    while pos + 16 <= len(blob):
        sec, usec, incl, orig = struct.unpack(end + "IIII", blob[pos:pos + 16])
        pos += 16
        if pos + incl > len(blob):
            break
        out.append((sec + (usec / usec_scale) / 1e6, blob[pos:pos + incl], orig))
        pos += incl
    return out, snap, dlt


def parse(frame):
    """(src, dst, sport, dport, seq, ack, flags, win, paylen, opts) or None."""
    if len(frame) < ETH_HDR + 20:
        return None
    if struct.unpack(">H", frame[12:14])[0] != ETHERTYPE_IPV4:
        return None

    ip = frame[ETH_HDR:]
    ihl = (ip[0] & 0x0F) * 4
    if ihl < 20 or len(ip) < ihl:
        return None
    if ip[9] != 6:                      # not TCP
        return None

    total = struct.unpack(">H", ip[2:4])[0]
    src = ".".join(str(b) for b in ip[12:16])
    dst = ".".join(str(b) for b in ip[16:20])

    tcp = ip[ihl:]
    if len(tcp) < 20:
        return None
    sport, dport = struct.unpack(">HH", tcp[0:4])
    seq, ack = struct.unpack(">II", tcp[4:12])
    off = (tcp[12] >> 4) * 4
    flags = tcp[13]
    win = struct.unpack(">H", tcp[14:16])[0]

    # From the IP total length, not from what was captured: a snaplen-96
    # capture holds no payload at all and the length is the whole point.
    paylen = total - ihl - off
    if paylen < 0:
        paylen = 0

    opts = tcp[20:off] if len(tcp) >= off else b""
    return src, dst, sport, dport, seq, ack, flags, win, paylen, opts


def mss_of(opts):
    i = 0
    while i < len(opts):
        kind = opts[i]
        if kind == 0:
            break
        if kind == 1:
            i += 1
            continue
        if i + 1 >= len(opts):
            break
        length = opts[i + 1]
        if length < 2:
            break
        if kind == 2 and length == 4 and i + 4 <= len(opts):
            return struct.unpack(">H", opts[i + 2:i + 4])[0]
        i += length
    return None


FLAG_FIN, FLAG_SYN, FLAG_RST, FLAG_PSH, FLAG_ACK = 1, 2, 4, 8, 16


class Side:
    def __init__(self):
        self.segments = 0
        self.bytes = 0
        self.retrans = 0
        self.retrans_bytes = 0
        self.dupacks = 0
        self.max_dupack_run = 0
        self.win_min = None
        self.win_max = 0
        self.win_last = 0
        self.zero_windows = 0
        self.mss = None
        self.sizes = defaultdict(int)
        self.seen = set()               # (seq, len) already sent
        self.highest = None
        self.last_ack = None
        self.ack_run = 0
        self.pure_acks = 0
        self.ack_delays = []
        self.max_inflight = 0
        self.inflight_at_win = 0        # peer's advertised window at that moment
        self.gaps = []                  # idle time before each data segment
        self.last_data_ts = None
        self.syn = 0
        self.fin = 0
        self.rst = 0


def audit(path, top=6):
    packets, snap, dlt = read_pcap(path)
    if dlt != 1:
        raise SystemExit("%s: link type %d, expected 1 (DLT_EN10MB)" % (path, dlt))

    # tests/trace/a2065pcap.py stamps its records with a counter because the
    # emulator log has no clock in it at all.  Detect that rather than print
    # microsecond percentiles computed from a fiction.
    synthetic = bool(packets) and all(int(ts) == 0 for ts, _, _ in packets)

    flows = defaultdict(lambda: {"sides": defaultdict(Side),
                                 "first": None, "last": None,
                                 "data_at": defaultdict(list)})
    nontcp = 0

    for ts, frame, orig in packets:
        p = parse(frame)
        if p is None:
            nontcp += 1
            continue
        src, dst, sport, dport, seq, ack, flags, win, paylen, opts = p

        a, b = (src, sport), (dst, dport)
        key = tuple(sorted([a, b]))
        flow = flows[key]
        if flow["first"] is None:
            flow["first"] = ts
        flow["last"] = ts

        side = flow["sides"][a]
        other = flow["sides"][b]

        if flags & FLAG_SYN:
            side.syn += 1
            m = mss_of(opts)
            if m:
                side.mss = m
        if flags & FLAG_FIN:
            side.fin += 1
        if flags & FLAG_RST:
            side.rst += 1

        side.win_last = win
        side.win_max = max(side.win_max, win)
        side.win_min = win if side.win_min is None else min(side.win_min, win)
        if win == 0 and not (flags & FLAG_RST):
            side.zero_windows += 1

        if paylen > 0:
            # Unacknowledged bytes at the moment this segment left, against
            # the window the OTHER side had advertised. When these two are
            # equal the sender is stalled ON THE WINDOW and nothing else --
            # not the CPU, not the link, not the tick.
            if other.last_ack is not None:
                flight = (seq + paylen - other.last_ack) & 0xFFFFFFFF
                if flight < (1 << 31) and flight > side.max_inflight:
                    side.max_inflight = flight
                    side.inflight_at_win = other.win_last
            if side.last_data_ts is not None:
                side.gaps.append(ts - side.last_data_ts)
            side.last_data_ts = ts

            side.segments += 1
            side.bytes += paylen
            side.sizes[paylen] += 1
            if (seq, paylen) in side.seen:
                side.retrans += 1
                side.retrans_bytes += paylen
            else:
                side.seen.add((seq, paylen))
            # Remember when this range became ACK-able, for the delay below.
            flow["data_at"][a].append((seq + paylen, ts))
            if side.highest is None or seq + paylen > side.highest:
                side.highest = seq + paylen
        if (flags & FLAG_ACK) and paylen == 0 and \
                not (flags & (FLAG_SYN | FLAG_FIN | FLAG_RST)):
            side.pure_acks += 1
            if side.last_ack is not None and ack == side.last_ack:
                side.dupacks += 1
                side.ack_run += 1
                side.max_dupack_run = max(side.max_dupack_run, side.ack_run)
            else:
                side.ack_run = 0
            side.last_ack = ack

            # How long the peer's newest covered byte had been waiting.
            arrivals = flow["data_at"][b]
            newest = None
            for end, at in arrivals:
                if end <= ack:
                    newest = at if newest is None else max(newest, at)
            if newest is not None:
                side.ack_delays.append(ts - newest)

    print("=" * 70)
    print("%s -- %d packets (%d not IPv4/TCP), snaplen %d"
          % (path, len(packets), nontcp, snap))
    if synthetic:
        print("NO CLOCK IN THIS FILE: order is real, every interval is not.")
        print("Loss, windows, MSS and sizes stand; timings are suppressed.")

    if not flows:
        print("no TCP flows")
        return

    for key, flow in sorted(flows.items(),
                            key=lambda kv: -sum(s.bytes for s in kv[1]["sides"].values())):
        span = (flow["last"] or 0) - (flow["first"] or 0)
        print("-" * 70)
        if synthetic:
            print("flow %s:%d <-> %s:%d"
                  % (key[0][0], key[0][1], key[1][0], key[1][1]))
        else:
            print("flow %s:%d <-> %s:%d   %.3f s"
                  % (key[0][0], key[0][1], key[1][0], key[1][1], span))
        for who, side in flow["sides"].items():
            if side.segments == 0 and side.pure_acks == 0 and side.syn == 0:
                continue
            rate = (side.bytes / span) if span > 0 and not synthetic else 0
            if synthetic:
                print("  %-21s %6d seg  %9d B"
                      % ("%s:%d" % who, side.segments, side.bytes))
            else:
                print("  %-21s %6d seg  %9d B  %8.0f B/s"
                      % ("%s:%d" % who, side.segments, side.bytes, rate))
            print("      window   min %-6d max %-6d  zero-window %d"
                  % (side.win_min if side.win_min is not None else -1,
                     side.win_max, side.zero_windows))
            print("      mss offered %s   SYN %d  FIN %d  RST %d"
                  % (side.mss, side.syn, side.fin, side.rst))
            flag = "  <-- LOSS" if side.retrans else ""
            print("      retransmitted %d segments, %d bytes%s"
                  % (side.retrans, side.retrans_bytes, flag))
            if side.max_inflight:
                pct = (100.0 * side.max_inflight / side.inflight_at_win
                       if side.inflight_at_win else 0.0)
                note = "  <-- WINDOW-LIMITED" if pct >= 95.0 else ""
                print("      max bytes in flight %d against a %d window (%.0f%%)%s"
                      % (side.max_inflight, side.inflight_at_win, pct, note))
            if side.gaps and not synthetic:
                g = sorted(side.gaps)
                n = len(g)
                print("      gap before a data segment ms: p50 %.1f  p90 %.1f  max %.1f"
                      % (g[n // 2] * 1000, g[int(n * 0.9)] * 1000, g[-1] * 1000))
            print("      pure ACKs %d, duplicate %d, longest run %d"
                  % (side.pure_acks, side.dupacks, side.max_dupack_run))
            if side.ack_delays and not synthetic:
                d = sorted(side.ack_delays)
                n = len(d)
                print("      ACK delay ms: min %.1f  p50 %.1f  p90 %.1f  max %.1f"
                      % (d[0] * 1000, d[n // 2] * 1000,
                         d[int(n * 0.9)] * 1000, d[-1] * 1000))
                # A histogram in the buckets that name a mechanism.
                buckets = [(0.002, "<2ms  immediate"),
                           (0.025, "<25ms one tick"),
                           (0.12, "<120ms fast timer"),
                           (0.25, "<250ms DELAYED ACK"),
                           (1e9, ">250ms")]
                counts = [0] * len(buckets)
                for v in d:
                    for i, (edge, _) in enumerate(buckets):
                        if v < edge:
                            counts[i] += 1
                            break
                parts = ["%s %d" % (name, c)
                         for (_, name), c in zip(buckets, counts) if c]
                print("        " + " | ".join(parts))
            if side.sizes:
                common = sorted(side.sizes.items(), key=lambda kv: -kv[1])[:top]
                print("      sizes: " +
                      "  ".join("%d x%d" % (sz, n) for sz, n in common))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap", nargs="+")
    args = ap.parse_args()
    for path in args.pcap:
        audit(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
