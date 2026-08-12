#!/usr/bin/env python3
"""
netpeer, the other end, on the host, for the nc / telnet runs.

tools/amiberry-run.sh attaches the emulator's SLIRP user-mode NAT.  From
inside the guest the host is 10.0.2.2, so a server bound to 127.0.0.1 here is
reachable from the Amiga at 10.0.2.2:<port>.  That is how the client half of all three
commands is tested against something that is not a mock.

Three servers, one process, one log:

  echo    accepts, greets, and sends back whatever it is given.  What `nc`
          talks to.
  telnet  a real IAC negotiation: it offers WILL ECHO, WILL SUPPRESS-GO-AHEAD,
          DO TERMINAL-TYPE and DO WINDOW-SIZE, and RECORDS what the client
          answers.  Refusing an option correctly is a thing that has to be
          seen from the other side to be believed, and this is the side.

SPDX-License-Identifier: MIT
"""

import argparse
import os
import socket
import socketserver
import sys
import threading
import time

LOG_LOCK = threading.Lock()
LOG_FILE = None


def log(tag, msg):
    line = "[%7.2f] %-6s %s" % (time.monotonic() - START, tag, msg)
    with LOG_LOCK:
        print(line, flush=True)
        if LOG_FILE:
            LOG_FILE.write(line + "\n")
            LOG_FILE.flush()


START = time.monotonic()


# --------------------------------------------------------------------- echo --

class EchoHandler(socketserver.BaseRequestHandler):
    def handle(self):
        log("echo", "connection from %s:%d" % self.client_address[:2])
        self.request.sendall(b"echo server ready\r\n")
        total = 0
        while True:
            try:
                data = self.request.recv(4096)
            except OSError:
                break
            if not data:
                break
            total += len(data)
            log("echo", "got %d bytes: %r" % (len(data), data[:120]))
            try:
                self.request.sendall(data)
            except OSError:
                break
        log("echo", "closed after %d bytes" % total)
        try:
            self.request.shutdown(socket.SHUT_WR)
        except OSError:
            pass


# ----------------------------------------------------------------- daytime --

DAYTIME_BODY = b"AmiNetXDuo daytime, line one\r\nand line two\r\n"


class DaytimeHandler(socketserver.BaseRequestHandler):
    """RFC 867 in spirit: send a fixed body and close.

    The echo server above never closes, which makes it useless to a command
    that reads until end of file, and reading until end of file is exactly
    what `Type TCP:...` and `Copy TCP:... TO ...` do.  This is the other end
    of those: a finite stream with a real FIN at the end of it, so that what
    the Amiga wrote to disk can be compared byte for byte.
    """

    def handle(self):
        log("daytime", "connection from %s:%d" % self.client_address[:2])
        self.request.sendall(DAYTIME_BODY)
        self.request.shutdown(socket.SHUT_WR)
        log("daytime", "sent %d bytes and closed" % len(DAYTIME_BODY))


# ------------------------------------------------------------------- telnet --

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
OPT_ECHO, OPT_SGA, OPT_TTYPE, OPT_NAWS = 1, 3, 24, 31

VERB = {WILL: "WILL", WONT: "WONT", DO: "DO", DONT: "DONT"}
OPTN = {0: "BINARY", 1: "ECHO", 3: "SGA", 5: "STATUS", 24: "TERMINAL-TYPE",
        31: "WINDOW-SIZE", 32: "TERMINAL-SPEED", 34: "LINEMODE",
        39: "ENVIRONMENT"}


