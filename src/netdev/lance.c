/*
 * anxnet.device: the Am7990/Am79C960 LANCE, for the A2065 and the Ariadne.
 *
 * Both cards put the chip's two registers (RAP selects, RDP reads and writes)
 * and a 32 KB SRAM in the Zorro window.  The rings, the buffers and the init
 * block all live in that SRAM, so every address the chip is given is an offset
 * within it.
 *
 * The Ariadne's SRAM port is wired crossed, so every descriptor and init-block
 * word must be pre-swapped (card->lance_swap).  Frame data is unaffected: the
 * crossing cancels between the CPU's write and the chip's read.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_nic.h"
#include "netdev_cards.h"
#include "netdev_bsdtypes.h"
#include "lancereg.h"
#include "dp8390.h"     /* the DP8390_TX_* return codes are the shared contract */
#include "n68k_iocopy.h"

#ifdef NETDEV_TRACE
extern VOID netdev_trace_val(const char *tag, ULONG v);
#define LE_TRACE(t, v)  netdev_trace_val((t), (ULONG)(v))
#else
#define LE_TRACE(t, v)  ((VOID)0)
#endif

/* ------------------------------------------------------- shared RAM ------ */

/*
 * WHY RECEIVE COSTS TWO COPIES ON THIS BOARD, AND WHY THAT IS THE FLOOR.
 *
 * The descriptors below hold `LE_RXB_OFF + i * LE_BUFSZ` -- OFFSETS INTO THE
 * CARD'S OWN SRAM, not Amiga bus addresses.  The Am7990's address space is the
 * 32 KB behind mem_off; it cannot be pointed at host memory.  So a received
 * frame is always DMAed into board SRAM first, and getting it into an NX_PACKET
 * is a copy the hardware requires.  The second copy is the POSIX boundary:
 * recv() puts bytes in the caller's buffer.
 *
 * Both are therefore structural, and both are at their measured floor -- the
 * fused copy+checksum and the packet-to-application copy are hand-written asm,
 * benchmarked seven ways per CPU in bench/sumbench.c, and since dd818cc7 both
 * ends of the first one are longword aligned.
 *
 * DO NOT CITE RX_DIRECT_COMPLETE AS "the one-copy design, refuted".  It is
 * nothing of the kind: CMakeLists.txt:919 describes it as completing a waiting
 * stream recv() on the IP thread, and its own note says what it buys applies to
 * 0.35% of dequeues against arm-and-disarm bookkeeping on every blocking recv.
 * It is a LATENCY option.  Removing a copy was never tried because on this
 * board there is no copy available to remove.
 */

/*
 * The rings and buffers, as offsets into the board's SRAM.  Eight receive
 * buffers is a frame every 1.2 ms at 10 Mbit before the ring can overrun.
 */
#define LE_RX_LOG       3
#define LE_RX_RING      (1u << LE_RX_LOG)       /* 8 */
#define LE_TX_LOG       2
#define LE_TX_RING      (1u << LE_TX_LOG)       /* 4 */
#define LE_BUFSZ        1536

#define LE_INIT_OFF     0x0000                  /* 24 bytes */
#define LE_RXD_OFF      0x0020                  /* 8 x 8 */
#define LE_TXD_OFF      0x0060                  /* 4 x 8 */

/*
 * THE RECEIVE BUFFERS START TWO BYTES IN, AND THAT IS THE POINT.
 *
 * What the receive path reads out of this buffer is not the frame, it is the
 * PAYLOAD: netdev_payload() hands the copy hook `frame + 14`
 * (netdev_event.c:145).  A buffer at 0 mod 4 therefore delivers a source
 * pointer at 2 mod 4, and _n68k_copy_sum_longwords -- the fused copy and
 * checksum, the largest single item on the receive profile at 15% -- reads it
 * with `movem.l a1@+` (n68k_checksum.S:345).
 *
 * That routine has NO aligned/unaligned split.  It issues the same longword
 * accesses either way, so a source at 2 mod 4 does not take a different code
 * path, it takes the CPU's misaligned-longword penalty on every load.
 * bench/sumbench.c timed it at the real phase, 200 x 1460 bytes on an A1200:
 *
 *     variant     aligned      src +2       cost
 *     lm14        148.3 ns/B   203.4 ns/B   +37.2%
 *     ldmovem     159.9        207.9        +30.0%
 *
 * Two earlier attempts to recover that both moved the DESTINATION and both
 * lost: AMI_SANA2_RX_PAD 0 trades this for a misaligned IP header and a
 * misaligned packet->app copy, and reading aligned longwords from `from - 2`
 * and shift-combining them measured 425.5 ns/B against 199.1 (af07fc38).
 * Neither considered moving the SOURCE, which costs nothing at all: the Am7990
 * takes a byte-granular buffer address in RMD0/RMD1 and this board's SRAM is
 * 16 bits wide, so any EVEN start is as good to the chip as any other.  Two
 * bytes in puts `frame + 14` at 0 mod 4 against a destination already there
 * (AMI_SANA2_RX_PAD 2 plus the 14-byte header is 16).
 *
 * Everything else that reads this buffer stays word-aligned and legal on a
 * 68000: the type at `frame + 12` is a word at 2 mod 4, the addresses go
 * through three word moves, and the broadcast test's one longword read at
 * 2 mod 4 is even, so it is two bus cycles as it always was.
 *
 * MEASURED, playhouse3/a2065, clean build per arm with the library and device
 * md5s printed before a round ran, six rounds alternating which arm went
 * first, all twelve rc=0:
 *
 *     medians      before       after       delta
 *     tcp-rx       5,564,476    5,736,227   +3.09%
 *       position 1 5,529,779    5,863,958   +6.04%
 *       position 2 5,599,174    5,693,392   +1.68%
 *     tcp-tx       3,115,306    3,123,097   +0.25%
 *
 * The flat transmit is the control, not a disappointment: the transmit buffers
 * keep their old phase, so a receive-only change is exactly what should show
 * here.  Predicted 4.1% (15.1% of the profile times the 27.1% penalty) against
 * 3.09% measured -- short frames carry the same per-call overhead over fewer
 * bytes, and the ACKs a bulk receive sends are all short.
 *
 * AND FITZ DOES NOT MOVE AT ALL, which is the more useful half of the result.
 * Six sittings an arm, alternated: read 3,500.5 -> 3,489.0 (-0.33%), write
 * 2,601 -> 2,598 (-0.12%), ranges overlapping throughout.
 *
 * That is the OPPOSITE of 792a8cdf, which bought +3% on iperf and +9% on Fitz,
 * and the pair of results says what each workload is actually bound by:
 *
 *     saving                     iperf bulk      Fitz        transmit
 *     per-wake / latency         +3%             +9%         +5.3%
 *     per-byte (this change)     +3.09%          flat        flat
 *
 * Fitz reads 32 KB chunks over a request and a response, so its clock is
 * dominated by round trips rather than by the cost of a byte; iperf bulk is
 * the other way round.  So "Fitz gains about three times what iperf does" is
 * NOT a property of this tree -- it was a property of that one change, and
 * this one shows the ratio going the other way.  Expect a per-byte win to show
 * on bulk receive and nowhere else.
 */
#define LE_RXB_PHASE    2
#define LE_RXB_OFF      (0x0100 + LE_RXB_PHASE)
/*
 * Past the end of the receive region and back on a longword.
 *
 * TRANSMIT HAS THE SAME SKEW AND THIS DOES NOT FIX IT.  The frame is built in
 * this buffer from byte 0, so the CopyFrom hook's DESTINATION is txbuf + 14,
 * two bytes out of phase exactly as the receive source was -- the penalty
 * moves to the store side instead of the load side.  It is left alone here on
 * purpose: it is a separate measurement on a direction that carries about half
 * the bytes, and folding it into the same arm would make one A/B answer two
 * questions.  Do not read this line as "transmit is already aligned".
 */
#define LE_TXB_OFF      (0x0100 + LE_RX_RING * LE_BUFSZ + 4)
#define LE_END          (LE_TXB_OFF + LE_TX_RING * LE_BUFSZ)

/*
 * The phase is load-bearing, so it is checked rather than trusted.  A buffer
 * size that is not a multiple of four would give each buffer in the ring a
 * different phase, and only the first would be aligned.
 */
#if ((LE_RXB_OFF + NETDEV_HDR_LEN) & 3u) != 0
#error "LE_RXB_OFF must put the Ethernet payload (frame + NETDEV_HDR_LEN) on a \
longword: that alignment is worth 27-37% of the fused copy, which is 15% of \
receive"
#endif
#if (LE_BUFSZ & 3u) != 0
#error "LE_BUFSZ must be a multiple of 4 or the buffers in the ring do not \
share the phase LE_RXB_OFF was chosen for"
#endif
#if (LE_RXB_OFF & 1u) != 0
#error "an odd buffer start is an address error on a 68000 and a word the \
LANCE cannot place"
#endif
#if LE_TXB_OFF < (LE_RXB_OFF + LE_RX_RING * LE_BUFSZ)
#error "the transmit buffers overlap the receive ring"
#endif

static volatile UBYTE *le_ram(NetdevNic *nic)
{
    return nic->board + nic->card->mem_off;
}

/*
 * The board's crossing applies to every 16-bit access through the window, not
 * only to the SRAM: on the Ariadne a word written to a chip register arrives
 * with its halves exchanged.  Frame data needs no swap at any width.
 */
static UWORD le_swap(NetdevNic *nic, UWORD v)
{
    return nic->card->lance_swap ? (UWORD)((v >> 8) | (v << 8)) : v;
}

/* A descriptor or init-block word, in the order the chip reads it. */
static VOID le_put16(NetdevNic *nic, ULONG off, UWORD v)
{
    volatile UWORD *p = (volatile UWORD *)(volatile void *)(le_ram(nic) + off);

    *p = le_swap(nic, v);
}

static UWORD le_get16(NetdevNic *nic, ULONG off)
{
    const volatile UWORD *p =
        (const volatile UWORD *)(const volatile void *)(le_ram(nic) + off);

    return le_swap(nic, *p);
}

/* ---------------------------------------------------------- registers ---- */

/*
 * RAP selects, RDP reads and writes.  netdev_bus's byte accessors are not used
 * here: a LANCE register is a word and half of one is not a register.
 */
#ifndef LANCE_CSR_GET
static volatile UWORD *le_rdp(NetdevNic *nic)
{
    return (volatile UWORD *)(volatile void *)
           (nic->board + nic->card->reg_off);
}

static volatile UWORD *le_rap(NetdevNic *nic)
{
    return (volatile UWORD *)(volatile void *)
           (nic->board + nic->card->reg_off + 2);
}

static UWORD le_csr_get(NetdevNic *nic, UWORD csr)
{
    *le_rap(nic) = le_swap(nic, csr);
    return le_swap(nic, *le_rdp(nic));
}

static VOID le_csr_put(NetdevNic *nic, UWORD csr, UWORD v)
{
    *le_rap(nic) = le_swap(nic, csr);
    *le_rdp(nic) = le_swap(nic, v);
}

/* A RAP/RDP pair is stateful rather than memory.  The seams let the host test
   model its write-one-to-clear and INIT behaviour; they compile to the two
   direct helpers above in the device. */
#define LANCE_CSR_GET(nic, csr)       le_csr_get((nic), (csr))
#define LANCE_CSR_PUT(nic, csr, val)  le_csr_put((nic), (csr), (val))
#endif

LONG lance_init(NetdevNic *nic);

/* ---------------------------------------------------------------- stop --- */

