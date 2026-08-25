| AmiNetXDuo, the commands' own startup code.
|
| Replaces the toolchain's crt0.o under -nostartfiles.  These commands take
| their arguments through ReadArgs(), print through dos.library's Printf() and
| allocate through AllocVec(), so the only thing they ever wanted from a C
| runtime is a Process, a DOSBase and a call to main().
|
| What the toolchain crt0 costs, measured on the pinned toolchain over all 28
| shipped commands.  Its ____start walks an INIT_LIST whose entries are
| __initlibraries and __initcpp.  __initlibraries opens every library named in
| LIB_LIST, and to decide between OpenLibrary() and OpenResource() it asks
| strstr(name, ".resource").  LIB_LIST has exactly one entry, dos.library,
| put there by libc's __dosbase.o.  That one strstr() call is the whole reason
| newlib's two-way strstr is in every binary, and it brings strchr, memchr and
| memcmp with it:
|
|     crt0.o        772       strstr      1,580      strchr     260
|     memchr        152       memcmp        128      strcat      96
|     strcpy         20       strlen         20      __dosbase   20
|
| 3,048 bytes per command, 18% of `ping`, and no line in this tree calls any
| of it -- src/tools, src/config and src/common name none of strstr, strchr,
| memchr, memcmp, strcat, strcpy or strlen.  This file is about 150 bytes and
| does the same job.
|
| It also settles the question toolchain-side.  The local (non-pinned) 15.2.0
| build ships a crt0 that parses the command line into argv through libc's
| __nocommandline.o, which calls calloc, which reaches _impure_ptr, which
| drags in findfp and the whole stdio close-down machinery -- another 7.5 KB
| that nothing calls.  Under -nostartfiles neither toolchain can add anything.
|
| What main() sees is unchanged.  The toolchain crt0 passes argc = 1 and
| argv = the raw command line from a Shell, argc = 0 and argv = the WBStartup
| message from Workbench.  It never builds an argv array on the pinned
| toolchain either.  This does exactly the same, which is what
| tool_from_workbench() reads and all any command here looks at.
|
| Every instruction below is in the original 68000 set and none was dropped
| from the 68060, so one file serves all four CPU configurations.
|
| SPDX-License-Identifier: MIT

        .text
        .even
        .globl  _____start
        .globl  _exit
        .globl  __exit
        .globl  _SysBase
        .globl  _DOSBase
        .globl  __WBenchMsg

LVOForbid       = -132
LVOFindTask     = -294
LVOGetMsg       = -372
LVOReplyMsg     = -378
LVOWaitPort     = -384
LVOCloseLibrary = -414
LVOOpenLibrary  = -552

AttnFlags       = 296
AFFB_68020      = 1
AFFB_68060      = 7

pr_MsgPort      = 92
pr_CLI          = 172

RETURN_FAIL     = 20

| The entry must be the first byte of the first hunk.  AmigaOS jumps to the
| start of what LoadSeg() loaded, and there is no ENTRY() in the linker script
| to say otherwise.  aminetxduo_add_tool() names this file first in
| add_executable() so its .text is the first input to *(.text), and
| cmake/check-startup-first.cmake reads the map back and refuses a link where
| it is not.
_____start:
        movem.l d2/a2/a6,sp@-           | the Shell's callee-saved set
        move.l  sp,___savedSp           | where exit() has to get back to
        movea.l a0,a2                   | argv on the Shell path: the raw line

        movea.l 4:w,a6
        move.l  a6,_SysBase

        suba.l  a1,a1
        jsr     a6@(LVOFindTask)
        movea.l d0,a0

        moveq   #1,d2                   | argc
        tst.l   a0@(pr_CLI)
        bne.s   .Lopendos

| Workbench.  Nothing here has a GUI, so this path only has to take the
| WBStartup message and reply to it, which is what lets Workbench unload the
| command again.  main() is still called, sees argc == 0 and says through
| tool_from_workbench() that this is a Shell command.
        lea     a0@(pr_MsgPort),a0
        movea.l a0,a2
        jsr     a6@(LVOWaitPort)
        movea.l a2,a0
        jsr     a6@(LVOGetMsg)
        move.l  d0,__WBenchMsg
        movea.l d0,a2                   | argv on the Workbench path
        moveq   #0,d2

.Lopendos:
        lea     .Ldosname(pc),a1
        moveq   #0,d0
        jsr     a6@(LVOOpenLibrary)
        move.l  d0,_DOSBase
        bne.s   .Lmain

| No dos.library, so there is nothing to print the complaint with.  The
| toolchain crt0 writes "Can't open dos.library" through DOSBase, which is the
| one it just failed to open.
        moveq   #RETURN_FAIL,d0
        move.l  d0,sp@-
        jsr     _exit

.Lmain:
#ifdef AMINETXDUO_TOOL_RT
| Which form of src/common/ami_udivdi3.c's helpers this machine wants, and the
| reference that pulls that archive ahead of libgcc's.  The 68060 traps the
| 64-bit MULU.L/DIVU.L forms, so it gets the 68020 arm without them.
        moveq   #0,d0
        moveq   #0,d1
        btst    #AFFB_68020,a6@(AttnFlags+1)
        beq.s   .Lrtgo
        moveq   #1,d0
        moveq   #1,d1
        btst    #AFFB_68060,a6@(AttnFlags+1)
        beq.s   .Lrtgo
        moveq   #0,d1
.Lrtgo:
        move.l  d1,sp@-
        move.l  d0,sp@-
        jsr     _ami_rt_cpu_select
        addq.l  #8,sp
#endif

        move.l  a2,sp@-
        move.l  d2,sp@-
        jsr     _main
        addq.l  #8,sp
        move.l  d0,sp@-
        jsr     _exit

| exit(int).  Reached by falling out of main() and by any command that calls
| it.  It abandons the stack rather than returning through its callers, which
| is what makes the second case work.
_exit:
__exit:
        move.l  sp@(4),d2
        movea.l _SysBase,a6

        movea.l _DOSBase,a1
        move.l  a1,d0
        beq.s   .Lnodos
        jsr     a6@(LVOCloseLibrary)

.Lnodos:
        tst.l   __WBenchMsg
        beq.s   .Lreturn
        jsr     a6@(LVOForbid)          | no Permit: the Process ends here
        movea.l __WBenchMsg,a1
        jsr     a6@(LVOReplyMsg)

.Lreturn:
        move.l  ___savedSp,sp
        move.l  d2,d0
        movem.l sp@+,d2/a2/a6
        rts

.Ldosname:
        .asciz  "dos.library"
        .even

        .bss
        .even
_SysBase:
        .space  4
_DOSBase:
        .space  4
__WBenchMsg:
        .space  4
___savedSp:
        .space  4