class TelnetHandler(socketserver.BaseRequestHandler):
    def handle(self):
        log("telnet", "connection from %s:%d" % self.client_address[:2])

        # Everything a real telnetd opens with, including two options the
        # client is expected to refuse.
        offer = bytes([IAC, WILL, OPT_ECHO,
                       IAC, WILL, OPT_SGA,
                       IAC, DO, OPT_TTYPE,
                       IAC, DO, OPT_NAWS])
        self.request.sendall(offer)
        self.request.sendall(b"AmiNetXDuo test telnet server\r\nlogin: ")

        answers = []
        line = bytearray()
        state = 0
        verb = 0

        while True:
            try:
                data = self.request.recv(4096)
            except OSError:
                break
            if not data:
                break

            for b in data:
                if state == 0:
                    if b == IAC:
                        state = 1
                    elif b in (13, 10):
                        if line:
                            text = bytes(line).decode("latin-1")
                            log("telnet", "line: %r" % text)
                            self.request.sendall(
                                b"you said: " + bytes(line) + b"\r\n")
                            if text.strip().lower() in ("quit", "exit", "bye"):
                                self.request.sendall(b"goodbye\r\n")
                                log("telnet", "answers: %s" %
                                    ", ".join(answers))
                                return
                            line.clear()
                    else:
                        line.append(b)
                elif state == 1:
                    if b == IAC:
                        line.append(IAC)
                        state = 0
                    elif b in (WILL, WONT, DO, DONT):
                        verb = b
                        state = 2
                    elif b == SB:
                        state = 3
                    else:
                        state = 0
                elif state == 2:
                    name = "%s %s" % (VERB[verb], OPTN.get(b, str(b)))
                    answers.append(name)
                    log("telnet", "client answered %s" % name)
                    state = 0
                elif state == 3:
                    if b == IAC:
                        state = 4
                elif state == 4:
                    state = 0 if b == SE else 3

        log("telnet", "closed; answers: %s" % (", ".join(answers) or "(none)"))


# The in-memory file set the tftp server serves.  It outlived the ftp server
# that used to share it.
FILES = {
    "hello.txt": b"Hello from the host.\r\nSecond line.\r\nThird line.\r\n",
    "binary.dat": bytes(range(256)) * 4,
}


# --------------------------------------------------------------------- tftp --
#
# RFC 1350, octet mode, both directions.
#
# Port 69 needs root on this host, so the port is an argument and the guest is
# told to use it.  That costs nothing: TFTP's interesting property is not the
# well-known port, it is the TRANSFER IDENTIFIER, the answer comes from a
# port the server picks, and every later packet belongs to that port.  Each
# session below therefore gets its own socket, exactly as a real server does,
# which is what makes the client's TID handling something this test can prove
# rather than assume.

TFTP_FILES = dict(FILES)
TFTP_FILES["big.bin"] = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(100000))
# Exactly four blocks: the case that needs a trailing EMPTY data block, and the
# one a client that stops at "short block" gets wrong.
TFTP_FILES["exact.bin"] = bytes(range(256)) * 8

TFTP_BLOCK = 512


def tftp_request_parts(data):
    """opcode, filename, mode, or (None, None, None) if it is not a request."""
    if len(data) < 4:
        return None, None, None
    opcode = int.from_bytes(data[:2], "big")
    fields = data[2:].split(b"\x00")
    if len(fields) < 2:
        return opcode, None, None
    return opcode, fields[0].decode("latin-1"), fields[1].decode("latin-1")


def tftp_error(sock, addr, code, text):
    sock.sendto(b"\x00\x05" + code.to_bytes(2, "big")
                + text.encode("latin-1") + b"\x00", addr)


def tftp_session(request, client, log_tag):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 0))
    sock.settimeout(10)

    opcode, name, mode = tftp_request_parts(request)
    log(log_tag, "%s %r mode %r from %s:%d (my TID is %d)"
        % ({1: "RRQ", 2: "WRQ"}.get(opcode, "op %s" % opcode), name, mode,
           client[0], client[1], sock.getsockname()[1]))

    try:
        if opcode == 1:
            tftp_send_file(sock, client, name, log_tag)
        elif opcode == 2:
            tftp_receive_file(sock, client, name, log_tag)
        else:
            tftp_error(sock, client, 4, "not a request")
    except socket.timeout:
        log(log_tag, "the client stopped answering")
    except OSError as exc:
        log(log_tag, "session failed: %s" % exc)
    finally:
        sock.close()


