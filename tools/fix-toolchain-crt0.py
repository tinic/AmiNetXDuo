#!/usr/bin/env python3
"""Repair the two newlib crt0.o bugs in a m68k-amigaos toolchain.

    tools/fix-toolchain-crt0.py <toolchain-root> [--check]

    Nothing here is needed against a toolchain built from bebbo/gcc amiga15.2
    at 168be3619 or later; see FIXED UPSTREAM below. --check reports such a
    tree as "immune" and succeeds.

    THIS IS NOT TRANSITIONAL. Both bugs are fixed in bebbo's git and neither
    fix is in any toolchain anybody installs: the prebuilt m68k-amigaos images
    lag upstream by years, not months, and the one this project pins is named
    for GCC 10 and predates both fixes by eight months. Expect to be running
    this script for the life of the project, and keep it that way, verified
    in every configuration, and loud when it meets something new. It has
    already stopped two releases that would otherwise have shipped broken.

    TWO SEPARATE BUGS live in the same crt0.c and are repaired independently:
    the frame skew (below), and the argv indirection (SECOND BUG, further
    down). A toolchain can have either, both or neither.

WHAT IS WRONG

    newlib/libc/sys/amigaos/crt0.c has ____start record the stack pointer
    AFTER its own prologue, and exit() restore that pointer and then let its
    OWN epilogue pop ____start's frame:

        ____start:  movem.l d2/a2/a6,-(sp)      3 registers, sp -= 12
                    move.l  sp,__savedSp
        exit:       movem.l d2/d7/a2/a6,-(sp)   4 registers
                    ...
                    move.l  __savedSp,sp
                    movem.l (sp)+,d2/d7/a2/a6   pops 4, sp += 16
                    rts

    That is correct only if GCC gives the two functions the same callee-saved
    set, and crt0.c's `register unsigned __d7 __asm("d7")` guarantees it does
    not. The frame is popped four bytes too far, the rts reads the longword
    ABOVE the return address, and the program jumps to whatever is there --
    under a CLI, the stack size RunCommand pushed (0x1000, or 0x30D40 after
    `stack 200000`). Every command dies the moment it returns to the Shell.

    It is NOT libnix, despite what an earlier version of this file said.
    libnix produces ncrt0.o/nbcrt0.o and friends, hand-written assembler that
    keeps no frame and is immune, -noixemul sidesteps the bug entirely.
    Reported upstream at codeberg.org/bebbo/amiga-gcc.

    Latent since 2018: GCC 6.5 emitted no movem in either function, so
    __savedSp landed exactly on the return address. GCC 15.2 callee-saves a
    register variable written only by an asm, where 6.5 did not.

WHY HERE AND NOT IN EVERY LINK LINE

    This was first worked around per command, patch a private crt0 and link
    it first under -nostartfiles. That has to be repeated by every target that
    links a command, has to be right once per multilib, and leaves the
    toolchain broken for anything not built through our CMake. The defect is
    the toolchain's, so the repair belongs in the toolchain:
    tools/fetch-toolchain.sh runs this after unpacking, before installing.

HOW IT DECIDES WHAT TO PATCH

    By symbol boundaries from the toolchain's own objdump, not by byte
    patterns. The first attempt anchored on `move.l sp,__savedSp` (23cf) and
    silently MISSED the six baserel variants, which reach __savedSp through a4
    and never emit that opcode, they were reported as "skipped", which reads
    as benign. All eleven crt0.o in the tree carry the bug.

    VERIFIED 2026-07-27: crt0.c marks BOTH ____start (line 48) and exit (line
    94) __entrypoint, read from bebbo/newlib-cygwin branch `amiga` at 0909ae9.
    That is what makes the upstream fix safe, were only ____start marked,
    suppressing its save while exit kept a frame would put __savedSp BELOW the
    return address, which is the `refused` case below and worse than the
    original bug. callfuncs and __restore_a4 carry it too.

    ____start's frame is widened to match exit's, rather than exit's narrowed
    to match ____start's, because exit still USES d7 to carry the return code:
    narrowing would stop d7 being saved while it is still clobbered, breaking
    the callee-saved contract with the Shell. Widening preserves it.
    ____start's own epilogues are widened with it, they are unreachable,
    since exit() never returns, but a 3-register pop against a 4-register push
    is a trap for the next reader.

FIXED UPSTREAM, AND NOT THE WAY WE ASKED

    Reported as codeberg.org/bebbo/amiga-gcc issue #12. The report proposed a
    newlib change, give exit's return code static storage instead of a
    register variable, so the allocator cannot produce the mismatch. An
    earlier version of this docstring described that as "the upstream source
    fix", which it never was: it was our suggestion, written up as though it
    described something that had happened.

    bebbo fixed the COMPILER instead, in gcc/config/m68k/m68k.cc, and carried
    it to amiga15.2 (168be3619), amiga13.4 and amiga16.1:

        static bool
        m68k_save_reg (unsigned int regno, bool interrupt_handler)
        {
        + tree attrs = TYPE_ATTRIBUTES (TREE_TYPE (current_function_decl));
        + if (lookup_attribute ("entrypoint", attrs))
        +   return false;

    crt0.c already marked both ____start and exit with __entrypoint, the
    attribute was registered (config/m68k/amigaos.h) and the macro predefined
    (config/m68k/m68kamigaos.h), and m68k_save_reg() simply ignored it. An
    entry point has no caller to preserve registers for, so it must get no
    prologue at all; once that holds, __savedSp is recorded with nothing
    pushed and lands exactly on the return address, and neither function has
    an epilogue to disagree about.

    It is the better fix, and for a reason visible in the disassembly: exit
    STILL uses d7 (`movel d0,d7` on entry, `movel d7,d0` before the rts) to
    carry the return code across _callfuncs and the library closes. Removing
    the use, what was proposed from here, would have meant restructuring
    exit. Suppressing the save keeps the code and fixes every __entrypoint
    function rather than this one file.

    A toolchain built from those branches therefore needs nothing from this
    script, and reports as "immune" below rather than as an error.

    NOTE ON WHAT "immune" DOES AND DOES NOT SAY. It says the two functions
    agree because neither keeps a frame, which is the shape the fix produces.
    It does NOT prove the fix is what produced it: the toolchain in use here
    is dated well before 168be3619 and is already frameless, for a reason not
    established, a hand build, a different newlib, or an older compiler that
    happened to allocate nothing (GCC 6.5 did exactly that, which is why the
    defect stayed latent from 2018). The check reports the shape it sees and
    leaves the cause alone.

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
    never has to name a register, which is what lets one rule cover the
    plain model and baserel without knowing either register set.
    """
    return int(f"{m:016b}"[::-1], 2)


