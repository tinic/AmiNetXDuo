#!/usr/bin/env python3
"""Repair the libnix crt0.o register-save bug in a m68k-amigaos toolchain.

    tools/fix-toolchain-crt0.py <toolchain-root> [--check]

WHAT IS WRONG

    The libnix crt0.o shipped in the amigadev/crosstools image saves three
    registers at _start and restores four at ___exit:

        _start:   movem.l d2/a2/a6,-(sp)        48e7 2022    SP -= 12
                  move.l  sp,__savedSp          23cf ....
        ___exit:  movea.l __savedSp,sp
                  movem.l (sp)+,d2/d7/a2/a6     4cdf 4484    SP += 16
                  rts

    The extra d7 shifts the frame by one longword. a6 is loaded from
    RunCommand's return address, SP ends four bytes high, and the rts returns
    into the stack-size word RunCommand pushes ABOVE its return address rather
    than the return address itself. Every command dies the moment it returns
    to the Shell, and the corrupt value is the stack size (0x1000 on a 4 KB
    Shell, 0x30D40 after `stack 200000`).

    It is invisible until a compiled command runs to completion under a real
    Shell -- which is why it survived every emulator run this project has
    made, and cost most of a debugging session on WinUAE to find.

WHY HERE AND NOT IN EVERY LINK LINE

    This was first worked around per command: patch a private copy of crt0.o
    and link it first under -nostartfiles. That works and it is the wrong
    shape. It has to be repeated by every target that links a command, it has
    to be right once per CPU multilib -- there are FOUR affected crt0.o files,
    one each for 68000, 68020, 68020+881 and 68060 -- and it leaves the
    toolchain itself broken for anything that does not go through our CMake.

    The defect is in the toolchain, so the repair belongs in the toolchain.
    tools/fetch-toolchain.sh runs this after unpacking and before installing,
    so the tree that lands in the cache is correct and nothing downstream has
    to know this bug ever existed.

    The proper fix is upstream in bebbo's libnix. Note the image's own
    manifest names AmigaPorts/libnix, whose crt0 differs and does NOT have
    this bug, so the shipped binary does not match the source the image
    claims. Until that is resolved, this is the seam.

WHAT IT DOES NOT DO

    It does not touch a crt0 that is already correct, so it is safe to re-run
    and safe on a toolchain that has been fixed upstream. It refuses to guess:
    the save is only rewritten where it is IMMEDIATELY followed by the
    `move.l sp,__savedSp` store, which is what distinguishes _start's prologue
    from any other three-register movem in the file.

SPDX-License-Identifier: MIT
"""

import pathlib
import sys

SAVE_BUGGY = bytes.fromhex("48e72022")   # movem.l d2/a2/a6,-(sp)
SAVE_FIXED = bytes.fromhex("48e72122")   # movem.l d2/d7/a2/a6,-(sp)
SAVE_TAIL = bytes.fromhex("23cf")        # move.l sp,(xxx).L -- the __savedSp store
RESTORE_4 = bytes.fromhex("4cdf4484")    # movem.l (sp)+,d2/d7/a2/a6


def sites(data, save):
    """Offsets of `save` immediately followed by the __savedSp store."""
    out = []
    i = 0
    while i + 6 <= len(data):
        if data[i:i + 4] == save and data[i + 4:i + 6] == SAVE_TAIL:
            out.append(i)
            i += 6
        else:
            i += 1
    return out


def repair(path, check_only):
    data = bytearray(path.read_bytes())
    buggy = sites(data, SAVE_BUGGY)
    fixed = sites(data, SAVE_FIXED)
    restores = data.count(RESTORE_4)

    # A crt0 with neither shape is not one we understand. Say so rather than
    # reporting success over a file that may have a third variant of the bug.
    if not buggy and not fixed:
        return ("skipped", "no _start save/__savedSp pair -- not this crt0 shape")

    if not buggy:
        return ("ok", f"already correct ({len(fixed)} site)")

    if len(buggy) != 1 or restores != 1:
        # More than one candidate means the anchor is not selective enough
        # here; refusing is better than patching the wrong prologue.
        return ("refused",
                f"expected 1 save and 1 restore, found {len(buggy)} and {restores}")

    if check_only:
        return ("buggy", f"needs the d7 save at offset 0x{buggy[0]:x}")

    data[buggy[0] + 2] = 0x21            # 2022 -> 2122
    path.write_bytes(bytes(data))
    return ("patched", f"added d7 to the _start save at offset 0x{buggy[0]:x}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check_only = "--check" in sys.argv[1:]
    if len(args) != 1:
        sys.stderr.write(__doc__.split("\n\n")[1].strip() + "\n")
        return 2

    root = pathlib.Path(args[0])
    if not root.is_dir():
        sys.stderr.write(f"fix-toolchain-crt0: no such directory: {root}\n")
        return 2

    found = sorted(root.rglob("crt0.o"))
    if not found:
        sys.stderr.write(f"fix-toolchain-crt0: no crt0.o under {root}\n")
        return 1

    counts = {}
    for p in found:
        state, note = repair(p, check_only)
        counts[state] = counts.get(state, 0) + 1
        if state in ("patched", "buggy", "refused"):
            print(f"  {state:8s} {p.relative_to(root)}: {note}")

    summary = ", ".join(f"{n} {s}" for s, n in sorted(counts.items()))
    print(f"fix-toolchain-crt0: {len(found)} crt0.o examined -- {summary}")

    if counts.get("refused"):
        return 1
    # --check is a test: a toolchain still carrying the bug is a failure.
    if check_only and counts.get("buggy"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
