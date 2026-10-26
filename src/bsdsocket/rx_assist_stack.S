| Stack-argument ABI even with -mregparm=3. All original-stack accesses
| precede the first swap or follow the second; no process-global state.
| SPDX-License-Identifier: MIT
        .text
        .even
        .globl _bsd_rx_stack_call
_bsd_rx_stack_call:
        movem.l d2/a2-a3/a6,sp@-
        movea.l sp@(20),a2
        movea.l sp@(24),a3
        move.l  sp@(28),d2
        movea.l 4:w,a6
        movea.l a2,a0
        jsr     a6@(-732)
        move.l  d2,sp@-
        jsr     a3@
        addq.l  #4,sp
        move.l  d0,d2
        movea.l a2,a0
        jsr     a6@(-732)
        move.l  d2,d0
        movem.l sp@+,d2/a2-a3/a6
        rts
