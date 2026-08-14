| AmiNetXDuo, bulk moves for card memory and for a mirrored data port.
|
| VOID n68k_copy_longs(APTR to, const APTR from, ULONG longs);
| VOID n68k_port_in(APTR to, const APTR port, ULONG blocks);
| VOID n68k_port_out(APTR port, const APTR from, ULONG blocks);
|
| WHY THESE ARE NOT n68k_copy_bytes.  That routine brings the destination to a
| longword boundary with byte moves and falls to a byte loop on a 68000 whose
| two pointers disagree in parity.  Neither is allowed on the far side here:
|
|   Hydra and the ASDG LAN Rover map a 16-bit buffer whose byte lanes are not
|   separately selectable, so a byte write reaches BOTH halves on the real
|   boards even where an emulator accepts it (src/netdev/ed.c).
|
|   The X-Surf 100's 32-bit window is 128 bytes of one FIFO mirrored end to
|   end.  A byte access there is a FIFO access of the wrong width.
|
| So these move longwords and nothing narrower; the caller keeps the odd head
| and tail, which it has to spell as word accesses anyway.  Both pointers must
| be even, which is what the callers already guarantee -- a 68000 fetches a
| longword from any even address, 2 mod 4 included, at one extra bus cycle.
|
| The technique is n68k_copy.S's and the measurements there apply unchanged:
| movem.l moves eight longwords per instruction pair, and four pairs unrolled
| into a 128-byte block is where the rest of the win is.  The bus cycles are
| the same either way -- eight long accesses per pair, whatever issued them --
| so what this saves is instruction fetch and decode.  That is most of the cost
| under an emulator, which charges per instruction, and a smaller share on real
| Zorro II, where one long access is 0.28-0.56 us and the bus is the wall.
|
| n68k_port_* take 32-byte blocks and address the port at a FIXED address,
| because the port does not advance a host address -- the chip advances
| internally and every address in the window is the same FIFO.  32 and not 128
| because 32 bytes is exactly what netdev_bus_probe_wide() verifies before it
| promotes a card to NETDEV_DMODE_LONG (NETDEV_BUS_PROBE_LEN).
|
| SPDX-License-Identifier: MIT

#ifdef AMINETXDUO_NET68K_ASM

        .text
        .even
        .globl  _n68k_copy_longs
        .globl  _n68k_port_in
        .globl  _n68k_port_out
        .globl  _n68k_port_in_w
        .globl  _n68k_port_out_w

| The 68060 makes movem microcoded and non-pipelining while ordinary moves
| dual-issue, so it takes the plain loop.  Same bet, and the same lack of a
| measurement behind it, as n68k_copy.S -- see the note there.
#if defined(__mc68060__)
#define N68K_NO_MOVEM 1
#endif

| a0 = to, a1 = from, d0 = longwords
_n68k_copy_longs:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d0

        move.l  d0,d1
        lsr.l   #5,d1                   | 32-longword, 128-byte blocks
        beq     .Lcl_rest
        andi.l  #31,d0
.Lcl_big:
#ifdef N68K_NO_MOVEM
        .rept   32
        move.l  a1@+,a0@+
        .endr
#else
        movem.l a1@+,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@
        movem.l a1@+,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@(32)
        movem.l a1@+,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@(64)
        movem.l a1@+,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@(96)
        lea     a0@(128),a0
#endif
        subq.l  #1,d1
        bne     .Lcl_big

.Lcl_rest:
        tst.l   d0
        beq     .Lcl_out
.Lcl_one:
        move.l  a1@+,a0@+
        subq.l  #1,d0
        bne     .Lcl_one
.Lcl_out:
        movem.l sp@+,d2-d7/a2-a3
        rts

| a0 = to, a1 = port, d0 = 32-byte blocks
_n68k_port_in:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d0
        tst.l   d0
        beq     .Lpi_out
.Lpi:
#ifdef N68K_NO_MOVEM
        .rept   8
        move.l  a1@,a0@+
        .endr
#else
        movem.l a1@,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@
        lea     a0@(32),a0
#endif
        subq.l  #1,d0
        bne     .Lpi
.Lpi_out:
        movem.l sp@+,d2-d7/a2-a3
        rts

