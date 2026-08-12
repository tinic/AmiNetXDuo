#!/usr/bin/env python3
"""Strip top-level const from the register parameters of the NDK inlines.

    tools/fix-toolchain-inline-const.py <toolchain-root> [--check]

An inline in m68k-amigaos/ndk-include/inline/ passes its arguments through
the LPn macros in inline/macros.h, which bind each one to a register with a
local register variable:

    register t2 _n2 __asm("d2") = _v2;

When t2 is const-qualified at the top level, GCC drops that binding for an
argument whose value is a link-time constant and satisfies the "rf" operand
from wherever the allocator likes.  The library then reads the argument out
of a register nobody wrote.  Nothing warns.

    Write(out, (APTR)t_log_buffer, len)     const APTR   -> #buf into d0
    Read (out, (APTR)t_log_buffer, len)           APTR   -> #buf into d2

Only the qualifier differs; sfdc generated it from the SDK prototype, where
it is documentation.  The same shape reaches an address register: CopyMem
takes a const APTR source in a0 and gets #src in d1 instead, and TypeOfMem
takes one in a1 and gets it in d0.

Removing the qualifier from a parameter type changes nothing a caller can
observe -- it only stops the macro declaring a const object for the value on
its way to the register -- so this rewrites every such parameter in every
LPn in every inline header.  Top-level only: `const struct TagItem *` is a
pointer to const, the register variable is not const, and it is left alone.

Idempotent, and a tree with no top-level const left reports as clean.  This
has to run for the life of the pin: the pinned image is sfdc 1.11f output
and every image anybody installs carries the same headers.

SPDX-License-Identifier: MIT
"""

import pathlib
import re
import sys

# An LPn call, from the macro name to its matching close paren.  A depth
# counter over the text is enough here and does not need a C parser.
LP_START = re.compile(r"\bLP\d+[A-Z]*\s*\(")

# `, const IDENT ,` -- one identifier and then the comma, so a `*` anywhere
# between them takes it out of scope.  Line continuations put a backslash and
# a newline inside the whitespace, which \s covers.
TOP_CONST = re.compile(r"(,\s*)const\s+([A-Za-z_][A-Za-z0-9_]*)(\s*,)")


def lp_spans(text):
    """(start, end) of every LPn argument list in the file."""
    spans = []
    for m in LP_START.finditer(text):
        depth = 1
        i = m.end()
        while i < len(text) and depth:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        if depth == 0:
            spans.append((m.end(), i - 1))
    return spans


def rewrite(text):
    """The file with top-level const gone from LPn parameters, and a count."""
    out = []
    at = 0
    n = 0
    for start, end in lp_spans(text):
        if start < at:               # nested LPn, already covered
            continue
        body, hits = TOP_CONST.subn(r"\1\2\3", text[start:end])
        out.append(text[at:start])
        out.append(body)
        n += hits
        at = end
    out.append(text[at:])
    return "".join(out), n


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check_only = "--check" in sys.argv[1:]
    if len(args) != 1:
        sys.stderr.write("usage: fix-toolchain-inline-const.py "
                         "<toolchain-root> [--check]\n")
        return 2

    root = pathlib.Path(args[0])
    inc = root / "m68k-amigaos" / "ndk-include" / "inline"
    if not inc.is_dir():
        sys.stderr.write("fix-toolchain-inline-const: no %s\n" % inc)
        return 2

    # The headers are Latin-1 and a stray high byte in a comment must not stop
    # the rewrite, so they are read and written as Latin-1 throughout.
    headers = sorted(inc.glob("*.h"))
    if not headers:
        sys.stderr.write("fix-toolchain-inline-const: no headers in %s\n" % inc)
        return 2

    state = "buggy" if check_only else "patched"
    total = 0
    touched = 0
    for h in headers:
        text = h.read_text(encoding="latin-1")
        fixed, n = rewrite(text)
        if not n:
            continue
        total += n
        touched += 1
        print("  %-8s %s: %d parameter(s)" % (state, h.relative_to(root), n))
        if check_only:
            continue
        # Written through a temporary in the same directory and renamed, so a
        # compile running against this toolchain reads one version or the
        # other and never a half-written header.
        tmp = h.with_suffix(".h.tmp")
        tmp.write_text(fixed, encoding="latin-1")
        tmp.replace(h)

    verb = "carried by" if check_only else "repaired in"
    print("fix-toolchain-inline-const: %d headers examined, %d top-level "
          "const parameter(s) %s %d" % (len(headers), total, verb, touched))
    return 1 if (check_only and total) else 0


if __name__ == "__main__":
    sys.exit(main())
