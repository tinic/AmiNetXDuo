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
#include "dp8390reg.h"
#include "ne2000reg.h"

#define AX88190_NODEID_OFFSET   0x400

#ifdef NETDEV_TRACE
extern VOID netdev_trace_val(const char *tag, ULONG v);
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
     * Both buffers must be ULONG arrays: netdev_bus.c refuses the 32-bit path
     * for a buffer that is not 4-aligned and silently falls back to 16-bit
     * moves, so a misaligned probe promotes the mode on no evidence.
     */
    ULONG  outbuf[NETDEV_BUS_PROBE_LEN / 4];
    ULONG  inbuf[NETDEV_BUS_PROBE_LEN / 4];
    UBYTE *out = (UBYTE *)outbuf;
    UBYTE *back = (UBYTE *)inbuf;
    UWORD  i;

    if (nic->bus.wide == NULL)
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

static LONG ne2000_attach(NetdevNic *nic)
{
    UBYTE romdata[32];
    UWORD i;

    if (!ne2000_detect(nic))
        return -1;

    nic->mem_start = 16384;
    nic->mem_size  = 16384;

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
        NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STA);
        NIC_PUT(nic, ED_P0_DCR, ED_DCR_WTS);
        ne2000_readmem(nic, AX88190_NODEID_OFFSET, nic->factory,
                       NETDEV_ADDR_LEN);
    }
    else
    {
        for (i = 0; i < NETDEV_ADDR_LEN; i++)
            nic->factory[i] = romdata[i * 2];
    }

    /*
     * Clear the group bit in the ROM address: the DP8390 comparator treats bit
     * 0 of octet 0 as the group bit, so a PROM like the DFE-670TXD's
     * 01:D4:FF:03:00:20 never matches its own unicast frames.  A no-op on every
     * card whose PROM is right.
     */
    nic->mac_source = (UBYTE)ANXDIAG_MAC_PROM;

    if ((nic->factory[0] & 1u) != 0)
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
    dp8390_reset
};
