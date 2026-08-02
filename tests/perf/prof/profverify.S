| AmiNetXDuo -- known-hot kernels for the profiler's self-test.
|
| These are assembly and not C for one reason: the check they exist for is
| "did the sample land inside this exact function", and that needs the exact
| byte range.  A compiler is free to inline, reorder, fold two of these
| together or split one across the section; an explicit _end label after each
| is not.  profverify.c computes [_pv_spin_x, _pv_spin_x_end) and counts.
|
| Each kernel is a self-contained loop with no calls, so during its phase
| essentially every sample must land inside it.  If the vector read the frame
| at the wrong offset the recorded value would be half the SR concatenated
| with half the PC -- an address nowhere near these ranges -- so the in-range
| fraction is a sharp test rather than a plausible-looking one.
|
| SPDX-License-Identifier: MIT

        .text
        .even

| ---------------------------------------------------------------------------
| VOID pv_spin_a(ULONG reps) -- two instructions per iteration.
| ---------------------------------------------------------------------------
        .globl  _pv_spin_a
        .globl  _pv_spin_a_end
_pv_spin_a:
        move.l  sp@(4),d0
        beq     .Lpv_a_out
.Lpv_a:
        subq.l  #1,d0
        bne     .Lpv_a
.Lpv_a_out:
        rts
_pv_spin_a_end:

| ---------------------------------------------------------------------------
| VOID pv_spin_b(ULONG reps) -- three, and a different body so that nothing
| downstream can fold it onto pv_spin_a.
| ---------------------------------------------------------------------------
        .globl  _pv_spin_b
        .globl  _pv_spin_b_end
_pv_spin_b:
        move.l  sp@(4),d0
        beq     .Lpv_b_out
        moveq   #0,d1
.Lpv_b:
        add.l   d0,d1
        subq.l  #1,d0
        bne     .Lpv_b
.Lpv_b_out:
        rts
_pv_spin_b_end:

| ---------------------------------------------------------------------------
| VOID pv_spin_c(ULONG reps) -- five.  d2 and up are callee-saved, so this
| stays inside the scratch registers.
| ---------------------------------------------------------------------------
        .globl  _pv_spin_c
        .globl  _pv_spin_c_end
_pv_spin_c:
        move.l  sp@(4),d0
        beq     .Lpv_c_out
        moveq   #0,d1
.Lpv_c:
        add.l   d0,d1
        eor.l   d0,d1
        add.l   d1,d1
        subq.l  #1,d0
        bne     .Lpv_c
.Lpv_c_out:
        rts
_pv_spin_c_end:
