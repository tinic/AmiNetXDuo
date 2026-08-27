#!/usr/bin/env python3
"""Two DHCPv6 servers on one link, and only one of them says OPTION_PREFERENCE.

The lab had no server that could send RFC 8415 option 7, so a two-server link
picked by arrival order and tests/ipv6/run-dhcpv6.sh could not tell preference
from a race.  This is that server: it answers a SOLICIT with TWO Advertises
carrying different DUIDs, different addresses and different preference values,
in whichever order the caller asks for, and then reports which of the two the
client's Request named.

RAW ETHERNET AND NOT A UDP SOCKET.  Binding UDP 547 needs CAP_NET_BIND_SERVICE,
which the lab's capability-carrying python3 does not have; AF_PACKET needs
CAP_NET_RAW, which it does.  Sending whole frames also gives each of the two
servers a link-local source address of its own, so a capture separates them
without reading DUIDs.

  python3-cap dhcpv6-prefserver.py --iface ens18 --order high-first

Prints key=value lines and nothing else.

SPDX-License-Identifier: MIT
"""

import argparse
import socket
import struct
import sys
import time

ETH_P_ALL = 0x0003
SOL_PACKET = 263
PACKET_ADD_MEMBERSHIP = 1
PACKET_MR_ALLMULTI = 2

MSG_SOLICIT = 1
MSG_ADVERTISE = 2
MSG_REQUEST = 3
MSG_CONFIRM = 4
MSG_RENEW = 5
MSG_REBIND = 6
MSG_REPLY = 7
MSG_RELEASE = 8
MSG_DECLINE = 9
MSG_INFORMATION_REQUEST = 11

OPT_CLIENTID = 1
OPT_SERVERID = 2
OPT_IA_NA = 3
OPT_IAADDR = 5
OPT_ORO = 6
OPT_PREFERENCE = 7
OPT_STATUS_CODE = 13
OPT_DNS_SERVERS = 23

ALL_SERVERS = "ff02::1:2"


def say(**kw):
    for k, v in kw.items():
        sys.stdout.write("%s=%s\n" % (k, v))
    sys.stdout.flush()


def mac_bytes(text):
    return bytes(int(p, 16) for p in text.split(":"))


def linklocal_from_mac(mac):
    """The EUI-64 link-local a host would give itself for this MAC."""
    b = bytearray(16)
    b[0] = 0xFE
    b[1] = 0x80
    b[8] = mac[0] ^ 0x02
    b[9] = mac[1]
    b[10] = mac[2]
    b[11] = 0xFF
    b[12] = 0xFE
    b[13] = mac[3]
    b[14] = mac[4]
    b[15] = mac[5]
    return bytes(b)


def opt(code, payload):
    return struct.pack("!HH", code, len(payload)) + payload


def parse_options(blob):
    """[(code, value)] in wire order.  A truncated option ends the walk."""
    out = []
    i = 0
    while i + 4 <= len(blob):
        code, length = struct.unpack("!HH", blob[i:i + 4])
        if i + 4 + length > len(blob):
            break
        out.append((code, blob[i + 4:i + 4 + length]))
        i += 4 + length
    return out


def first_option(options, code):
    for c, v in options:
        if c == code:
            return v
    return None


def udp6_checksum(src, dst, payload):
    pseudo = src + dst + struct.pack("!IHH", len(payload), 0, 17)
    data = pseudo + payload
    if len(data) % 2:
        data += b"\x00"
    total = 0
    for i in range(0, len(data), 2):
        total += (data[i] << 8) | data[i + 1]
        total = (total & 0xFFFF) + (total >> 16)
    total = ~total & 0xFFFF
    return total if total else 0xFFFF


def build_frame(src_mac, dst_mac, src_ip, dst_ip, src_port, dst_port, body):
    udp = struct.pack("!HHHH", src_port, dst_port, 8 + len(body), 0) + body
    csum = udp6_checksum(src_ip, dst_ip, udp)
    udp = udp[:6] + struct.pack("!H", csum) + udp[8:]

    ipv6 = struct.pack("!IHBB", 0x60000000, len(udp), 17, 255) + src_ip + dst_ip
    return dst_mac + src_mac + b"\x86\xdd" + ipv6 + udp


