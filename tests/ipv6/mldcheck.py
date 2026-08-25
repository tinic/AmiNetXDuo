#!/usr/bin/env python3
# WHAT THE GUEST PUT ON THE WIRE, read out of tests/ipv6/run-mld.sh's capture.
# SPDX-License-Identifier: MIT

import argparse
import ipaddress
import struct
import sys

ETH_P_IPV6 = 0x86DD
NH_HOP_BY_HOP = 0
NH_ICMPV6 = 58

MLD_QUERY = 130
MLD_V1_REPORT = 131
MLD_DONE = 132
MLD_V2_REPORT = 143

MODE_IS_EXCLUDE = 2
CHANGE_TO_INCLUDE_MODE = 3
CHANGE_TO_EXCLUDE_MODE = 4

ROUTER_ALERT_OPTION = 5


class Message(object):
    def __init__(self, t, src_mac, src_ip, dst_ip, mtype, hop_limit,
                 router_alert, groups):
        self.t = t
        self.src_mac = src_mac
        self.src_ip = src_ip
        self.dst_ip = dst_ip
        self.type = mtype
        self.hop_limit = hop_limit
        self.router_alert = router_alert
        self.groups = groups

    def has(self, group, record_type=None):
        for rt, g in self.groups:
            if g != group:
                continue
            if record_type is None or rt == record_type:
                return True
        return False


def read_pcap(path):
    with open(path, "rb") as fh:
        data = fh.read()

    if len(data) < 24:
        raise SystemExit("pcap too short: %s" % path)

    magic = data[:4]
    if magic == b"\xd4\xc3\xb2\xa1":
        endian, nano = "<", False
    elif magic == b"\xa1\xb2\xc3\xd4":
        endian, nano = ">", False
    elif magic == b"\x4d\x3c\xb2\xa1":
        endian, nano = "<", True
    elif magic == b"\xa1\xb2\x3c\x4d":
        endian, nano = ">", True
    else:
        raise SystemExit("not a classic pcap: %s" % path)

    link = struct.unpack(endian + "I", data[20:24])[0]
    if link != 1:
        raise SystemExit("expected Ethernet (1), got link type %d" % link)

    off = 24
    while off + 16 <= len(data):
        sec, frac, caplen, _ = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        frame = data[off:off + caplen]
        off += caplen
        yield sec + (frac / 1e9 if nano else frac / 1e6), frame


