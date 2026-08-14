/*
 * Copyright (c) 2012 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Frank Wille.
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
 * AmiNetXDuo: adapted from NetBSD sys/arch/amiga/dev/if_ed_zbus.c (rev 1.2).
 * The card geometry -- register, buffer and PROM offsets, the stride, the DCR
 * value, the ring-wrap arithmetic in ring_copy and the 4-byte header's byte
 * order -- is NetBSD's.  What was replaced:
 *
 *   bus_space_*   -> plain moves through the board window; the buffer is
 *                    mapped, so there is no bus abstraction to go through
 *   mbuf chains   -> one linear frame
 *   test_mem      -> a pattern pass before the zero pass, see ed_test_mem()
 *
 * The chip itself is dp8390.c, unchanged and shared with ne2000.c: same
 * register programming, same ring geometry, same ISR drain, same multicast
 * hash.  All that differs is how the packet buffer is reached, which is the
 * three function pointers NetdevNic already carries.
 *
 * SPDX-License-Identifier: MIT AND BSD-2-Clause-NetBSD
 */

#include "netdev_nic.h"
#include "dp8390.h"
#include "netdev_bsdtypes.h"
#include "dp8390reg.h"

#define NIC_GET(nic, reg)       netdev_bus_r8(&(nic)->bus, (reg))
#define NIC_PUT(nic, reg, val)  netdev_bus_w8(&(nic)->bus, (reg), (UBYTE)(val))

/*
 * The packet buffer window.  Recomputed rather than cached, so this core adds
 * no field to NetdevNic: board and card are both already there because every
 * core needs them.
 */
static volatile UBYTE *ed_buf(NetdevNic *nic)
{
    return nic->board + nic->card->mem_off;
}

/*
 * unsigned long, not ULONG: ULONG is 32 bits and this file is also compiled
 * for a 64-bit host by test/test_netdev_ed.c, where the truncation is a
 * -Werror diagnostic and the answer would be wrong anyway.
 */
#define ED_ODD(p)   ((((unsigned long)(const void *)(p)) & 1ul) != 0)

/*
 * Word moves only.  The buffer is 16 bits wide on both boards and the byte
 * lanes are not separately selectable, so a byte write puts the same value in
 * both halves on real hardware even where the emulator accepts it.  Every
 * caller here starts on an even chip address; ED_PAGE_SIZE is 256 and the
 * header is 4 bytes, so nothing rounds it to an odd one.
 *
 * The odd-host-address arms are not reachable from today's callers -- the
 * staging buffers are longword-aligned and a ring wrap resumes on a page
 * boundary -- so test_netdev_ed.c drives them directly rather than leaving
 * two branches that read as handled and have never run.
 */
static VOID ed_copy_in(const volatile UBYTE *src, UBYTE *dst, UWORD len)
{
    const volatile UWORD *s = (const volatile UWORD *)(const volatile void *)src;
    UWORD i;

    if (!ED_ODD(dst))
    {
        UWORD *d = (UWORD *)(APTR)dst;

        for (i = 0; i + 2 <= len; i += 2)
            *d++ = *s++;
        if (i < len)
            dst[i] = (UBYTE)(*s >> 8);
        return;
    }

    for (i = 0; i + 2 <= len; i += 2)
    {
        UWORD w = *s++;

        dst[i]     = (UBYTE)(w >> 8);
        dst[i + 1] = (UBYTE)w;
    }
    if (i < len)
        dst[i] = (UBYTE)(*s >> 8);
}

static VOID ed_copy_out(volatile UBYTE *dst, const UBYTE *src, UWORD len)
{
    volatile UWORD *d = (volatile UWORD *)(volatile void *)dst;
    UWORD i;

    if (!ED_ODD(src))
    {
        const UWORD *s = (const UWORD *)(const void *)src;

        for (i = 0; i + 2 <= len; i += 2)
            *d++ = *s++;
        if (i < len)
            *d = (UWORD)(src[i] << 8);
        return;
    }

    for (i = 0; i + 2 <= len; i += 2)
        *d++ = (UWORD)(((UWORD)src[i] << 8) | src[i + 1]);
    if (i < len)
        *d = (UWORD)(src[i] << 8);
}

static VOID ed_fill(volatile UBYTE *dst, UWORD val, UWORD len)
{
    volatile UWORD *d = (volatile UWORD *)(volatile void *)dst;
    UWORD i;

    for (i = 0; i + 2 <= len; i += 2)
        *d++ = val;
    if (i < len)
        *d = val;
}

/* ---------------------------------------------------------- buffer access - */