class Server(object):
    """One of the two DHCPv6 servers: a DUID, an address, a preference."""

    def __init__(self, name, mac, address, preference, t1, t2, lifetime):
        self.name = name
        self.mac = mac_bytes(mac)
        self.ip = linklocal_from_mac(self.mac)
        self.duid = struct.pack("!HH", 3, 1) + self.mac      # DUID-LL
        self.address = socket.inet_pton(socket.AF_INET6, address)
        self.address_text = address
        self.preference = preference
        self.t1 = t1
        self.t2 = t2
        self.lifetime = lifetime

    def ia_na(self, iaid):
        iaaddr = opt(OPT_IAADDR,
                     self.address + struct.pack("!II", self.lifetime,
                                                self.lifetime))
        return opt(OPT_IA_NA, struct.pack("!III", iaid, self.t1, self.t2)
                   + iaaddr)

    def message(self, msg_type, xid, client_id, iaid, with_preference):
        body = struct.pack("!B", msg_type) + xid
        body += opt(OPT_CLIENTID, client_id)
        body += opt(OPT_SERVERID, self.duid)
        body += self.ia_na(iaid)
        if with_preference:
            body += opt(OPT_PREFERENCE, struct.pack("!B", self.preference))
        return body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iface", default="ens18")
    ap.add_argument("--order", default="alternate",
                    choices=["high-first", "high-last", "alternate"])
    ap.add_argument("--high-pref", type=int, default=200)
    ap.add_argument("--low-pref", type=int, default=10)
    ap.add_argument("--high-address", default="fd00:aa5:2::a")
    ap.add_argument("--low-address", default="fd00:aa5:2::b")
    ap.add_argument("--high-mac", default="02:00:6a:00:aa:01")
    ap.add_argument("--low-mac", default="02:00:6a:00:bb:01")
    ap.add_argument("--gap-ms", type=int, default=120,
                    help="between the two Advertises, inside the client's "
                         "first retransmission window")
    ap.add_argument("--lifetime", type=int, default=600)
    ap.add_argument("--seconds", type=int, default=300)
    ap.add_argument("--client-mac", default="",
                    help="answer only this client; empty answers every one")
    args = ap.parse_args()

    high = Server("high", args.high_mac, args.high_address, args.high_pref,
                  args.lifetime // 2, (args.lifetime * 4) // 5, args.lifetime)
    low = Server("low", args.low_mac, args.low_address, args.low_pref,
                 args.lifetime // 2, (args.lifetime * 4) // 5, args.lifetime)

    by_duid = {high.duid: high, low.duid: low}
    only = mac_bytes(args.client_mac) if args.client_mac else None

    try:
        sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                             socket.htons(ETH_P_ALL))
    except PermissionError:
        say(result="badinvocation", reason="no_cap_net_raw")
        return 2

    try:
        sock.bind((args.iface, 0))
    except OSError as exc:
        say(result="badinvocation", reason="bind_%s" % exc.errno)
        return 2

    # The client talks to ff02::1:2, so the NIC has to keep multicast it did
    # not join.  CAP_NET_ADMIN, which the lab's python3 carries.
    try:
        idx = socket.if_nametoindex(args.iface)
        mreq = struct.pack("IHH8s", idx, PACKET_MR_ALLMULTI, 0, b"")
        sock.setsockopt(SOL_PACKET, PACKET_ADD_MEMBERSHIP, mreq)
        allmulti = "yes"
    except OSError:
        allmulti = "no"

    say(server_ready="yes", iface=args.iface, order=args.order,
        allmulti=allmulti,
        high_duid=high.duid.hex(), high_pref=high.preference,
        high_address=high.address_text, high_linklocal=
        socket.inet_ntop(socket.AF_INET6, high.ip),
        low_duid=low.duid.hex(), low_pref=low.preference,
        low_address=low.address_text, low_linklocal=
        socket.inet_ntop(socket.AF_INET6, low.ip))

    sock.settimeout(1.0)
    deadline = time.time() + args.seconds
    solicits = 0
    high_first = (args.order != "high-last")

    while time.time() < deadline:
        try:
            frame = sock.recv(2048)
        except socket.timeout:
            continue

        if len(frame) < 14 + 40 + 8:
            continue
        if frame[12:14] != b"\x86\xdd":
            continue

        client_mac = frame[6:12]
        ipv6 = frame[14:54]
        if ipv6[6] != 17:                       # next header must be UDP
            continue
        src_ip = ipv6[8:24]
        udp = frame[54:]
        sport, dport, ulen = struct.unpack("!HHH", udp[0:6])
        if dport != 547 or sport != 546:
            continue
        body = udp[8:8 + (ulen - 8)]
        if len(body) < 4:
            continue
        if only is not None and client_mac != only:
            continue

        msg_type = body[0]
        xid = body[1:4]
        options = parse_options(body[4:])
        client_id = first_option(options, OPT_CLIENTID)
        ia_na = first_option(options, OPT_IA_NA)
        if client_id is None:
            continue
        iaid = struct.unpack("!I", ia_na[0:4])[0] if ia_na and len(ia_na) >= 4 \
            else 0

        if msg_type == MSG_SOLICIT:
            solicits += 1
            order = ([high, low] if high_first else [low, high])
            say(solicit_seen=solicits,
                solicit_client=socket.inet_ntop(socket.AF_INET6, src_ip),
                solicit_xid=xid.hex(),
                advertise_order=",".join(s.name for s in order))

            for n, server in enumerate(order):
                if n:
                    time.sleep(args.gap_ms / 1000.0)
                msg = server.message(MSG_ADVERTISE, xid, client_id, iaid, True)
                sock.send(build_frame(server.mac, client_mac, server.ip,
                                      src_ip, 547, 546, msg))
                say(**{"advertise_sent_%d" % n: server.name,
                       "advertise_pref_%d" % n: server.preference})

            if args.order == "alternate":
                high_first = not high_first

        elif msg_type in (MSG_REQUEST, MSG_RENEW, MSG_REBIND, MSG_CONFIRM):
            server_id = first_option(options, OPT_SERVERID)
            picked = by_duid.get(server_id) if server_id else None
            say(**{"%s_serverid" % {MSG_REQUEST: "request",
                                    MSG_RENEW: "renew",
                                    MSG_REBIND: "rebind",
                                    MSG_CONFIRM: "confirm"}[msg_type]:
                   (server_id.hex() if server_id else "none"),
                   "%s_picked" % {MSG_REQUEST: "request",
                                  MSG_RENEW: "renew",
                                  MSG_REBIND: "rebind",
                                  MSG_CONFIRM: "confirm"}[msg_type]:
                   (picked.name if picked else "unknown")})

            if picked is None:
                continue

            msg = picked.message(MSG_REPLY, xid, client_id, iaid, False)
            sock.send(build_frame(picked.mac, client_mac, picked.ip, src_ip,
                                  547, 546, msg))
            say(reply_sent=picked.name, reply_address=picked.address_text)

        elif msg_type == MSG_RELEASE:
            server_id = first_option(options, OPT_SERVERID)
            picked = by_duid.get(server_id) if server_id else None
            say(release_seen="yes",
                release_picked=(picked.name if picked else "unknown"))
            if picked is None:
                continue
            body_out = struct.pack("!B", MSG_REPLY) + xid
            body_out += opt(OPT_CLIENTID, client_id)
            body_out += opt(OPT_SERVERID, picked.duid)
            body_out += opt(OPT_STATUS_CODE, struct.pack("!H", 0) + b"done")
            sock.send(build_frame(picked.mac, client_mac, picked.ip, src_ip,
                                  547, 546, body_out))

    say(result="stopped", solicits=solicits)
    return 0


if __name__ == "__main__":
    sys.exit(main())
