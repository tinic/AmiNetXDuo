/*
 * Copyright (c) 1997, 1998 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jason R. Thorpe of the Numerical Aerospace Simulation Facility,
 * NASA Ames Research Center.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Device driver for National Semiconductor DS8390/WD83C690 based ethernet
 * adapters.
 *
 * Copyright (c) 1994, 1995 Charles M. Hannum.  All rights reserved.
 *
 * Copyright (C) 1993, David Greenman.  This software may be used, modified,
 * copied, distributed, and sold, in both source and binary form provided that
 * the above copyright and these terms are retained.  Under no circumstances is
 * the author responsible for the proper functioning of this software, nor does
 * the author assume any responsibility for damages incurred with its use.
 */

/*
 * AmiNetXDuo: adapted from NetBSD sys/dev/ic/ne2000.c (rev 1.79).  The remote
 * DMA sequences, the presence/width detection and the memory sizing are
 * NetBSD's.  ne2000reg.h beside this file is NetBSD's, verbatim.
 *
 * SPDX-License-Identifier: MIT AND BSD-2-Clause-NetBSD
 */

#include "netdev_nic.h"
#include "dp8390.h"
#include "netdev_bsdtypes.h"
#include "netdev_clock.h"
#include "netdev_macgen.h"
#include "aminetxduo/anxs2ext.h"
#include "dp8390reg.h"
#include "ne2000reg.h"

#define AX88190_NODEID_OFFSET   0x400

/*
 * The AX88796B's flow-control register: whole-file register 0x1a, unpaged,
 * reset default 0x07.  FLWC turns on IEEE 802.3x: when the receive ring's
 * free page count falls to the high-water mark the MAC sends a PAUSE frame
 * and the link partner holds its transmit until the ring drains, instead of
 * the ring filling, the chip raising OVW and dp8390_intr() resetting it --
 * which discards every frame in the ring and every transmit still queued.
 * HWPC is kept at the reset default, seven 256-byte pages.
 *
 * WRITTEN, NEVER READ.  On a plain NE2000 registers 0x18..0x1f are the reset
 * port, and the emulators model them that way for every card: Amiberry
 * routes a WRITE there to a no-op and a READ to a chip reset.  A
 * read-modify-write would reset the emulated card under the probe.
 */
#define AX88796_FCR             0x1a
#define AX88796_FCR_FLWC        0x80
#define AX88796_FCR_HWPC_RESET  0x07

/*
 * FLWC above makes the MAC send PAUSE and honour it only on a link where
 * PAUSE was negotiated, and the AX88796B's PHY comes out of reset
 * advertising 10/100 half and full and no PAUSE (MR4 01e1).  So no link
 * partner ever agreed, whatever its port was set to, and the ring overran
 * instead.  Measured on an A3000 (68060 at 50 MHz, Zorro III) against a
 * TL-SG1016PE port with flow control on, 2026-10-05:
 *
 *     window                 PAUSE agreed   LAN Mbit/s   overruns
 *     8 frames (ring fit)    no             16.6         0
 *     32 frames              no              5.3         410
 *     32 frames              yes            21.9         0
 *     64 frames              yes            16.0-21.3    0
 *
 * So the PHY is told to advertise PAUSE, and while the partner agrees the
 * card answers ANXD_CMD_RX_CAPACITY with the 32 frames that measured best
 * rather than its ring (netdev_nic.h, rx_pause_capacity).
 *
 * The PHY is internal, at MII address 0x10, reached by bit-banging clause 22
 * frames through MEMR: whole-file register 0x14, unpaged like FCR.  MDC is
 * bit 0, MDIR bit 1 (set: the PHY drives MDIO), MDI bit 2 (what it drives),
 * MDO bit 3 (what we drive).  The framing and the edge each bit is set up
 * and sampled on are Linux's mdio-bitbang.c.  No delays: the PHY wants at
 * most 2.5 MHz on MDC, a bit is three bus writes, and a Zorro access is
 * several hundred nanoseconds.  Nothing else in the driver touches MEMR.
 */
#define AX88796_MEMR            0x14
#define AX88796_MEMR_MDC        0x01
#define AX88796_MEMR_MDIR       0x02
#define AX88796_MEMR_MDI        0x04
#define AX88796_MEMR_MDO        0x08
#define AX88796_PHY_ADDR        0x10
#define AX88796_PHYID1          0x003bL
#define AX88796_PHYID2          0x1841L

#define MII_BMCR                0
#define  MII_BMCR_ANENABLE      0x1000
#define  MII_BMCR_ANRESTART     0x0200
#define MII_BMSR                1
#define  MII_BMSR_LINK          0x0004
#define  MII_BMSR_ANDONE        0x0020
#define MII_PHYID1              2
#define MII_PHYID2              3
#define MII_ANAR                4
#define  MII_ANAR_10FD          0x0040
#define  MII_ANAR_100HD         0x0080
#define  MII_ANAR_100FD         0x0100
#define  MII_ANAR_PAUSE         0x0400
#define  MII_ANAR_MODES         0x03e0
#define  MII_ANAR_10_MODES      0x0060
#define MII_ANLPAR              5

/* 32 frames of the 1536 bytes the ring stores a full one in: what the
   table above measured best, and what ami_bsd_tcp_window_fit() turns into
   a 46,720-byte window at an MSS of 1460. */
#define AX88796_PAUSE_RX_CAPACITY   (32UL * 1536UL)

/* Blanks between looks at the negotiation: a second at 50 Hz. */
#define AX88796_PHY_TICKS       50u

/* Advertisements in a row that did not stick before giving up: a PHY that
   will not keep the bit is not one this code knows. */
#define AX88796_PHY_TRIES       3u

#ifdef NETDEV_TRACE
#define NE_TRACE(t, v)  netdev_trace_val((t), (ULONG)(v))
#else
#define NE_TRACE(t, v)  ((VOID)0)
#endif

/* ------------------------------------------------------------- plumbing --- */

#ifdef NETDEV_TIME
extern ULONG netdev_time_rdc;  /* netdev_device.c reports it */
#endif

/*
 * Overridable, the same way el3.c's EL3_RAW_GET is and for the same reason:
 * src/netdev/test/test_netdev_ne2000.c includes this file whole and puts a
 * chip behind these four.  The word-read path itself is not what that test
 * drives -- the arithmetic is a big-endian fact and test_netdev_bus.c is where
 * it is pinned -- so the register file it models is indexed, not addressed.
 */
#ifndef NIC_GET
#define NIC_GET(nic, reg)       netdev_bus_r8(&(nic)->bus, (reg))
#define NIC_PUT(nic, reg, val)  netdev_bus_w8(&(nic)->bus, (reg), (UBYTE)(val))
#define ASIC_GET(nic, reg)      netdev_bus_ra8(&(nic)->bus, (reg))
#define ASIC_PUT(nic, reg, val) netdev_bus_wa8(&(nic)->bus, (reg), (UBYTE)(val))
#endif

#define NE2000_RESET_STATUS_WAIT_US  10000u
#define NE2000_RESET_STATUS_SPINS      100u

/*
 * There is no timer open when a unit is probed and a device cannot Delay(), so
 * the millisecond arms are measured against the beam with the bus-read count
 * kept as a floor.  Anything under NETDEV_WAIT_MIN_US stays the plain count: it
 * paces bus cycles, which do not get faster when the CPU does.
 */
static VOID ne_delay(NetdevNic *nic, ULONG us)
{
    NetdevWait w;

    netdev_wait_begin(&w, us, us * 4u);

    do
        (VOID)NIC_GET(nic, ED_P0_CR);
    while (!netdev_wait_done(&w));
}

/* A few register reads' worth of bus time, for the chip to fill its port. */
static VOID dp_pause_reads(NetdevNic *nic, UWORD n)
{
    while (n-- != 0)
        (VOID)NIC_GET(nic, ED_P0_CR);
}