def parse_mld(frame):
    """None unless the frame is an MLD message."""
    if len(frame) < 54:
        return None
    if struct.unpack("!H", frame[12:14])[0] != ETH_P_IPV6:
        return None

    src_mac = ":".join("%02x" % b for b in frame[6:12])
    ip = frame[14:]
    next_header = ip[6]
    hop_limit = ip[7]
    src_ip = ipaddress.IPv6Address(ip[8:24])
    dst_ip = ipaddress.IPv6Address(ip[24:40])

    payload = ip[40:]
    router_alert = False

    if next_header == NH_HOP_BY_HOP:
        if len(payload) < 8:
            return None
        ext_len = (payload[1] + 1) * 8
        i = 2
        while i < ext_len and i < len(payload):
            if payload[i] == 0:
                i += 1
                continue
            if i + 1 >= len(payload):
                break
            if payload[i] == ROUTER_ALERT_OPTION:
                router_alert = True
            i += 2 + payload[i + 1]
        next_header = payload[0]
        payload = payload[ext_len:]

    if next_header != NH_ICMPV6 or len(payload) < 4:
        return None

    mtype = payload[0]
    if mtype not in (MLD_QUERY, MLD_V1_REPORT, MLD_DONE, MLD_V2_REPORT):
        return None

    groups = []
    if mtype == MLD_V2_REPORT:
        if len(payload) < 8:
            return None
        count = struct.unpack("!H", payload[6:8])[0]
        off = 8
        for _ in range(count):
            if off + 20 > len(payload):
                break
            record_type = payload[off]
            aux = payload[off + 1]
            nsrc = struct.unpack("!H", payload[off + 2:off + 4])[0]
            group = ipaddress.IPv6Address(payload[off + 4:off + 20])
            groups.append((record_type, group))
            off += 20 + (nsrc * 16) + (aux * 4)
    else:
        if len(payload) < 24:
            return None
        groups.append((None, ipaddress.IPv6Address(payload[8:24])))

    return Message(0, src_mac, src_ip, dst_ip, mtype, hop_limit,
                   router_alert, groups)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pcap", required=True)
    ap.add_argument("--guest-mac", required=True)
    ap.add_argument("--solicited", required=True,
                    help="the guest's solicited-node group")
    ap.add_argument("--group", default="ff02::c",
                    help="the group the guest joins and leaves by hand")
    args = ap.parse_args()

    guest_mac = args.guest_mac.lower()
    solicited = ipaddress.IPv6Address(args.solicited)
    probe = ipaddress.IPv6Address(args.group)

    messages = []
    for t, frame in read_pcap(args.pcap):
        m = parse_mld(frame)
        if m is None:
            continue
        m.t = t
        messages.append(m)

    ours = [m for m in messages if m.src_mac == guest_mac]
    theirs = [m for m in messages if m.src_mac != guest_mac]

    print("frames_mld=%d guest_mld=%d peer_mld=%d" % (len(messages), len(ours), len(theirs)))

    if not ours:
        print("RESULT=no_guest_mld")
        return 2

    failures = []

    bad_alert = [m for m in ours if not m.router_alert]
    bad_hop = [m for m in ours if m.hop_limit != 1]
    bad_src = [m for m in ours
               if not (m.src_ip.is_link_local or m.src_ip == ipaddress.IPv6Address("::"))]

    print("router_alert_missing=%d hop_limit_wrong=%d source_wrong=%d"
          % (len(bad_alert), len(bad_hop), len(bad_src)))
    for name, bad in (("router_alert", bad_alert), ("hop_limit", bad_hop),
                      ("source", bad_src)):
        if bad:
            failures.append(name)

    first_query = min([m.t for m in theirs if m.type == MLD_QUERY], default=None)
    unsolicited = [m for m in ours if first_query is None or m.t < first_query]

    join_reports = [m for m in unsolicited
                    if (m.type == MLD_V2_REPORT and m.has(solicited, CHANGE_TO_EXCLUDE_MODE))
                    or (m.type == MLD_V1_REPORT and m.has(solicited))]
    print("join_report_solicited=%d" % len(join_reports))
    if not join_reports:
        failures.append("join_report_solicited")

    all_nodes = ipaddress.IPv6Address("ff02::1")
    leaked = [m for m in ours if m.has(all_nodes)]
    print("all_nodes_reported=%d" % len(leaked))
    if leaked:
        failures.append("all_nodes_exemption")

    probe_join = [m for m in ours
                  if (m.type == MLD_V2_REPORT and m.has(probe, CHANGE_TO_EXCLUDE_MODE))
                  or (m.type == MLD_V1_REPORT and m.has(probe))]
    probe_done = [m for m in ours if m.type == MLD_DONE and m.has(probe)]
    probe_leave = probe_done + [m for m in ours
                                if m.type == MLD_V2_REPORT
                                and m.has(probe, CHANGE_TO_INCLUDE_MODE)]
    print("probe_join_report=%d probe_leave=%d probe_done_v1=%d"
          % (len(probe_join), len(probe_leave), len(probe_done)))
    if not probe_join:
        failures.append("probe_join_report")
    if not probe_leave:
        failures.append("probe_leave")

    v2_answered = 0
    control_windows = control_answered = 0
    suppressed_windows = suppressed = 0

    for q in theirs:
        if q.type != MLD_QUERY:
            continue

        window = [m for m in ours if q.t < m.t <= q.t + 12.0]
        peer_first = [m for m in theirs
                      if q.t < m.t <= q.t + 3.0
                      and m.type == MLD_V1_REPORT and m.has(solicited)]

        v2_here = [m for m in window
                   if m.type == MLD_V2_REPORT and m.has(solicited, MODE_IS_EXCLUDE)]
        v1_here = [m for m in window if m.type == MLD_V1_REPORT and m.has(solicited)]

        v2_answered += len(v2_here)

        if peer_first:
            suppressed_windows += 1
            if not v1_here:
                suppressed += 1
        elif v1_here:
            control_windows += 1
            control_answered += 1
        elif v2_here:
            pass
        else:
            control_windows += 1

    print("query_answers_v2=%d control_windows=%d control_answered=%d"
          % (v2_answered, control_windows, control_answered))
    print("suppression_windows=%d suppressed=%d"
          % (suppressed_windows, suppressed))

    if v2_answered == 0:
        failures.append("query_answer_v2")
    if control_answered == 0:
        failures.append("query_answer_v1")

    if suppressed_windows == 0:
        print("suppression=unreachable_on_this_segment")
    elif suppressed != suppressed_windows:
        failures.append("suppression")
    else:
        print("suppression=held")

    if failures:
        print("failed=%s" % ",".join(failures))
        print("RESULT=fail")
        return 1

    print("RESULT=pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