/*
 * WHY THE FIRST WORD IS SWAPPED AND THE SECOND IS NOT.
 *
 * ED_DCR_BOS is set, which is what makes the chip lay packet DATA down in
 * host byte order on a 68000: the byte the receiver saw first ends up at the
 * lower address.  It does NOT do the same to the whole 4-byte header, because
 * status/next-page and the byte count are two separate 16-bit quantities to
 * the chip and only the first is a byte pair.  In memory:
 *
 *   +0  next_packet    +1  rsr    +2  count low    +3  count high
 *
 * so the first word reads back swapped and the second reads back in the
 * chip's own little-endian order.  Both NetBSD's ed_zbus_read_hdr() and
 * Linux's hydra_get_8390_hdr() encode exactly this, and Amiberry's DP8390
 * emulation writes exactly this (qemuvga/ne2000.cpp, the `s->dcfg & 2` arm of
 * ne2000_receive).  Assuming one uniform swap gets the count backwards and
 * every frame is then rejected as over-length.
 */
static VOID ed_read_hdr(NetdevNic *nic, LONG src, NetdevRing *hdr)
{
    const volatile UWORD *p =
        (const volatile UWORD *)(const volatile void *)(ed_buf(nic) + src);
    UWORD w0 = p[0];
    UWORD w1 = p[1];

    hdr->next_packet = (UBYTE)(w0 >> 8);
    hdr->rsr         = (UBYTE)w0;
    hdr->count       = (UWORD)(((w1 & 0xffu) << 8) | (w1 >> 8));
}

/*
 * The wrap is an `if`, not a loop, so nothing here can spin however corrupt
 * the ring is -- which matters because this runs in the interrupt server.
 *
 * head cannot underflow, and the reason is not local: src is packet_ptr + 4,
 * packet_ptr is mem_ring + (next_packet - rec_page_start) * 256, and
 * dp8390_rint() only ever assigns next_packet a value it has range-checked
 * against [rec_page_start, rec_page_stop).  So src <= mem_end - 252 and
 * amount <= NETDEV_RXBUF_MAX, which is smaller than the smallest ring this
 * table has, so the tail cannot wrap a second time either.
 */
static LONG ed_ring_copy(NetdevNic *nic, LONG src, UBYTE *dst, UWORD amount)
{
    volatile UBYTE *base = ed_buf(nic);

    if (src + (LONG)amount > nic->mem_end)
    {
        UWORD head = (UWORD)(nic->mem_end - src);

        ed_copy_in(base + src, dst, head);
        amount = (UWORD)(amount - head);
        src    = nic->mem_ring;
        dst   += head;
    }

    ed_copy_in(base + src, dst, amount);

    return src + amount;
}

/*
 * No DMA to program and no ISR.RDC to wait for: the frame is moved into the
 * transmit slot and that is the whole of it, which is also why this cannot
 * fail the way ne2000_write_buf() can.  Short frames are zero-padded here
 * rather than by the caller, because the pad has to reach the card.
 */
static UWORD ed_write_buf(NetdevNic *nic, const UBYTE *frame, UWORD len,
                          LONG buf)
{
    volatile UBYTE *dst = ed_buf(nic) + buf;

    ed_copy_out(dst, frame, len);

    if (len < NETDEV_FRAME_MIN)
    {
        /*
         * ed_copy_out() already zeroed the low half of the last word of an
         * odd frame, so the pad starts at the next word and never steps back
         * over the byte that was just written.
         */
        UWORD done = (UWORD)((len + 1u) & ~1u);

        ed_fill(dst + done, 0, (UWORD)(NETDEV_FRAME_MIN - done));
        len = NETDEV_FRAME_MIN;
    }

    return len;
}

/* ---------------------------------------------------------------- probe --- */

/*
 * NetBSD zeroes the buffer and reads back zero.  Two things that would pass:
 * a window that ignores writes and reads as zero, and a window that decodes
 * fewer address lines than the buffer has, so that the top half is an alias
 * of the bottom.  Neither is hypothetical -- the second is precisely what
 * Amiberry did to the LAN Rover's 32K until ef28da7e, and the symptom was a
 * receive ring that died the first time it crossed page 0x40.
 *
 * So: a distinct pattern per page over the WHOLE buffer, then a second sweep
 * that reads every page back.  A page that aliases another has been
 * overwritten by the time it is read.  Only then the zero pass, which is what
 * leaves the ring clean.  Read back through ed_copy_in(), the same path the
 * receive ring uses, so this exercises the code that matters.
 */
static UWORD ed_mem_seed(LONG off)
{
    return (UWORD)(0x5a5au ^ (UWORD)off ^ (UWORD)(off >> 5));
}

