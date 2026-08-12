#!/usr/bin/env python3
"""httppeer, the host end every guest-side network test is pointed at.

tools/amiberry-run.sh attaches the emulator's SLIRP user-mode NAT, so a server
bound to 127.0.0.1 here is what the guest reaches at 10.0.2.2:<port>.  Same arrangement
as tests/tools/netpeer.py, which this imports rather than reimplements for the
FTP half.

WHY THE SERVERS ARE HAND-ROLLED

    http.server cannot do the half of this that matters.  The suite needs a
    server that truncates a body it has already promised a Content-Length for,
    one that answers a request with a RESET, one that accepts and then says
    nothing forever, and one that answers with bytes that are not HTTP.  Those
    are the cases where a TCP stack crashes rather than returns an error, so
    they are the reason the file exists; a framework that guarantees a
    well-formed response cannot produce any of them.

WHAT IS ON WHICH PORT (BASE defaults to 7100)

    BASE+0    http        the main HTTP/1.1 server, keep-alive, ranges, chunked
    BASE+1    https       RSA leaf, 2-certificate chain      rsa2.test
    BASE+2    https       RSA leaf, 3-certificate chain      rsa3.test
    BASE+3    https       RSA leaf, 4-certificate chain      rsa4.test
    BASE+4    https       ECDSA leaf, 2-certificate chain    ec2.test
    BASE+5    https       ECDSA leaf, 3-certificate chain    ec3.test
    BASE+6    https       expired leaf                       expired.test
    BASE+7    https       self-signed leaf                   selfsigned.test
    BASE+10   ftp         tests/tools/netpeer.py's FtpHandler
    BASE+20   raw         accept, read the request, close with nothing sent
    BASE+21   raw         accept and RESET immediately
    BASE+22   raw         accept and never say anything, ever
    BASE+23   raw         answer with bytes that are not HTTP
    BASE+99   -           deliberately NOT listening: connection refused

DETERMINISTIC BODIES

    One 2 MiB buffer, seeded, generated once.  /bytes/N is its first N bytes,
    so the host knows every byte the guest should have received without either
    side sending a manifest, a range is a slice of it, and a 1,200,000-byte
    download and a 1,024-byte one are checked the same way.

SPDX-License-Identifier: MIT
"""

import argparse
import hashlib
import os
import random
import socket
import ssl
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
import netpeer                                          # noqa: E402

START = time.monotonic()
LOG_LOCK = threading.Lock()
LOG_FILE = None
QUIET_PATHS = ("/bytes/", "/chunked/")


def log(tag, msg):
    line = "[%7.2f] %-8s %s" % (time.monotonic() - START, tag, msg)
    with LOG_LOCK:
        print(line, flush=True)
        if LOG_FILE:
            LOG_FILE.write(line + "\n")
            LOG_FILE.flush()


# ------------------------------------------------------------------ bodies --

MASTER_SIZE = 2 * 1024 * 1024
MASTER = random.Random(20260725).randbytes(MASTER_SIZE)


def master(n):
    if n > MASTER_SIZE:
        raise ValueError("body larger than the master buffer")
    return MASTER[:n]


# -------------------------------------------------------------- HTTP guts ---


class Closed(Exception):
    """The endpoint wants the connection gone, and not politely."""

    def __init__(self, reset=False):
        self.reset = reset


class Conn:
    """One accepted connection, plain or TLS, with a pushback buffer."""

    def __init__(self, sock, tag):
        self.sock = sock
        self.tag = tag
        self.buf = b""
        self.closed = False

    def readline(self, limit=1 << 20):
        while b"\r\n" not in self.buf:
            if len(self.buf) > limit:
                raise Closed()
            chunk = self.sock.recv(65536)
            if not chunk:
                raise Closed()
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise Closed()
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, data):
        try:
            self.sock.sendall(data)
        except OSError:
            raise Closed()

    def reset(self):
        """RST rather than FIN.  linger 0 is the portable way to ask."""
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                 struct.pack("ii", 1, 0))
        except OSError:
            pass
        self.close()

    def close(self):
        if not self.closed:
            self.closed = True
            try:
                self.sock.close()
            except OSError:
                pass


