#!/usr/bin/env python3
"""Repair the newlib crt0.o frame-skew bug in a m68k-amigaos toolchain.

    tools/fix-toolchain-crt0.py <toolchain-root> [--check]

WHAT IS WRONG

    newlib/libc/sys/amigaos/crt0.c has ____start record the stack pointer
    AFTER its own prologue, and exit() restore that pointer and then let its
    OWN epilogue pop ____start's frame:

        ____start:  movem.l d2/a2/a6,-(sp)      3 registers, sp -= 12
                    move.l  sp,__savedSp
        exit:       movem.l d2/d7/a2/a6,-(sp)   4 registers
                    ...
                    move.l  __savedSp,sp
                    movem.l (sp)+,d2/d7/a2/a6   pops 4 -- sp += 16
                    rts

    That is correct only if GCC gives the two functions the same callee-saved
    set, and crt0.c's `register unsigned __d7 __asm("d7")` guarantees it does
    not. The frame is popped four bytes too far, the rts reads the longword
    ABOVE the return address, and the program jumps to whatever is there --
    under a CLI, the stack size RunCommand pushed (0x1000, or 0x30D40 after
    `stack 200000`). Every command dies the moment it returns to the Shell.

    It is NOT libnix, despite what an earlier version of this file said.
    libnix produces ncrt0.o/nbcrt0.o and friends, hand-written assembler that
    keeps no frame and is immune -- -noixemul sidesteps the bug entirely.
    Reported upstream at codeberg.org/bebbo/amiga-gcc.

    Latent since 2018: GCC 6.5 emitted no movem in either function, so
    __savedSp landed exactly on the return address. GCC 15.2 callee-saves a
    register variable written only by an asm, where 6.5 did not.

WHY HERE AND NOT IN EVERY LINK LINE

    This was first worked around per command -- patch a private crt0 and link
    it first under -nostartfiles. That has to be repeated by every target that
    links a command, has to be right once per multilib, and leaves the
    toolchain broken for anything not built through our CMake. The defect is
    the toolchain's, so the repair belongs in the toolchain:
    tools/fetch-toolchain.sh runs this after unpacking, before installing.

HOW IT DECIDES WHAT TO PATCH

    By symbol boundaries from the toolchain's own objdump, not by byte
    patterns. The first attempt anchored on `move.l sp,__savedSp` (23cf) and
    silently MISSED the six baserel variants, which reach __savedSp through a4
    and never emit that opcode -- they were reported as "skipped", which reads
    as benign. All eleven crt0.o in the tree carry the bug.

    ____start's frame is widened to match exit's, rather than exit's narrowed
    to match ____start's, because exit still USES d7 to carry the return code:
    narrowing would stop d7 being saved while it is still clobbered, breaking
    the callee-saved contract with the Shell. Widening preserves it.
    ____start's own epilogues are widened with it -- they are unreachable,
    since exit() never returns, but a 3-register pop against a 4-register push
    is a trap for the next reader.

    The upstream source fix goes the other way, dropping the d7 register
    variable from exit, because source can remove the use as well as the save.
    An object file cannot.

SPDX-License-Identifier: MIT
"""

import os
import pathlib
import re
import shutil
import subprocess
import sys

MOVEM_SAVE = 0x48E7          # movem.l <regs>,-(sp)
MOVEM_REST = 0x4CDF          # movem.l (sp)+,<regs>


def rev16(m):
    """movem's two mask orders are bit-reversed images of each other.

    -(sp) counts a7,a6..a0,d7..d0 from bit 0; (sp)+ counts d0..d7,a0..a7. So
    the save mask matching a given restore mask is its bit reversal, and this
    never has to name a register -- which is what lets one rule cover the
    plain model and baserel without knowing either register set.
    """
    return int(f"{m:016b}"[::-1], 2)


def find_objdump(root, sample):
    """A working m68k objdump -- preferably the tree's own.

    The tree's is preferred because it is guaranteed to match the objects it
    ships. It is not guaranteed to RUN: the pinned toolchain is a Linux
    x86-64 build, so on macOS it fails with "Exec format error" -- the same
    reason tools/ci.sh cannot be reproduced there. So each candidate is tried
    on a real crt0.o and the first that actually works is used; any m68k
    objdump reads the same Amiga hunk format.
    """
    cands = [p for p in root.rglob("*m68k-amigaos-objdump*") if p.is_file()]
    # Then anything the environment already knows about: AMIGA_TOOLCHAIN_ROOT
    # is what tools/amiga-toolchain.sh exports, and on a developer machine it
    # often points at a DIFFERENT toolchain from the one being repaired --
    # which is fine here, since only the disassembler is borrowed.
    env_root = os.environ.get("AMIGA_TOOLCHAIN_ROOT")
    if env_root:
        cands.append(pathlib.Path(env_root) / "bin" / "m68k-amigaos-objdump")
    override = os.environ.get("AMINETXDUO_OBJDUMP")
    if override:
        cands.insert(0, pathlib.Path(override))
    which = shutil.which("m68k-amigaos-objdump")
    if which:
        cands.append(pathlib.Path(which))
    for c in cands:
        try:
            r = subprocess.run([str(c), "-d", str(sample)],
                               capture_output=True, text=True)
            if r.returncode == 0 and "Disassembly" in r.stdout:
                return c
        except OSError:
            continue
    return None


def text_file_offset(objdump, path):
    """File offset of .text.

    objdump -d reports offsets within the SECTION; these are Amiga hunk
    objects with a header in front, so section offset 0 is file offset 0x28 in
    practice and NOT zero. Writing a section offset straight into the file
    corrupts it -- which is exactly what an earlier version of this script did
    to seven of eleven crt0.o before objdump stopped being able to read them.
    """
    out = subprocess.run([str(objdump), "-h", str(path)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        return None
    for line in out.stdout.splitlines():
        m = re.match(r"^\s*\d+\s+\.text\s+[0-9a-f]+\s+[0-9a-f]+\s+"
                     r"[0-9a-f]+\s+([0-9a-f]+)", line)
        if m:
            return int(m.group(1), 16)
    return None


def functions(objdump, path):
    """{name: [(offset, opcode, mask), ...]} for every movem, per function."""
    out = subprocess.run([str(objdump), "-d", str(path)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        return None
    fns, cur = {}, None
    for line in out.stdout.splitlines():
        m = re.match(r"^([0-9a-f]+) <([^>]+)>:", line)
        if m:
            cur = m.group(2)
            fns[cur] = []
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+([0-9a-f]{4}) ([0-9a-f]{4})\s", line)
        if m and cur:
            op, mask = int(m.group(2), 16), int(m.group(3), 16)
            if op in (MOVEM_SAVE, MOVEM_REST):
                fns[cur].append((int(m.group(1), 16), op, mask))
    return fns


def repair(objdump, path, check_only):
    fns = functions(objdump, path)
    if fns is None:
        return ("refused", "objdump could not read it")

    start = next((v for k, v in fns.items() if k.endswith("____start")), None)
    exit_ = next((v for k, v in fns.items() if k.endswith("__exit")), None)
    if start is None or exit_ is None:
        return ("skipped", "no ____start/exit pair -- not this crt0 shape")

    saves = [e for e in exit_ if e[1] == MOVEM_SAVE]
    if len(saves) != 1:
        return ("refused", f"exit has {len(saves)} prologues, expected 1")
    want_save = saves[0][2]
    want_rest = rev16(want_save)

    # Every movem in ____start has to describe the same set as exit's.
    wrong = [e for e in start
             if (e[1] == MOVEM_SAVE and e[2] != want_save)
             or (e[1] == MOVEM_REST and e[2] != want_rest)]
    if not wrong:
        return ("ok", f"____start already matches exit (save 0x{want_save:04x})")

    if check_only:
        return ("buggy",
                f"{len(wrong)} movem in ____start disagree with exit's "
                f"0x{want_save:04x}")

    base = text_file_offset(objdump, path)
    if base is None:
        return ("refused", "cannot locate .text in the file")

    data = bytearray(path.read_bytes())
    for off, op, cur in wrong:
        at = base + off
        # Refuse unless the bytes on disk are the instruction objdump saw --
        # the only guard against writing at a wrong offset.
        if (data[at] << 8 | data[at + 1]) != op or (data[at + 2] << 8 | data[at + 3]) != cur:
            return ("refused",
                    f"file offset 0x{at:x} does not hold the movem objdump "
                    f"reported at section 0x{off:x}")
        want = want_save if op == MOVEM_SAVE else want_rest
        data[at + 2] = (want >> 8) & 0xFF
        data[at + 3] = want & 0xFF
    path.write_bytes(bytes(data))
    return ("patched",
            f"{len(wrong)} movem in ____start -> exit's set (0x{want_save:04x})")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check_only = "--check" in sys.argv[1:]
    if len(args) != 1:
        sys.stderr.write("usage: fix-toolchain-crt0.py <toolchain-root> [--check]\n")
        return 2

    root = pathlib.Path(args[0])
    if not root.is_dir():
        sys.stderr.write(f"fix-toolchain-crt0: no such directory: {root}\n")
        return 2

    found = sorted(root.rglob("crt0.o"))
    if not found:
        sys.stderr.write(f"fix-toolchain-crt0: no crt0.o under {root}\n")
        return 1

    objdump = find_objdump(root, found[0])
    if objdump is None:
        sys.stderr.write("fix-toolchain-crt0: no WORKING m68k-amigaos-objdump "
                         "(the tree's own may be built for another host)\n")
        return 1

    counts = {}
    for p in found:
        state, note = repair(objdump, p, check_only)
        counts[state] = counts.get(state, 0) + 1
        if state in ("patched", "buggy", "refused"):
            print(f"  {state:8s} {p.relative_to(root)}: {note}")

    print(f"fix-toolchain-crt0: {len(found)} crt0.o examined -- "
          + ", ".join(f"{n} {s}" for s, n in sorted(counts.items())))

    if counts.get("refused"):
        return 1
    if check_only and counts.get("buggy"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