def tftp_send_file(sock, client, name, log_tag):
    blob = TFTP_FILES.get(name)
    if blob is None:
        log(log_tag, "no such file: %r" % name)
        tftp_error(sock, client, 1, "no such file")
        return

    block = 1
    offset = 0
    while True:
        chunk = blob[offset:offset + TFTP_BLOCK]
        packet = b"\x00\x03" + (block & 0xFFFF).to_bytes(2, "big") + chunk

        for attempt in range(5):
            sock.sendto(packet, client)
            try:
                reply, who = sock.recvfrom(1024)
            except socket.timeout:
                continue
            if who != client:
                tftp_error(sock, who, 5, "unknown transfer ID")
                continue
            if len(reply) >= 4 and reply[:2] == b"\x00\x04" \
                    and int.from_bytes(reply[2:4], "big") == (block & 0xFFFF):
                break
        else:
            log(log_tag, "gave up on block %d" % block)
            return

        offset += len(chunk)
        block += 1
        if len(chunk) < TFTP_BLOCK:
            break

    log(log_tag, "sent %r, %d bytes in %d blocks" % (name, len(blob), block - 1))


def tftp_receive_file(sock, client, name, log_tag):
    sock.sendto(b"\x00\x04\x00\x00", client)          # ACK of the WRQ

    got = bytearray()
    expect = 1
    while True:
        try:
            packet, who = sock.recvfrom(TFTP_BLOCK + 64)
        except socket.timeout:
            log(log_tag, "the client stopped sending after %d bytes" % len(got))
            return
        if who != client:
            tftp_error(sock, who, 5, "unknown transfer ID")
            continue
        if len(packet) < 4 or packet[:2] != b"\x00\x03":
            continue

        block = int.from_bytes(packet[2:4], "big")
        if block != (expect & 0xFFFF):
            # A duplicate: acknowledge it again and wait for the one we want.
            sock.sendto(b"\x00\x04" + packet[2:4], client)
            continue

        got += packet[4:]
        sock.sendto(b"\x00\x04" + packet[2:4], client)
        expect += 1

        if len(packet) - 4 < TFTP_BLOCK:
            break

    UPLOADS[name] = bytes(got)
    log(log_tag, "received %r, %d bytes; first 16 = %r"
        % (name, len(got), bytes(got[:16])))


def tftp_server(bind, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind, port))
    while True:
        try:
            data, addr = srv.recvfrom(1024)
        except OSError:
            return
        threading.Thread(target=tftp_session, args=(data, addr, "tftp"),
                         daemon=True).start()


# -------------------------------------------------------------------- whois --
#
# RFC 3912 in twenty lines, which is all of it: read a line, write a record,
# hang up.  What is worth testing here is not the record, it is the REFERRAL,
# so `referral.test` is answered with a `refer:` line pointing back at this
# same server.  That exercises the follow, and then the client's own guard
# against a server that refers you to itself.

WHOIS_RECORDS = {
    "plain.test": "domain:       PLAIN.TEST\r\n"
                  "organisation: AmiNetXDuo test rig\r\n"
                  "created:      2026-07-26\r\n",
    # Refers to the server it came from, which is a loop and not a referral.
    # The client has to notice that and stop.
    "referral.test": "domain:       REFERRAL.TEST\r\n"
                     "refer:        %s\r\n"
                     "organisation: AmiNetXDuo test rig\r\n",
    # Refers somewhere else, which is the real shape: without FOLLOW the
    # client prints the line to type next, with it the client goes there.
    "chain.test": "domain:       CHAIN.TEST\r\n"
                  "refer:        127.0.0.1\r\n"
                  "organisation: AmiNetXDuo test rig\r\n",
}


class WhoisHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(15)
        query = b""
        while b"\n" not in query and len(query) < 256:
            chunk = self.request.recv(64)
            if not chunk:
                break
            query += chunk

        name = query.decode("latin-1").strip().lower()
        log("whois", "query %r from %s:%d"
            % (name, self.client_address[0], self.client_address[1]))

        record = WHOIS_RECORDS.get(name)
        if record is None:
            body = "No match for \"%s\".\r\n" % name.upper()
        elif "%s" in record:
            body = record % self.server.advertise
        else:
            body = record

        self.request.sendall(body.encode("latin-1"))
        log("whois", "answered %d bytes" % len(body))