def find_objdump(root, sample):
    """A working m68k objdump, preferably the tree's own.

    The tree's is preferred because it is guaranteed to match the objects it
    ships. It is not guaranteed to RUN: the pinned toolchain is a Linux
    x86-64 build, so on macOS it fails with "Exec format error", the same
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
    corrupts it, which is exactly what an earlier version of this script did
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
    # Two symbol-line formats, because binutils versions disagree and the
    # toolchains in play are different versions:
    #     00000000 <_____start>:              (newer)
    #     00000000 00000000 _____start:       (the pinned 2.39 build)
    # Only the second appears on the pinned toolchain, so matching just the
    # first parsed nothing, reported every file as "skipped", and passed.
    SYMBOL = (re.compile(r"^([0-9a-f]+) <([^>]+)>:"),
              re.compile(r"^[0-9a-f]+ [0-9a-f]+ (\S+):\s*$"))
    fns, cur = {}, None
    for line in out.stdout.splitlines():
        m = SYMBOL[0].match(line)
        if m:
            cur = m.group(2)
            fns[cur] = []
            continue
        m = SYMBOL[1].match(line)
        if m:
            cur = m.group(1)
            fns[cur] = []
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+([0-9a-f]{4}) ([0-9a-f]{4})\s", line)
        if m and cur:
            op, mask = int(m.group(2), 16), int(m.group(3), 16)
            if op in (MOVEM_SAVE, MOVEM_REST):
                fns[cur].append((int(m.group(1), 16), op, mask))
    return fns


# --------------------------------------------------------------- SECOND BUG
# THE ARGV INDIRECTION, codeberg.org/bebbo/amiga-gcc issue #8.  crt0.c declared
# `char * __argv[];` instead of `char ** __argv;`, so ____start passes &__argv.
#
# THE REPAIR is a two-byte opcode swap. `pea` and `move.l ...,-(sp)` are the
# same length in both addressing modes used here and put their operand in the
# same place, so the relocation is untouched and only the opcode word changes.

PEA_ABS, PUSH_ABS = 0x4879, 0x2F39      # pea <abs>.l   / move.l <abs>.l,-(sp)
PEA_A4, PUSH_A4 = 0x486C, 0x2F2C        # pea a4@(d16)  / move.l a4@(d16),-(sp)
PEA_A4_32, PUSH_A4_32 = 0x4874, 0x2F34  # pea a4@(bd32) / move.l a4@(bd32),-(sp)
ARGV_FIX = {PEA_ABS: PUSH_ABS, PEA_A4: PUSH_A4, PEA_A4_32: PUSH_A4_32}
PUSH_OPS = (PUSH_ABS, PUSH_A4, PUSH_A4_32)

# THE 32-BIT BASEREL FORM NEEDS ITS DISPLACEMENT CHECKED, and the others do
# not. objdump prints the addend as part of the symbol for the short forms --
# ".bss" vs ".bss+0x8", so requiring a bare ".bss" already proves the target
# is __argv. The libb32 multilibs put the addend in the instruction instead and
# print a bare "DREL32 .bss" for __savedSp (.bss+8) just the same, so the only
# way to tell __argv from __savedSp there is to read the displacement.
PEA_A4_32_TAIL = bytes((0x01, 0x70, 0x00, 0x00, 0x00, 0x00))   # (bd=0,a4)

# Shape B (see argv_sites): the address is kept in an address register and the
# REGISTER is pushed. `move.l an,-(sp)` and `move.l (an),-(sp)` differ only in
# the source addressing mode, 001nnn against 010nnn, so this too is a
# two-byte swap that leaves everything around it alone.
LEA_MASK, LEA_OP = 0xF1C0, 0x41C0       # lea <ea>,an  (an in bits 11..9)
PUSH_AN = 0x2F08                        # move.l an,-(sp)   | reg
PUSH_AN_IND = 0x2F10                    # move.l (an),-(sp) | reg
ADDA_IMM = 0xD1FC                       # adda.l #imm32,an  (an in bits 11..9)


def instructions(objdump, path):
    """[[offset, first_word, reloc_target_or_None, text], ...] in .text order."""
    out = subprocess.run([str(objdump), "-dr", str(path)],
                         capture_output=True, text=True)
    if out.returncode != 0:
        return None
    insns = []
    for line in out.stdout.splitlines():
        m = re.match(r"^\s*([0-9a-f]+):\s+([0-9a-f]{4})[0-9a-f ]*\t(.*)$", line)
        if m:
            insns.append([int(m.group(1), 16), int(m.group(2), 16), None,
                          m.group(3).strip()])
            continue
        # A relocation line annotates the instruction just emitted.
        m = re.match(r"^\s+[0-9a-f]+:\s+\S+\s+(\S+)\s*$", line)
        if m and insns:
            insns[-1][2] = m.group(1)
    return insns


def _writes_areg(text):
    """The address register this instruction's destination is, or None.

    Deliberately crude and deliberately conservative: it only has to notice
    that some register stopped holding what we thought. `movel d0,a6@` writes
    THROUGH a6 and leaves a6 itself alone, and does not match, because the
    text ends `,a6@` rather than `,a6`.
    """
    m = re.search(r",%?a([0-7])$", text)
    return int(m.group(1)) if m else None


def _addend(text):
    """The leading displacement of this instruction's memory operand, or None.

    THE ONLY RELIABLE WAY TO TELL __argv FROM ITS NEIGHBOURS. Both objdumps
    print the addend, and neither reliably distinguishes the symbol otherwise:

        this machine's 2.4x   lea 0 <_____start>,a6      RELOC32 .bss
                              move.l 14 <..+0x14>,-(sp)  RELOC32 .bss
        the pinned 2.39       lea 0 0 ___argv,a6         RELOC32 .bss
                              move.l 14 14 ___argc,-(sp) RELOC32 .bss

    The relocation reads `.bss` for EVERY symbol in that section, __argv,
    __argc, __savedSp, __commandline alike. An earlier version of this file
    assumed a bare `.bss` meant offset zero because that happened to hold on
    the development machine, where __argc is a common symbol and gets its own
    relocation. It does not hold on the toolchain that builds releases.
    __argv is at .bss+0, so the addend is the discriminator.

    THE DISPLACEMENT COMES FIRST, and it is not always leading. `pea a4@(0)`
    has its addend inside the parentheses, and reading the leading token there
    yields "a4", which is valid hex, parses as 0xa4, and quietly disqualified
    seven of the eleven baserel files the first time this check was added.
    """
    m = re.search(r"a[0-7]@\((-?[0-9a-fx]+)\)", text)       # pea a4@(20)
    if m:
        return int(m.group(1), 16)
    m = re.search(r"\b([0-9a-f]+)\(%?a[0-7]\)", text)        # 20(a4), MIT
    if m:
        return int(m.group(1), 16)
    m = re.match(r"^\S+\s+#?([0-9a-f]+)\b", text)            # pea 0 <sym>
    return int(m.group(1), 16) if m else None


def argv_sites(objdump, path):
    """Offsets of the argv push before each `jsr _main`, with their opcode.

    Anchored on the ADJACENT PUSH PAIR, a push referencing .bss immediately
    followed by one referencing __argc, and then confirmed by a _main
    reference within the next few instructions.

    Anchoring on the call itself does not work. The 68020 baserel multilibs
    reach main with a bsr.l, which objdump renders as TWO lines (`bsrs` plus a
    bogus `orib`) carrying RELRELOC32 rather than RELOC32, so "the instruction
    two before the _main reloc" is not the argv push there. That mistake
    silently skipped six of eleven files, benign-looking output for exactly
    the variants a 68020 build uses.

    The .bss reference must carry NO addend: __argv is at .bss+0, while
    __savedSp (+8) and cleanupflag (+0xc) would print one and must not be
    touched.

    TWO SHAPES, because the bug is in the source and the compiler is free to
    express it however it likes. Which one appears is a property of the
    toolchain BUILD, not of the multilib:

      A. pea __argv, a direct push of the address
      B. lea __argv,an ... move.l an,-(sp)

    Shape B is what the pinned toolchain emits: it needs the address in a
    register anyway (to store into __argv on the Workbench path), so it keeps
    it in a6 and pushes the register. The address never appears as an operand
    of the push at all, so a matcher written for shape A sees NOTHING, which
    is exactly what happened, and why v0.6.2's release job failed rather than
    shipping a second broken archive.

    Anchoring on the argc push does not work either. Under the pinned
    toolchain __argc is a LOCAL .bss symbol at +0x14, so its relocation reads
    `.bss`, not `___argc` as it does where argc is a common symbol. The
    confirmation is therefore structural: two adjacent pushes followed closely
    by a reference to main.

    Returns [(offset, old_word, new_word), ...].
    """
    insns = instructions(objdump, path)
    if insns is None:
        return None

    def calls_main(i):
        # Binutils 2.39 renders the call as `jsr 0 0 _main` and emits NO
        # relocation line for it, so the reloc field alone finds nothing --
        # which is precisely why v0.6.3's release job still failed. Check the
        # disassembly text as well.
        for _, _, r, t in insns[i + 2:i + 6]:
            if r in ("_main", "main") or re.search(r"\b_?main\b", t):
                return True
        return False

    def pushes_next(i):
        return i + 1 < len(insns) and 0x2F00 <= insns[i + 1][1] <= 0x2FFF

    sites = []
    holds_argv = {}     # address register -> currently holds &__argv
    for i, (off, word, reloc, text) in enumerate(insns):
        # ---- shape B, first half: lea <__argv>,an
        if (word & LEA_MASK) == LEA_OP and reloc == ".bss" \
                and _addend(text) == 0:
            holds_argv[(word >> 9) & 7] = True
            continue

        # ---- shape C, first half: moveal a4,an / addal #<__argv>,an
        #
        # The baserel multilibs cannot name __argv as an absolute address, so
        # they seed the register with the base and add the displacement. The
        # `moveal a4,an` immediately before is required: an + <.bss offset> is
        # only &__argv if an started at a4, and without that check any
        # register carrying anything could be adopted.
        if (word & 0xF1FF) == ADDA_IMM and reloc == ".bss" \
                and _addend(text) == 0:
            n = (word >> 9) & 7
            # `moveal a4,a6` here, `movea.l a4,a6` under the pinned binutils.
            if i and re.match(rf"^move[a.l]*\s+%?a4,%?a{n}$", insns[i - 1][3]):
                holds_argv[n] = True
                continue

        # ---- shape A: a push whose own operand is __argv
        if reloc == ".bss" and _addend(text) == 0 \
                and (word in ARGV_FIX or word in PUSH_OPS):
            if pushes_next(i) and calls_main(i):
                sites.append((off, word, ARGV_FIX.get(word, word)))
                continue

        # ---- shape B/C, second half: the push of the register itself
        if (word & 0xFFF8) == PUSH_AN:
            n = word & 7
            if holds_argv.get(n) and pushes_next(i) and calls_main(i):
                sites.append((off, word, PUSH_AN_IND | n))
                continue

        # ---- the same site once already repaired, or correct to begin with.
        # Without this a repaired tree reports every file "skipped" and fails
        # its own --check, which is how it must NOT behave: --check runs
        # unconditionally in CI against a toolchain this script just fixed.
        if (word & 0xFFF8) == PUSH_AN_IND:
            n = word & 7
            if holds_argv.get(n) and pushes_next(i) and calls_main(i):
                sites.append((off, word, word))
                continue

        # Anything that redefines an address register invalidates it.
        w = _writes_areg(text)
        if w is not None:
            holds_argv.pop(w, None)
    return sites


def repair_argv(objdump, path, check_only):
    sites = argv_sites(objdump, path)
    if sites is None:
        return ("refused", "objdump could not read it")
    if not sites:
        return ("skipped", "no argv/argc push pair ahead of a call to main")

    wrong = [(off, old, new) for off, old, new in sites if new != old]
    if not wrong:
        return ("immune", f"{len(sites)} call site(s) already push __argv "
                          f"by value")
    if check_only:
        return ("buggy", f"{len(wrong)} of {len(sites)} call site(s) push "
                         f"&__argv instead of __argv")

    base = text_file_offset(objdump, path)
    if base is None:
        return ("refused", "cannot locate .text in the file")

    data = bytearray(path.read_bytes())
    for off, old, new in wrong:
        at = base + off
        # Same guard as the frame repair: refuse unless the bytes on disk are
        # the instruction objdump reported. Section offsets are not file
        # offsets, and getting that wrong once already destroyed seven files.
        if (data[at] << 8 | data[at + 1]) != old:
            return ("refused",
                    f"file offset 0x{at:x} does not hold the instruction "
                    f"objdump reported at section 0x{off:x}")
        # See PEA_A4_32_TAIL: only this form can be confused with __savedSp.
        if old == PEA_A4_32 and \
                bytes(data[at + 2:at + 8]) != PEA_A4_32_TAIL:
            return ("refused",
                    f"file offset 0x{at:x} is a 32-bit baserel pea whose "
                    f"displacement is not (bd=0,a4), not __argv")
        data[at] = (new >> 8) & 0xFF
        data[at + 1] = new & 0xFF
    path.write_bytes(bytes(data))
    return ("patched", f"{len(wrong)} call site(s): push &__argv -> "
                       f"move.l __argv,-(sp)")


def repair(objdump, path, check_only):
    fns = functions(objdump, path)
    if fns is None:
        return ("refused", "objdump could not read it")

    start = next((v for k, v in fns.items() if k.endswith("____start")), None)
    exit_ = next((v for k, v in fns.items() if k.endswith("__exit")), None)
    if start is None or exit_ is None:
        return ("skipped", "no ____start/exit pair, not this crt0 shape")

    saves = [e for e in exit_ if e[1] == MOVEM_SAVE]

    # THE FIXED COMPILER'S SHAPE, and it is not a failure.
    #
    # bebbo/gcc 168be3619 (branch amiga15.2, 2026-07-27) makes m68k_save_reg()
    # return false for any function carrying the `entrypoint` attribute, which
    # crt0.c puts on both ____start and exit. Neither then saves anything, so
    # __savedSp is recorded with no frame pushed and lands exactly on the
    # return address, which is what `move.l __savedSp,sp / rts` needs.
    #
    # Nothing to repair, and nothing wrong. Reporting it as "refused" told a
    # user with a REPAIRED toolchain that theirs was broken.
    if not saves:
        if not start:
            return ("immune", "neither ____start nor exit keeps a frame "
                              "-- nothing to repair")

        # exit frameless but ____start not is WORSE than the original bug:
        # __savedSp would point below the return address rather than above it,
        # so the rts reads a saved register instead of overshooting. Nothing
        # here can repair it, widening exit is not possible without knowing
        # what it clobbers, so say so plainly.
        return ("refused",
                f"exit keeps no frame but ____start has {len(start)} movem: "
                f"__savedSp would point BELOW the return address")

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

    # The two bugs are independent, so they are counted and reported
    # independently, a tree can be immune to one and carry the other, which
    # is exactly the state the pinned toolchain was in.
    rc = 0
    for label, fn in (("frame skew", repair), ("argv indirection", repair_argv)):
        counts = {}
        printed_immune = False  # one line is enough for a whole clean tree
        print(f"== {label}")
        for p in found:
            state, note = fn(objdump, p, check_only)
            counts[state] = counts.get(state, 0) + 1
            if state in ("patched", "buggy", "refused"):
                print(f"  {state:8s} {p.relative_to(root)}: {note}")
            elif state == "immune" and not printed_immune:
                printed_immune = True
                print(f"  {state:8s} {p.relative_to(root)}: {note}")

        print(f"fix-toolchain-crt0: {len(found)} crt0.o examined, "
              + ", ".join(f"{n} {s}" for s, n in sorted(counts.items())))
        if counts.get("refused"):
            rc = 1
        if check_only and counts.get("buggy"):
            rc = 1
        # Skipping EVERYTHING is not a pass, that is how an earlier version
        # returned success over an unrepaired toolchain. "buggy" counts as
        # understood: under --check a wholly unrepaired tree is every file
        # buggy and none ok, which is a real verdict, not a parse failure.
        if not any(counts.get(s) for s in ("ok", "patched", "immune", "buggy")):
            sys.stderr.write(f"fix-toolchain-crt0: {label}: nothing was "
                             f"verified on any crt0.o, the disassembly was "
                             f"not understood\n")
            rc = 1
    return rc



if __name__ == "__main__":
    sys.exit(main())
