"""Print `<hexoffset> <hexlength>` for a symbol's section in a link map.

A hand-copied WATCH offset goes stale the moment anything above it in the link
changes size, and the window then reports whatever now occupies the address --
which is exactly how a run got aimed at the wrong function.

    python3 watchoff.py <map> <symbol>
"""
import re
import sys

SEC = re.compile(r"^\s\.text\S*\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s")
SYM = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+(\S+)\s*$")


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: watchoff.py <map> <symbol>")

    mapfile, want = sys.argv[1], sys.argv[2]
    base = size = None

    with open(mapfile, errors="replace") as fh:
        for line in fh:
            m = SEC.match(line)
            if m:
                base, size = int(m.group(1), 16), int(m.group(2), 16)
                continue
            m = SYM.match(line)
            if m and m.group(2) == want:
                if base is None:
                    sys.exit("%s: no section line before %s" % (mapfile, want))
                # The symbol's own address, and what is left of its section.
                at = int(m.group(1), 16)
                if not (base <= at < base + size):
                    sys.exit("%s: %s at %x is outside its section %x+%x"
                             % (mapfile, want, at, base, size))
                print("%x %x" % (at, base + size - at))
                return

    sys.exit("%s: no symbol %s" % (mapfile, want))


main()
