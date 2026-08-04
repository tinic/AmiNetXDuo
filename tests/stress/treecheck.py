#!/usr/bin/env python3
"""
Does the tree on the fileserver hold the same bytes as the tree on the guest's
disk?

    tests/stress/treecheck.py manifest DIR > tree.manifest
    tests/stress/treecheck.py check tree.manifest DIR

CONTENT, AS A MULTISET, NOT AS A PATH MAP.  The bytes make two hops before they
land, UAE's directory filesystem, which encodes host names AmigaDOS cannot
spell, and Fitz, which writes AmigaDOS names back onto a POSIX server, so the
path a file arrives under is not necessarily the path it left under, and a
path-keyed comparison reports encoding as corruption.  What must be identical
is the set of file contents and how many times each appears.

This is the weaker half of the check on purpose.  The strong half is
comparetree on the guest, which sees both ends as AmigaDOS sees them and
compares the datestamp, the protection bits and the comment as well.

SPDX-License-Identifier: MIT
"""

import collections
import hashlib
import os
import sys


def digests(top):
    counts = collections.Counter()
    total = 0
    for root, _, names in os.walk(top):
        for n in names:
            p = os.path.join(root, n)
            if not os.path.isfile(p) or os.path.islink(p):
                continue
            h = hashlib.sha256()
            size = 0
            with open(p, "rb") as fh:
                while True:
                    b = fh.read(1 << 20)
                    if not b:
                        break
                    size += len(b)
                    h.update(b)
            counts[h.hexdigest()] += 1
            total += size
    return counts, total


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2

    if sys.argv[1] == "manifest":
        counts, total = digests(sys.argv[2])
        print("# files %d bytes %d" % (sum(counts.values()), total))
        for d in sorted(counts):
            print("%s %d" % (d, counts[d]))
        return 0

    if sys.argv[1] == "check":
        want = collections.Counter()
        for line in open(sys.argv[2]):
            if line.startswith("#"):
                continue
            d, n = line.split()
            want[d] = int(n)
        got, total = digests(sys.argv[3])

        missing = sum((want - got).values())
        extra = sum((got - want).values())
        name = os.path.basename(sys.argv[3].rstrip("/"))

        if missing == 0 and extra == 0:
            print("%s: %d files, %d bytes, all content matches"
                  % (name, sum(got.values()), total))
            return 0
        print("%s: %d files, %d bytes -- %d missing, %d not in the source"
              % (name, sum(got.values()), total, missing, extra))
        return 1

    sys.stderr.write("unknown mode %s\n" % sys.argv[1])
    return 2


if __name__ == "__main__":
    sys.exit(main())
