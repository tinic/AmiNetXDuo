| AmiNetXDuo, bulk moves for card memory and for a mirrored data port.
|
| VOID n68k_copy_longs(APTR to, const APTR from, ULONG longs);
| VOID n68k_port_in(APTR to, const APTR port, ULONG blocks);
| VOID n68k_port_out(APTR port, const APTR from, ULONG blocks);
|
| These are not n68k_copy_bytes.  That routine brings the destination to a
| longword boundary with byte moves, and falls to a byte loop on a 68000 whose
| two pointers disagree in parity.  Neither is allowed on the far side here:
|
|   Hydra and the ASDG LAN Rover map a 16-bit buffer whose byte lanes are not
|   separately selectable, so a byte write reaches both halves on the real
|   boards even where an emulator accepts it (src/netdev/ed.c).
|
|   The 32-bit window of the X-Surf 100 is 128 bytes of one FIFO mirrored end
|   to end.  A byte access there is a FIFO access of the wrong width.
|
| So these move longwords and nothing narrower.  The caller keeps the odd head
| and tail, which it has to spell as word accesses anyway.  Both pointers must
| be even, which is what the callers already guarantee.  A 68000 fetches a
| longword from any even address, 2 mod 4 included, at one extra bus cycle.
|
| The technique is that of n68k_copy.S and the measurements there apply
| unchanged: movem.l moves eight longwords per instruction pair, and four pairs
| unrolled into a 128-byte block is where the rest of the win is.  The bus
| cycles are the same either way, eight long accesses per pair whatever issued
| them, so what this saves is instruction fetch and decode.  That is most of
| the cost under an emulator, which charges per instruction, and a smaller
| share on real Zorro II, where one long access is 0.28-0.56 us and the bus is
| the wall.
|
| n68k_port_* take 32-byte blocks and address the port at a fixed address,
| because the port does not advance a host address.  The chip advances
| internally and every address in the window is the same FIFO.  The block is 32
| bytes and not 128 because 32 bytes is exactly what netdev_bus_probe_wide()
| checks before it promotes a card to NETDEV_DMODE_LONG (NETDEV_BUS_PROBE_LEN).
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
| dual-issue, so it takes the plain loop.  The same bet, and the same lack of a
| measurement behind it, as n68k_copy.S.  See the note there.
|
| This branch is unreached in what ships.  The default build is
| AMINETXDUO_CPU=any: -m68000 codegen, so __mc68060__ is never defined and
| every machine gets the movem form.  That form is correct everywhere, because
| movem.l is an original 68000 instruction, and what a 68060 loses is an
| untested tuning guess.
|
| A run-time choice is what src/net68k/n68k_cpu.c does for the checksum and the
| copy, and it needs the same here: both forms assembled under suffixed names,
| and a vector set from AttnFlags at device init, because anxnet.device is its
| own binary and never opens the library that selects.  That is worth doing
| once somebody measures an 060 with a real card, and not before, because the
| bet it settles has never had a number behind it.
|
| It has since been run there.  The shipping binary, this movem form built
| -m68000, ran on an A3000 at 68060 with an A2065: rc=0, read 4651 KB/s, write
| 2372, no exception in the log.  The 68060-targeted build measured 4678/2268
| in a different block, which under the interleaving rule means only that there
| is no large difference either way.  So the movem form is correct and not
| slower on an 060, and the dispatch this note describes stays unbuilt.
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
| The word port is one address and is not mirrored, so nothing can be batched
| on the port side: sixteen words are sixteen accesses however they are
| spelled.  What is left is instruction count, and the answer is the plainest
| one, sixteen moves and two of loop.
|
| The first version paired the reads into longwords and pushed them out eight
| at a time with movem.l, on the theory that halving the memory-side cycles was
| worth something.  Building the longwords costs a move.l and a swap each: 33
| instructions per 16 words against 18 here.  cnet.device, the driver of the
| PCMCIA card itself, spells it the plain way and enters a 16-way block by
| computed jump for the remainder.  Disassembling it is what showed the pairing
| was a loss.  The bus cycles were never in question, only the instructions
| around them, which are most of the cost under an emulator and a real if
| smaller one on a 68020 with a 256-byte cache.
_n68k_port_in_w:
        movea.l sp@(4),a0
        movea.l sp@(8),a1
        move.l  sp@(12),d0
        tst.l   d0
        beq     .Lpiw_out
.Lpiw:
        .rept   16
        move.w  a1@,a0@+
        .endr
        subq.l  #1,d0
        bne     .Lpiw
.Lpiw_out:
        rts

| a0 = port, a1 = from, d0 = 32-byte blocks.  The reverse of the above.
_n68k_port_out_w:
        movea.l sp@(4),a0
        movea.l sp@(8),a1
        move.l  sp@(12),d0
        tst.l   d0
        beq     .Lpow_out
.Lpow:
        .rept   16
        move.w  a1@+,a0@
        .endr
        subq.l  #1,d0
        bne     .Lpow
.Lpow_out:
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

| ULONG n68k_port_in_w_sum(void *to, const volatile void *port, ULONG bytes)
|
| Drain `bytes` of frame from a fixed 16-bit data port into `to`, summing as
| the words go past: the 32-bit ones-complement longword sum with per-add
| end-around carry and a zero-padded, byte-positioned tail -- exactly what
| n68k_copy_sum_longwords() computes over the same bytes, because the
| verifier that consumes the answer cannot tell who produced it.  The
| destination must be even; the caller guarantees it (the receive slot's
| payload pointer is longword aligned by construction).
        .globl  _n68k_port_in_w_sum
_n68k_port_in_w_sum:
        movem.l d2-d5,sp@-
        movea.l sp@(20),a0              | to
        movea.l sp@(24),a1              | port
        move.l  sp@(28),d0              | bytes
        moveq   #0,d1                   | sum
        moveq   #0,d5                   | ADDX source: add carry only

        move.l  d0,d3
        lsr.l   #2,d3                   | whole longwords
        move.l  d3,d4
        and.l   #3,d4                   | longwords after four-way blocks
        lsr.l   #2,d3
        beq     .Lpws_rest
.Lpws_block:
| Four longwords per branch.  ADD sets X to the carry and ADDX with a zero
| source folds it back without the taken/not-taken branch the old loop paid
| once per longword.  MOVE does not change X, and the next ADD establishes it
| afresh, so the four copies can be written literally without flag plumbing.
        .rept   4
        move.w  a1@,d2
        swap    d2
        move.w  a1@,d2                  | d2 = (w0 << 16) | w1
        move.l  d2,a0@+
        add.l   d2,d1
        addx.l  d5,d1                   | end-around carry, per add
        .endr
        subq.l  #1,d3
        bne     .Lpws_block
.Lpws_rest:
        tst.l   d4
        beq     .Lpws_tail
.Lpws_one:
        move.w  a1@,d2
        swap    d2
        move.w  a1@,d2
        move.l  d2,a0@+
        add.l   d2,d1
        addx.l  d5,d1
        subq.l  #1,d4
        bne     .Lpws_one
.Lpws_tail:
        and.l   #3,d0                   | 0..3 bytes remain
        beq     .Lpws_done
        moveq   #0,d2
        move.w  a1@,d3                  | first tail word off the port
        cmp.l   #1,d0
        beq     .Lpws_t1
        cmp.l   #2,d0
        beq     .Lpws_t2
| three bytes: the word, then the next word's high byte, low lane zero.
| The isolation is done before the byte is positioned, not after: `move.w`
| leaves the upper half of d3 holding the word just consumed, and shifting
| that left would OR a whole stale word into the answer.  A three-byte tail
| is what a 331-byte payload has, which is what a DHCP offer is, so the
| frames this corrupted were exactly the ones a machine needs to boot.
        move.w  d3,a0@+
        swap    d3
        move.l  d3,d2                   | word << 16
        move.w  a1@,d3
        and.l   #0xff00,d3              | the byte alone, already at 15..8
        or.l    d3,d2
        lsr.l   #8,d3                   | and in the low lane to be stored
        move.b  d3,a0@                  | stored as the third byte
        bra     .Lpws_tadd
.Lpws_t1:
| one byte: the word's high byte, positioned as bits 31..24.  The swap is
| the whole trick: b0 << 8 in the low half becomes b0 << 24 in the high.
        move.l  d3,d2
        and.l   #0xff00,d2
        swap    d2
        lsr.w   #8,d3
        move.b  d3,a0@
        bra     .Lpws_tadd
.Lpws_t2:
| two bytes: the word, positioned in the upper half
        move.w  d3,a0@+
        swap    d3
        move.l  d3,d2
        bra     .Lpws_tadd
.Lpws_tadd:
        add.l   d2,d1
        addx.l  d5,d1
.Lpws_done:
        move.l  d1,d0
        movem.l sp@+,d2-d5
        rts

#endif /* AMINETXDUO_NET68K_ASM */