VOID lance_halt(NetdevNic *nic)
{
    LANCE_CSR_PUT(nic, LE_CSR0, LE_C0_STOP);
    nic->running = FALSE;
}

/*
 * STOP and re-INIT, which is the only recovery this chip has.  lance_init()
 * rebuilds the rings in the board's SRAM, zeroes txb_inuse, and bounds its own
 * IDON wait.
 */
VOID lance_reset(NetdevNic *nic)
{
    nic->resets++;
    lance_halt(nic);
    (VOID)lance_init(nic);
}

/* -------------------------------------------------------------- filter --- */

/*
 * The logical address filter lives in the init block, so a multicast change is
 * a re-initialisation.  No register holds the filter while the chip runs.
 */
static VOID le_write_init(NetdevNic *nic)
{
    ULONG rxd = (ULONG)LE_RXD_OFF;
    ULONG txd = (ULONG)LE_TXD_OFF;
    UWORD i;

    le_put16(nic, LE_INIT_OFF + 0, nic->promisc ? LE_MODE_PROM : 0);

    /* PADR, low byte first: the chip reads the address little-endian. */
    for (i = 0; i < 3; i++)
    {
        le_put16(nic, LE_INIT_OFF + 2 + i * 2,
                 (UWORD)(nic->mac[i * 2] | (nic->mac[i * 2 + 1] << 8)));
    }

    for (i = 0; i < 4; i++)
    {
        le_put16(nic, LE_INIT_OFF + 8 + i * 2,
                 (UWORD)(nic->mar[i * 2] | (nic->mar[i * 2 + 1] << 8)));
    }

    LE_TRACE("le: padr ", ((ULONG)nic->mac[0] << 24) | ((ULONG)nic->mac[1] << 16) |
                          ((ULONG)nic->mac[2] << 8) | nic->mac[3]);
    le_put16(nic, LE_INIT_OFF + 16, (UWORD)(rxd & 0xffff));
    le_put16(nic, LE_INIT_OFF + 18,
             (UWORD)(((rxd >> 16) & 0xff) | (LE_RX_LOG << 13)));
    le_put16(nic, LE_INIT_OFF + 20, (UWORD)(txd & 0xffff));
    le_put16(nic, LE_INIT_OFF + 22,
             (UWORD)(((txd >> 16) & 0xff) | (LE_TX_LOG << 13)));
}

VOID lance_setfilter(NetdevNic *nic)
{
    BOOL was = nic->running;

    /* The logical filter can only be changed by INIT, which rebuilds both
       rings.  Do not throw away descriptors the chip still owns: the shell
       has already completed those writes to their callers.  le_tint() applies
       the latest filter as soon as the ring is empty. */
    if (was && nic->txb_inuse != 0)
    {
        nic->filter_pending = TRUE;
        return;
    }

    nic->filter_pending = FALSE;
    lance_halt(nic);
    le_write_init(nic);
    if (was)
        (VOID)lance_init(nic);
}

/* ---------------------------------------------------------------- init --- */

static VOID le_rings(NetdevNic *nic)
{
    UWORD i;

    for (i = 0; i < LE_RX_RING; i++)
    {
        ULONG d   = LE_RXD_OFF + (ULONG)i * 8;
        ULONG buf = LE_RXB_OFF + (ULONG)i * LE_BUFSZ;

        le_put16(nic, d + 0, (UWORD)(buf & 0xffff));
        le_put16(nic, d + 2, (UWORD)(LE_R1_OWN | ((buf >> 16) & 0xff)));
        /* BCNT is two's complement, and the unused high bits read as ones. */
        le_put16(nic, d + 4, (UWORD)(0xf000 | ((UWORD)(-LE_BUFSZ) & 0x0fff)));
        le_put16(nic, d + 6, 0);
    }

    for (i = 0; i < LE_TX_RING; i++)
    {
        ULONG d   = LE_TXD_OFF + (ULONG)i * 8;
        ULONG buf = LE_TXB_OFF + (ULONG)i * LE_BUFSZ;

        le_put16(nic, d + 0, (UWORD)(buf & 0xffff));
        le_put16(nic, d + 2, (UWORD)((buf >> 16) & 0xff));   /* not OWN */
        le_put16(nic, d + 4, 0xf000);
        le_put16(nic, d + 6, 0);
    }

    nic->rx_next  = 0;
    nic->tx_next  = 0;
    nic->tx_done  = 0;
    nic->txb_inuse = 0;
}