STATUS_TEXT = {
    200: "OK", 201: "Created", 202: "Accepted", 204: "No Content",
    206: "Partial Content", 301: "Moved Permanently", 302: "Found",
    303: "See Other", 307: "Temporary Redirect", 308: "Permanent Redirect",
    400: "Bad Request", 401: "Unauthorized", 403: "Forbidden",
    404: "Not Found", 405: "Method Not Allowed", 410: "Gone",
    413: "Payload Too Large", 418: "I am a teapot",
    500: "Internal Server Error", 502: "Bad Gateway",
    503: "Service Unavailable",
}


def head_block(code, headers):
    out = "HTTP/1.1 %d %s\r\n" % (code, STATUS_TEXT.get(code, "Status"))
    for k, v in headers:
        out += "%s: %s\r\n" % (k, v)
    return out.encode("latin-1") + b"\r\n"


class Request:
    def __init__(self, method, target, version, headers, body):
        self.method = method
        self.target = target
        self.version = version
        self.headers = headers
        self.body = body
        if "?" in target:
            self.path, _, q = target.partition("?")
        else:
            self.path, q = target, ""
        self.query = {}
        for pair in q.split("&"):
            if "=" in pair:
                k, _, v = pair.partition("=")
                self.query[k] = v

    def header(self, name, default=None):
        return self.headers.get(name.lower(), default)

    def qint(self, name, default):
        try:
            return int(self.query.get(name, default))
        except ValueError:
            return default


def read_request(conn):
    line = conn.readline()
    if not line:
        raise Closed()
    parts = line.decode("latin-1").split(" ")
    if len(parts) < 3:
        raise Closed()
    method, target, version = parts[0], parts[1], parts[2]

    headers = {}
    while True:
        h = conn.readline()
        if h == b"":
            break
        name, _, value = h.decode("latin-1").partition(":")
        headers[name.strip().lower()] = value.strip()

    # 100-continue before the body, or curl waits out its own second.
    if headers.get("expect", "").lower() == "100-continue":
        conn.send(b"HTTP/1.1 100 Continue\r\n\r\n")

    body = b""
    if headers.get("transfer-encoding", "").lower() == "chunked":
        while True:
            size_line = conn.readline()
            size = int(size_line.split(b";")[0], 16)
            if size == 0:
                while conn.readline() != b"":
                    pass
                break
            body += conn.read(size)
            conn.read(2)
    elif "content-length" in headers:
        body = conn.read(int(headers["content-length"]))

    return Request(method, target, version, headers, body)


# ---------------------------------------------------------------- handlers --


def parse_range(value, total):
    """One byte range, the only shape curl -r produces for a single range."""
    if not value.startswith("bytes="):
        return None
    spec = value[6:]
    if "," in spec:
        return None
    first, _, last = spec.partition("-")
    if first == "":
        n = int(last)
        if n <= 0:
            return None
        start = max(0, total - n)
        end = total - 1
    else:
        start = int(first)
        end = int(last) if last else total - 1
    if start >= total:
        return None
    end = min(end, total - 1)
    if end < start:
        return None
    return start, end


def send_body(conn, req, code, body, ctype="application/octet-stream",
              extra=(), close=False):
    headers = [("Content-Type", ctype), ("Accept-Ranges", "bytes")]
    headers.extend(extra)

    if req.method == "HEAD":
        headers.append(("Content-Length", str(len(body))))
        conn.send(head_block(code, headers))
        return close

    rng = req.header("range")
    if rng and code == 200:
        got = parse_range(rng, len(body))
        if got is None:
            conn.send(head_block(416, [("Content-Range", "bytes */%d"
                                        % len(body)),
                                       ("Content-Length", "0")]))
            return close
        start, end = got
        body = body[start:end + 1]
        code = 206
        headers.append(("Content-Range", "bytes %d-%d/%d"
                        % (start, end, len(body) + start)))

    headers.append(("Content-Length", str(len(body))))
    if close:
        headers.append(("Connection", "close"))
    conn.send(head_block(code, headers))
    conn.send(body)
    return close


def send_chunked(conn, req, body, chunk=1400, delay=0.0, trailer=False):
    extra = [("Transfer-Encoding", "chunked"),
             ("Content-Type", "application/octet-stream")]
    if trailer:
        extra.append(("Trailer", "X-Body-Sha256"))
    conn.send(head_block(200, extra))
    if req.method == "HEAD":
        return False
    for i in range(0, len(body), chunk):
        piece = body[i:i + chunk]
        conn.send(b"%x\r\n" % len(piece) + piece + b"\r\n")
        if delay:
            time.sleep(delay)
    if trailer:
        conn.send(b"0\r\nX-Body-Sha256: %s\r\n\r\n"
                  % hashlib.sha256(body).hexdigest().encode())
    else:
        conn.send(b"0\r\n\r\n")
    return False


