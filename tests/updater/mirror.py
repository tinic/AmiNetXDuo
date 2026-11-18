#!/usr/bin/env python3
"""The update mirror the scenario tests point UpdateAmiNetXDuo at.

    tests/updater/mirror.py --root DIR [--bind ADDR] [--port N] [--log FILE]

Plain HTTP, as the mirror for the no-TLS flavours is.  It runs on a PEER on
the guest's segment, not on the machine running the emulator: a bridged guest
cannot reach the host it runs on (tests/tls/run-hangup.sh has the why).

WHAT IT SERVES

    /t/<token>/<tree>/<channel>/<name>   DIR/<tree>/<channel>/<name>
    /t/<token>/probe                     "probe <token>\\n", for the harness

<token> names the scenario run, so the request log tells one run's traffic
from the next without restarting the server.  <tree> is one of the mirror
trees tests/updater/make-releases.sh writes (good, badsig, drop, ...).  Only
files that exist are served, HTTP/1.0 with a Content-Length and a close.

A tree holding a file named DROP cuts every *.lha body short: the headers
promise the whole file, half of it is sent, and the connection is closed.

THE LOG, one line per request, flushed:

    req=<n> token=<token> path=<tree/channel/name|probe> status=<code>
        bytes=<promised> sent=<sent>

and `ready port=<n>` once it listens.

SPDX-License-Identifier: MIT
"""

import argparse
import http.server
import os
import socket
import sys
import threading

LOCK = threading.Lock()
COUNT = [0]
LOG = [None]


def log(line):
    with LOCK:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()
        if LOG[0]:
            LOG[0].write(line + "\n")
            LOG[0].flush()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    root = "."

    def log_message(self, fmt, *args):
        pass

    def record(self, token, path, status, promised, sent):
        with LOCK:
            COUNT[0] += 1
            n = COUNT[0]
        log("req=%d token=%s path=%s status=%d bytes=%d sent=%d"
            % (n, token or "-", path or "-", status, promised, sent))

    def answer(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parts = self.path.split("?", 1)[0].split("/")
        # "", "t", token, rest...
        if len(parts) < 4 or parts[1] != "t" or not parts[2]:
            self.answer(404, b"no such path\n")
            self.record(None, self.path[:80], 404, 0, 0)
            return
        token = parts[2]
        rest = parts[3:]
        if rest == ["probe"]:
            body = ("probe %s\n" % token).encode()
            self.answer(200, body)
            self.record(token, "probe", 200, len(body), len(body))
            return
        # tree/channel/name, each a plain name: no traversal.
        if len(rest) != 3 or any(p in ("", ".", "..") or p.startswith(".")
                                 for p in rest):
            self.answer(404, b"no such file\n")
            self.record(token, "/".join(rest)[:80], 404, 0, 0)
            return
        rel = "/".join(rest)
        root = os.path.realpath(self.root)
        path = os.path.realpath(os.path.join(root, *rest))
        if not path.startswith(root + os.sep):
            self.answer(404, b"no such file\n")
            self.record(token, rel, 404, 0, 0)
            return
        if not os.path.isfile(path):
            self.answer(404, b"no such file\n")
            self.record(token, rel, 404, 0, 0)
            return
        size = os.path.getsize(path)
        limit = size
        if rest[2].endswith(".lha") and \
                os.path.exists(os.path.join(self.root, rest[0], "DROP")):
            limit = size // 2
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Connection", "close")
        self.end_headers()
        sent = 0
        try:
            with open(path, "rb") as f:
                while sent < limit:
                    chunk = f.read(min(16384, limit - sent))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    sent += len(chunk)
            if limit < size:
                self.wfile.flush()
                self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.record(token, rel, 200, size, sent)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8480)
    ap.add_argument("--log")
    args = ap.parse_args()

    Handler.root = os.path.abspath(args.root)
    if args.log:
        LOG[0] = open(args.log, "a")
    http.server.ThreadingHTTPServer.allow_reuse_address = True
    srv = http.server.ThreadingHTTPServer((args.bind, args.port), Handler)
    log("ready port=%d" % args.port)
    srv.serve_forever()


if __name__ == "__main__":
    main()
