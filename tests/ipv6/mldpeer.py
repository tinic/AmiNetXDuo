#!/usr/bin/env python3
# THE OTHER HOST ON THE LINK, FOR tests/ipv6/run-mld.sh.
# SPDX-License-Identifier: MIT

import argparse
import ipaddress
import socket
import struct
import sys

ETH_P_IPV6 = 0x86DD
NEXT_HEADER_HOP_BY_HOP = 0
NEXT_HEADER_ICMPV6 = 58

MLD_QUERY = 130
MLD_V1_REPORT = 131
MLD_DONE = 132
MLD_V2_REPORT = 143

ALL_NODES = "ff02::1"


def iface_mac(iface):
    with open("/sys/class/net/%s/address" % iface) as fh:
        return bytes(int(b, 16) for b in fh.read().strip().split(":"))


def iface_linklocal(iface):
    """The interface's fe80:: address, which is the only legal source for an
    MLD message from a host that has one (RFC 2710 section 3)."""
    with open("/proc/net/if_inet6") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) < 6 or parts[5] != iface:
                continue
            addr = ipaddress.IPv6Address(bytes.fromhex(parts[0]))
            if addr.is_link_local:
                return addr.packed
    raise SystemExit("no link-local address on %s" % iface)


def multicast_mac(dest):
    """RFC 2464 section 7: 33:33 then the low 32 bits of the address."""
    return b"\x33\x33" + dest[12:16]


def checksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = 0
    for i in range(0, len(data), 2):
        total += (data[i] << 8) | data[i + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def icmpv6_checksum(src, dst, payload):
    pseudo = src + dst + struct.pack("!IHBB", len(payload), 0, 0, NEXT_HEADER_ICMPV6)
    return checksum(pseudo + payload)


def hop_by_hop_router_alert():
    """Next Header 58, length 0 meaning eight octets, Router Alert carrying
    the MLD value 0 (RFC 2711 section 2.1), then a two-octet PadN."""
    return struct.pack("!BBBBHBB", NEXT_HEADER_ICMPV6, 0, 5, 2, 0, 1, 0)


def mld_v1_message(msg_type, group, mrd_ms):
    body = struct.pack("!BBHHH", msg_type, 0, 0, mrd_ms, 0) + group
    return body


def mld_v2_query(group, max_response_code, qrv=2, qqic=125):
    body = struct.pack("!BBHHH", MLD_QUERY, 0, 0, max_response_code, 0)
    body += group
    body += struct.pack("!BBH", qrv & 0x7, qqic, 0)
    return body


def with_checksum(message, src, dst):
    body = bytearray(message)
    body[2:4] = b"\x00\x00"
    body[2:4] = struct.pack("!H", icmpv6_checksum(src, dst, bytes(body)))
    return bytes(body)


def build_frame(src_mac, src_ip, dst_ip, message):
    message = with_checksum(message, src_ip, dst_ip)
    hbh = hop_by_hop_router_alert()
    payload = hbh + message

    ipv6 = struct.pack("!IHBB", 0x60000000, len(payload), NEXT_HEADER_HOP_BY_HOP, 1)
    ipv6 += src_ip + dst_ip

    eth = multicast_mac(dst_ip) + src_mac + struct.pack("!H", ETH_P_IPV6)
    return eth + ipv6 + payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iface", required=True)
    ap.add_argument("kind", choices=["query-v1", "query-v2", "report-v1", "done-v1"])
    ap.add_argument("--group", default="::",
                    help=":: on a query means General Query")
    ap.add_argument("--mrd", type=int, default=2000,
                    help="Maximum Response Delay, milliseconds")
    ap.add_argument("--count", type=int, default=1)
    args = ap.parse_args()

    group = ipaddress.IPv6Address(args.group).packed
    src_mac = iface_mac(args.iface)
    src_ip = iface_linklocal(args.iface)

    if args.kind == "query-v1":
        dst_ip = group if args.group != "::" else ipaddress.IPv6Address(ALL_NODES).packed
        message = mld_v1_message(MLD_QUERY, group, args.mrd)
    elif args.kind == "query-v2":
        dst_ip = group if args.group != "::" else ipaddress.IPv6Address(ALL_NODES).packed
        message = mld_v2_query(group, args.mrd)
    elif args.kind == "report-v1":
        if args.group == "::":
            raise SystemExit("report-v1 needs --group")
        dst_ip = group
        message = mld_v1_message(MLD_V1_REPORT, group, 0)
    else:
        if args.group == "::":
            raise SystemExit("done-v1 needs --group")
        dst_ip = ipaddress.IPv6Address("ff02::2").packed
        message = mld_v1_message(MLD_DONE, group, 0)

    frame = build_frame(src_mac, src_ip, dst_ip, message)

    try:
        sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    except PermissionError:
        raise SystemExit(
            "no CAP_NET_RAW.  Run under a capable copy:\n"
            "  cp $(command -v python3) ~/python3-cap\n"
            "  sudo /usr/sbin/setcap cap_net_raw+eip ~/python3-cap")
    sock.bind((args.iface, 0))
    for _ in range(args.count):
        sock.send(frame)
    sock.close()

    print("sent=%d kind=%s group=%s bytes=%d"
          % (args.count, args.kind, args.group, len(frame)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