| a0 = to, a1 = port, d0 = 32-byte blocks
|
| The word port is ONE address and is not mirrored, so nothing can be batched
| on the port side: sixteen words are sixteen reads however they are spelled.
| What the block form removes is everything else.  The C loop compiles to a
| move, an add, a compare and a branch for every word, and this is the path
| every NE2000-family card that is not an X-Surf 100 takes for a whole frame --
| 750 iterations of it.  Pairing the reads into longwords and pushing them out
| eight at a time with movem.l leaves 16 reads, 8 swaps and one movem per 32
| bytes: the same 16 port cycles, half the memory cycles, and a quarter of the
| instructions.  Big-endian order comes out right because the first word read
| is the high half, which is what swap puts it in.
_n68k_port_in_w:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d0
        tst.l   d0
        beq     .Lpiw_out
.Lpiw:
        move.w  a1@,d2
        swap    d2
        move.w  a1@,d2
        move.w  a1@,d3
        swap    d3
        move.w  a1@,d3
        move.w  a1@,d4
        swap    d4
        move.w  a1@,d4
        move.w  a1@,d5
        swap    d5
        move.w  a1@,d5
        move.w  a1@,d6
        swap    d6
        move.w  a1@,d6
        move.w  a1@,d7
        swap    d7
        move.w  a1@,d7
        move.w  a1@,d1
        swap    d1
        move.w  a1@,d1
        movea.l d1,a2
        move.w  a1@,d1
        swap    d1
        move.w  a1@,d1
        movea.l d1,a3
#ifdef N68K_NO_MOVEM
        move.l  d2,a0@+
        move.l  d3,a0@+
        move.l  d4,a0@+
        move.l  d5,a0@+
        move.l  d6,a0@+
        move.l  d7,a0@+
        move.l  a2,a0@+
        move.l  a3,a0@+
#else
        movem.l d2-d7/a2-a3,a0@
        lea     a0@(32),a0
#endif
        subq.l  #1,d0
        bne     .Lpiw
.Lpiw_out:
        movem.l sp@+,d2-d7/a2-a3
        rts

| a0 = port, a1 = from, d0 = 32-byte blocks.  The mirror image: eight longwords
| in with one movem.l, sixteen words out to the one port address.
_n68k_port_out_w:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d0
        tst.l   d0
        beq     .Lpow_out
.Lpow:
#ifdef N68K_NO_MOVEM
        move.l  a1@+,d2
        move.l  a1@+,d3
        move.l  a1@+,d4
        move.l  a1@+,d5
        move.l  a1@+,d6
        move.l  a1@+,d7
        movea.l a1@+,a2
        movea.l a1@+,a3
#else
        movem.l a1@+,d2-d7/a2-a3
#endif
        move.l  d2,d1
        swap    d1
        move.w  d1,a0@
        move.w  d2,a0@
        move.l  d3,d1
        swap    d1
        move.w  d1,a0@
        move.w  d3,a0@
        move.l  d4,d1
        swap    d1
        move.w  d1,a0@
        move.w  d4,a0@
        move.l  d5,d1
        swap    d1
        move.w  d1,a0@
        move.w  d5,a0@
        move.l  d6,d1
        swap    d1
        move.w  d1,a0@
        move.w  d6,a0@
        move.l  d7,d1
        swap    d1
        move.w  d1,a0@
        move.w  d7,a0@
        move.l  a2,d1
        swap    d1
        move.w  d1,a0@
        move.l  a2,d1
        move.w  d1,a0@
        move.l  a3,d1
        swap    d1
        move.w  d1,a0@
        move.l  a3,d1
        move.w  d1,a0@
        subq.l  #1,d0
        bne     .Lpow
.Lpow_out:
        movem.l sp@+,d2-d7/a2-a3
        rts

| a0 = port, a1 = from, d0 = 32-byte blocks
_n68k_port_out:
        movem.l d2-d7/a2-a3,sp@-
        movea.l sp@(36),a0
        movea.l sp@(40),a1
        move.l  sp@(44),d0
        tst.l   d0
        beq     .Lpo_out
.Lpo:
#ifdef N68K_NO_MOVEM
        .rept   8
        move.l  a1@+,a0@
        .endr
#else
        movem.l a1@+,d2-d7/a2-a3
        movem.l d2-d7/a2-a3,a0@
#endif
        subq.l  #1,d0
        bne     .Lpo
.Lpo_out:
        movem.l sp@+,d2-d7/a2-a3
        rts

#endif /* AMINETXDUO_NET68K_ASM */
