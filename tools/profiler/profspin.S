| profspin -- known-hot kernels, for proving the profiler against a program it
| is not part of.
|
| These are assembly and not C for one reason: the check they exist for is "did
| the sample land inside this exact function", and that needs the exact byte
| range.  A compiler is free to inline, reorder, fold two of these together or
| split one across the section; an explicit _end label after each is not.
|
| Each kernel is a self-contained loop with no calls, so during its phase
| essentially every sample must land inside it.  If the vector read the frame
| at the wrong offset the recorded value would be half the SR concatenated with
| half the PC -- an address nowhere near these ranges -- so the in-range
| fraction is a sharp test rather than a plausible-looking one.
|
| SPDX-License-Identifier: MIT

        .text
        .even

| ---------------------------------------------------------------------------
| VOID spin_a(ULONG reps) -- two instructions per iteration.
| ---------------------------------------------------------------------------
        .globl  _spin_a
        .globl  _spin_a_end
_spin_a:
        move.l  sp@(4),d0
        beq     .Lspin_a_out
.Lspin_a:
        subq.l  #1,d0
        bne     .Lspin_a
.Lspin_a_out:
        rts
_spin_a_end:

| ---------------------------------------------------------------------------
| VOID spin_b(ULONG reps) -- three, and a different body so that nothing
| downstream can fold it onto spin_a.
| ---------------------------------------------------------------------------
        .globl  _spin_b
        .globl  _spin_b_end
_spin_b:
        move.l  sp@(4),d0
        beq     .Lspin_b_out
        moveq   #0,d1
.Lspin_b:
        add.l   d0,d1
        subq.l  #1,d0
        bne     .Lspin_b
.Lspin_b_out:
        rts
_spin_b_end:

| ---------------------------------------------------------------------------
| VOID spin_c(ULONG reps) -- five.  d2 and up are callee-saved, so this stays
| inside the scratch registers.
| ---------------------------------------------------------------------------
        .globl  _spin_c
        .globl  _spin_c_end
_spin_c:
        move.l  sp@(4),d0
        beq     .Lspin_c_out
        moveq   #0,d1
.Lspin_c:
        add.l   d0,d1
        eor.l   d0,d1
        add.l   d1,d1
        subq.l  #1,d0
        bne     .Lspin_c
.Lspin_c_out:
        rts
_spin_c_end:
