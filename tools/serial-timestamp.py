#!/usr/bin/env python3
"""Read the guest's serial port and write it out twice: verbatim, and stamped.

The verbatim copy is the one every harness already greps, and it stays
byte-for-byte what nc(1) used to append -- a line prefix there would break
anything anchored to the start of a line.

The stamped copy is what makes a latency visible at all. Guest output carries
no clock, so "AddNetInterface took three seconds" was not a thing the logs
could be asked: the boot path's cost was only ever inferred from a total. Each
line is stamped when its FIRST byte arrives, not its newline, so a line the
guest emits before a long wait is dated to the wait's start rather than its
end.

Times are host local time, to the millisecond, which is the same clock a pcap
taken on the host uses.

    serial-timestamp.py <host> <port> <raw-path> <stamped-path>

Exits non-zero without writing anything if the connection cannot be made, so a
caller can retry the way it retried nc.
"""
import datetime
import socket
import sys


def main(argv):
    if len(argv) != 5:
        sys.stderr.write("usage: serial-timestamp.py host port raw stamped\n")
        return 2

    host, port, raw_path, ts_path = argv[1], int(argv[2]), argv[3], argv[4]

    try:
        conn = socket.create_connection((host, port), timeout=10)
    except OSError:
        return 1

    # The emulator blocks until something connects, so from here on the run
    # depends on this process staying out of the way: no timeout on the read,
    # and both files appended to and flushed per chunk.
    conn.settimeout(None)

    with open(raw_path, "ab", buffering=0) as raw, \
            open(ts_path, "a", buffering=1) as stamped:

        pending = bytearray()
        stamp = None

        while True:
            try:
                chunk = conn.recv(4096)
            except OSError:
                break
            if not chunk:
                break

            raw.write(chunk)

            for byte in chunk:
                if not pending:
                    stamp = datetime.datetime.now()
                if byte == 0x0A:
                    write_line(stamped, stamp, pending)
                    pending = bytearray()
                else:
                    pending.append(byte)

        # Whatever the guest left unterminated -- a prompt, or the last thing
        # it managed before it stopped -- is worth as much as any whole line.
        if pending:
            write_line(stamped, stamp, pending)

    conn.close()
    return 0


def write_line(out, stamp, pending):
    text = bytes(pending).decode("latin-1").rstrip("\r")
    out.write("[%s] %s\n" % (stamp.strftime("%H:%M:%S.") +
                             "%03d" % (stamp.microsecond // 1000), text))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