LONG lance_init(NetdevNic *nic)
{
    UWORD n = 1000;
    UWORD csr0;

    LANCE_CSR_PUT(nic, LE_CSR0, LE_C0_STOP);

    le_rings(nic);
    le_write_init(nic);
    nic->filter_pending = FALSE;

    /* Where the init block is, split across CSR1 and CSR2. */
    LANCE_CSR_PUT(nic, LE_CSR1, (UWORD)(LE_INIT_OFF & 0xffff));
    LANCE_CSR_PUT(nic, LE_CSR2, (UWORD)((LE_INIT_OFF >> 16) & 0xff));
    LANCE_CSR_PUT(nic, LE_CSR3, 0);     /* no BSWP: the board does the lanes */

    LANCE_CSR_PUT(nic, LE_CSR0, LE_C0_INIT);

    /*
     * IDON says the chip has read the init block: a DMA of 24 bytes from RAM
     * the CPU just wrote, so the wait is short.  The bound exists so a card
     * that never answers cannot hang the machine.
     */
    do
    {
        csr0 = LANCE_CSR_GET(nic, LE_CSR0);
    }
    while ((csr0 & (LE_C0_IDON | LE_C0_ERR)) == 0 && --n != 0);

    LE_TRACE("le: init csr0 ", csr0);
    if ((csr0 & LE_C0_IDON) == 0)
        return -1;

    LANCE_CSR_PUT(nic, LE_CSR0, LE_C0_IDON);        /* acknowledge */
    LANCE_CSR_PUT(nic, LE_CSR0, LE_C0_STRT | LE_C0_INEA);

    nic->running = TRUE;
    LE_TRACE("le: running ", LANCE_CSR_GET(nic, LE_CSR0));

    return 0;
}

/* ------------------------------------------------------------- receive --- */

