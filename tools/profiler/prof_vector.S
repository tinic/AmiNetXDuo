| Profile, the level-4 autovector interposer.
|
| This runs as the FIRST instruction after the CPU takes the interrupt
| exception, which is the only reason the profile can be trusted.  At entry SP
| is the supervisor stack pointer and points at the frame the CPU has just
| built and nothing else:
|
|       0(SP)   SR                      word
|       2(SP)   PC                      long
|       6(SP)   format/vector word      68010 and up only
|
| Identical on 68000, 68010, 68020, 68030, 68040 and 68060, the later chips
| append a word, they do not move SR or PC.  Nothing here needs to know which
| CPU it is on.  The format word is recorded anyway so prof_write() can assert
| it was $0 for every sample, which is what would catch a 68020 throwaway
| frame (format $1, pushed when SR's M bit is set) rather than silently
| reporting an address two bytes into the wrong place.
|
| An Exec interrupt server cannot do this.  By the time the AddIntServer()
| dispatcher reaches a server it has pushed d0/d1/a0/a1/a5/a6 over the frame
| and the distance back to it is undocumented, and a guessed offset yields a
| valid-looking address, not a crash.
|
| THE TIMESTAMP is one `move.l 0xdff004,d1`: VPOSR in the high word, VHPOSR in
| the low.  The beam position is the only free-running counter a plain OCS
| 68000 machine has that can be read in a single instruction with no latch to
| disturb and no chip to acknowledge.  All the decoding, vpos, hpos,
| unwrapping across frames, is host side, so the cost here is two
| instructions and the handler stays short enough not to distort what it is
| measuring.  ReadEClock() was the alternative and is a library call that
| Disable()s, from a context where Exec does not yet know an interrupt is in
| progress; that is a lot of risk for 1.4 us of resolution over 0.56.
|
| Registers: the interrupted code's registers are all still live here, so
| everything touched is saved and restored.  CCR is not, deliberately: the
| interrupted CCR is inside the saved SR on the stack and comes back with the
| chained handler's RTE.
|
| Chaining: SP is put back on the frame, the saved vector is pushed and RTS'd
| to.  Exec's own dispatch then runs exactly as before, so every other level-4
| source still works.
|
| SPDX-License-Identifier: MIT

        .text
        .even

| ---------------------------------------------------------------------------
| The vector itself.  16 bytes of registers, so the frame is at sp@(16).
| ---------------------------------------------------------------------------
        .globl  _prof_vector
_prof_vector:
        movem.l d0-d1/a0-a1,sp@-        | 16 bytes: the frame is now at sp@(16)
        addq.l  #1,_prof_hits
        move.l  _prof_next,a0
        cmp.l   _prof_limit,a0
        bcc     .Lprof_full
        move.l  0xdff004,d1             | VPOSR:VHPOSR, before anything else
        move.l  _prof_taskptr,a1
        move.l  a1@,d0                  | SysBase->ThisTask
        move.l  sp@(18),a0@+            | ps_PC     = 16 + 2
        move.w  sp@(16),a0@+            | ps_SR     = 16 + 0
        move.w  sp@(22),a0@+            | ps_Format = 16 + 6
        move.l  d0,a0@+                 | ps_Task
        move.l  d1,a0@+                 | ps_Time
        move.l  a0,_prof_next
        bra     .Lprof_out
.Lprof_full:
        addq.l  #1,_prof_dropped
.Lprof_out:
        movem.l sp@+,d0-d1/a0-a1        | SP is back on the frame
        move.l  _prof_chain,sp@-
        rts                             | ...and back on it again, in Exec

| ---------------------------------------------------------------------------
| The cia.resource ICR vector, for the fallback sources.  cia.resource's own
| level-N server reads the ICR, which is what acknowledges the chip, and
| then calls this.  All the sampling has already happened above, so there is
| nothing to do but count, which is worth doing: prof_hits counts every
| interrupt on the level and this counts only ours, so the two together say
| how much of the profile came from some other source on the same level.
| ---------------------------------------------------------------------------
        .globl  _prof_cia_stub
_prof_cia_stub:
        addq.l  #1,_prof_ownticks
        rts

| ---------------------------------------------------------------------------
| The audio interrupt handler, installed with SetIntVector().
|
| It clears INTREQ itself rather than trusting Exec's dispatcher to have done
| it.  Not defensive programming: leaving it to Exec hung the machine solidly
| every time, the request bit stayed set, the interrupt re-entered as fast
| as the CPU could return from it, and the run timed out with no output at all.
|
| The bit is a variable because the channel is chosen at run time: a program
| under test may want channel 3 for itself, in which case the profiler moves
| rather than fighting it.
| ---------------------------------------------------------------------------
        .globl  _prof_audio_stub
_prof_audio_stub:
        addq.l  #1,_prof_ownticks
        move.w  _prof_intf,0xdff09c     | INTREQ &= ~INTF_AUDx
        rts

| ---------------------------------------------------------------------------
| ULONG prof_read_vbr(VOID), via Supervisor(), so it must end in RTE.
|
| MOVEC VBR,D0 is hand-encoded because this file is also assembled for the
| 68000 profile, where the assembler rejects the mnemonic outright.  It is
| never CALLED there: prof.c tests AttnFlags for AFF_68010 first.
| ---------------------------------------------------------------------------
        .globl  _prof_read_vbr
_prof_read_vbr:
        .short  0x4e7a,0x0801           | movec vbr,d0
        .short  0x4e73                  | rte
