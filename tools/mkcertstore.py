#!/usr/bin/env python3
"""Build DEVS:Internet/certificates from a PEM CA bundle.

    tools/mkcertstore.py [--output FILE] cacert.pem [more.pem ...]

The output is the indexed binary tls.library reads.  See the header comment in
src/tlslib/tls_store.c for why it is indexed rather than a PEM bundle: the
Mozilla root set is ~120 certificates and ~125 KB, the machine has four
megabytes, and a chain needs exactly one of them.  Only the 12-byte-per-root
index is ever resident; the one root a chain asks for is read from the file
during the handshake.

    0   'A' 'C' 'S' '1'
    4   ULONG count
    8   ULONG index_offset      always 16
   12   ULONG data_offset
   16   count x { ULONG key, ULONG offset, ULONG length }, sorted by key
  ...   the DER blobs, concatenated

Everything is big-endian, which is the 68000's own order, so the Amiga side
byte-swaps nothing.

THE KEY

    FNV-1a 32 over the certificate's SUBJECT Name -- the DER SEQUENCE
    including its tag and length bytes, not the parsed fields.  tls.library
    computes the same hash over a certificate's ISSUER Name, so a lookup is
    exactly the RFC 5280 rule "issuer matches subject when the encoded Names
    are equal", and neither side has to agree with the other about what an
    attribute means.

    Duplicate keys are dropped with a warning: two roots with identical
    subject Names are indistinguishable to any correct implementation, so
    keeping both would only make the choice arbitrary.

UPDATING

    Fetch a current bundle and re-run:

        curl -o cacert.pem https://curl.se/ca/cacert.pem
        tools/mkcertstore.py --output certificates cacert.pem
        copy certificates DEVS:Internet/certificates

    tls.library re-reads the index when the file's size or datestamp changes,
    so the replacement takes effect on the next connection.

This script has no dependencies beyond the standard library on purpose -- it
has to run in CI, on a developer's machine and on whatever a packager has.
The DER walking below is about sixty lines and is the entire reason no
cryptography package is needed.

SPDX-License-Identifier: MIT
"""

import argparse
import base64
import re
import struct
import sys

MAGIC = b"ACS1"
HEADER_SIZE = 16
ENTRY_SIZE = 12

# Must match TLS_ROOT_DER_MAX in src/tlslib/tls_internal.h.  A root bigger than
# the buffer that reads it would be indexed and then silently unusable, so this
# is an error rather than a warning.
MAX_DER = 2560

FNV_OFFSET = 2166136261
FNV_PRIME = 16777619
MASK32 = 0xFFFFFFFF


def fnv1a(data):
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & MASK32
    # Zero is "no key" on the Amiga side; fold it exactly as tls_store.c does.
    return h if h != 0 else FNV_PRIME


# --------------------------------------------------------------------- DER --

def tlv(buf, offset):
    """(tag, header_length, content_length) of the TLV at `offset`."""
    if offset + 2 > len(buf):
        raise ValueError("truncated DER")

    tag = buf[offset]
    if tag & 0x1F == 0x1F:
        raise ValueError("multi-byte tags are not supported")

    length = buf[offset + 1]
    if length & 0x80:
        n = length & 0x7F
        if n == 0 or n > 4:
            raise ValueError("indefinite or oversized length")
        length = int.from_bytes(buf[offset + 2:offset + 2 + n], "big")
        header = 2 + n
    else:
        header = 2

    if offset + header + length > len(buf):
        raise ValueError("TLV runs past the end of the certificate")

    return tag, header, length


def subject_name_der(der):
    """The subject Name TLV of a DER certificate, tag and length included.

    Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signature }
    TBSCertificate ::= SEQUENCE {
        version [0] EXPLICIT DEFAULT v1,
        serialNumber, signature, issuer, validity, subject, ... }

    The same walk src/tlslib/tls_store.c does for the issuer, one field later.
    """
    tag, header, length = tlv(der, 0)
    if tag != 0x30:
        raise ValueError("not a certificate: outer tag %#x" % tag)

    # into Certificate
    pos = header
    tag, header, length = tlv(der, pos)
    if tag != 0x30:
        raise ValueError("no TBSCertificate")

    pos += header                       # into TBSCertificate
    end = pos + length

    # [0] EXPLICIT version, optional
    tag, header, length = tlv(der, pos)
    if tag == 0xA0:
        pos += header + length

    for _ in range(4):                  # serial, signature, issuer, validity
        tag, header, length = tlv(der, pos)
        pos += header + length
        if pos > end:
            raise ValueError("TBSCertificate walk overran")

    tag, header, length = tlv(der, pos)
    if tag != 0x30:
        raise ValueError("subject is not a SEQUENCE (tag %#x)" % tag)

    return der[pos:pos + header + length]


# --------------------------------------------------------------------- PEM --

PEM_RE = re.compile(
    rb"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", re.S)


def read_pem(path):
    with open(path, "rb") as fh:
        blob = fh.read()

    out = []
    for m in PEM_RE.finditer(blob):
        body = re.sub(rb"\s+", b"", m.group(1))
        out.append(base64.b64decode(body))
    return out


# -------------------------------------------------------------------- main --

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bundle", nargs="+", help="PEM file(s)")
    ap.add_argument("--output", "-o", default="certificates")
    ap.add_argument("--quiet", "-q", action="store_true")
    args = ap.parse_args()

    ders = []
    for path in args.bundle:
        found = read_pem(path)
        if not found:
            print("%s: no certificates found" % path, file=sys.stderr)
            return 2
        ders.extend(found)

    entries = {}
    skipped_dup = 0
    skipped_big = 0
    skipped_bad = 0

    for der in ders:
        if len(der) > MAX_DER:
            skipped_big += 1
            continue
        try:
            key = fnv1a(subject_name_der(der))
        except ValueError as exc:
            if not args.quiet:
                print("skipping a certificate: %s" % exc, file=sys.stderr)
            skipped_bad += 1
            continue

        if key in entries:
            if entries[key] != der:
                skipped_dup += 1
            continue

        entries[key] = der

    if not entries:
        print("nothing to write", file=sys.stderr)
        return 2

    keys = sorted(entries)
    data_offset = HEADER_SIZE + len(keys) * ENTRY_SIZE

    index = bytearray()
    blobs = bytearray()
    offset = data_offset
    for key in keys:
        der = entries[key]
        index += struct.pack(">III", key, offset, len(der))
        blobs += der
        offset += len(der)

    with open(args.output, "wb") as fh:
        fh.write(MAGIC)
        fh.write(struct.pack(">III", len(keys), HEADER_SIZE, data_offset))
        fh.write(index)
        fh.write(blobs)

    total = data_offset + len(blobs)
    if not args.quiet:
        print("%s: %d roots, %d bytes (%d index, %d certificates)"
              % (args.output, len(keys), total, len(index), len(blobs)))
        if skipped_big:
            print("  %d skipped: larger than TLS_ROOT_DER_MAX (%d bytes)"
                  % (skipped_big, MAX_DER), file=sys.stderr)
        if skipped_dup:
            print("  %d skipped: duplicate subject name" % skipped_dup,
                  file=sys.stderr)
        if skipped_bad:
            print("  %d skipped: unparseable" % skipped_bad, file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