static VOID le_rint(NetdevNic *nic)
{
    UWORD guard = LE_RX_RING * 2;

    while (guard-- != 0)
    {
        ULONG d = LE_RXD_OFF + (ULONG)nic->rx_next * 8;
        UWORD md1 = le_get16(nic, d + 2);
        UWORD len;

        if ((md1 & LE_R1_OWN) != 0)
            return;                     /* the chip still has it */

        len = (UWORD)(le_get16(nic, d + 6) & 0x0fff);

        if ((md1 & LE_R1_ERR) != 0 || len <= 4 || len > LE_BUFSZ)
        {
            nic->rx_errors++;
        }
        else
        {
            /*
             * The buffer is in shared RAM the CPU can address, so the frame is
             * handed up where it lies and the opener's CopyToBuff reads it
             * once.  No wrap case: a LANCE buffer holds one whole frame.
             */
            const UBYTE *fp = (const UBYTE *)(le_ram(nic) +
                                              LE_RXB_OFF +
                                              (ULONG)nic->rx_next * LE_BUFSZ);

            nic->rx_packets++;
            if (nic->rx != NULL)
                nic->rx(nic->rx_arg, fp, (UWORD)(len - 4));  /* drop the FCS */
        }

        /*
         * Hand the buffer back.  TWO writes, not three.
         *
         * RMD2 (d + 4) is BCNT, the size of the buffer, and it is a HOST
         * field: the chip reads it to know where the buffer ends and does not
         * write it, so what le_rings() put there at init is still there.
         * Linux's own a2065 driver agrees by construction -- it sets the
         * length once in lance_init_ring() and lance_rx() never touches it.
         *
         * WHAT THIS REMOVES IS A BUS CYCLE, NOT A CALL, and that distinction
         * is the whole reason this one is worth landing alone.  It came out of
         * a three-item bundle (perf/rx-redundant-work) that the profile priced
         * at 1.8% and the rig measured at rx -0.98%.  The other two removed
         * _nxe_ wrapper checks, and the profile is built -DAMINETXDUO_LTO=OFF
         * while the shipping build has LTO on (CMakeLists.txt:108), so those
         * wrappers were already inlined and folded away -- their share was
         * work that was not being done.  A word through the board's window is
         * not like that: no inliner can remove it, and it is the expensive
         * kind of access on this path.
         *
         * RMD3 (d + 6) is MCNT and the chip writes it, so clearing it looks
         * redundant too -- it is only read above, after OWN has been seen
         * clear, which is the chip saying it has just written it.  IT STAYS.
         * That argument rests on what the chip does rather than on anything
         * this file can check, the reference driver does clear it, and "the
         * emulator did not complain" is not evidence about an Am7990.
         *
         * RMD1 (d + 2) carries OWN, and that write is what gives the buffer
         * back, so it goes last and every time.
         *
         * MEASURED ALONE, and the contrast is the evidence for the rule above.
         * Six rounds alternated, clean build per arm, md5s printed:
         *
         *     medians       before       after       delta
         *     tcp-rx        5,736,512    5,824,081   +1.53%
         *       position 1  5,757,833    5,799,477   +0.72%
         *       position 2  5,708,409    5,871,054   +2.85%
         *     tcp-tx        3,124,139    3,133,475   +0.30%
         *
         * Ahead in both positions -- AND IT DID NOT REPRODUCE.  A second
         * sitting of the same two commits, rebuilt to the same md5s, gave
         * 5,817,806 -> 5,817,696 = -0.00% with the positions split (+1.53%,
         * -2.10%).  Two sittings, +1.53% and nothing.
         *
         * SO THIS IS NOT A MEASURED WIN.  It removes one Zorro word write per
         * received frame, which is real work no inliner can take away, and it
         * costs nothing -- that is why it stays.  But the rate does not show
         * it, and the +1.53% is withdrawn.
         *
         * IT ALSO WITHDRAWS AN ARGUMENT I BUILT ON IT.  The bundle carrying
         * this item plus two _nxe_ wrapper removals measured -0.98%, and I read
         * the contrast with +1.53% as proof that the wrappers' profile share
         * was work the LTO build had already folded away.  With the +1.53% gone
         * the contrast is gone: the bundle and this item alone are both nothing
         * within this rig's resolution.  The LTO caveat still stands on its own
         * terms -- the profile is built -DAMINETXDUO_LTO=OFF and the shipping
         * build is not, so a thin static wrapper is a symbol in one and inlined
         * in the other -- but it is a fact about the build, not something these
         * two measurements proved.
         */
        le_put16(nic, d + 6, 0);
        le_put16(nic, d + 2, (UWORD)(LE_R1_OWN | (md1 & 0x00ff)));

        nic->rx_next = (UWORD)((nic->rx_next + 1) & (LE_RX_RING - 1));
    }
}

/* ------------------------------------------------------------ transmit --- */