def handle_request(conn, req, server):
    """Return True when the connection must not be reused."""
    path = req.path

    if path == "/hello":
        return send_body(conn, req, 200, b"hello from the host\n", "text/plain")

    if path == "/empty":
        return send_body(conn, req, 200, b"", "text/plain")

    if path.startswith("/bytes/"):
        return send_body(conn, req, 200, master(int(path[7:])))

    if path.startswith("/chunked/"):
        return send_chunked(conn, req, master(int(path[9:])))

    if path.startswith("/trailer/"):
        return send_chunked(conn, req, master(int(path[9:])), trailer=True)

    if path.startswith("/drip/"):
        n = int(path[6:])
        pieces = req.qint("chunks", 8)
        delay = req.qint("ms", 200) / 1000.0
        return send_chunked(conn, req, master(n),
                            chunk=max(1, (n + pieces - 1) // pieces),
                            delay=delay)

    if path.startswith("/slowheaders/"):
        n = int(path[13:])
        body = master(n)
        conn.send(b"HTTP/1.1 200 OK\r\n")
        for name, value in (("Content-Type", "application/octet-stream"),
                            ("X-Slow-1", "a"), ("X-Slow-2", "b"),
                            ("X-Slow-3", "c"),
                            ("Content-Length", str(len(body)))):
            time.sleep(0.4)
            conn.send(("%s: %s\r\n" % (name, value)).encode())
        time.sleep(0.4)
        conn.send(b"\r\n")
        if req.method != "HEAD":
            conn.send(body)
        return False

    if path.startswith("/status/"):
        code = int(path[8:])
        if code in (204, 304):
            conn.send(head_block(code, []))
            return False
        return send_body(conn, req, code,
                         ("status %d\n" % code).encode(), "text/plain")

    if path.startswith("/redirect/"):
        n = int(path[10:])
        if n <= 0:
            return send_body(conn, req, 200, master(1024))
        code = int(req.query.get("code", "302"))
        return send_body(conn, req, code, b"redirecting\n", "text/plain",
                         extra=[("Location", "/redirect/%d?code=%d"
                                 % (n - 1, code))])

    if path == "/redirect-loop":
        return send_body(conn, req, 302, b"round and round\n", "text/plain",
                         extra=[("Location", "/redirect-loop")])

    if path == "/redirect-refused":
        return send_body(conn, req, 302, b"nowhere\n", "text/plain",
                         extra=[("Location", "http://%s:%d/hello"
                                 % (server.advertise, server.base + 99))])

    if path == "/bigheaders":
        count = req.qint("count", 100)
        size = req.qint("size", 200)
        extra = [("X-Pad-%03d" % i, "p" * size) for i in range(count)]
        return send_body(conn, req, 200, b"big headers\n", "text/plain",
                         extra=extra)

    if path == "/echo-method":
        return send_body(conn, req, 200,
                         (req.method + "\n").encode(), "text/plain")

    if path == "/upload":
        digest = hashlib.sha256(req.body).hexdigest()
        log("http", "upload: %d bytes, sha256 %s" % (len(req.body), digest))
        return send_body(conn, req, 200,
                         ("len=%d\nsha256=%s\n" % (len(req.body), digest))
                         .encode(), "text/plain")

    if path == "/auth/basic":
        want = "Basic YW1pZ2E6c2VjcmV0"          # amiga:secret
        if req.header("authorization") != want:
            return send_body(conn, req, 401, b"who?\n", "text/plain",
                             extra=[("WWW-Authenticate",
                                     'Basic realm="aminetxduo"')])
        return send_body(conn, req, 200, master(1024))

    if path.startswith("/close-delimited/"):
        n = int(path[17:])
        conn.send(head_block(200, [("Content-Type", "application/octet-stream"),
                                   ("Connection", "close")]))
        if req.method != "HEAD":
            conn.send(master(n))
        raise Closed()

    if path.startswith("/truncate/"):
        n = int(path[10:])
        conn.send(head_block(200, [("Content-Type", "application/octet-stream"),
                                   ("Content-Length", str(n))]))
        conn.send(master(n // 4))
        log("http", "truncate: promised %d, sent %d, closing" % (n, n // 4))
        raise Closed()

    if path.startswith("/reset/"):
        n = int(path[7:])
        conn.send(head_block(200, [("Content-Type", "application/octet-stream"),
                                   ("Content-Length", str(n))]))
        conn.send(master(min(n // 4, 4096)))
        log("http", "reset: promised %d, sent %d, RESET" % (n, min(n // 4, 4096)))
        raise Closed(reset=True)

    if path == "/never":
        log("http", "never: request read, answering nothing")
        while True:
            time.sleep(5)

    if path.startswith("/halfclose/"):
        # Answer, then close only the WRITE direction and keep reading.  This
        # is `nc -N`, it is every FTP data connection, and it is the shape
        # that found the shutdown(SHUT_WR)-sends-a-RESET bug in this stack.
        n = int(path[11:])
        conn.send(head_block(200, [("Content-Type", "application/octet-stream"),
                                   ("Content-Length", str(n))]))
        conn.send(master(n))
        try:
            conn.sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        log("http", "halfclose: %d bytes sent, write side closed, still "
                    "reading" % n)
        try:
            conn.sock.settimeout(30)
            while conn.sock.recv(4096):
                pass
        except OSError:
            pass
        raise Closed()

    if path == "/setcookies":
        n = req.qint("n", 40)
        extra = [("Set-Cookie", "pad%03d=%s; Path=/" % (i, "c" * 40))
                 for i in range(n)]
        return send_body(conn, req, 200, b"cookies set\n", "text/plain",
                         extra=extra)

    if path == "/needbigrequest":
        size = req.qint("min", 2000)
        total = sum(len(k) + len(v) + 4 for k, v in req.headers.items())
        log("http", "needbigrequest: %d header bytes (want >%d)"
            % (total, size))
        if total <= size:
            return send_body(conn, req, 400,
                             ("reqbytes=%d\n" % total).encode(), "text/plain")
        return send_body(conn, req, 200,
                         ("reqbytes=%d\n" % total).encode(), "text/plain")

    if path.startswith("/longredirect/"):
        n = int(path[14:])
        return send_body(conn, req, 302, b"go somewhere long\n", "text/plain",
                         extra=[("Location", "/qlen?q=" + "q" * n)])

    if path == "/qlen":
        return send_body(conn, req, 200,
                         ("qlen=%d\n" % len(req.query.get("q", ""))).encode(),
                         "text/plain")

    if path == "/gone":
        return send_body(conn, req, 410, b"gone\n", "text/plain")

    return send_body(conn, req, 404, b"no such endpoint\n", "text/plain")


ACCEPTED = {}
OPEN = {}
COUNT_LOCK = threading.Lock()


def serve_http_conn(sock, tag, server, peer):
    conn = Conn(sock, tag)
    served = 0

    # One line per CONNECTION even when the requests on it are quiet.  This is
    # not decoration: it is the measurement that told the 40-way parallel
    # failure apart from every other explanation.  The host had ACCEPTED 213
    # connections while only 145 transfers completed on the Amiga, so the SYN
    # was reaching the peer and the answer was not getting back, which is a
    # very different bug from "the guest never dialled".
    with COUNT_LOCK:
        ACCEPTED[tag] = ACCEPTED.get(tag, 0) + 1
        OPEN[tag] = OPEN.get(tag, 0) + 1
        log(tag, "connect from %s:%d (accepted %d, %d open)"
            % (peer[0], peer[1], ACCEPTED[tag], OPEN[tag]))

    try:
        try:
            while True:
                req = read_request(conn)
                served += 1
                if not req.path.startswith(QUIET_PATHS):
                    log(tag, "%s %s (req %d on this connection)"
                        % (req.method, req.target, served))
                close = handle_request(conn, req, server)
                if close or req.header("connection", "").lower() == "close" \
                        or req.version == "HTTP/1.0":
                    break
        except Closed as exc:
            if exc.reset:
                conn.reset()
                return
        except (OSError, ValueError, ssl.SSLError) as exc:
            log(tag, "connection from %s ended: %s" % (peer, exc))
        conn.close()
    finally:
        with COUNT_LOCK:
            OPEN[tag] = OPEN.get(tag, 1) - 1


# --------------------------------------------------------------- listeners --


class Listener(threading.Thread):
    def __init__(self, bind, port, tag, handler, stop, tls=None):
        super().__init__(daemon=True)
        self.bind = bind
        self.port = port
        self.tag = tag
        self.handler = handler
        self.stop = stop
        self.tls = tls
        self.advertise = "10.0.2.2"
        self.base = 7100
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((bind, port))
        self.sock.listen(128)
        self.sock.settimeout(0.5)

    def run(self):
        while not self.stop.is_set():
            try:
                raw, peer = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._one, args=(raw, peer),
                             daemon=True).start()
        self.sock.close()

    def _one(self, raw, peer):
        accepted_at = time.time()
        log(self.tag, "accept from %s:%d" % (peer[0], peer[1]))
        try:
            # The listener carries a 0.5 s timeout so run() can poll the stop
            # flag, and whether an accepted socket inherits it has changed
            # across CPython versions.  Set it here rather than depend on that:
            # a 14 MHz 68020 needs tens of seconds for the public-key
            # arithmetic in a handshake, so a server that gives up early
            # measures its own patience instead of the client.
            raw.settimeout(float(os.environ.get("AMINETXDUO_PEER_HS_TIMEOUT",
                                                "300")))
            if self.tls is not None:
                # MSG_PEEK, so the handshake below still sees these bytes.  A
                # TLS record header is type[1] version[2] length[2], and a
                # ClientHello is type 0x16 with 0x0301 on the record layer
                # whatever version it really wants; an empty peek means the
                # client connected and sent nothing at all, which is a very
                # different failure from a rejected hello.
                if os.environ.get("AMINETXDUO_PEER_PEEK") == "1":
                    try:
                        head = raw.recv(48, socket.MSG_PEEK)
                        log(self.tag, "first %d bytes: %s"
                            % (len(head), head[:48].hex()))
                    except OSError as exc:
                        log(self.tag, "peek failed: %s" % exc)
                try:
                    raw = self.tls.wrap_socket(raw, server_side=True)
                except (ssl.SSLError, OSError) as exc:
                    log(self.tag, "TLS handshake from %s:%d failed"
                        " after %.1fs: %s"
                        % (peer[0], peer[1],
                           time.time() - accepted_at, exc))
                    try:
                        raw.close()
                    except OSError:
                        pass
                    return
                log(self.tag, "TLS up: %s %s"
                    % (raw.version(), raw.cipher()[0] if raw.cipher() else "?"))
            self.handler(raw, self.tag, self, peer)
        except Exception as exc:                        # noqa: BLE001
            log(self.tag, "handler died: %r" % (exc,))


def raw_empty(sock, tag, server, peer):
    """Accept, read whatever arrives, then close having sent nothing."""
    try:
        sock.settimeout(5)
        sock.recv(65536)
    except OSError:
        pass
    log(tag, "empty reply to %s:%d" % peer[:2])
    sock.close()


def raw_reset(sock, tag, server, peer):
    """Accept and RESET without reading anything."""
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                        struct.pack("ii", 1, 0))
    except OSError:
        pass
    log(tag, "reset on %s:%d" % peer[:2])
    sock.close()


HELD = []


def raw_never(sock, tag, server, peer):
    """Accept and say nothing, forever.  The socket must stay referenced."""
    log(tag, "holding %s:%d open with no reply" % peer[:2])
    HELD.append(sock)
    while len(HELD) > 32:
        try:
            HELD.pop(0).close()
        except OSError:
            pass


def raw_garbage(sock, tag, server, peer):
    """Answer with something that is not a status line."""
    try:
        sock.settimeout(5)
        sock.recv(65536)
        sock.sendall(b"\x00\x01\x02 this is not HTTP at all \xff\xfe\r\n" * 4)
    except OSError:
        pass
    log(tag, "garbage to %s:%d" % peer[:2])
    sock.close()


# --------------------------------------------------------------------- TLS --


def tls_context(certchain, key):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    # Pinned to 1.2 by default so a host OpenSSL that prefers 1.3 cannot turn a
    # stack test into a version negotiation.  AMINETXDUO_PEER_TLS13=1 raises the
    # ceiling, which is the only way to exercise the TLS 1.3 path against a
    # chain short enough to verify at 14 MHz.
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    if os.environ.get("AMINETXDUO_PEER_TLS13") == "1":
        ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    else:
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    # Every one of these is in src/tls/ami_tls_crypto.c's table.  Naming them
    # keeps a host OpenSSL upgrade from picking something the Amiga cannot do
    # and turning a stack test into a negotiation failure.
    ctx.set_ciphers("ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
                    "ECDHE-ECDSA-AES128-SHA256:ECDHE-RSA-AES128-SHA256:"
                    "AES128-GCM-SHA256:AES256-SHA256:AES128-SHA256:@SECLEVEL=0")
    ctx.set_ecdh_curve("prime256v1")
    ctx.load_cert_chain(certchain, key)
    return ctx


TLS_SERVERS = (
    # offset, name,          chain file,         key file
    (1, "rsa2",       "leaf-rsa2.chain.pem",       "leaf-rsa2.key.pem"),
    (2, "rsa3",       "leaf-rsa3.chain.pem",       "leaf-rsa3.key.pem"),
    (3, "rsa4",       "leaf-rsa4.chain.pem",       "leaf-rsa4.key.pem"),
    (4, "ec2",        "leaf-ec2.chain.pem",        "leaf-ec2.key.pem"),
    (5, "ec3",        "leaf-ec3.chain.pem",        "leaf-ec3.key.pem"),
    (6, "expired",    "leaf-expired.chain.pem",    "leaf-expired.key.pem"),
    (7, "selfsigned", "leaf-selfsigned.chain.pem", "leaf-selfsigned.key.pem"),
)


# -------------------------------------------------------------------- main --


def main():
    global LOG_FILE

    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--base-port", type=int, default=7100)
    ap.add_argument("--advertise", default="10.0.2.2",
                    help="what the guest calls this host: goes in the PASV "
                         "reply and in redirect Locations")
    ap.add_argument("--pki", help="directory of tests/peer/mkpki.sh output; "
                                  "without it the https ports do not open")
    ap.add_argument("--log")
    ap.add_argument("--seconds", type=int, default=3600)
    ap.add_argument("--ftp-blob", type=int, default=131072,
                    help="size of the binary file the FTP server offers")
    args = ap.parse_args()

    if args.log:
        LOG_FILE = open(args.log, "w")

    base = args.base_port
    stop = threading.Event()
    listeners = []

    def add(offset, tag, handler, tls=None):
        lis = Listener(args.bind, base + offset, tag, handler, stop, tls)
        lis.advertise = args.advertise
        lis.base = base
        lis.start()
        listeners.append(lis)
        log("start", "%-11s on %s:%d" % (tag, args.bind, base + offset))

    add(0, "http", serve_http_conn)

    if args.pki:
        missing = []
        for offset, name, chain, key in TLS_SERVERS:
            cpath = os.path.join(args.pki, chain)
            kpath = os.path.join(args.pki, key)
            if not (os.path.exists(cpath) and os.path.exists(kpath)):
                missing.append(name)
                continue
            add(offset, "https-" + name, serve_http_conn,
                tls=tls_context(cpath, kpath))
        if missing:
            log("start", "!! no certificates for: %s" % ", ".join(missing))
    else:
        log("start", "no --pki: the https ports are NOT open")

    add(20, "empty", raw_empty)
    add(21, "rst", raw_reset)
    add(22, "never", raw_never)
    add(23, "garbage", raw_garbage)

    # FTP, out of tests/tools/netpeer.py.  Its in-memory file set gains one
    # deterministic blob so a download can be hash-checked like every other.
    # The ftp removal took FtpHandler with it, so this is skipped rather than
    # fatal, without the guard every caller of this peer, http included,
    # dies here on an AttributeError before it serves a byte.
    ftp_up = 0
    if hasattr(netpeer, "FtpHandler"):
        netpeer.FILES["blob.bin"] = master(args.ftp_blob)
        netpeer.FILES["big.bin"] = master(512 * 1024)
        ftp = netpeer.Threaded((args.bind, base + 10), netpeer.FtpHandler)
        ftp.advertise = args.advertise
        ftp.active_via_loopback = True
        threading.Thread(target=ftp.serve_forever, daemon=True).start()
        ftp_up = 1
        log("start", "%-11s on %s:%d (blob.bin %d B, big.bin %d B)"
            % ("ftp", args.bind, base + 10, args.ftp_blob, 512 * 1024))
    else:
        log("start", "netpeer has no FtpHandler: the ftp port is NOT open")

    log("start", "pid %d, %d listeners, nothing on %d (refused case)"
        % (os.getpid(), len(listeners) + ftp_up, base + 99))
    print("ready", flush=True)

    try:
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            time.sleep(1)
    except KeyboardInterrupt:
        pass

    stop.set()
    log("stop", "shutting down")
    return 0


if __name__ == "__main__":
    sys.exit(main())
