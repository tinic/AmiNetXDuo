#!/usr/bin/env python3
"""A deliberately rude TLS peer, for the four ways a handshake can end badly.

One listener per behaviour, so a single Amiga-side run can walk all of them
without the host having to be told anything between commands:

    4443  rst      read the ClientHello, then RST (SO_LINGER 0)
    4444  fin      read the ClientHello, then an orderly close
    4445  hang     read the ClientHello and answer nothing, ever
    4446  garbage  read the ClientHello, answer with bytes that are not TLS

The point is not to be a TLS server.  It is to reach the state a real server
reaches when it gives up on a slow client -- which is what a 14 MHz 68020
looks like to anything on the modern internet -- without waiting for a real
server to time out, and to do it deterministically.

Binds 127.0.0.1 on purpose: fs-uae's SLIRP maps the guest's 10.0.2.2 to the
host loopback, so that is the whole reachable surface, and it keeps macOS from
asking about incoming connections.

SPDX-License-Identifier: MIT
"""

import socket
import struct
import sys
import threading

BEHAVIOURS = (
    (4443, "rst"),
    (4444, "fin"),
    (4445, "hang"),
    (4446, "garbage"),
)

# Anything the guest sends before we act.  A ClientHello from tls.library is
# about 128 bytes; we only need to know it arrived.
READ_MAX = 4096


def serve(port, behaviour, stop):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(4)
    listener.settimeout(0.5)

    held = []           # 'hang' must not close, so the sockets stay referenced

    while not stop.is_set():
        try:
            conn, peer = listener.accept()
        except socket.timeout:
            continue
        except OSError:
            break

        try:
            conn.settimeout(10.0)
            try:
                first = conn.recv(READ_MAX)
            except (socket.timeout, OSError):
                first = b""

            print("%s: %d bytes from %s:%d" % (behaviour, len(first),
                                               peer[0], peer[1]),
                  flush=True)

            if behaviour == "rst":
                # linger 0 turns close() into a reset rather than a FIN.
                conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                struct.pack("ii", 1, 0))
                conn.close()
            elif behaviour == "fin":
                conn.shutdown(socket.SHUT_WR)
                conn.close()
            elif behaviour == "garbage":
                # A plausible-looking record header over nonsense, so the
                # failure happens in the record parser and not in the socket.
                conn.sendall(b"\x16\x03\x03\x00\x20" + bytes(range(32)))
                conn.close()
            elif behaviour == "hang":
                held.append(conn)
                if len(held) > 8:
                    held.pop(0).close()
        except OSError:
            pass

    for conn in held:
        try:
            conn.close()
        except OSError:
            pass
    listener.close()


def main():
    stop = threading.Event()
    threads = []

    for port, behaviour in BEHAVIOURS:
        t = threading.Thread(target=serve, args=(port, behaviour, stop),
                             daemon=True)
        t.start()
        threads.append(t)

    print("ready", flush=True)

    try:
        while True:
            line = sys.stdin.readline()
            if line == "" or line.strip() == "quit":
                break
    except KeyboardInterrupt:
        pass

    stop.set()
    for t in threads:
        t.join(timeout=2.0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