static int ne_memcmp(const UBYTE *a, const UBYTE *b, UWORD n)
{
    while (n-- != 0)
    {
        if (*a++ != *b++)
            return 1;
    }

    return 0;
}

/*
 * The first byte of a buffer readback that did not match, put in the probe
 * record.  The wrote/read pair separates the causes: $5a/$00 is a byte lane
 * that is not there, $5a/$a5 at an even offset is a swapped word.
 */
static VOID ne_note_mismatch(NetdevNic *nic, const UBYTE *want,
                             const UBYTE *got, UWORD n, LONG base)
{
    UWORD i;

    for (i = 0; i < n; i++)
    {
        if (want[i] != got[i])
        {
            netdev_diag_note(ANXDIAG_BUF_SEEN, netdev_diag_card(nic->card),
                             ((ULONG)((base + (LONG)i) & 0xffffL) << 16) |
                             ((ULONG)want[i] << 8) | (ULONG)got[i]);
            return;
        }
    }
}

/* --------------------------------------------------------- remote DMA ----- */

/*
 * Program a remote read of `amount` bytes from `src`, and remember where that
 * leaves the pointer.  `over` is how much more the burst may cover beyond
 * `amount`, so a caller with a contiguous next read pays for one setup.
 */
static VOID ne2000_dma_start(NetdevNic *nic, LONG src, UWORD amount, UWORD over)
{
    UWORD total = (UWORD)(amount + over);

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STA);

    NIC_PUT(nic, ED_P0_RBCR0, total);
    NIC_PUT(nic, ED_P0_RBCR1, total >> 8);
    NIC_PUT(nic, ED_P0_RSAR0, src);
    NIC_PUT(nic, ED_P0_RSAR1, src >> 8);

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD0 | ED_CR_PAGE_0 | ED_CR_STA);

    nic->dma_pos  = src + (LONG)amount;
    nic->dma_left = over;
}

/*
 * Put the chip in a position to hand out `rounded` bytes from `src`, without
 * saying who reads them.  Separate from ne2000_readmem() because the fused
 * drain below needs the same arrangement and a different read.
 */
static VOID ne2000_dma_for(NetdevNic *nic, LONG src, UWORD rounded)
{
    /*
     * The burst already running is at this address with room to spare, so the
     * chip needs telling nothing: reading the port continues it.
     */
    if (nic->dma_left >= rounded && nic->dma_pos == src)
    {
        nic->dma_pos  = src + (LONG)rounded;
        nic->dma_left = (UWORD)(nic->dma_left - rounded);
    }
    else
    {
        ne2000_dma_start(nic, src, rounded, 0);
    }
}

static VOID ne2000_readmem(NetdevNic *nic, LONG src, UBYTE *dst, UWORD amount)
{
    amount = (UWORD)((amount + 1u) & ~1u);

    ne2000_dma_for(nic, src, amount);

    netdev_bus_rdata(&nic->bus, dst, amount);
}

static VOID ne2000_writemem(NetdevNic *nic, const UBYTE *src, LONG dst,
                            UWORD len)
{
    UWORD maxwait = 100;

    nic->dma_left = 0;

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STA);
    NIC_PUT(nic, ED_P0_ISR, ED_ISR_RDC);

    NIC_PUT(nic, ED_P0_RBCR0, len);
    NIC_PUT(nic, ED_P0_RBCR1, len >> 8);
    NIC_PUT(nic, ED_P0_RSAR0, dst);
    NIC_PUT(nic, ED_P0_RSAR1, dst >> 8);

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD1 | ED_CR_PAGE_0 | ED_CR_STA);

    netdev_bus_wdata(&nic->bus, src, len);

    while ((NIC_GET(nic, ED_P0_ISR) & ED_ISR_RDC) != ED_ISR_RDC &&
           --maxwait != 0)
        ne_delay(nic, 1);
}

static VOID ne2000_read_hdr(NetdevNic *nic, LONG src, NetdevRing *hdr)
{
    UBYTE raw[4];

    /*
     * Overshoot the header deliberately: the body follows it in the ring.  The
     * burst is bounded by what is left before the ring wraps, so it can never
     * read past the end of the buffer.
     */
    {
        LONG  room = nic->mem_end - (src + 4);
        UWORD over = (UWORD)((room > (LONG)NETDEV_RXBUF_MAX)
                             ? NETDEV_RXBUF_MAX : (room > 0 ? room : 0));

        ne2000_dma_start(nic, src, 4, over);
        netdev_bus_rdata(&nic->bus, raw, 4);
    }

    /*
     * The chip stores the header little-endian and the data port is
     * byte-swapped by the card, so the bytes arrive in chip order: status,
     * next page, count low, count high.
     */
    hdr->rsr         = raw[0];
    hdr->next_packet = raw[1];
    hdr->count       = (UWORD)(raw[2] | (raw[3] << 8));
}

static LONG ne2000_ring_copy(NetdevNic *nic, LONG src, UBYTE *dst, UWORD amount)
{
    if (src + (LONG)amount > nic->mem_end)
    {
        UWORD head = (UWORD)(nic->mem_end - src);

        ne2000_readmem(nic, src, dst, head);
        amount = (UWORD)(amount - head);
        src    = nic->mem_ring;
        dst   += head;
    }

    ne2000_readmem(nic, src, dst, amount);

    return src + amount;
}

/*
 * ring_copy for a direct-receive destination, with the checksum of what was
 * moved for the price of the move.  Declined, having done nothing at all, when
 * the read would wrap the ring: the second segment would begin at whatever
 * byte phase the first left off at, and the sum is over longwords counted from
 * the start of the payload.  A wrap is one frame in a ring's worth, and the
 * caller has an ordinary path for it.
 */
static BOOL ne2000_ring_copy_sum(NetdevNic *nic, LONG src, UBYTE *dst,
                                 UWORD amount, ULONG *sum)
{
    if (src + (LONG)amount > nic->mem_end)
        return FALSE;
    if (!netdev_bus_can_sum(&nic->bus, dst))
        return FALSE;

    /* The chip is told the same rounded-up count ne2000_readmem() would tell
       it, because it hands out whole words either way.  What differs is that
       the drain below stores only the bytes the caller asked for. */
    ne2000_dma_for(nic, src, (UWORD)((amount + 1u) & ~1u));

    *sum = netdev_bus_rdata_sum(&nic->bus, dst, amount);

    return TRUE;
}

/*
 * The frame is already linear and already padded to the Ethernet minimum by the
 * caller, so this is one remote-DMA write.  Returns the length transmitted.
 */
static UWORD ne2000_write_buf(NetdevNic *nic, const UBYTE *frame, UWORD len,
                              LONG buf)
{
    UWORD maxwait = 100;

    if (len < NETDEV_FRAME_MIN)
        len = NETDEV_FRAME_MIN;

    nic->dma_left = 0;          /* the write below moves the pointer */

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STA);
    NIC_PUT(nic, ED_P0_ISR, ED_ISR_RDC);

    /*
     * RBCR only bounds the DMA into buffer RAM; what goes on the wire is TBCR,
     * which dp8390_xmit() sets from the unrounded txb_len.  A rounded-up odd
     * frame parks one extra byte in the transmit slot and never transmits it.
     */
    NIC_PUT(nic, ED_P0_RBCR0, len);
    NIC_PUT(nic, ED_P0_RBCR1, len >> 8);
    NIC_PUT(nic, ED_P0_RSAR0, buf);
    NIC_PUT(nic, ED_P0_RSAR1, buf >> 8);

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD1 | ED_CR_PAGE_0 | ED_CR_STA);

    netdev_bus_wdata(&nic->bus, frame, (UWORD)((len + 1u) & ~1u));

    while ((NIC_GET(nic, ED_P0_ISR) & ED_ISR_RDC) != ED_ISR_RDC &&
           --maxwait != 0)
    {
#ifdef NETDEV_TIME
        netdev_time_rdc++;
#endif
        (VOID)NIC_GET(nic, ED_P0_CRDA1);
        (VOID)NIC_GET(nic, ED_P0_CRDA0);
    }

    if (maxwait == 0)
    {
        nic->tx_errors++;
        dp8390_reset(nic);
        return 0;
    }

    return len;
}

/* ---------------------------------------------------------------- probe --- */

static const UBYTE ne_test_pattern[32] = "THIS is A memory TEST pattern";

/*
 * Can this bus need cnet16's word reads?  Mirrors what netdev_bus_set_getodd()
 * refuses, and is asked first so that no card whose registers are plain
 * adjacent bytes has its detection sequence changed by any of this.
 */
static BOOL ne2000_odd_window(const NetdevNic *nic)
{
    return (BOOL)(nic->bus.odd != NULL && nic->bus.regmap == NULL &&
                  nic->bus.stride == 1u);
}

/*
 * Do odd-numbered registers read correctly the way they are being read now?
 * ISR after a reset has ED_ISR_RST set, and BNRY (register 3, odd, read/write,
 * the ring not yet programmed) round-trips two complementary patterns.  All
 * three bytes come back rather than a verdict: one verdict covers three cards.
 */
static ULONG ne2000_odd_seen(NetdevNic *nic)
{
    UBYTE isr;
    UBYTE lo;
    UBYTE hi;

    isr = NIC_GET(nic, ED_P0_ISR);

    NIC_PUT(nic, ED_P0_BNRY, 0x5a);
    lo = NIC_GET(nic, ED_P0_BNRY);

    NIC_PUT(nic, ED_P0_BNRY, 0xa5);
    hi = NIC_GET(nic, ED_P0_BNRY);

    return ((ULONG)isr << 16) | ((ULONG)lo << 8) | (ULONG)hi;
}

static BOOL ne2000_odd_isr_ok(ULONG seen)
{
    return (BOOL)((((seen >> 16) & 0xffu) & ED_ISR_RST) == ED_ISR_RST);
}

static BOOL ne2000_odd_reads_ok(ULONG seen)
{
    return (BOOL)(ne2000_odd_isr_ok(seen) &&
                  ((seen >> 8) & 0xffu) == 0x5au &&
                  (seen & 0xffu) == 0xa5u);
}

/*
 * Did the chip come out of the reset above?
 *
 * ED_CR_STA is deliberately not in the mask: some NE2000 clones come out of
 * reset with CR bit 1 stuck set and read back 0x23 where the datasheet says
 * 0x21 (Netgear FA411).  A floating bus reads 0xff, which has TXP set and
 * fails the comparison either way.
 */
static BOOL ne2000_cr_reset_ok(UBYTE cr)
{
    return (BOOL)((cr & (ED_CR_RD2 | ED_CR_TXP | ED_CR_STP)) ==
                  (ED_CR_RD2 | ED_CR_STP));
}

/*
 * The reset port is whole-file register 31, so its read is itself one of the
 * odd-register reads that cnet16 performs as a word.  The complete pulse stays
 * in one function: changing getodd and merely rereading ISR proves nothing.
 */
static VOID ne2000_probe_reset(NetdevNic *nic)
{
    UBYTE tmp = ASIC_GET(nic, NE2000_ASIC_RESET);

    nic->reset_id = tmp;
    ne_delay(nic, 10000);
    ASIC_PUT(nic, NE2000_ASIC_RESET, tmp);
    ne_delay(nic, 5000);

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STP);
    ne_delay(nic, 5000);
}

/*
 * NetBSD's ne2000_detect, minus the NE1000 arm: no card in this family has an
 * 8-bit buffer, and the byte-mode write it uses to find one is invasive.
 */
static BOOL ne2000_detect(NetdevNic *nic)
{
    UBYTE test_buffer[32];
    UBYTE tmp;
    NetdevWait reset_wait;

    ne2000_probe_reset(nic);

    tmp = NIC_GET(nic, ED_P0_CR);
    netdev_diag_note(ANXDIAG_CR_READ, netdev_diag_card(nic->card), (ULONG)tmp);
    if (!ne2000_cr_reset_ok(tmp))
    {
        /*
         * THE RESET NEVER REACHED THE CARD.
         *
         * The NE2000 reset port is whole-file register 31, which is ODD, and
         * ne2000_probe_reset() strobes it by READING it.  On a clone that
         * asserts -IOIS16 unconditionally -- the cnet16 class, the CNet
         * SinglePoint and the NetGear FA411 -- a byte read of an odd register
         * is not a cycle the card answers, so the strobe does nothing and CR
         * is whatever the previous owner of the socket left in it.  A card
         * that has already been driven once, which is every warm reboot, then
         * fails this test and is called incompatible.
         *
         * The word path cannot be probed first: ne2000_odd_seen() below reads
         * ISR after a reset, and there has not been one.  cnet16 has no
         * chicken and egg because it does not probe at all -- it reads that
         * port as a word from its very first call (cnet16.device 1.9, the
         * reset routine: `move.w reset_port-1+even,-(sp) / move.b 1(sp),d0`)
         * and ships a second binary for the cards that do not need it.
         *
         * So: strobe it again through the word path and ask once more.  Writes
         * are unaffected either way -- they are byte-wide into the odd window
         * in both drivers -- so only the read had to move.
         */
        UWORD ci = netdev_diag_card(nic->card);

        if (!ne2000_odd_window(nic) || !netdev_bus_set_getodd(&nic->bus))
        {
            nic->diag_why = (UBYTE)ANXDIAG_WHY_CR;
            return FALSE;
        }

        netdev_diag_note(ANXDIAG_CR_RETRY, ci, 1);
        NE_TRACE("ne: reset port as a word ", 0);
        ne2000_probe_reset(nic);

        tmp = NIC_GET(nic, ED_P0_CR);
        netdev_diag_note(ANXDIAG_CR_READ, ci, (ULONG)tmp);
        if (!ne2000_cr_reset_ok(tmp))
        {
            /* Word reads are never the state a failure is left in. */
            nic->bus.getodd = 0;
            nic->diag_why = (UBYTE)ANXDIAG_WHY_CR;
            return FALSE;
        }
    }

    /*
     * A Fast-Ethernet NE2000 clone that asserts -IOIS16 unconditionally decodes
     * 16-bit I/O cycles and nothing else, so a byte read of an odd register is
     * bus noise -- and every ISR poll, CR readback and ring pointer here is
     * odd.  CR is even and cannot tell such a card apart; register 31 is odd.
     */
    if (!ne2000_odd_window(nic))
    {
        tmp = NIC_GET(nic, ED_P0_ISR);
        if ((tmp & ED_ISR_RST) != ED_ISR_RST)
        {
            nic->diag_why = (UBYTE)ANXDIAG_WHY_ODD;
            return FALSE;
        }
    }
    else
    {
        UWORD ci       = netdev_diag_card(nic->card);
        /* Which mode the first reading is taken in.  It is the word path
           already whenever the reset above had to be strobed through it, and
           the record must not call that reading a byte one. */
        BOOL  was_word = (BOOL)(nic->bus.getodd != 0);
        ULONG plain    = ne2000_odd_seen(nic);

        /* The odd window, recorded whatever happens next: a PCMCIA row that
           reached here with none is reading the ASIC reset at an odd address
           in the even window. */
        netdev_diag_note(ANXDIAG_ODDWIN, ci, (ULONG)(APTR)nic->bus.odd);
        netdev_diag_note(was_word ? ANXDIAG_ODD_WORD : ANXDIAG_ODD_PLAIN,
                         ci, plain);

        if (!ne2000_odd_reads_ok(plain))
        {
            ULONG word;

            /* Already the word path and it still does not read: there is no
               third way to try. */
            if (was_word)
            {
                nic->diag_why =
                    (UBYTE)(ne2000_odd_isr_ok(plain) ? ANXDIAG_WHY_ODD_BNRY
                                                     : ANXDIAG_WHY_ODD);
                return FALSE;
            }

            netdev_diag_note(ANXDIAG_ODD_RETRY, ci, 1);

            if (!netdev_bus_set_getodd(&nic->bus))
            {
                nic->diag_why = (UBYTE)ANXDIAG_WHY_ODD;
                return FALSE;
            }

            NE_TRACE("ne: trying cnet16 odd reads ", 0);
            ne2000_probe_reset(nic);
            word = ne2000_odd_seen(nic);
            netdev_diag_note(ANXDIAG_ODD_WORD, ci, word);

            if (!ne2000_odd_reads_ok(word))
            {
                /* Back to plain bytes.  Word reads are never the state a
                   failure is left in, so nothing downstream and no second
                   probe inherits a mode this one did not earn. */
                nic->bus.getodd = 0;

                /* Which of the two questions failed.  Both modes answering the
                   ISR read and neither round-tripping a write is a different
                   card from one where nothing answered at all. */
                nic->diag_why =
                    (UBYTE)((ne2000_odd_isr_ok(plain) ||
                             ne2000_odd_isr_ok(word))
                                ? ANXDIAG_WHY_ODD_BNRY : ANXDIAG_WHY_ODD);
                return FALSE;
            }
            NE_TRACE("ne: odd registers read as words ", 1);
        }

        /* Which mode this card ended up in, recorded here rather than only
           after a successful attach: a card that gets past this and fails the
           buffer test still has to say how its registers were being read. */
        netdev_diag_note(ANXDIAG_GETODD, ci, (ULONG)nic->bus.getodd);
    }

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STA);

    netdev_wait_begin(&reset_wait, NE2000_RESET_STATUS_WAIT_US,
                      NE2000_RESET_STATUS_SPINS);
    do
    {
        if ((NIC_GET(nic, ED_P0_ISR) & ED_ISR_RST) == ED_ISR_RST)
        {
            NIC_PUT(nic, ED_P0_ISR, ED_ISR_RST);
            break;
        }
        ne_delay(nic, 100);
    }
    while (!netdev_wait_done(&reset_wait));

    /* Monitor mode, so the buffer test is not raced by an arriving frame. */
    NIC_PUT(nic, ED_P0_RCR, ED_RCR_MON);
    NIC_PUT(nic, ED_P0_DCR, ED_DCR_FT1 | ED_DCR_LS | ED_DCR_WTS);
    NIC_PUT(nic, ED_P0_PSTART, 16384 >> ED_PAGE_SHIFT);
    NIC_PUT(nic, ED_P0_PSTOP, (16384 + 16384) >> ED_PAGE_SHIFT);

    nic->bus.dmode = NETDEV_DMODE_WORD;
    ne2000_writemem(nic, ne_test_pattern, 16384, sizeof(ne_test_pattern));
    ne2000_readmem(nic, 16384, test_buffer, sizeof(test_buffer));

    NIC_PUT(nic, ED_P0_ISR, 0xff);

    if (ne_memcmp(ne_test_pattern, test_buffer,
                  sizeof(ne_test_pattern)) != 0)
    {
        ne_note_mismatch(nic, ne_test_pattern, test_buffer,
                         (UWORD)sizeof(ne_test_pattern), 0);
        nic->diag_why = (UBYTE)ANXDIAG_WHY_BUFFER;
        return FALSE;
    }

    return TRUE;
}

/*
 * Decide whether the 32-bit window is really the data port.  A card that does
 * not have one answers with a mismatch and stays on the 16-bit path.  The
 * result is what S2_GETSPECIALSTATS reports as the transfer mode.
 */
static VOID ne2000_probe_wide(NetdevNic *nic)
{
    /*
     * The m68k ABI only promises word-aligned ULONG arrays.  Round pointers
     * into one extra ULONG of storage so both probe legs really exercise the
     * longword windows; otherwise netdev_bus.c silently uses 16-bit moves and
     * the probe can promote LONG without ever touching either window.
     */
    ULONG  outbuf[NETDEV_BUS_PROBE_LEN / 4 + 1];
    ULONG  inbuf[NETDEV_BUS_PROBE_LEN / 4 + 1];
    UBYTE *out = (UBYTE *)(((unsigned long)(void *)outbuf + 3ul) & ~3ul);
    UBYTE *back = (UBYTE *)(((unsigned long)(void *)inbuf + 3ul) & ~3ul);
    UWORD  i;

    if (nic->bus.wide == NULL || nic->bus.wide_write == NULL)
        return;

    /* Leg 1: written narrow, read wide. */
    for (i = 0; i < NETDEV_BUS_PROBE_LEN; i++)
        out[i] = (UBYTE)(0x5au ^ (i * 7u));

    nic->bus.dmode = NETDEV_DMODE_WORD;
    ne2000_writemem(nic, out, 16384, NETDEV_BUS_PROBE_LEN);
    nic->bus.dmode = NETDEV_DMODE_LONG;
    ne2000_readmem(nic, 16384, back, NETDEV_BUS_PROBE_LEN);
    if (ne_memcmp(out, back, NETDEV_BUS_PROBE_LEN) != 0)
    {
        nic->bus.dmode = NETDEV_DMODE_WORD;
        return;
    }

    /*
     * Leg 2: written wide, read narrow, which is the direction that transmits.
     * A different pattern, so a buffer left over from leg 1 cannot pass it.
     */
    for (i = 0; i < NETDEV_BUS_PROBE_LEN; i++)
        out[i] = (UBYTE)(0xa5u ^ (i * 3u));

    nic->bus.dmode = NETDEV_DMODE_LONG;
    ne2000_writemem(nic, out, 16384 + 256, NETDEV_BUS_PROBE_LEN);
    nic->bus.dmode = NETDEV_DMODE_WORD;
    ne2000_readmem(nic, 16384 + 256, back, NETDEV_BUS_PROBE_LEN);
    if (ne_memcmp(out, back, NETDEV_BUS_PROBE_LEN) != 0)
        return;

    nic->bus.dmode = NETDEV_DMODE_LONG;
}

/*
 * netdev_cache.c's question: four distinct words written to the card and
 * read back through the port.  A data cache in the way answers the first
 * word four times -- one fill, three hits on the same address -- so the
 * mismatch is at word 1.  Reset first: this runs before detect, on a chip
 * in whatever state the last driver left it.  Every wait inside is bounded
 * by a count, so a stale ISR read costs a timeout and not a hang.
 */
static const UBYTE ne_coherence_pattern[8] =
{
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
};

/*
 * The AX88796B's flow control.  Reading the reset port resets the MAC and
 * puts FCR back to its default 0x07, so this follows every reset pulse after
 * attach, not only attach itself (F-287).  Gated on the card row: on an
 * RTL8019 or a real DP8390 this offset is not a register.
 */
static VOID ne2000_ax88796_flow(NetdevNic *nic)
{
    if (nic->card->ax88796)
        NIC_PUT(nic, AX88796_FCR, AX88796_FCR_FLWC | AX88796_FCR_HWPC_RESET);
}

/* One bit to the PHY: set up with MDC low, taken on the rising edge. */
static VOID ax_mii_out(NetdevNic *nic, UWORD bit)
{
    UBYTE v = (UBYTE)(bit != 0 ? AX88796_MEMR_MDO : 0);

    NIC_PUT(nic, AX88796_MEMR, v);
    NIC_PUT(nic, AX88796_MEMR, v | AX88796_MEMR_MDC);
    NIC_PUT(nic, AX88796_MEMR, v);
}

/* One bit from the PHY: it drives after the rising edge, read after the
   falling one. */
static UWORD ax_mii_in(NetdevNic *nic)
{
    NIC_PUT(nic, AX88796_MEMR, AX88796_MEMR_MDIR | AX88796_MEMR_MDC);
    NIC_PUT(nic, AX88796_MEMR, AX88796_MEMR_MDIR);

    return (UWORD)((NIC_GET(nic, AX88796_MEMR) & AX88796_MEMR_MDI) != 0);
}

