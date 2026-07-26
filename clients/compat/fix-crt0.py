#!/usr/bin/env python3
"""
Repair this toolchain's crt0.o, which passes the WRONG argv to main().

THE BUG

    m68k-amigaos/lib/crt0.o calls main() like this:

        pea      ___argv          ; 4879 xxxxxxxx   -- pushes &__argv
        move.l   ___argc,-(sp)    ; 2f39 xxxxxxxx
        jsr      _main            ; 4eb9 xxxxxxxx

    `__argv` is a POINTER (crt0.o's whole .bss is 16 bytes: __argv at 0,
    __savedSp at 8), so `pea` pushes its address and main() receives one
    level of indirection too many.  Everything downstream is then wrong in a
    way that looks like memory corruption rather than a calling convention
    error:

        argv[0]  is the real argv array, whose first byte is the high byte of
                 a pointer -- zero -- so it prints as an empty string
        argv[1]  is whatever bss word follows __argv, which is 0, so it is
                 NULL
        argv[2]  is __savedSp, a stack pointer, which also prints empty
        argv[3]  is off the end

    Measured on the emulated 68020, `argvtest alpha beta`:

        broken:  argc=3  argv[0]=<>  argv[1]=<NULL>  argv[2]=<>
        fixed:   argc=3  argv[0]=<SYS:argvtest>  argv[1]=<alpha>  argv[2]=<beta>

    Nothing in AmiNetXDuo had ever noticed, because every command in this
    tree takes its arguments through ReadArgs() and only ever reads `argc`
    (to tell a Workbench launch from a Shell one).  A ported Unix client
    reads argv and nothing else, so for curl this is not a detail.

THE FIX

    `pea (xxx).L` and `move.l (xxx).L,-(sp)` are both six bytes -- 4879 and
    2f39 with the same 32-bit absolute operand -- so the repair is a two-byte
    opcode swap that leaves every offset and every relocation exactly where
    it was.  No relocation is added, removed or moved, which is what makes it
    safe to do to an object file with a script instead of rebuilding the
    toolchain.

    There are two call sites, the Shell one and the Workbench one, and both
    are patched.

WHY PATCH RATHER THAN WRITE OUR OWN crt0

    A replacement would have to reproduce __initlibraries, the INIT/EXIT/CTOR
    /DTOR list walking, the Workbench message handling and the newlib
    reentrancy setup, and would then rot against the toolchain.  Two bytes
    will not.

    The output is a COPY, in the build directory.  The installed toolchain is
    never modified: a build host is not ours to change, and the next person to
    fetch the pinned toolchain must get the same result as this one.

    usage:  fix-crt0.py <input crt0.o> <output crt0.o>

SPDX-License-Identifier: MIT
"""

import sys

PEA_ABS = b'\x48\x79'
MOVE_ABS_PUSH = b'\x2f\x39'
JSR_ABS = b'\x4e\xb9'


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: fix-crt0.py <in crt0.o> <out crt0.o>\n")
        return 2

    data = bytearray(open(argv[1], 'rb').read())

    # The call sequence, matched on opcodes only so that the relocated
    # operands (which are zero in the object file and filled in at link time)
    # do not have to be predicted:
    #
    #     pea (a).L ; move.l (b).L,-(sp) ; jsr (main).L
    #
    # Sixteen bytes.  Searching for the whole sequence rather than for a bare
    # `pea` is what keeps this from patching some unrelated instruction.
    patched = 0
    i = 0
    while i + 16 <= len(data):
        if (data[i:i + 2] == PEA_ABS and
                data[i + 6:i + 8] == MOVE_ABS_PUSH and
                data[i + 12:i + 14] == JSR_ABS):
            data[i:i + 2] = MOVE_ABS_PUSH
            patched += 1
            i += 16
            continue
        i += 1

    if patched == 0:
        # Either the toolchain has been fixed upstream or its crt0 is built
        # differently.  Either way, copying it through unchanged is right, and
        # saying so is better than silently producing a working binary for a
        # reason nobody understands.
        sys.stderr.write(
            "fix-crt0: no 'pea/move.l/jsr' main() call found -- "
            "copying crt0.o unchanged.\n"
            "          Check that argv works before trusting this build:\n"
            "          clients/curl/run-fsuae.sh runs `curl --version`, which\n"
            "          needs argv to say anything at all.\n")
    elif patched != 2:
        sys.stderr.write(
            "fix-crt0: patched %d call site(s), expected 2 "
            "(the Shell one and the Workbench one).\n" % patched)

    open(argv[2], 'wb').write(bytes(data))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
