| AmiNetXDuo, the green realm's m68k context switch.
| VOID _tx_green_switch(APTR *save_slot, APTR new_sp);
|
| A context is a stack whose top holds d2-d7/a2-a6 (44 bytes) under a return
| address; every green switch is solicited, so the caller-saved half and the CCR
| need nothing.  _tx_green_stack_build() (tx_amiga_green.c) lays the same 44-byte
| shape for a never-run thread, and both files say 44.
| PROTOCOL: the caller holds exactly one Forbid(); the resumed side Permit()s.
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

| First activation of a gate's side-stack frame (tx_amiga_green.c lays it: eleven
| registers with the TX_AMIGA_GATE pointer in the a2 slot, this entry as the
| return address).  The switch above has just popped it, so a2 holds the gate.
        .globl  __tx_gate_park_entry
__tx_gate_park_entry:
        move.l  a2,sp@-
        jsr     __tx_gate_park
        illegal                         | the parker must not return

#endif
