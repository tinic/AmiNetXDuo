#!/usr/bin/env python3
"""
netpeer -- the other end, on the host, for the nc / telnet / ftp runs.

tools/fsuae-run.sh attaches FS-UAE's SLIRP user-mode NAT.  From inside the
guest the host is 10.0.2.2, so a server bound to 127.0.0.1 here is reachable
from the Amiga at 10.0.2.2:<port>.  That is how the client half of all three
commands is tested against something that is not a mock.

Three servers, one process, one log:

  echo    accepts, greets, and sends back whatever it is given.  What `nc`
          talks to.
  telnet  a real IAC negotiation: it offers WILL ECHO, WILL SUPPRESS-GO-AHEAD,
          DO TERMINAL-TYPE and DO WINDOW-SIZE, and RECORDS what the client
          answers.  Refusing an option correctly is a thing that has to be
          seen from the other side to be believed, and this is the side.
  ftp     RFC 959, enough of it: USER/PASS/SYST/TYPE/PWD/CWD/PASV/PORT/
          LIST/RETR/STOR/SIZE/QUIT, over a handful of in-memory files.

THE ONE ACCOMMODATION, and it is only for active mode
-----------------------------------------------------
In active FTP the SERVER connects back to the client.  The client is inside
the NAT, so its address (10.0.2.15) is not routable from here; the only way in
is FS-UAE's own `slirp_redir`, which listens on a port of the HOST's and
forwards it to the same port of the guest's.  So when --active-via-loopback is
given, the PORT command's port number is honoured and its ADDRESS is replaced
with 127.0.0.1.

That is a property of the test rig, not of the client: the Amiga still binds,
listens, sends a correct PORT with its own address in it, and accepts an
inbound connection it did not initiate.  Every part of the client's active
mode is exercised.  What is faked is the route back through the NAT.

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


# ---------------------------------------------------------------------- ftp --

FILES = {
    "hello.txt": b"Hello from the host.\r\nSecond line.\r\nThird line.\r\n",
    "binary.dat": bytes(range(256)) * 4,
}

UPLOADS = {}


class FtpHandler(socketserver.BaseRequestHandler):
    def setup(self):
        self.rfile = self.request.makefile("rb")
        self.binary = True
        self.pasv_sock = None
        self.port_target = None
        self.cwd = "/"

    def reply(self, text):
        log("ftp", "--> %s" % text)
        self.request.sendall(text.encode("latin-1") + b"\r\n")

    def open_data(self):
        """The data connection, whichever way round this transfer is."""
        if self.pasv_sock is not None:
            self.pasv_sock.settimeout(30)
            conn, addr = self.pasv_sock.accept()
            log("ftp", "passive data connection from %s:%d" % addr[:2])
            self.pasv_sock.close()
            self.pasv_sock = None
            return conn

        if self.port_target is None:
            self.reply("425 Use PORT or PASV first.")
            return None

        host, port = self.port_target
        if self.server.active_via_loopback:
            log("ftp", "PORT named %s:%d; dialling 127.0.0.1:%d "
                       "(the SLIRP forward)" % (host, port, port))
            host = "127.0.0.1"
        else:
            log("ftp", "dialling back to %s:%d" % (host, port))

        conn = socket.create_connection((host, port), timeout=30)
        log("ftp", "active data connection established")
        self.port_target = None
        return conn

    def handle(self):
        log("ftp", "connection from %s:%d" % self.client_address[:2])
        self.reply("220 AmiNetXDuo test FTP server")

        while True:
            raw = self.rfile.readline()
            if not raw:
                break
            line = raw.decode("latin-1").rstrip("\r\n")
            if not line:
                continue

            shown = line
            if line.upper().startswith("PASS"):
                shown = "PASS ********"
            log("ftp", "<-- %s" % shown)

            parts = line.split(" ", 1)
            cmd = parts[0].upper()
            arg = parts[1] if len(parts) > 1 else ""

            if cmd == "USER":
                self.reply("331 Password required for %s." % arg)
            elif cmd == "PASS":
                self.reply("230 Logged in.")
            elif cmd == "SYST":
                self.reply("215 UNIX Type: L8")
            elif cmd == "TYPE":
                self.binary = arg.upper().startswith("I")
                self.reply("200 Type set to %s." % arg.upper())
            elif cmd == "PWD":
                self.reply('257 "%s" is the current directory.' % self.cwd)
            elif cmd == "CWD":
                self.cwd = arg if arg.startswith("/") else self.cwd + arg
                self.reply("250 Directory changed to %s." % self.cwd)
            elif cmd == "CDUP":
                self.reply("250 Directory changed to /.")
            elif cmd == "SIZE":
                blob = FILES.get(arg) or UPLOADS.get(arg)
                if blob is None:
                    self.reply("550 %s: no such file." % arg)
                else:
                    self.reply("213 %d" % len(blob))
            elif cmd == "NOOP":
                self.reply("200 OK.")
            elif cmd == "PASV":
                if self.pasv_sock:
                    self.pasv_sock.close()
                self.pasv_sock = socket.socket()
                self.pasv_sock.setsockopt(socket.SOL_SOCKET,
                                          socket.SO_REUSEADDR, 1)
                self.pasv_sock.bind(("0.0.0.0", 0))
                self.pasv_sock.listen(1)
                port = self.pasv_sock.getsockname()[1]
                a = self.server.advertise.split(".")
                self.reply("227 Entering Passive Mode (%s,%s,%s,%s,%d,%d)." %
                           (a[0], a[1], a[2], a[3], port >> 8, port & 0xFF))
            elif cmd == "PORT":
                try:
                    n = [int(v) for v in arg.split(",")]
                    self.port_target = ("%d.%d.%d.%d" % tuple(n[:4]),
                                        (n[4] << 8) | n[5])
                except (ValueError, IndexError):
                    self.reply("501 Bad PORT.")
                    continue
                self.reply("200 PORT command successful.")
            elif cmd in ("LIST", "NLST"):
                self.reply("150 Opening data connection for %s." % cmd)
                try:
                    conn = self.open_data()
                except OSError as exc:
                    log("ftp", "data connection FAILED: %s" % exc)
                    self.reply("425 Cannot open data connection.")
                    continue
                if conn is None:
                    continue
                if cmd == "NLST":
                    body = "".join("%s\r\n" % n for n in sorted(FILES))
                else:
                    body = "".join(
                        "-rw-r--r-- 1 host host %8d Jul 25 12:00 %s\r\n"
                        % (len(FILES[n]), n) for n in sorted(FILES))
                conn.sendall(body.encode("latin-1"))
                conn.close()
                self.reply("226 Transfer complete.")
            elif cmd == "RETR":
                blob = FILES.get(arg) or UPLOADS.get(arg)
                if blob is None:
                    self.reply("550 %s: no such file." % arg)
                    continue
                self.reply("150 Opening data connection for %s (%d bytes)."
                           % (arg, len(blob)))
                try:
                    conn = self.open_data()
                except OSError as exc:
                    log("ftp", "data connection FAILED: %s" % exc)
                    self.reply("425 Cannot open data connection.")
                    continue
                if conn is None:
                    continue
                conn.sendall(blob)
                conn.close()
                self.reply("226 Transfer complete.")
            elif cmd == "STOR":
                self.reply("150 Ready for %s." % arg)
                try:
                    conn = self.open_data()
                except OSError as exc:
                    log("ftp", "data connection FAILED: %s" % exc)
                    self.reply("425 Cannot open data connection.")
                    continue
                if conn is None:
                    continue
                blob = b""
                while True:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    blob += chunk
                conn.close()
                UPLOADS[arg] = blob
                log("ftp", "stored %s: %d bytes, %r"
                    % (arg, len(blob), blob[:120]))
                self.reply("226 Transfer complete.")
            elif cmd == "QUIT":
                self.reply("221 Goodbye.")
                break
            else:
                self.reply("502 %s is not implemented here." % cmd)

        log("ftp", "session ended")


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

    log("dial", "never reached %s:%d in %d attempts -- nothing is listening "
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
    ap.add_argument("--ftp-port", type=int, default=7021)
    ap.add_argument("--advertise", default="10.0.2.2",
                    help="address to put in the 227 PASV reply -- what the "
                         "guest must dial, not what we are bound to")
    ap.add_argument("--active-via-loopback", action="store_true",
                    help="in active mode dial 127.0.0.1 instead of the "
                         "address the client named; see the module comment")
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

    ftp = Threaded((args.bind, args.ftp_port), FtpHandler)
    ftp.advertise = args.advertise
    ftp.active_via_loopback = args.active_via_loopback
    servers.append(("ftp", ftp))

    for name, srv in servers:
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        log("start", "%s on %s:%d" % (name, args.bind,
                                      srv.server_address[1]))

    if args.dial:
        threading.Thread(
            target=dialer,
            args=(args.dial, args.dial_message,
                  time.monotonic() + args.dial_for),
            daemon=True).start()
        log("start", "will dial into %s for up to %d s"
            % (args.dial, args.dial_for))

    log("start", "pid %d, advertising %s for PASV%s"
        % (os.getpid(), args.advertise,
           ", active dials 127.0.0.1" if args.active_via_loopback else ""))

    try:
        time.sleep(args.seconds)
    except KeyboardInterrupt:
        pass

    log("stop", "shutting down")
    return 0


if __name__ == "__main__":
    sys.exit(main())
