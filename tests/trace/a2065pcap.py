#!/usr/bin/env python3
"""Turn FS-UAE's A2065 log into a pcap file.

WHAT THIS IS AND WHY IT IS THE HOST-SIDE VIEW

FS-UAE 3.2.35 has no packet-dump option: there is no such switch on the
command line, no ``uae_*_pcap`` key in the binary, and libpcap is not linked.
The only ``pcap`` strings in the executable are three Windows-only winpcap
failure messages.

It does not need one.  The emulated Commodore A2065 writes every frame it
handles, in both directions, complete, as hex into ``<base_dir>/Cache/Logs/
fs-uae.log.txt``, unconditionally whenever ``network_card = a2065``, with no
option to request it and none to suppress it.  That output is produced inside
the emulated hardware, below every line of AmiNetXDuo, so it is independent of
the guest-side capture by construction: a frame that appears here and not in
the guest's own pcap was lost between the card and NetX Duo, which is exactly
the shape of the AMI_SANA2_RX_DEPTH_IPV4 defect.

WHAT IT CANNOT TELL YOU

  * There are NO TIMESTAMPS.  Not "low resolution", absent.  The records
    below are stamped with a synthetic monotonic counter so that ORDER is
    preserved and Wireshark will open the file; every interval in it is a
    fiction.  Take every timing from the guest pcap, which has real
    GetSysTime() microseconds, and take loss and ordering from this one.
  * The ``SRC:`` field of the ``A2065<-``/``A2065<*`` header lines is wrong --
    the emulator prints bytes from the wrong offset.  This parser ignores the
    header lines entirely and reads only the hex dumps, which are right.

FRAME PAIRING

Each frame is dumped once as a ``pre:``/``post:`` pair.  ``post:`` is what
crossed; in every sample examined the two are identical.  Direction is decided
from the Ethernet source address against the guest's MAC (announced by the
``A2065: 'slirp' xx:xx:...`` line, and confirmed by the initialisation block),
NOT from the surrounding header lines, because the pairing between a dump and
the header above or below it is not consistent between the two directions.

SPDX-License-Identifier: MIT
"""

import argparse
import re
import struct
import sys

PRE_RE = re.compile(r"^(pre|post):\s*(\d+):\s*(.*)$")

# WinUAE's own A2065 dump, from -a2065log2 (tools/winuae-run.sh brings the log
# back).  `7990*>` is a frame that crossed outbound and `7990<*` one the card
# accepted; the `->` and `<!` lines are the same frames one step earlier, so
# taking only the starred pair counts each frame once.  Every header line is
# followed by the frame as one unbroken hex string.
WU_RE = re.compile(r"7990(\*>|<\*)DST:.*\sS=(\d+)")
MAC_RE = re.compile(r"^A2065:\s*'\w+'\s*([0-9A-Fa-f:]{17})")
INIT_RE = re.compile(r"^A2065:\s.*\s([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\s*$")

PCAP_MAGIC = 0xA1B2C3D4
DLT_EN10MB = 1


def parse_hex(text):
    """'.FF.FF.00' -> bytes. Trailing dots and line noise are dropped."""
    out = bytearray()
    for part in text.split("."):
        part = part.strip()
        if len(part) != 2:
            continue
        try:
            out.append(int(part, 16))
        except ValueError:
            break
    return bytes(out)


def read_winuae_frames(path):
    """[(frame_bytes, wire_length), ...] from a WinUAE log."""
    frames = []
    want = None

    with open(path, "r", encoding="latin-1", errors="replace") as fh:
        for line in fh:
            m = WU_RE.search(line)
            if m:
                want = int(m.group(2))
                continue
            if want is None:
                continue
            hexpart = line.rsplit(":", 1)[-1].strip()
            if re.fullmatch(r"[0-9A-Fa-f]{28,}", hexpart):
                frames.append((bytes.fromhex(hexpart), want))
            want = None

    return frames, set()


def read_frames(path):
    """[(frame_bytes, guest_mac_seen), ...] in log order."""
    frames = []
    guest_macs = set()
    pending = None          # (length, bytes) from the last `pre:`

    with open(path, "r", encoding="latin-1", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")

            m = MAC_RE.match(line)
            if m:
                guest_macs.add(m.group(1).lower())
                continue

            m = INIT_RE.match(line)
            if m:
                guest_macs.add(m.group(1).lower())
                continue

            m = PRE_RE.match(line)
            if not m:
                continue

            kind, length, hexpart = m.group(1), int(m.group(2)), m.group(3)
            data = parse_hex(hexpart)

            if kind == "pre":
                pending = (length, data)
                continue

            # `post:` closes the pair. Prefer it: it is what crossed.
            chosen = data if data else (pending[1] if pending else b"")
            if chosen:
                frames.append((chosen, length))
            pending = None

    return frames, guest_macs


def macstr(raw):
    return ":".join("%02x" % b for b in raw)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", help="fs-uae.log.txt, or a WinUAE log with --winuae")
    ap.add_argument("--winuae", action="store_true",
                    help="read WinUAE's -a2065log2 dump instead of FS-UAE's")
    ap.add_argument("-o", "--out", required=True, help="pcap to write")
    ap.add_argument("--snap", type=int, default=0,
                    help="truncate each frame to this many bytes (0 = whole)")
    args = ap.parse_args()

    if args.winuae:
        frames, guest_macs = read_winuae_frames(args.log)
    else:
        frames, guest_macs = read_frames(args.log)

    if not frames:
        print("a2065pcap: no A2065 frame dumps in %s" % args.log)
        if args.winuae:
            print("           (did the run set AMINETXDUO_WINUAE_ARGS=-a2065log2 ?)")
        else:
            print("           (was the run made with tools/fsuae-run.sh -n ?)")
        return 1

    # The MAC the A2065 announces has zeroes where the initialisation block
    # has the real bytes, so trust whichever appears as a source address.
    src_counts = {}
    for data, _ in frames:
        if len(data) >= 12:
            src_counts[macstr(data[6:12])] = src_counts.get(macstr(data[6:12]), 0) + 1

    guest = None
    for mac in guest_macs:
        if mac in src_counts:
            guest = mac
            break
    if guest is None and src_counts:
        # Fall back to the most frequent source that is not SLIRP's 52:55:...
        cands = [(n, m) for m, n in src_counts.items() if not m.startswith("52:55")]
        if cands:
            guest = max(cands)[1]

    tx = rx = 0
    with open(args.out, "wb") as out:
        out.write(struct.pack(">IHHiIII", PCAP_MAGIC, 2, 4, 0, 0,
                              args.snap or 65535, DLT_EN10MB))
        for i, (data, wirelen) in enumerate(frames):
            if len(data) >= 12:
                if macstr(data[6:12]) == guest:
                    tx += 1
                else:
                    rx += 1
            body = data[:args.snap] if args.snap else data
            # Synthetic clock: one microsecond per frame, ORDER ONLY.
            out.write(struct.pack(">IIII", 0, i, len(body), wirelen))
            out.write(body)

    print("a2065pcap: %d frames -> %s" % (len(frames), args.out))
    print("           guest MAC %s: %d out, %d in" % (guest, tx, rx))
    print("           TIMESTAMPS ARE SYNTHETIC, order only, never intervals")
    return 0


if __name__ == "__main__":
    sys.exit(main())
