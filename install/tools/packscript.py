#!/usr/bin/env python3
"""Remove Installer comments/indentation without changing tokens or line numbers.

The Installer retains its input while parsing; avoid spending scarce RAM on
source comments. Keep the readable original in the repository.
SPDX-License-Identifier: MIT
"""

import sys
from checkscript import tokenise


def compact(source):
    output = []
    quote = None
    escaped = False
    comment = False
    for c in source:
        if comment:
            if c == "\n":
                comment = False
                output.append(c)
        elif quote:
            output.append(c)
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            output.append(c)
        elif c == ";":
            comment = True
        else:
            output.append(c)
    packed = "\n".join(line.strip() for line in "".join(output).split("\n"))
    errors = []
    original = tokenise(source, "source", errors)
    result = tokenise(packed, "packed", errors)
    if errors or original != result:
        raise ValueError("Installer packing changed tokens/line numbers or invalid source")
    return packed


if __name__ == "__main__":
    sys.stdout.write(compact(sys.stdin.read()))
