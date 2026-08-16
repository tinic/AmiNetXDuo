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
 * NetBSD's.  What was replaced:
 *
 *   bus_space_*   -> netdev_bus.h, so the register stride is the card's
 *   mbuf chains   -> one linear frame
 *   delay()       -> a bus-read spin; there is no timer at device-init time
 *   printf        -> counters
 *
 * Added, and not from NetBSD: netdev_ne2000_probe_wide().  The X-Surf 100 has
 * a second, 32-bit image of the same data port.  A driver that assumes it is
 * there silently transfers garbage on a board that does not have one, and a
 * driver that assumes it is not there gives up half the throughput.  It is
 * therefore written and read back before it is used, and the answer is
 * reported rather than guessed.
 *
 * ne2000reg.h beside this file is NetBSD's, verbatim.
 *
 * SPDX-License-Identifier: MIT AND BSD-2-Clause-NetBSD
 */

#include "netdev_nic.h"
#include "dp8390.h"
#include "netdev_bsdtypes.h"
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

#define NIC_GET(nic, reg)       netdev_bus_r8(&(nic)->bus, (reg))
#define NIC_PUT(nic, reg, val)  netdev_bus_w8(&(nic)->bus, (reg), (UBYTE)(val))
#define ASIC_GET(nic, reg)      netdev_bus_ra8(&(nic)->bus, (reg))
#define ASIC_PUT(nic, reg, val) netdev_bus_wa8(&(nic)->bus, (reg), (UBYTE)(val))

/*
 * There is no timer open when a unit is probed, and a device cannot call
 * Delay().  A read of a register the chip always answers is a real Zorro bus
 * cycle, 280 ns at the fastest a Zorro II board can be and slower on
 * everything else, so a count is a lower bound on the microseconds.  Only the
 * reset path waits, and the chip is ready long before the wait ends.
 */
