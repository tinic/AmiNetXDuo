#!/usr/bin/env python3
"""Host half of forbidrec's workload.  One key=value line per subcommand.

  loadhost.py churn HOST PORT SECS THREADS [get]   TCP connect/close (or a GET)
  loadhost.py udpin HOST SECS                       datagrams to random ports,
                                                    a new socket (port) each
  loadhost.py udpecho PORT COUNT SECS               echo on PORT..PORT+COUNT-1
  loadhost.py acceptclose PORT SECS                 accept and close at once
"""
import random
import select
import socket
import sys
import threading
import time


def churn(host, port, secs, threads, get):
    end = time.time() + secs
    stats = {"ok": 0, "fail": 0}
    lock = threading.Lock()

    def worker():
        ok = fail = 0
        while time.time() < end:
            s = socket.socket()
            s.settimeout(3)
            try:
                s.connect((host, port))
                if get:
                    s.sendall(b"GET /forbidrec-%d HTTP/1.0\r\n\r\n" % random.randrange(1 << 30))
                    while s.recv(4096):
                        pass
                ok += 1
            except OSError:
                fail += 1
            finally:
                s.close()
        with lock:
            stats["ok"] += ok
            stats["fail"] += fail

    ts = [threading.Thread(target=worker) for _ in range(threads)]
    t0 = time.time()
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    el = time.time() - t0
    print("loadhost mode=churn get=%d ok=%d fail=%d secs=%.0f per_s=%.0f"
          % (get, stats["ok"], stats["fail"], el, stats["ok"] / el))


def udpin(host, secs):
    end = time.time() + secs
    n = 0
    while time.time() < end:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.sendto(b"forbidrec", (host, random.randrange(30000, 60000)))
            n += 1
        except OSError:
            pass
        s.close()
        time.sleep(0.002)
    print("loadhost mode=udpin sent=%d secs=%d per_s=%d" % (n, secs, n // secs))


def udpecho(port, count, secs):
    socks = []
    for p in range(port, port + count):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("0.0.0.0", p))
        socks.append(s)
    end = time.time() + secs
    n = 0
    while time.time() < end:
        r, _, _ = select.select(socks, [], [], 1.0)
        for s in r:
            data, addr = s.recvfrom(2048)
            s.sendto(data, addr)
            n += 1
    print("loadhost mode=udpecho echoed=%d ports=%d" % (n, count))


def acceptclose(port, secs):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("0.0.0.0", port))
    ls.listen(64)
    ls.settimeout(1.0)
    end = time.time() + secs
    n = 0
    while time.time() < end:
        try:
            c, _ = ls.accept()
            c.close()
            n += 1
        except socket.timeout:
            pass
    print("loadhost mode=acceptclose accepted=%d" % n)


def main():
    a = sys.argv[1:]
    if a[0] == "churn":
        churn(a[1], int(a[2]), int(a[3]), int(a[4]), len(a) > 5 and a[5] == "get")
    elif a[0] == "udpin":
        udpin(a[1], int(a[2]))
    elif a[0] == "udpecho":
        udpecho(int(a[1]), int(a[2]), int(a[3]))
    elif a[0] == "acceptclose":
        acceptclose(int(a[1]), int(a[2]))
    else:
        sys.exit(2)


main()