/* Preamble, start, opcode (2 read, 1 write), PHY address, register. */
static VOID ax_mii_head(NetdevNic *nic, UWORD op, UWORD reg)
{
    UWORD i;

    NIC_PUT(nic, AX88796_MEMR, 0);
    for (i = 0; i < 32; i++)
        ax_mii_out(nic, 1);
    ax_mii_out(nic, 0);
    ax_mii_out(nic, 1);
    ax_mii_out(nic, op & 2u);
    ax_mii_out(nic, op & 1u);
    for (i = 5; i-- != 0; )
        ax_mii_out(nic, (AX88796_PHY_ADDR >> i) & 1u);
    for (i = 5; i-- != 0; )
        ax_mii_out(nic, (reg >> i) & 1u);
}

/* A PHY register, or -1 when nothing drove the turnaround low: no PHY. */
static LONG ax_mii_read(NetdevNic *nic, UWORD reg)
{
    ULONG v = 0;
    UWORD ta;
    UWORD i;

    ax_mii_head(nic, 2u, reg);
    NIC_PUT(nic, AX88796_MEMR, AX88796_MEMR_MDIR);
    ta = ax_mii_in(nic);
    for (i = 0; i < 16; i++)
        v = (v << 1) | ax_mii_in(nic);
    (VOID)ax_mii_in(nic);

    return (ta != 0) ? -1L : (LONG)v;
}

static VOID ax_mii_write(NetdevNic *nic, UWORD reg, UWORD val)
{
    UWORD i;

    ax_mii_head(nic, 1u, reg);
    ax_mii_out(nic, 1);
    ax_mii_out(nic, 0);
    for (i = 16; i-- != 0; )
        ax_mii_out(nic, (val >> i) & 1u);
    NIC_PUT(nic, AX88796_MEMR, AX88796_MEMR_MDIR);
}

/*
 * What a paused link lets the card hold, from the three registers: link up
 * and negotiation done, full duplex resolved (IEEE 802.3 Annex 28B: the best
 * mode both offer), and PAUSE offered by both.  The AX88796B advertises
 * symmetric PAUSE only, so the asymmetric bit plays no part.  0 otherwise,
 * and on any read that failed.
 */
static ULONG ax88796_pause_capacity(LONG bmsr, LONG anar, LONG anlpar)
{
    LONG both;

    if (bmsr < 0 || anar < 0 || anlpar < 0)
        return 0;
    if ((bmsr & (MII_BMSR_LINK | MII_BMSR_ANDONE)) !=
        (MII_BMSR_LINK | MII_BMSR_ANDONE))
        return 0;

    both = anar & anlpar;
    if ((both & MII_ANAR_PAUSE) == 0)
        return 0;
    if ((both & MII_ANAR_100FD) == 0 &&
        ((both & MII_ANAR_100HD) != 0 || (both & MII_ANAR_10FD) == 0))
        return 0;               /* half duplex: there is no PAUSE */

    return AX88796_PAUSE_RX_CAPACITY;
}

/* Offer PAUSE and renegotiate, when the PHY is negotiating and does not
   offer it already.  The link drops for the second or two that takes. */
static VOID ax88796_advertise(NetdevNic *nic)
{
    LONG anar = ax_mii_read(nic, MII_ANAR);
    LONG bmcr = ax_mii_read(nic, MII_BMCR);
    UWORD want;

    if (anar < 0 || bmcr < 0)
        return;
    if ((bmcr & MII_BMCR_ANENABLE) == 0)
        return;                 /* forced speed: nothing is negotiated */

    want = (UWORD)(anar | MII_ANAR_PAUSE);
    if (nic->link_speed_limit == 10)
        want = (UWORD)((want & ~MII_ANAR_MODES) | MII_ANAR_10_MODES);
    if (want == (UWORD)anar)
        return;
    ax_mii_write(nic, MII_ANAR, want);
    ax_mii_write(nic, MII_BMCR, (UWORD)(bmcr | MII_BMCR_ANRESTART));
}

/* Limit the advertised modes, never force duplex. Read back ANAR before
   accepting a request, and preserve the attach-time modes for last Close. */
static BOOL ax88796_set_link_speed(NetdevNic *nic, ULONG mbps)
{
    LONG anar;
    LONG bmcr;
    UWORD want;

    if (!nic->ax_phy || (mbps != 0 && mbps != 10))
        return FALSE;
    anar = ax_mii_read(nic, MII_ANAR);
    bmcr = ax_mii_read(nic, MII_BMCR);
    if (anar < 0 || bmcr < 0)
        return FALSE;
    want = (UWORD)((anar & ~MII_ANAR_MODES) | MII_ANAR_PAUSE |
                  (mbps == 10 ? MII_ANAR_10_MODES : nic->ax_default_anar));
    if (want != (UWORD)anar)
    {
        ax_mii_write(nic, MII_ANAR, want);
        if (ax_mii_read(nic, MII_ANAR) != want)
        {
            ax_mii_write(nic, MII_ANAR, (UWORD)anar);
            return FALSE;
        }
    }
    if (nic->link_speed_limit != mbps || want != (UWORD)anar ||
        (bmcr & MII_BMCR_ANENABLE) == 0)
    {
        LONG control;
        ax_mii_write(nic, MII_BMCR,
                     (UWORD)(bmcr | MII_BMCR_ANENABLE | MII_BMCR_ANRESTART));
        control = ax_mii_read(nic, MII_BMCR);
        if (control < 0 || (control & MII_BMCR_ANENABLE) == 0)
        {
            ax_mii_write(nic, MII_ANAR, (UWORD)anar);
            ax_mii_write(nic, MII_BMCR, (UWORD)bmcr);
            return FALSE;
        }
    }
    nic->link_speed_limit = mbps;
    nic->rx_pause_capacity = 0;
    nic->link_bps = mbps == 10 ? 10000000UL : 0;
    return TRUE;
}

/*
 * At attach, at task level and before the interrupt server exists: find the
 * PHY by its identifier -- on anything else MEMR is not this register -- and
 * offer PAUSE.
 */
static VOID ne2000_ax88796_phy(NetdevNic *nic)
{
    nic->ax_phy            = 0;
    nic->ax_phy_tick       = 0;
    nic->ax_phy_tries      = 0;
    nic->rx_pause_capacity = 0;
    nic->set_link_speed    = NULL;
    nic->link_speed_limit  = 0;

    if (!nic->card->ax88796)
        return;
    if (ax_mii_read(nic, MII_PHYID1) != AX88796_PHYID1 ||
        ax_mii_read(nic, MII_PHYID2) != AX88796_PHYID2)
        return;

    nic->ax_phy = 1;
    {
        LONG anar = ax_mii_read(nic, MII_ANAR);
        if (anar < 0)
        {
            nic->ax_phy = 0;
            return;
        }
        nic->ax_default_anar = (UWORD)(anar & MII_ANAR_MODES);
    }
    nic->set_link_speed = ax88796_set_link_speed;
    ax88796_advertise(nic);
}

/*
 * DL10019 / DL10022 PHY and duplex, after Linux pcnet_cs (mdio_*, read_eeprom,
 * write_asic) and NetBSD dl10019.c.  The chips have no PAUSE, and the MAC
 * does not follow the PHY: at a full-duplex link it still defers its own
 * transmits to carrier until told otherwise, which on an A1200 cost a third
 * of a 10 Mbit/s download.  ASIC registers, relative to the data port:
 * $0C GPIO (MII bit-bang, low nibble preserved), $0D DIAG (DL10022 duplex),
 * $0E serial EEPROM / internal ASIC port (DL10019 duplex, register 4).
 */
#define DL_GPIO             0x0c
#define DL_DIAG             0x0d
#define DL_EEPROM           0x0e
#define DL_MDIO_CLK         0x80
#define DL_MDIO_OUT         0x40
#define DL_MDIO_WRITE       0x30        /* direction: $10 DL10019, $20 DL10022 */
#define DL_MDIO_IN          0x10
#define DL_EE_EEP           0x40
#define DL_EE_ASIC          0x10
#define DL_EE_CS            0x08
#define DL_EE_CK            0x04
#define DL_EE_DO            0x02
#define DL_EE_DI            0x01
#define DL_EE_READ          0x06
#define DL19_FDUPLX         0x0400
#define DL_LINK_TICKS       50          /* blanks between looks: one second */

static UWORD dl_mii_read(NetdevNic *nic, UBYTE phy, UBYTE loc)
{
    ULONG cmd = (0x06UL << 10) | ((ULONG)phy << 5) | (ULONG)loc;
    UBYTE m   = (UBYTE)(ASIC_GET(nic, DL_GPIO) & 0x0fu);
    UWORD r   = 0;
    WORD  i;

    for (i = 0; i < 32; i++)
    {
        ASIC_PUT(nic, DL_GPIO, m | DL_MDIO_WRITE | DL_MDIO_OUT);
        ASIC_PUT(nic, DL_GPIO, m | DL_MDIO_WRITE | DL_MDIO_OUT | DL_MDIO_CLK);
    }
    for (i = 13; i >= 0; i--)
    {
        UBYTE d = (UBYTE)(DL_MDIO_WRITE |
                          ((cmd & (1UL << i)) ? DL_MDIO_OUT : 0));
        ASIC_PUT(nic, DL_GPIO, m | d);
        ASIC_PUT(nic, DL_GPIO, m | d | DL_MDIO_CLK);
    }
    for (i = 19; i > 0; i--)
    {
        ASIC_PUT(nic, DL_GPIO, m);
        r = (UWORD)((r << 1) |
                    ((ASIC_GET(nic, DL_GPIO) & DL_MDIO_IN) ? 1u : 0u));
        ASIC_PUT(nic, DL_GPIO, m | DL_MDIO_CLK);
    }
    return (UWORD)(r >> 1);
}

static VOID dl_mii_write(NetdevNic *nic, UBYTE phy, UBYTE loc, UWORD val)
{
    ULONG cmd = (0x05UL << 28) | ((ULONG)phy << 23) | ((ULONG)loc << 18) |
                (1UL << 17) | (ULONG)val;
    UBYTE m   = (UBYTE)(ASIC_GET(nic, DL_GPIO) & 0x0fu);
    WORD  i;

    for (i = 0; i < 32; i++)
    {
        ASIC_PUT(nic, DL_GPIO, m | DL_MDIO_WRITE | DL_MDIO_OUT);
        ASIC_PUT(nic, DL_GPIO, m | DL_MDIO_WRITE | DL_MDIO_OUT | DL_MDIO_CLK);
    }
    for (i = 31; i >= 0; i--)
    {
        UBYTE d = (UBYTE)(DL_MDIO_WRITE |
                          ((cmd & (1UL << i)) ? DL_MDIO_OUT : 0));
        ASIC_PUT(nic, DL_GPIO, m | d);
        ASIC_PUT(nic, DL_GPIO, m | d | DL_MDIO_CLK);
    }
    for (i = 1; i >= 0; i--)
    {
        ASIC_PUT(nic, DL_GPIO, m);
        ASIC_PUT(nic, DL_GPIO, m | DL_MDIO_CLK);
    }
}

static UWORD dl_eeprom_read(NetdevNic *nic, UWORD loc)
{
    UWORD cmd = (UWORD)(loc | (DL_EE_READ << 8));
    UWORD r   = 0;
    WORD  i;

    ASIC_PUT(nic, DL_EEPROM, 0);
    ASIC_PUT(nic, DL_EEPROM, DL_EE_EEP | DL_EE_CS);
    for (i = 10; i >= 0; i--)
    {
        UBYTE d = (UBYTE)((cmd & (1u << i)) ? DL_EE_DO : 0);
        ASIC_PUT(nic, DL_EEPROM, DL_EE_EEP | DL_EE_CS | d);
        ASIC_PUT(nic, DL_EEPROM, DL_EE_EEP | DL_EE_CS | d | DL_EE_CK);
    }
    ASIC_PUT(nic, DL_EEPROM, DL_EE_EEP | DL_EE_CS);
    for (i = 16; i > 0; i--)
    {
        ASIC_PUT(nic, DL_EEPROM, DL_EE_EEP | DL_EE_CS | DL_EE_CK);
        r = (UWORD)((r << 1) |
                    ((ASIC_GET(nic, DL_EEPROM) & DL_EE_DI) ? 1u : 0u));
        ASIC_PUT(nic, DL_EEPROM, DL_EE_EEP | DL_EE_CS);
    }
    ASIC_PUT(nic, DL_EEPROM, 0);
    return r;
}

/* An internal ASIC register is written by an EEPROM read with the ASIC
   select bit set, the data shifted out on DI. */
static VOID dl_asic_write(NetdevNic *nic, UWORD loc, UWORD data)
{
    UWORD cmd = (UWORD)((loc | (DL_EE_READ << 8)) >> 1);
    WORD  i;

    data |= dl_eeprom_read(nic, loc);

    ASIC_PUT(nic, DL_EEPROM, 0);
    ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_CS | DL_EE_DI);
    for (i = 9; i >= 0; i--)
    {
        UBYTE d = (UBYTE)((cmd & (1u << i)) ? DL_EE_DO : 0);
        ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_CS | DL_EE_DI | d);
        ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_CS | DL_EE_DI | d | DL_EE_CK);
        ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_CS | DL_EE_DI | d);
    }
    ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_CS);
    ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_CS | DL_EE_CK);
    ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_CS);
    for (i = 15; i >= 0; i--)
    {
        UBYTE d = (UBYTE)((data & (1u << i)) ? DL_EE_DI : 0);
        ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_CS | d);
        ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_CS | d | DL_EE_CK);
        ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_CS | d);
    }
    ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_DI);
    ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_DI | DL_EE_CK);
    ASIC_PUT(nic, DL_EEPROM, DL_EE_ASIC | DL_EE_DI);
    ASIC_PUT(nic, DL_EEPROM, 0);
}

/* The first MII address whose status register answers, 0xff for none. */
static UBYTE dl_find_phy(NetdevNic *nic)
{
    UBYTE p;

    for (p = 1; p <= 32; p++)
    {
        UWORD bmsr = dl_mii_read(nic, (UBYTE)(p & 31u), 1);

        if (bmsr != 0 && bmsr != 0xffffu)
            return (UBYTE)(p & 31u);
    }
    return 0xffu;
}

/* The link as negotiated: the MAC's duplex follows it, and the speed is
   what S2_DEVICEQUERY reports. */
static VOID dl_link(NetdevNic *nic)
{
    UWORD bmsr;
    UWORD both;
    UBYTE fdx;

    (VOID)dl_mii_read(nic, nic->dl_phy, 1);     /* the link bit latches low */
    bmsr = dl_mii_read(nic, nic->dl_phy, 1);
    if ((bmsr & 0x0004u) == 0)
        return;

    both = (UWORD)(dl_mii_read(nic, nic->dl_phy, 4) &
                   dl_mii_read(nic, nic->dl_phy, 5));
    fdx  = (UBYTE)((both & 0x0140u) != 0);
    nic->link_bps = (both & 0x0180u) ? 100000000UL : 10000000UL;

    if (fdx == nic->dl_fdx)
        return;
    nic->dl_fdx = fdx;
    if (nic->dl_kind == 22)
        ASIC_PUT(nic, DL_DIAG, fdx ? 4u : 0u);
    else
        dl_asic_write(nic, 4, fdx ? DL19_FDUPLX : 0);
}

/*
 * Once a blank under Disable(), with the interrupt server out
 * (netdev_device.c): every AX88796_PHY_TICKS a look at the negotiation,
 * three register reads.  The link bit latches low, so a link that dropped
 * since the last look answers 0 for a second.  A PHY found no longer
 * offering PAUSE -- something reset it -- is told again.
 */
static BOOL ne2000_tick(NetdevNic *nic)
{
    LONG bmsr;
    LONG anar;
    LONG anlpar;

    if (nic->dl_kind != 0 && ++nic->dl_tick >= DL_LINK_TICKS)
    {
        nic->dl_tick = 0;
        dl_link(nic);
    }

    if (!nic->ax_phy || ++nic->ax_phy_tick < AX88796_PHY_TICKS)
        return FALSE;
    nic->ax_phy_tick = 0;

    bmsr   = ax_mii_read(nic, MII_BMSR);
    anar   = ax_mii_read(nic, MII_ANAR);
    anlpar = ax_mii_read(nic, MII_ANLPAR);

    nic->rx_pause_capacity = ax88796_pause_capacity(bmsr, anar, anlpar);
    if (anar >= 0 && (anar & MII_ANAR_PAUSE) != 0 &&
        (nic->link_speed_limit != 10 ||
         (anar & MII_ANAR_MODES) == MII_ANAR_10_MODES))
        nic->ax_phy_tries = 0;
    else if (anar >= 0 && nic->ax_phy_tries < AX88796_PHY_TRIES)
    {
        nic->ax_phy_tries++;
        ax88796_advertise(nic);
    }

    return FALSE;
}

static BOOL ne2000_coherent(NetdevNic *nic)
{
    ULONG  inbuf[2];
    UBYTE *back  = (UBYTE *)inbuf;
    UBYTE  saved = nic->bus.dmode;

    ne2000_probe_reset(nic);
    ne2000_ax88796_flow(nic);           /* the pulse cleared it (F-287) */
    NIC_PUT(nic, ED_P0_RCR, ED_RCR_MON);
    NIC_PUT(nic, ED_P0_DCR, ED_DCR_FT1 | ED_DCR_LS | ED_DCR_WTS);

    nic->bus.dmode = NETDEV_DMODE_WORD;
    ne2000_writemem(nic, ne_coherence_pattern, 16384,
                    (UWORD)sizeof(ne_coherence_pattern));
    ne2000_readmem(nic, 16384, back, (UWORD)sizeof(ne_coherence_pattern));
    nic->bus.dmode = saved;

    NIC_PUT(nic, ED_P0_ISR, 0xff);

    return (BOOL)(ne_memcmp(ne_coherence_pattern, back,
                            (UWORD)sizeof(ne_coherence_pattern)) == 0);
}

/* --------------------------------------------------------------- attach --- */

/*
 * NetBSD zeroes and reads back the whole buffer before it trusts the card.
 * Without it a board with bad buffer RAM is accepted and the receive ring is
 * left holding whatever was there.  A page at a time, so staging stays small.
 */
static BOOL ne2000_test_mem(NetdevNic *nic)
{
    ULONG  zero[ED_PAGE_SIZE / 4];
    ULONG  back[ED_PAGE_SIZE / 4];
    UWORD  i;
    LONG   off;

    for (i = 0; i < ED_PAGE_SIZE / 4; i++)
        zero[i] = 0;

    for (off = 0; off < nic->mem_size; off += ED_PAGE_SIZE)
    {
        ne2000_writemem(nic, (const UBYTE *)zero, nic->mem_start + off,
                        ED_PAGE_SIZE);
        ne2000_readmem(nic, nic->mem_start + off, (UBYTE *)back, ED_PAGE_SIZE);
        if (ne_memcmp((const UBYTE *)zero, (const UBYTE *)back,
                      ED_PAGE_SIZE) != 0)
        {
            ne_note_mismatch(nic, (const UBYTE *)zero, (const UBYTE *)back,
                             (UWORD)ED_PAGE_SIZE, nic->mem_start + off);
            return FALSE;
        }
    }

    return TRUE;
}

/*
 * D-Link DL10019 / DL10022 (DFE-670TXD and the NE2000-compatible PC Cards
 * built on them): the station address is in ASIC registers 4..9 ($14..$19),
 * with a card ID at $1A and a checksum at $1B over $14..$1B that makes the
 * eight bytes sum to $FF (Linux pcnet_cs get_dl10019, NetBSD dl10019.c).  $1F
 * is the reset port and is not read.  The chip also carries more than the
 * standard 16 KB: 32 KB on the DFE-670TXD (D-Link data sheet), see below.
 */
static BOOL ne2000_dl100xx(NetdevNic *nic, UBYTE *mac)
{
    UBYTE sum = 0;
    UBYTE b;
    UWORD i;

    for (i = 0; i < 8; i++)
    {
        b = ASIC_GET(nic, 4u + i);
        if (i < NETDEV_ADDR_LEN)
            mac[i] = b;
        sum = (UBYTE)(sum + b);
    }

    return (BOOL)(sum == 0xffu && netdev_mac_usable(mac) && (mac[0] & 1u) == 0);
}

/*
 * Whether [start, start + size) is that many distinct bytes of buffer RAM: a
 * page number written at the head of every page reads back from every page.
 * A part with less memory mirrors it, and the mirror reads back the last
 * page written to it instead.
 */
static BOOL ne2000_mem_distinct(NetdevNic *nic, LONG start, LONG size)
{
    UBYTE tag[4];
    UBYTE back[4];
    LONG  off;

    for (off = 0; off < size; off += ED_PAGE_SIZE)
    {
        tag[0] = (UBYTE)(off >> 8);
        tag[1] = (UBYTE)~tag[0];
        tag[2] = 0x5au;
        tag[3] = 0xa5u;
        ne2000_writemem(nic, tag, start + off, 4);
    }
    for (off = 0; off < size; off += ED_PAGE_SIZE)
    {
        ne2000_readmem(nic, start + off, back, 4);
        if (back[0] != (UBYTE)(off >> 8) || back[1] != (UBYTE)~back[0] ||
            back[2] != 0x5au || back[3] != 0xa5u)
            return FALSE;
    }
    return TRUE;
}

static LONG ne2000_attach(NetdevNic *nic)
{
    UBYTE romdata[32];
    UBYTE dlmac[NETDEV_ADDR_LEN];
    BOOL  dl;
    UWORD i;

    if (!ne2000_detect(nic))
        return -1;

    nic->mem_start = 16384;
    nic->mem_size  = 16384;

    /* A DL100xx with its 24 KB intact takes all of it: half again the receive
       ring, which on a card read slower than the wire delivers is the window
       a long path gets before the ring overruns. */
    dl = ne2000_dl100xx(nic, dlmac);
    nic->dl_kind = 0;
    nic->dl_fdx  = 0xffu;
    nic->link_bps = 0;
    /*
     * The DFE-670TXD carries 32 KB, at $8000-$FFFF; $4000-$7FFF is the same
     * RAM with A15 ignored, which is why 32 KB placed at 16K fails (FreeBSD).
     * The page registers stop at $FF, so the ring ends one page short: 127
     * pages, 18 receive frames beside three transmit buffers.  24 KB at 24K
     * is the next best for a part that carries less.
     */
    if (dl && ne2000_mem_distinct(nic, 32768, 32512))
    {
        nic->mem_start = 32768;
        nic->mem_size  = 32512;
    }
    else if (dl && ne2000_mem_distinct(nic, 24576, 24576))
    {
        nic->mem_start = 24576;
        nic->mem_size  = 24576;
    }
    if (dl)
    {
        nic->dl_phy = dl_find_phy(nic);
        if (nic->dl_phy != 0xffu)
        {
            nic->dl_kind = (UBYTE)((nic->reset_id == 0x91u ||
                                    nic->reset_id == 0x99u) ? 22 : 19);

            /*
             * 10BASE-T only.  These chips have no PAUSE, and the host reads
             * their port slower than 100 Mbit/s delivers: on a PiStorm A1200
             * 1.23 us a word, 12.7 Mbit/s.  A 100 Mbit/s burst overruns the
             * ring, and a download fell to 5.9 Mbit/s against 9.4 at
             * 10 Mbit/s, where nothing overruns.  The duplex and speed follow
             * from the tick once autonegotiation has finished.
             */
            dl_mii_write(nic, nic->dl_phy, 4, 0x0061);
            dl_mii_write(nic, nic->dl_phy, 0, 0x1200);
        }
    }

    nic->cr_proto  = ED_CR_RD2;

    /*
     * rcr_proto is zero for every part in the table.  A new row could need
     * ED_RCR_INTT with a retried ISR acknowledge (AX88190/AX88790), the two
     * ISR.RDC waits skipped, or byte-wide DMA: none of that is implemented.
     */
    nic->rcr_proto = 0;
    nic->dcr_reg   = ED_DCR_FT1 | ED_DCR_LS | ED_DCR_WTS;

    nic->read_hdr  = ne2000_read_hdr;
    nic->ring_copy = ne2000_ring_copy;
    nic->ring_copy_sum = ne2000_ring_copy_sum;
    nic->frame_at  = NULL;   /* a port has no address to hand out */
    nic->write_buf = ne2000_write_buf;

    ne2000_probe_wide(nic);
    nic->rx_flags_supported = (UBYTE)(
        nic->bus.dmode != NETDEV_DMODE_BYTE ? ANXD_S2_RXF_VERIFIED : 0);

    /*
     * ANXD_CMD_RX_BATCH: one reply for every frame a service pass takes,
     * instead of one per frame.  dp8390_rint() walks the whole ring in a
     * pass, and on a fast link it is not one frame an interrupt: a burst
     * the sender put on the wire back to back is in the ring together, and
     * more so with PAUSE holding the rest behind it.  Each frame otherwise
     * costs a ReplyMsg(), a Signal(), a GetMsg() and a re-post across the
     * driver and the reader.  The claim and the end-of-pass flush are the
     * shell's (netdev_direct.c, netdev_interrupt_do), the same for every
     * core; a pass that takes one frame answers one, as a read would.
     */
    nic->rx_batches = 1;

    /*
     * Where the station address is depends on the part.  The AX88796 keeps it
     * at AX88190_NODEID_OFFSET; a board wired as a plain NE2000 images a serial
     * ROM into the first 32 buffer bytes with 0x57 0x57 at the end.  Read the
     * ROM image first and believe it only on that signature.
     */
    ne2000_readmem(nic, 0, romdata, sizeof(romdata));
    if (romdata[28] == 0x57 && romdata[30] == 0x57)
    {
        for (i = 0; i < NETDEV_ADDR_LEN; i++)
            nic->factory[i] = romdata[i * 2];
    }
    else if (nic->card->ax88796)
    {
        UBYTE saved = nic->bus.dmode;
        UBYTE id[8];

        /*
         * The node ID, through the 16-bit port whatever the data mode, with
         * the FIFO threshold the frame path uses.  This read once set DCR to
         * WTS alone and took the longword window: on a real X-Surf 100 in an
         * A3000 the window's first longword of the burst came back stale
         * (A4 A6 A3 AC 28 CD for 28 CD 4C FF F2 A6, the card then on the
         * wrong address) while the same six bytes through the port, read
         * after a few register reads' pause, were right every time.  A
         * 32-bit access is two chip reads with no wait between them, and
         * with a one-word threshold the second finds nothing ready.  Six
         * bytes once at attach: the port is the right path.
         */
        NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STA);
        NIC_PUT(nic, ED_P0_DCR, nic->dcr_reg);
        nic->bus.dmode = NETDEV_DMODE_WORD;
        ne2000_dma_start(nic, AX88190_NODEID_OFFSET, 8, 0);
        dp_pause_reads(nic, 8);
        netdev_bus_rdata(&nic->bus, id, 8);
        nic->bus.dmode = saved;
        for (i = 0; i < NETDEV_ADDR_LEN; i++)
            nic->factory[i] = id[i];
        netdev_diag_note(ANXDIAG_NE_NODEID_PORT, netdev_diag_card(nic->card),
                         ((ULONG)id[0] << 24) | ((ULONG)id[1] << 16) |
                         ((ULONG)id[2] << 8) | id[3]);
    }
    else
    {
        for (i = 0; i < NETDEV_ADDR_LEN; i++)
            nic->factory[i] = romdata[i * 2];
    }

    if (dl)
    {
        for (i = 0; i < NETDEV_ADDR_LEN; i++)
            nic->factory[i] = dlmac[i];
    }

    /*
     * Clear the group bit in the ROM address: the DP8390 comparator treats bit
     * 0 of octet 0 as the group bit, so a PROM like the DFE-670TXD's
     * 01:D4:FF:03:00:20 never matches its own unicast frames.  A no-op on every
     * card whose PROM is right.
     */
    nic->mac_source = (UBYTE)ANXDIAG_MAC_PROM;

    if (!netdev_mac_blank(nic->factory) && (nic->factory[0] & 1u) != 0)
    {
        nic->factory[0] &= (UBYTE)~1u;
        nic->mac_group_fix++;
        nic->mac_source = (UBYTE)ANXDIAG_MAC_PROM_FIXED;
        NE_TRACE("ne: rom group bit cleared ", (ULONG)nic->factory[0]);
    }

    /*
     * All-zero and all-ones both mean the PROM is not answering.  The card's
     * CIS comes first, which is where the PC Card standard puts a LAN address,
     * and a derived locally-administered address after that -- never a
     * hardcoded one, which two Amigas on one segment would share.
     */
    if (!netdev_mac_usable(nic->factory))
    {
        NE_TRACE("ne: prom has no address ", 0);

        if (netdev_mac_cis_node_id(nic->factory))
        {
            nic->mac_from_cis++;
            nic->mac_source = (UBYTE)ANXDIAG_MAC_CIS;
        }
        else
        {
            UBYTE fp[NETDEV_MAC_FP_MAX];
            UWORD n;
            ULONG salt = nic->serial ^
                         ((ULONG)nic->card->manid << 16) ^
                         (ULONG)nic->card->prodid ^
                         (ULONG)(APTR)nic->board;

            n = netdev_mac_fingerprint(fp, (UWORD)sizeof(fp), salt);
            netdev_mac_derive(fp, n, nic->factory);
            nic->mac_derived++;
            nic->mac_source = (UBYTE)ANXDIAG_MAC_DERIVED;
        }

        NE_TRACE("ne: address now ", ((ULONG)nic->factory[2] << 24) |
                                     ((ULONG)nic->factory[3] << 16) |
                                     ((ULONG)nic->factory[4] << 8) |
                                     (ULONG)nic->factory[5]);
        NE_TRACE("ne: address top ", ((ULONG)nic->factory[0] << 8) |
                                     (ULONG)nic->factory[1]);
    }

    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        nic->mac[i] = nic->factory[i];

    NIC_PUT(nic, ED_P0_ISR, 0xff);

    dp8390_config(nic);

    if (!ne2000_test_mem(nic))
    {
        /* Not WHY_BUFFER: the 32-byte probe in ne2000_detect() already passed,
           so the data port works and this is the RAM behind it. */
        nic->diag_why = (UBYTE)ANXDIAG_WHY_MEM;
        return -1;
    }

    dp8390_halt(nic);

    /*
     * Here, after the probe's reset pulse and before the first init.
     * dp8390_halt()/dp8390_init() program the DP8390 register file and leave
     * the MAC's configuration alone, so the watchdog's and the overwrite
     * recovery's resets keep this.  The reset port clears it, and
     * ne2000_coherent() strobes that port again on the first Open of a 68030
     * cache check, so it sets it again there (F-287).
     */
    ne2000_ax88796_flow(nic);
    ne2000_ax88796_phy(nic);

    return 0;
}

const struct NetdevNicOps netdev_nic_ne2000 =
{
    ne2000_attach,
    dp8390_init,
    dp8390_halt,
    dp8390_tx,
    dp8390_setfilter,
    dp8390_intr,
    dp8390_reset,
    ne2000_tick,        /* the AX88796B's PAUSE negotiation */
    ne2000_coherent,
    NULL                /* no task to end */
};