static VOID ne_delay(NetdevNic *nic, ULONG us)
{
    ULONG n = us * 4u;

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
 * record.  "The buffer did not read back" is true of a dead data port, of one
 * byte lane stuck, and of a card whose port swaps the halves of every word,
 * and the wrote/read pair separates them: $5a/$00 is a lane that is not there,
 * $5a/$a5 at an even offset is a swap.
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
 * leaves the pointer.  `over` is how much more the burst is allowed to cover
 * beyond `amount`, so a caller that knows the next read is contiguous can pay
 * for one setup instead of two.
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

static VOID ne2000_readmem(NetdevNic *nic, LONG src, UBYTE *dst, UWORD amount)
{
    amount = (UWORD)((amount + 1u) & ~1u);

    /*
     * The burst already running is at this address with room to spare, so the
     * chip needs telling nothing: reading the port continues it.  Six
     * register accesses saved on every frame.
     */
    if (nic->dma_left >= amount && nic->dma_pos == src)
    {
        nic->dma_pos  = src + (LONG)amount;
        nic->dma_left = (UWORD)(nic->dma_left - amount);
    }
    else
    {
        ne2000_dma_start(nic, src, amount, 0);
    }

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
     * Overshoot the header deliberately.  The body follows it in the ring, so
     * one setup covering both is one setup fewer per frame.  The burst is
     * bounded by what is left before the ring wraps, so it can never read past
     * the end of the buffer.  A frame that does wrap finds the position stale
     * and programs its own.
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
 * The frame is already linear and already padded to the Ethernet minimum by
 * the caller, so this is one remote-DMA write.  The return is the length the
 * chip was told to transmit.
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
     * RBCR is the frame length, odd or not, and the push below is rounded up.
     * That looks like a mismatch against ne2000_readmem(), which rounds before
     * it programs RBCR, but it is not the same question:
     *
     *   readmem rounds because the destination is host memory and the port
     *   hands out whole words, so the rounding protects the caller's buffer.
     *   Here RBCR only bounds the DMA into buffer RAM.  What goes on the wire
     *   is TBCR, which dp8390_xmit() sets from txb_len, which is the unrounded
     *   length.  A rounded-up odd frame therefore parks one extra byte in the
     *   transmit slot (1515 bytes worst case in a 1536-byte slot) and never
     *   transmits it.
     *
     *   The one hazard is ISR.RDC.  A part that terminates the DMA on a count
     *   that equals zero, rather than on a count exhausted, never asserts it
     *   for an odd RBCR.  The wait below would then time out, and the chip
     *   would be reset once per odd frame.  Measured on the emulated X-Surf
     *   100 (tests/tools/oddtx.c, transfer_mode=2): eight odd frames from 61
     *   to 1513 bytes, all error=0, resets_delta=0, and even-length control
     *   frames likewise.  This is also NetBSD's arrangement unchanged, which
     *   has run on real NE2000 hardware for decades.
     *
     *   Amiberry terminates on a count exhausted ("if (s->rcnt <= len)
     *   s->rcnt = 0"), so the rig cannot tell the two implementations apart.
     *   A real part that hangs here shows up as "Chip resets" climbing in
     *   S2_GETSPECIALSTATS, one per odd frame, with CMD_WRITE answering
     *   S2ERR_TX_FAILURE.  A rounded-up RBCR here is then the fix, and it
     *   costs nothing.
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
 * Can this bus need cnet16's word reads?  Mirrors what
 * netdev_bus_set_getodd() refuses, and is asked first so that no card whose
 * registers are plain adjacent bytes has its detection sequence changed by any
 * of this.  Only the PCMCIA row is a split stride-1 file.
 */
static BOOL ne2000_odd_window(const NetdevNic *nic)
{
    return (BOOL)(nic->bus.odd != NULL && nic->bus.regmap == NULL &&
                  nic->bus.stride == 1u);
}

/*
 * Do odd-numbered registers read correctly the way they are being read now?
 *
 * Two tests, because neither one alone is enough.
 *
 *   ISR after a reset has ED_ISR_RST set.  That is the value NetBSD checks,
 *   and it is why this is the right point in the sequence.  A mis-decoded read
 *   often returns 0xff, which has that bit set, so on its own it passes on the
 *   card this is looking for.
 *
 *   BNRY is register 3, odd, and read/write, and the ring is not programmed
 *   until dp8390_config(), so its value here belongs to nobody.  Two patterns,
 *   one the complement of the other, so neither a stuck-high nor a stuck-low
 *   bus can round-trip both.
 *
 * The chip is in page 0 and stopped, which is where the caller left it.
 *
 * All three reads happen every time, and the three bytes come back rather than
 * a verdict, because one verdict covers three different cards.  $ff $ff $ff is
 * a floating bus.  A wrong ISR with the round-trips intact is a chip that has
 * not finished resetting.  A good ISR with the round-trips dead is the
 * 16-bit-only card the word path exists for.  A write to BNRY costs nothing:
 * it is read/write, the chip is stopped, and dp8390_config() programs it
 * afterwards.
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
 * NetBSD's ne2000_detect, minus the NE1000 arm: no card in this family has an
 * 8-bit buffer, and the byte-mode write it uses to find one is invasive.
 * TRUE means a DS8390 answered and its 16 KB of buffer reads back.
 */
static BOOL ne2000_detect(NetdevNic *nic)
{
    UBYTE test_buffer[32];
    UBYTE tmp;
    UWORD i;

    tmp = ASIC_GET(nic, NE2000_ASIC_RESET);
    ne_delay(nic, 10000);
    ASIC_PUT(nic, NE2000_ASIC_RESET, tmp);
    ne_delay(nic, 5000);

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STP);
    ne_delay(nic, 5000);

    /*
     * ED_CR_STA is not in the mask, and NetBSD's is the only version of this
     * comparison that has it there.  Some NE2000 clones come out of reset with
     * CR bit 1 stuck set and read back 0x23 where the datasheet says 0x21.
     * cnet.device masks DSCM_START out of its own copy of this test
     * (cnetdevice.asm:3611-3615) for "buggy chips" and names the Netgear
     * FA411.  With the bit demanded clear, those cards fail detection and the
     * driver reports an empty slot.
     *
     * This must keep rejecting a window with nothing behind it, and it does.
     * A floating bus reads 0xff, which has TXP set and fails the comparison
     * whether STA is masked or not.
     */
    tmp = NIC_GET(nic, ED_P0_CR);
    netdev_diag_note(ANXDIAG_CR_READ, netdev_diag_card(nic->card), (ULONG)tmp);
    if ((tmp & (ED_CR_RD2 | ED_CR_TXP | ED_CR_STP)) !=
        (ED_CR_RD2 | ED_CR_STP))
    {
        nic->diag_why = (UBYTE)ANXDIAG_WHY_CR;
        return FALSE;
    }

    /*
     * The cnet16 probe, which is a probe rather than a second binary.
     *
     * A Fast-Ethernet NE2000 clone that asserts -IOIS16 unconditionally
     * decodes 16-bit I/O cycles and nothing else, so a byte read of an odd
     * register returns bus noise.  Every ISR poll, CR readback and CURR/BNRY
     * ring pointer this driver makes is an odd register, so the symptom is a
     * card that attaches and receives nothing.  cnet answers this with a
     * separate cnet16.device (cnetdevice.asm:551-556, naming the Netgear FA411
     * and the CNet SinglePoint 10/100) and leaves the user to work out which
     * of the two the card needs.
     *
     * CR, read just above, is register 0 and even, so it answers on such a
     * card and cannot tell one apart.  The first odd register touched is where
     * the question must be asked, and if it fails the word path is turned on
     * and it is asked again.  On a card that does not need this the first call
     * succeeds and nothing changes.  On a window with no chip behind it both
     * calls fail and the card is rejected as before.
     *
     * Every other card keeps NetBSD's test, unchanged, byte for byte.  A Zorro
     * board's registers are adjacent bytes at a stride and cannot need any of
     * this, so a new way for them to fail detection buys nothing.
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
        UWORD ci    = netdev_diag_card(nic->card);
        ULONG plain = ne2000_odd_seen(nic);

        /* The odd window, recorded whatever happens next.  A PCMCIA row that
           reached here with none would be reading the ASIC reset at an odd
           address in the even window, and nothing else in the report says
           so. */
        netdev_diag_note(ANXDIAG_ODDWIN, ci, (ULONG)(APTR)nic->bus.odd);
        netdev_diag_note(ANXDIAG_ODD_PLAIN, ci, plain);

        if (!ne2000_odd_reads_ok(plain))
        {
            ULONG word;

            netdev_diag_note(ANXDIAG_ODD_RETRY, ci, 1);

            if (!netdev_bus_set_getodd(&nic->bus))
            {
                nic->diag_why = (UBYTE)ANXDIAG_WHY_ODD;
                return FALSE;
            }

            NE_TRACE("ne: trying cnet16 odd reads ", 0);
            word = ne2000_odd_seen(nic);
            netdev_diag_note(ANXDIAG_ODD_WORD, ci, word);

            if (!ne2000_odd_reads_ok(word))
            {
                /* Back to plain bytes.  Word reads are kept only where they
                   demonstrably beat bytes, never as the state a failure is
                   left in, so nothing downstream and no second probe inherits
                   a mode this one did not earn. */
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

    for (i = 0; i < 100; i++)
    {
        if ((NIC_GET(nic, ED_P0_ISR) & ED_ISR_RST) == ED_ISR_RST)
        {
            NIC_PUT(nic, ED_P0_ISR, ED_ISR_RST);
            break;
        }
        ne_delay(nic, 100);
    }

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
 * result is what S2_GETSPECIALSTATS reports as the transfer mode, so the slow
 * path is visible rather than inferred.
 */
static VOID ne2000_probe_wide(NetdevNic *nic)
{
    /*
     * Both buffers are ULONG arrays, and that matters.  netdev_bus.c refuses
     * the 32-bit path for a buffer that is not 4-aligned and silently falls
     * back to 16-bit moves.  That is correct for a data path and wrong for a
     * probe: the readback then matches without the wide window ever having
     * been touched, and the mode is promoted on no evidence.  That happened
     * when the second leg read from `ne_test_pattern + 16`.  The pattern sits
     * at .text+0x8da, so +16 is 2 mod 4, and the leg meant to test the
     * transmit direction tested the 16-bit port.
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
     * Leg 2: written wide, read narrow, which is the direction that
     * transmits.  A different pattern, so that a buffer left over from leg 1
     * cannot make this one pass.
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
 * NetBSD zeroes and reads back the whole buffer before it trusts the card
 * (dp8390_test_mem / ne2000_test_mem).  Without it, a board with bad buffer
 * RAM is accepted, and the receive ring is left holding whatever was there,
 * which feeds the corrupt-header path in dp8390_rint().  A page at a time, so
 * the staging buffer stays small.
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
     * rcr_proto is zero for every part in the table.  What a new row can need,
     * none of which is implemented here (NetBSD ne2000.c, ne2000_attach):
     *
     *   AX88190 / AX88790   rcr_proto = ED_RCR_INTT, and the ISR acknowledge
     *                       must be retried, because one ISR write does not
     *                       always clear it on those parts.
     *   a part with no ISR.RDC   the two waits in this file must be skipped
     *                       rather than allowed to time out.  A timeout here
     *                       resets the chip.
     *   NE1000 / 8-bit      byte-wide remote DMA, 8 KB of buffer, and the
     *                       station address at romdata[i] rather than [i * 2].
     *
     * Flags for all three used to sit in NetdevNic assigned once and never
     * set, so the code read as though the quirks were handled.  They are not.
     */
    nic->rcr_proto = 0;
    nic->dcr_reg   = ED_DCR_FT1 | ED_DCR_LS | ED_DCR_WTS;

    nic->read_hdr  = ne2000_read_hdr;
    nic->ring_copy = ne2000_ring_copy;
    nic->frame_at  = NULL;   /* a port has no address to hand out */
    nic->write_buf = ne2000_write_buf;

    ne2000_probe_wide(nic);

    /*
     * Where the station address is depends on the part, not on the card, and
     * an X-Surf 100 is both.  The AX88796 keeps it at AX88190_NODEID_OFFSET.
     * A board wired as a plain NE2000, which is what the emulated one is and
     * what a re-badged clone can be, images a serial ROM into the first 32
     * bytes of the buffer with 0x57 0x57 at the end of it.  Read the ROM
     * image, believe it if the signature is there, and only then look in the
     * AX88796's own space.  A guess from the Zorro product ID gets one of the
     * two wrong.
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
     * Clear the group bit in the ROM address.
     *
     * The D-Link DFE-670TXD's PROM reads 01:D4:FF:03:00:20.  Programmed into
     * PAR0..5 as it stands, the DP8390 does not match its own unicast address,
     * because the comparator treats bit 0 of octet 0 as the group bit.  Every
     * frame the card transmits then carries a multicast source address, which
     * some switches drop and every receiver mis-learns.  cnet.device clears the
     * bit unconditionally and warns (cnetdevice.asm:3666-3672).  This is the
     * same fix, and it is a no-op on every card whose PROM is right.
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
     * And if there is no address in the PROM at all.
     *
     * All-zero and all-ones both mean the PROM is not answering.  ed.c has
     * rejected them since it was written (ed.c:406), and this path never
     * tested for them, so such a card enumerated a unit that came online and
     * was never delivered a frame.  A rejection is one answer, but not a
     * useful one, because a PCMCIA clone with a blank PROM works once it has
     * an address.
     *
     * The card's CIS comes first, which is where the PC Card standard puts a
     * LAN address and where a card built for a CIS-reading PC driver keeps it.
     * A derived locally-administered address comes after that.  Not
     * cnet.device's hardcoded 00:00:12:34:56:78 (cnetdevice.asm:5445-5447),
     * whose own comment is "replace this with your card's address!".  Two
     * Amigas running it on one segment answer each other's ARP.
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