static BOOL ed_test_mem(NetdevNic *nic)
{
    volatile UBYTE *base = ed_buf(nic) + nic->mem_start;
    ULONG  back[ED_PAGE_SIZE / 4];
    UWORD *words = (UWORD *)(APTR)back;
    LONG   off;
    UWORD  i;

    for (off = 0; off < nic->mem_size; off += ED_PAGE_SIZE)
        ed_fill(base + off, ed_mem_seed(off), ED_PAGE_SIZE);

    for (off = 0; off < nic->mem_size; off += ED_PAGE_SIZE)
    {
        UWORD seed = ed_mem_seed(off);

        ed_copy_in(base + off, (UBYTE *)(APTR)back, ED_PAGE_SIZE);
        for (i = 0; i < ED_PAGE_SIZE / 2; i++)
        {
            if (words[i] != seed)
                return FALSE;
        }
    }

    for (off = 0; off < nic->mem_size; off += ED_PAGE_SIZE)
        ed_fill(base + off, 0, ED_PAGE_SIZE);

    for (off = 0; off < nic->mem_size; off += ED_PAGE_SIZE)
    {
        ed_copy_in(base + off, (UBYTE *)(APTR)back, ED_PAGE_SIZE);
        for (i = 0; i < ED_PAGE_SIZE / 2; i++)
        {
            if (words[i] != 0)
                return FALSE;
        }
    }

    return TRUE;
}

/*
 * Does a DP8390 answer at all?  CURR on page 1 is the only ring register that
 * reads back what was written, so it is the one that can be measured.  Two
 * patterns, because one would pass against a bus that floats to it.  The
 * chip is left stopped and CURR is set properly by dp8390_init().
 */
static BOOL ed_test_regs(NetdevNic *nic)
{
    static const UBYTE pat[2] = { 0x55, 0xaa };
    UWORD i;
    BOOL  ok = TRUE;

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_1 | ED_CR_STP);
    (VOID)NIC_GET(nic, ED_P1_CR);

    for (i = 0; i < 2; i++)
    {
        NIC_PUT(nic, ED_P1_CURR, pat[i]);
        if (NIC_GET(nic, ED_P1_CURR) != pat[i])
            ok = FALSE;
    }

    NIC_PUT(nic, ED_P1_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STP);
    (VOID)NIC_GET(nic, ED_P0_CR);

    return ok;
}

/* --------------------------------------------------------------- attach --- */

static LONG ed_attach(NetdevNic *nic)
{
    const volatile UBYTE *prom;
    UBYTE ored = 0;
    UBYTE anded = 0xff;
    UWORD n = 5000;
    UWORD i;

    /* Stop the chip before anything else touches it. */
    NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STP);
    while ((NIC_GET(nic, ED_P0_ISR) & ED_ISR_RST) == 0 && --n != 0)
        (VOID)NIC_GET(nic, ED_P0_CR);

    /*
     * NetBSD: interrupts must be inactive while the PROM is read, because on
     * both boards the interrupt line shares one of its address lines.
     */
    NIC_PUT(nic, ED_P0_IMR, 0);
    NIC_PUT(nic, ED_P0_ISR, 0xff);

    if (!ed_test_regs(nic))
        return -1;

    prom = nic->board + nic->card->prom_off;
    for (i = 0; i < NETDEV_ADDR_LEN; i++)
    {
        UBYTE b = prom[i * nic->card->stride];

        nic->factory[i] = b;
        ored  |= b;
        anded &= b;
    }

    /*
     * All-zero or all-ones is a window that is not answering, and a group bit
     * in the first byte is not a station address whatever else it is.  Either
     * one enumerated would be a unit that comes online and is never delivered
     * a frame.
     */
    if (ored == 0 || anded == 0xff || (nic->factory[0] & 1u) != 0)
        return -1;

    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        nic->mac[i] = nic->factory[i];

    nic->mem_start = 0;
    nic->mem_size  = (LONG)nic->card->mem_size;

    nic->cr_proto  = ED_CR_RD2;
    nic->rcr_proto = 0;

    /*
     * Two-word FIFO threshold, no auto-init remote DMA, 68000 byte order,
     * word-wide transfers -- NetBSD's value.  ED_DCR_BOS is the one that is
     * not optional: without it the chip lays every received frame down
     * byte-swapped and ed_read_hdr() above reads a header that is not there.
     */
    nic->dcr_reg = ED_DCR_FT0 | ED_DCR_WTS | ED_DCR_LS | ED_DCR_BOS;

    nic->read_hdr  = ed_read_hdr;
    nic->ring_copy = ed_ring_copy;
    nic->write_buf = ed_write_buf;

    dp8390_config(nic);

    /* Monitor mode as well as stopped, same belt and braces as ne2000.c. */
    NIC_PUT(nic, ED_P0_RCR, ED_RCR_MON);

    if (!ed_test_mem(nic))
        return -1;

    dp8390_halt(nic);

    return 0;
}

const struct NetdevNicOps netdev_nic_ed =
{
    ed_attach,
    dp8390_init,
    dp8390_halt,
    dp8390_tx,
    dp8390_setfilter,
    dp8390_intr
};
