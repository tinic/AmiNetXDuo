| forbidrec: the exec patches.
|
| This block is COPIED into memory that is never freed, and the copy is what
| SetFunction() installs.  A task can be preempted anywhere in a stub, and a
| stub whose code was freed under it is a crash some seconds after the
| recorder exits.  So everything here is position independent and reads its
| data from the block's own tail.  The C bodies stay in the program; a stub
| reaches one only while D_ARMED is set, and counts itself in D_INFL first,
| so the program can disarm, wait for D_INFL to reach zero and unload.
|
| Every stub preserves every register.  Exec's Forbid, Permit, Disable and
| Enable do, and callers written in assembler rely on it.
|
| The C bodies run only while the caller holds Forbid() or Disable(), so no
| other task can enter one at the same time.  Forbid and Disable call the
| real function FIRST and then look at the count, so "the count is now 0"
| means this call left -1.  Permit and Enable look first: "the count is 0"
| means this call goes back to -1.
|
| SPDX-License-Identifier: MIT

        .set    IDNEST, 294             | ExecBase.IDNestCnt
        .set    TDNEST, 295             | ExecBase.TDNestCnt

        .set    D_OLDF, 0               | the vectors the stubs chain to
        .set    D_OLDP, 4
        .set    D_OLDD, 8
        .set    D_OLDE, 12
        .set    D_OLDW, 16
        .set    D_CF, 20                | C bodies, void f(ULONG caller_pc)
        .set    D_CP, 24
        .set    D_CD, 28
        .set    D_CE, 32
        .set    D_CWPRE, 36
        .set    D_CWPOST, 40
        .set    D_INFL, 44              | stubs inside a C body
        .set    D_ARMED, 48
        .set    D_SYSB, 52              | SysBase: a6 at the call need not be
        .set    D_SIZE, 56

        .text
        .even
        .globl  _fr_blob_start
        .globl  _fr_blob_end
        .globl  _fr_forbid
        .globl  _fr_permit
        .globl  _fr_disable
        .globl  _fr_enable
        .globl  _fr_wait
        .globl  _fr_data

_fr_blob_start:

_fr_forbid:
        move.l  a0,sp@-
        move.l  pc@(_fr_data+D_OLDF),a0
        jsr     a0@
        move.l  pc@(_fr_data+D_SYSB),a0
        tst.b   a0@(TDNEST)
        bne.s   9f
        movem.l d0-d1/a1,sp@-           | a0 is below: return PC at 16
        move.l  sp@(16),d1
        moveq   #D_CF,d0
        bsr.w   .Lcall
        movem.l sp@+,d0-d1/a1
9:      move.l  sp@+,a0
        rts

_fr_permit:
        move.l  a0,sp@-
        move.l  pc@(_fr_data+D_SYSB),a0
        tst.b   a0@(TDNEST)
        bne.s   9f
        movem.l d0-d1/a1,sp@-
        move.l  sp@(16),d1
        moveq   #D_CP,d0
        bsr.w   .Lcall
        movem.l sp@+,d0-d1/a1
9:      move.l  sp@+,a0
        move.l  pc@(_fr_data+D_OLDP),sp@-
        rts

_fr_disable:
        move.l  a0,sp@-
        move.l  pc@(_fr_data+D_OLDD),a0
        jsr     a0@
        move.l  pc@(_fr_data+D_SYSB),a0
        tst.b   a0@(IDNEST)
        bne.s   9f
        movem.l d0-d1/a1,sp@-
        move.l  sp@(16),d1
        moveq   #D_CD,d0
        bsr.w   .Lcall
        movem.l sp@+,d0-d1/a1
9:      move.l  sp@+,a0
        rts

_fr_enable:
        move.l  a0,sp@-
        move.l  pc@(_fr_data+D_SYSB),a0
        tst.b   a0@(IDNEST)
        bne.s   9f
        movem.l d0-d1/a1,sp@-
        move.l  sp@(16),d1
        moveq   #D_CE,d0
        bsr.w   .Lcall
        movem.l sp@+,d0-d1/a1
9:      move.l  sp@+,a0
        move.l  pc@(_fr_data+D_OLDE),sp@-
        rts

| Wait() inside Forbid() or Disable() breaks it until the task runs again.
| The hold so far is closed before the real Wait() and a new one opened after
| it, so the time the task slept is not counted as a hold.
_fr_wait:
        movem.l d0-d1/a0-a1,sp@-
        move.l  pc@(_fr_data+D_SYSB),a0
        tst.b   a0@(TDNEST)
        bpl.s   1f
        tst.b   a0@(IDNEST)
        bmi.s   2f
1:      move.l  sp@(16),d1
        moveq   #D_CWPRE,d0
        bsr.w   .Lcall
2:      movem.l sp@+,d0-d1/a0-a1
        move.l  pc@(_fr_data+D_OLDW),a0 | a0 is scratch for Wait()
        jsr     a0@
        movem.l d0-d1/a0-a1,sp@-
        move.l  pc@(_fr_data+D_SYSB),a0
        tst.b   a0@(TDNEST)
        bpl.s   3f
        tst.b   a0@(IDNEST)
        bmi.s   4f
3:      move.l  sp@(16),d1
        moveq   #D_CWPOST,d0
        bsr.w   .Lcall
4:      movem.l sp@+,d0-d1/a0-a1
        rts

| d0 = offset of the body in the table, d1 = the caller's PC.
.Lcall:
        lea     pc@(_fr_data),a0
        addq.l  #1,a0@(D_INFL)
        tst.l   a0@(D_ARMED)
        beq.s   8f
        move.l  a0@(0,d0:w),a1
        move.l  d1,d0                   | the PC: in d0 for -mregparm,
        move.l  d1,sp@-                 | on the stack for the default
        jsr     a1@
        addq.l  #4,sp
        lea     pc@(_fr_data),a0
8:      subq.l  #1,a0@(D_INFL)
        rts

        .even
_fr_data:
        .space  D_SIZE
_fr_blob_end:
