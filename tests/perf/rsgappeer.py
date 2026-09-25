#!/usr/bin/env python3
"""The Linux end of docs/plans/roadshow-rx-gap.md: a fixed-length TCP source.

  rsgappeer.py serve --port P --bytes N [--door-port D] [--door-bytes B]
                     [--stall-from K] [--lifetime S] [--bind ADDR]
  rsgappeer.py http get URL        body to stdout; rc 0 on 200
  rsgappeer.py http put URL        body from stdin; prints http_status=N
  rsgappeer.py selftest

Every connection on --port is sent exactly N bytes out of one buffer built in
memory before the first accept, then the write side is shut and the socket is
held until the guest closes, so `ms` is accept-to-delivered.  --door-port is a
second listener with its own numbering (default 4096 bytes); a guest that
completes one has made a round trip this host saw, and `client=` is the
guest's address.  --stall-from K accepts data connection K and every later
one and never sends: the injected hang the watchdog must recover from.

stdout is key=value only, one line per event, flushed as it happens:

  rsgappeer event=start port=17810 bytes=2000000 door_port=17811 ...
  rsgappeer event=conn kind=data conn=1 client=192.168.1.50:1024 \\
            bytes=2000000 ms_send=3120 ms=3301 result=ok
  rsgappeer event=conn kind=data conn=17 client=... bytes=0 result=stalled
  rsgappeer event=done data=16 door=1 ok=17 stalled=1 failed=0

SPDX-License-Identifier: MIT
"""

import argparse
import os
import platform
import select
import signal
import socket
import sys
import threading
import time

LOCK = threading.Lock()
COUNTS = {"data": 0, "door": 0, "ok": 0, "stalled": 0, "failed": 0}
STOP = threading.Event()


def emit(**kv):
    line = "rsgappeer " + " ".join("%s=%s" % (k, v) for k, v in kv.items())
    with LOCK:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()


def make_payload(n):
    # Not constant: a byte landed at the wrong offset must not look right to
    # anyone who later hashes the guest's RAM: file.
    block = bytes((i * 7 + 3) & 0xFF for i in range(65536))
    reps, tail = divmod(n, len(block))
    return memoryview(block * reps + block[:tail])


def serve_one(conn, addr, kind, num, payload, stall):
    client = "%s:%d" % addr
    t0 = time.monotonic()
    if stall:
        with LOCK:
            COUNTS["stalled"] += 1
        emit(event="conn", kind=kind, conn=num, client=client, bytes=0,
             result="stalled")
        # Hold it open, silent, until the guest goes away or we are stopped.
        while not STOP.is_set():
            r, _, _ = select.select([conn], [], [], 1.0)
            if r:
                try:
                    if not conn.recv(4096):
                        break
                except OSError:
                    break
        conn.close()
        return
    sent = 0
    result = "ok"
    try:
        step = 65536
        while sent < len(payload):
            n = conn.send(payload[sent:sent + step])
            if n <= 0:
                break
            sent += n
        ms_send = int((time.monotonic() - t0) * 1000)
        conn.shutdown(socket.SHUT_WR)
        conn.settimeout(120)
        while conn.recv(4096):
            pass
    except OSError as e:
        result = "error_%s" % (e.errno or "x")
        ms_send = int((time.monotonic() - t0) * 1000)
    ms = int((time.monotonic() - t0) * 1000)
    conn.close()
    if sent != len(payload):
        result = "short" if result == "ok" else result
    with LOCK:
        COUNTS["ok" if result == "ok" else "failed"] += 1
    emit(event="conn", kind=kind, conn=num, client=client, bytes=sent,
         ms_send=ms_send, ms=ms, result=result)


def listener(bind, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # A previous session's peer can take a few seconds to let go of the port.
    for attempt in range(20):
        try:
            s.bind((bind, port))
            break
        except OSError:
            if attempt == 19:
                raise
            time.sleep(1)
    s.listen(8)
    return s


def serve(args):
    payload = make_payload(args.bytes)
    door_payload = make_payload(args.door_bytes)
    socks = {listener(args.bind, args.port): "data"}
    if args.door_port:
        socks[listener(args.bind, args.door_port)] = "door"
    emit(event="start", port=args.port, bytes=args.bytes,
         door_port=args.door_port or 0, door_bytes=args.door_bytes,
         stall_from=args.stall_from or 0, kernel=platform.release(),
         host=platform.node(), pid=os.getpid())
    signal.signal(signal.SIGTERM, lambda *_: STOP.set())
    deadline = time.monotonic() + args.lifetime
    threads = []
    try:
        while not STOP.is_set() and time.monotonic() < deadline:
            r, _, _ = select.select(list(socks), [], [], 0.5)
            for s in r:
                conn, addr = s.accept()
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                kind = socks[s]
                with LOCK:
                    COUNTS[kind] += 1
                    num = COUNTS[kind]
                stall = (kind == "data" and args.stall_from and
                         num >= args.stall_from)
                t = threading.Thread(target=serve_one, daemon=True,
                                     args=(conn, addr, kind, num,
                                           payload if kind == "data"
                                           else door_payload, stall))
                t.start()
                threads.append(t)
    except KeyboardInterrupt:
        pass
    STOP.set()
    for t in threads:
        t.join(5)
    emit(event="done", data=COUNTS["data"], door=COUNTS["door"],
         ok=COUNTS["ok"], stalled=COUNTS["stalled"], failed=COUNTS["failed"])
    return 0 if COUNTS["failed"] == 0 else 1


def selftest():
    """Loopback: two full transfers and one stall, counted on both ends."""
    import subprocess
    port = 0
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    p = subprocess.Popen([sys.executable, __file__, "serve", "--bind",
                          "127.0.0.1", "--port", str(port), "--bytes",
                          "300001", "--stall-from", "3", "--lifetime", "20"],
                         stdout=subprocess.PIPE, text=True)
    p.stdout.readline()                               # event=start
    got = []
    for _ in range(2):
        c = socket.create_connection(("127.0.0.1", port))
        n = 0
        while True:
            b = c.recv(65536)
            if not b:
                break
            n += len(b)
        c.close()
        got.append(n)
    c = socket.create_connection(("127.0.0.1", port))
    c.settimeout(1.0)
    try:
        stalled = c.recv(1) == b""
    except socket.timeout:
        stalled = True
    c.close()
    lines = [p.stdout.readline().strip() for _ in range(3)]
    p.terminate()
    tail = p.stdout.read()
    p.wait(10)
    ok = (got == [300001, 300001] and stalled and
          sum("result=ok" in l and "bytes=300001" in l for l in lines) == 2 and
          sum("result=stalled" in l for l in lines) == 1 and
          "event=done" in tail)
    print("rsgappeer_selftest=%s client_bytes=%s" %
          ("PASS" if ok else "FAIL", ",".join(map(str, got))))
    return 0 if ok else 1


def http(method, url):
    """The door, driven from this host: the emulator host cannot reach its
    own bridged guest, and on hardware the controller's LAN end is here too."""
    import urllib.error
    import urllib.request
    data = sys.stdin.buffer.read() if method == "put" else None
    req = urllib.request.Request(url, data=data, method=method.upper())
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            status, body = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, body = e.code, b""
    except OSError:
        status, body = 0, b""
    if method == "get":
        sys.stdout.buffer.write(body)
        return 0 if status == 200 else 1
    print("http_status=%d" % status)
    return 0 if 200 <= status < 300 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--bind", default="0.0.0.0")
    s.add_argument("--port", type=int, required=True)
    s.add_argument("--bytes", type=int, required=True)
    s.add_argument("--door-port", type=int, default=0)
    s.add_argument("--door-bytes", type=int, default=4096)
    s.add_argument("--stall-from", type=int, default=0)
    s.add_argument("--lifetime", type=float, default=3600)
    h = sub.add_parser("http")
    h.add_argument("method", choices=["get", "put"])
    h.add_argument("url")
    sub.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "http":
        return http(a.method, a.url)
    return serve(a) if a.cmd == "serve" else selftest()


if __name__ == "__main__":
    sys.exit(main())