/* TRUE means a fatal descriptor error reset the rings. */
static BOOL le_tint(NetdevNic *nic)
{
    while (nic->txb_inuse != 0)
    {
        ULONG d   = LE_TXD_OFF + (ULONG)nic->tx_done * 8;
        UWORD md1 = le_get16(nic, d + 2);

        if ((md1 & LE_T1_OWN) != 0)
            return FALSE;               /* still being sent */

        if ((md1 & LE_T1_ERR) != 0)
        {
            UWORD md3 = le_get16(nic, d + 6);

            nic->tx_errors++;

            if ((md3 & LE_T3_LCOL) != 0)
                nic->collisions++;
            if ((md3 & LE_T3_RTRY) != 0)
                nic->collisions += 16;

            /* Am7990 BUFF and UFLO clear CSR0.TXON.  Merely retiring this
               descriptor leaves a unit which reports itself running but can
               never send again.  Rebuild both rings, the chip's documented
               recovery and the one the reference Am7990 drivers use. */
            if ((md3 & (LE_T3_BUFF | LE_T3_UFLO)) != 0)
            {
                lance_reset(nic);
                return TRUE;
            }
        }
        else
        {
            if ((md1 & LE_T1_ONE) != 0)
                nic->collisions++;
            else if ((md1 & LE_T1_MORE) != 0)
                nic->collisions += 2;   /* exact retry count is unavailable */
            nic->tx_packets++;
        }

        nic->tx_completed++;
        nic->txb_inuse--;
        nic->tx_done = (UWORD)((nic->tx_done + 1) & (LE_TX_RING - 1));
    }

    if (nic->filter_pending)
        lance_setfilter(nic);

    return FALSE;
}

/* The buffer the next transmit will use, for the shell to frame into. */
static UBYTE *lance_tx_at(NetdevNic *nic)
{
    if (nic->txb_inuse >= LE_TX_RING)
        return NULL;

    return (UBYTE *)(le_ram(nic) + LE_TXB_OFF +
                     (ULONG)nic->tx_next * LE_BUFSZ);
}

LONG lance_tx(NetdevNic *nic, const UBYTE *frame, UWORD len)
{
    ULONG           d;
    volatile UBYTE *buf;
    UWORD           i;

    if (!nic->running)
        return DP8390_TX_OFFLINE;
    if (nic->txb_inuse >= LE_TX_RING)
        return DP8390_TX_BUSY;

    if (len < NETDEV_FRAME_MIN)
        len = NETDEV_FRAME_MIN;

    d   = LE_TXD_OFF + (ULONG)nic->tx_next * 8;
    buf = le_ram(nic) + LE_TXB_OFF + (ULONG)nic->tx_next * LE_BUFSZ;

    /*
     * Frame bytes are not swapped: the board's crossed lanes cancel between the
     * CPU write and the chip's read.  Only descriptors need it.  Longwords, and
     * both ends are longword aligned -- nu_TxBuf is ULONG[] and the buffers sit
     * at 0x100 + n * 1536.
     */
    if (frame != (const UBYTE *)buf)
    {
        UWORD bulk = (UWORD)(len & (UWORD)~3u);

        if (bulk != 0)
            n68k_copy_longs(buf, frame, (ULONG)(bulk >> 2));
        for (i = bulk; i < len; i++)
            buf[i] = frame[i];
    }

    le_put16(nic, d + 4, (UWORD)(0xf000 | ((UWORD)(-(LONG)len) & 0x0fff)));
    le_put16(nic, d + 6, 0);
    le_put16(nic, d + 2,
             (UWORD)(LE_T1_OWN | LE_T1_STP | LE_T1_ENP |
                     (UWORD)(((LE_TXB_OFF + (ULONG)nic->tx_next * LE_BUFSZ)
                              >> 16) & 0xff)));

    nic->tx_next = (UWORD)((nic->tx_next + 1) & (LE_TX_RING - 1));
    nic->txb_inuse++;

    LANCE_CSR_PUT(nic, LE_CSR0, LE_C0_INEA | LE_C0_TDMD);

    return 0;
}

/* ----------------------------------------------------------- interrupt --- */