# ------------------------------------------------------------------- dial --

def dialer(target, message, deadline):
    """Connect INTO the guest, which is the whole point of slirp_redir.

    The Amiga's listener comes up somewhere in the middle of a boot this
    script cannot see, so this retries rather than assuming.  Each attempt is
    a fresh socket: a refused connect leaves the old one unusable.
    """
    host, port = target.rsplit(":", 1)
    port = int(port)
    attempts = 0

    while time.monotonic() < deadline:
        attempts += 1
        try:
            conn = socket.create_connection((host, port), timeout=5)
        except OSError:
            time.sleep(2)
            continue

        log("dial", "connected to %s:%d after %d attempts"
            % (host, port, attempts))
        try:
            conn.sendall(message.encode("latin-1"))
            conn.shutdown(socket.SHUT_WR)
            back = b""
            conn.settimeout(10)
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                back += chunk
            log("dial", "the Amiga sent back %d bytes: %r" % (len(back), back))
        except OSError as exc:
            log("dial", "transfer failed: %s" % exc)
        finally:
            conn.close()
        return

    log("dial", "never reached %s:%d in %d attempts, nothing is listening "
                "inside the guest, or the SLIRP forward is not there"
        % (host, port, attempts))


# -------------------------------------------------------------------- main --

class Threaded(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    global LOG_FILE

    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--echo-port", type=int, default=7001)
    ap.add_argument("--telnet-port", type=int, default=7023)
    ap.add_argument("--tftp-port", type=int, default=0,
                    help="UDP port for the TFTP server; 0 leaves it off")
    ap.add_argument("--whois-port", type=int, default=0,
                    help="TCP port for the whois server; 0 leaves it off")
    ap.add_argument("--daytime-port", type=int, default=0,
                    help="TCP port for the daytime server, a finite stream "
                         "that closes; 0 leaves it off")
    ap.add_argument("--advertise", default="10.0.2.2",
                    help="address substituted into whois records, what the "
                         "guest must dial, not what we are bound to")
    ap.add_argument("--log")
    ap.add_argument("--seconds", type=int, default=600)
    ap.add_argument("--dial",
                    help="host:port to connect INTO, retrying, once the "
                         "guest's listener appears")
    ap.add_argument("--dial-message", default="hello from the host\r\n")
    ap.add_argument("--dial-for", type=int, default=240,
                    help="how long to keep retrying --dial")
    args = ap.parse_args()

    if args.log:
        LOG_FILE = open(args.log, "w")

    servers = []

    echo = Threaded((args.bind, args.echo_port), EchoHandler)
    servers.append(("echo", echo))

    tn = Threaded((args.bind, args.telnet_port), TelnetHandler)
    servers.append(("telnet", tn))

    if args.whois_port:
        who = Threaded((args.bind, args.whois_port), WhoisHandler)
        who.advertise = args.advertise
        servers.append(("whois", who))

    if args.daytime_port:
        day = Threaded((args.bind, args.daytime_port), DaytimeHandler)
        servers.append(("daytime", day))

    for name, srv in servers:
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        log("start", "%s on %s:%d" % (name, args.bind,
                                      srv.server_address[1]))

    if args.tftp_port:
        threading.Thread(target=tftp_server,
                         args=(args.bind, args.tftp_port), daemon=True).start()
        log("start", "tftp on %s:%d (UDP), serving %s"
            % (args.bind, args.tftp_port, ", ".join(sorted(TFTP_FILES))))

    if args.dial:
        threading.Thread(
            target=dialer,
            args=(args.dial, args.dial_message,
                  time.monotonic() + args.dial_for),
            daemon=True).start()
        log("start", "will dial into %s for up to %d s"
            % (args.dial, args.dial_for))

    log("start", "pid %d, advertising %s"
        % (os.getpid(), args.advertise))

    try:
        time.sleep(args.seconds)
    except KeyboardInterrupt:
        pass

    log("stop", "shutting down")
    return 0


if __name__ == "__main__":
    sys.exit(main())
