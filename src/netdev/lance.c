/*
 * anxnet.device: the Am7990/Am79C960 LANCE, for the A2065 and the Ariadne.
 *
 * This is a second chip core beside dp8390.c, not a variant of it.  The two
 * families share the SANA-II shell above them and nothing else:
 *
 *                          DP8390 family              LANCE
 *   registers              16, three paged banks      RAP selects, RDP reads
 *   who moves the data     the CPU, always            the chip, by DMA
 *   buffers                a page ring on the card    descriptor rings in the
 *                                                     board's shared RAM
 *   bring-up               a register sequence        an initialisation block
 *   interrupt              read ISR, write it back    read CSR0, write 1s back
 *
 * Both cards put the chip's two registers and a 32 KB SRAM in the Zorro
 * window.  The LANCE DMAs into that SRAM and the CPU sees the same bytes, so
 * the rings, the buffers and the init block all live there, and every address
 * the chip is given is an offset within it.
 *
 * The Ariadne swaps the byte lanes.  Its SRAM port is wired crossed, so a word
 * the CPU writes is a word with its halves exchanged from the chip's side.
 * Frame data is unaffected, because the crossing cancels between the write and
 * the read, but every descriptor and init-block word must be pre-swapped.
 * card->lance_swap says so, and it is the only difference between the two
 * boards apart from where the registers are.
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
 * The rings and buffers, as offsets into the board's SRAM.  Eight receive
 * buffers is a frame every 1.2 ms at 10 Mbit before the ring can overrun,
 * which is longer than any span the interrupt path has ever measured.
 */
#define LE_RX_LOG       3
#define LE_RX_RING      (1u << LE_RX_LOG)       /* 8 */
#define LE_TX_LOG       2
#define LE_TX_RING      (1u << LE_TX_LOG)       /* 4 */
#define LE_BUFSZ        1536

#define LE_INIT_OFF     0x0000                  /* 24 bytes */
#define LE_RXD_OFF      0x0020                  /* 8 x 8 */
#define LE_TXD_OFF      0x0060                  /* 4 x 8 */
#define LE_RXB_OFF      0x0100
#define LE_TXB_OFF      (LE_RXB_OFF + LE_RX_RING * LE_BUFSZ)
#define LE_END          (LE_TXB_OFF + LE_TX_RING * LE_BUFSZ)

static volatile UBYTE *le_ram(NetdevNic *nic)
{
    return nic->board + nic->card->mem_off;
}

/*
 * The board's crossing applies to every 16-bit access through the window, not
 * only to the SRAM.  On the Ariadne a word written to a chip register arrives
 * with its halves exchanged, as a descriptor word does.  The first version
 * missed that, read a command register full of nonsense, and refused the card.
 *
 * Frame data needs no swap at any width, because the crossing cancels between
 * the CPU's access and the chip's, whichever way round it is.
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
 * RAP selects, RDP reads and writes.  Two 16-bit registers and nothing else,
 * which is why netdev_bus's byte accessors are not used here: a LANCE register
 * is a word and half of one is not a register.
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
 * STOP and re-INIT, which is the only recovery this chip has.  The rings live
 * in the board's SRAM, and lance_init() rebuilds them and zeroes txb_inuse.
 * No poll loop here, because lance_init() bounds its own IDON wait.
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
 * a re-initialisation.  That is the chip's design: no register holds the
 * filter while the chip runs.
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

    /* Where the init block is, split across CSR1 and CSR2. */
    LANCE_CSR_PUT(nic, LE_CSR1, (UWORD)(LE_INIT_OFF & 0xffff));
    LANCE_CSR_PUT(nic, LE_CSR2, (UWORD)((LE_INIT_OFF >> 16) & 0xff));
    LANCE_CSR_PUT(nic, LE_CSR3, 0);     /* no BSWP: the board does the lanes */

    LANCE_CSR_PUT(nic, LE_CSR0, LE_C0_INIT);

    /*
     * IDON says the chip has read the init block.  It is a DMA of 24 bytes
     * from RAM the CPU just wrote, so the wait is short.  The bound exists
     * because a card that never answers must not hang the machine.
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
             * once.  That is the same one-pass path the mapped-buffer DP8390
             * cards take, and here it needs no wrap case, because a LANCE
             * buffer holds one whole frame.
             */
            const UBYTE *fp = (const UBYTE *)(le_ram(nic) +
                                              LE_RXB_OFF +
                                              (ULONG)nic->rx_next * LE_BUFSZ);

            nic->rx_packets++;
            if (nic->rx != NULL)
                nic->rx(nic->rx_arg, fp, (UWORD)(len - 4));  /* drop the FCS */
        }

        /* Hand the buffer back. */
        le_put16(nic, d + 4, (UWORD)(0xf000 | ((UWORD)(-LE_BUFSZ) & 0x0fff)));
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

        nic->txb_inuse--;
        nic->tx_done = (UWORD)((nic->tx_done + 1) & (LE_TX_RING - 1));
    }

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
     * Frame bytes are not swapped, because the board's crossed lanes cancel
     * between the CPU write and the chip's read.  Only descriptors need it.
     *
     * Longwords, not the byte loop this started as, which cost 25% of the
     * write figure against a2065.device.  Both ends are longword-aligned:
     * nu_TxBuf is ULONG[] and the buffers sit at 0x100 + n * 1536.
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
     * The station address is in the autoconfig serial number, not in the board
     * window.  There is no address PROM on either card, and a read below the
     * SRAM returns zero.  Commodore put the low four bytes in the serial field
     * and left the OUI to the driver, which is why the card row carries it:
     * 00:80:10 for the A2065, 00:60:30 for the Ariadne.
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
