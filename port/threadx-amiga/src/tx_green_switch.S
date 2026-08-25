| AmiNetXDuo, the green realm's m68k context switch.
|
| VOID _tx_green_switch(APTR *save_slot, APTR new_sp);
|
| Save the callee-saved half of the machine on the current stack, park SP
| through save_slot, adopt new_sp, restore the same shape, return.  What a
| context IS, in this port: a stack whose top holds d2-d7/a2-a6 (44 bytes)
| under a return address.  d0-d1/a0-a1 and the CCR are caller-saved in the
| m68k ABI and dead across any call, so a SOLICITED switch -- and every green
| switch is solicited, the realm has no preemptive entry into a green context
| -- needs nothing else.  No FP state: the tree is built -m68000/-m68020 with
| no FPU code in the stack.
|
| The initial frame of a never-run thread is the same shape with zeroed
| registers and _tx_green_thread_begin as the return address; it is laid down
| by _tx_green_stack_build() (tx_amiga_green.c), so exactly one place besides
| this file knows the layout, and both say 44.
|
| PROTOCOL (tx_amiga_internal.h): the caller holds exactly one Forbid();
| the resumed side -- the code after ITS switch call, or the begin shim --
| Permit()s.  The switch itself therefore neither touches TDNestCnt nor cares
| that both contexts live in one Exec Task, which is the whole trick.
|
| SPDX-License-Identifier: MIT

#ifdef AMINETXDUO_GREEN_REALM

        .text
        .even
        .globl  __tx_green_switch

| sp@(4) = save_slot, sp@(8) = new_sp before the push; +44 after it.
__tx_green_switch:
        movem.l d2-d7/a2-a6,sp@-        | 11 registers, 44 bytes
        movea.l sp@(48),a0              | save_slot
        move.l  sp,a0@
        movea.l sp@(52),sp              | the other context
        movem.l sp@+,d2-d7/a2-a6
        rts

#endif