BOOL lance_intr(NetdevNic *nic)
{
    UWORD rounds = 8;
    UWORD csr0;

    if (!nic->running)
        return FALSE;

    csr0 = LANCE_CSR_GET(nic, LE_CSR0);
    if ((csr0 & LE_C0_INTR) == 0)
        return FALSE;

    do
    {
        /* The bits written back are the acknowledge.  INEA is kept set. */
        LANCE_CSR_PUT(
            nic, LE_CSR0,
            (UWORD)((csr0 & (LE_C0_BABL | LE_C0_CERR | LE_C0_MISS |
                             LE_C0_MERR | LE_C0_RINT | LE_C0_TINT)) |
                    LE_C0_INEA));

        if ((csr0 & LE_C0_MISS) != 0)
            nic->rx_errors++;

        if ((csr0 & LE_C0_RINT) != 0)
            le_rint(nic);
        if ((csr0 & LE_C0_TINT) != 0 && le_tint(nic))
            return TRUE;

        if ((csr0 & LE_C0_MERR) != 0)
        {
            /* A memory error means the chip lost the bus.  Nothing short of
               a restart puts the rings back in a known state. */
            nic->rx_errors++;
            lance_reset(nic);
            return TRUE;
        }

        csr0 = LANCE_CSR_GET(nic, LE_CSR0);
    }
    while ((csr0 & LE_C0_INTR) != 0 && --rounds != 0);

    /* Descriptor errors are not the only way either half can stop.  A real
       Am7990 exposes the stopped state in CSR0; emulators commonly leave both
       bits set forever, which is why checking only MERR misses this on real
       A2065 hardware. */
    if ((csr0 & LE_C0_RXON) == 0 || (csr0 & LE_C0_TXON) == 0)
    {
        if ((csr0 & LE_C0_RXON) == 0)
            nic->rx_errors++;
        if ((csr0 & LE_C0_TXON) == 0)
            nic->tx_errors++;
        lance_reset(nic);
    }

    return TRUE;
}

/* -------------------------------------------------------------- attach --- */

/*
 * A LANCE answers a STOP with a CSR0 that reads back STOP and nothing else.
 * An empty window reads 0xffff or 0x0000, and neither is that.
 */
LONG lance_attach(NetdevNic *nic)
{
    UWORD i;
    UWORD csr0;

    if (nic->card->mem_size < LE_END)
        return -1;

    LANCE_CSR_PUT(nic, LE_CSR0, LE_C0_STOP);
    csr0 = LANCE_CSR_GET(nic, LE_CSR0);
    LE_TRACE("le: attach csr0 ", csr0);
    if ((csr0 & 0xff00) != 0 || (csr0 & LE_C0_STOP) == 0)
    {
        nic->diag_why = (UBYTE)ANXDIAG_WHY_CSR;
        return -1;
    }

    /*
     * The station address is in the autoconfig serial number: there is no
     * address PROM on either card and a read below the SRAM returns zero.
     * Commodore put the low four bytes in the serial field and left the OUI to
     * the card row -- 00:80:10 for the A2065, 00:60:30 for the Ariadne.
     */
    nic->mac_source = (UBYTE)ANXDIAG_MAC_SERIAL;

    nic->factory[0] = (UBYTE)(nic->card->serial_oui >> 8);
    nic->factory[1] = (UBYTE)(nic->card->serial_oui);
    nic->factory[2] = (UBYTE)(nic->serial >> 24);
    nic->factory[3] = (UBYTE)(nic->serial >> 16);
    nic->factory[4] = (UBYTE)(nic->serial >> 8);
    nic->factory[5] = (UBYTE)(nic->serial);

    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        nic->mac[i] = nic->factory[i];

    nic->mem_start = 0;
    nic->mem_size  = (LONG)nic->card->mem_size;
    nic->mem_end   = (LONG)nic->card->mem_size;
    nic->mem_ring  = 0;
    nic->txb_cnt   = LE_TX_RING;
    nic->tx_at     = lance_tx_at;
    nic->ring_copy_sum = NULL;  /* no ring_copy either: see below */
    nic->frame_at  = NULL;      /* the receive path hands up the ring buffer
                                   itself, so there is nothing to translate */

    return 0;
}

const struct NetdevNicOps netdev_nic_lance =
{
    lance_attach,
    lance_init,
    lance_halt,
    lance_tx,
    lance_setfilter,
    lance_intr,
    lance_reset
};
