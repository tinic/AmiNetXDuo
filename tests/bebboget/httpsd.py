#!/usr/bin/env python3
"""An HTTPS server on the build host for bebboget and src/tools/fetch to pull from.

    tests/bebboget/httpsd.py --dir DIR --port 8443 --cert C --key K [--tls12]

WHY A LOCAL SERVER AND NOT A REAL URL

A test that reaches the public internet fails when the network is down, when a
CDN changes its cipher preference, and when somebody's front end decides a
14 MHz 68020 has taken too long over a handshake -- which it does; that last
one is a known property of this machine (docs/RESEARCH.md 11.8) and it would
turn a throughput test into a flake.  A server on 10.0.2.2 removes all three
and leaves the two things being measured: the TLS stack and the transport.

WHY NOT `openssl s_server -WWW`

It answers HTTP/1.0 with no Content-Length, so a client cannot tell a finished
body from a truncated one, and this test's whole point is comparing bytes.

WHAT IT SERVES

Only files that already exist in --dir, read-only, with Content-Length and no
keep-alive.  It listens on localhost only by default; --bind 0.0.0.0 is needed
for the emulated guest, which reaches the host as 10.0.2.2.

--tls12 pins the server to TLS 1.2 and --suite to one ciphersuite.  bebboget
speaks 1.0 through 1.3 and our fetch is a separate stack again, so pinning both
is how the comparison stops being "whose preferred algorithm got picked".

SPDX-License-Identifier: MIT
"""

import argparse
import http.server
import os
import ssl
import sys


class Handler(http.server.BaseHTTPRequestHandler):
    # HTTP/1.0 with an explicit Content-Length and a close: src/tools/fetch
    # sends HTTP/1.0 with Connection: close and does no chunked decoding, so
    # anything fancier here would be testing the server.
    protocol_version = "HTTP/1.0"
    root = "."

    def log_message(self, fmt, *args):
        # The negotiated version and cipher, per request.  Without it a
        # comparison of two TLS stacks is a comparison of whatever each of them
        # happened to be given, and nobody can tell afterwards.
        try:
            ver, cipher = self.connection.version(), self.connection.cipher()[0]
        except Exception:
            ver, cipher = "?", "?"
        sys.stderr.write("%s %s %s %s\n"
                         % (self.address_string(), ver, cipher, fmt % args))

    def do_GET(self):
        # No traversal: the request is reduced to a basename and looked up in
        # one directory.  It serves payloads to an emulator, but it is still a
        # listening socket on somebody's machine.
        name = os.path.basename(self.path.split("?", 1)[0].lstrip("/"))
        path = os.path.join(self.root, name)
        if not name or not os.path.isfile(path):
            self.send_error(404, "no such file")
            return
        size = os.path.getsize(path)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Connection", "close")
        self.end_headers()
        with open(path, "rb") as f:
            while True:
                chunk = f.read(65536)
                if not chunk:
                    break
                self.wfile.write(chunk)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--cert", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--tls12", action="store_true")
    ap.add_argument("--suite", help="pin the TLS 1.2 ciphersuite, OpenSSL name")
    ap.add_argument("--pidfile")
    args = ap.parse_args()

    Handler.root = os.path.abspath(args.dir)

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    if args.tls12:
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    # bebboget offers RSA key exchange and RSA signatures; a modern default
    # security level refuses some of what it can do, so the level is lowered
    # rather than the test being narrowed to whatever OpenSSL feels like today.
    # --suite leaves the negotiation one outcome, which is how two different
    # TLS implementations get compared on the same algorithm rather than on
    # whichever each of them preferred.
    want = (args.suite + ":@SECLEVEL=1") if args.suite else "DEFAULT:@SECLEVEL=1"
    try:
        ctx.set_ciphers(want)
    except ssl.SSLError:
        sys.stderr.write("cannot pin ciphers to %s\n" % want)
        raise
    ctx.load_cert_chain(args.cert, args.key)

    srv = http.server.ThreadingHTTPServer((args.bind, args.port), Handler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)

    if args.pidfile:
        with open(args.pidfile, "w") as f:
            f.write(str(os.getpid()))

    sys.stderr.write("listening on %s:%d serving %s\n"
                     % (args.bind, args.port, Handler.root))
    sys.stderr.flush()
    srv.serve_forever()


if __name__ == "__main__":
    main()
