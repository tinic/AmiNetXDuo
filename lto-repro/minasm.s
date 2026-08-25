| TU 4: hand-written 68k assembly, the interface LTO cannot see into.
|   LONG min_asm_sum(register const UBYTE *p __asm("a0"),
|                    register LONG n __asm("d0"));
        .text
        .globl  _min_asm_sum
_min_asm_sum:
        movem.l %d2,-(%sp)
        moveq   #0,%d2
        tst.l   %d0
        ble.b   .Ldone
        subq.l  #1,%d0
.Lloop:
        moveq   #0,%d1
        move.b  (%a0)+,%d1
        add.l   %d1,%d2
        dbra    %d0,.Lloop
.Ldone:
        move.l  %d2,%d0
        movem.l (%sp)+,%d2
        rts
