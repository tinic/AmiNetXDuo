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
 * ADDED, and not from NetBSD: netdev_ne2000_probe_wide().  The X-Surf 100 has
 * a second, 32-bit image of the same data port, and a driver that assumes it
 * is there silently transfers garbage on a board that does not have one, while
 * a driver that assumes it is not there silently gives up half the throughput.
 * So it is written and read back before it is used, and the answer is
 * reportable, not guessed.
 *
 * ne2000reg.h beside this file is NetBSD's, verbatim.
 *
 * SPDX-License-Identifier: MIT AND BSD-2-Clause-NetBSD
 */

#include "netdev_nic.h"
#include "dp8390.h"
#include "netdev_bsdtypes.h"
#include "dp8390reg.h"
#include "ne2000reg.h"

#define AX88190_NODEID_OFFSET   0x400

/* ------------------------------------------------------------- plumbing --- */

#define NIC_GET(nic, reg)       netdev_bus_r8(&(nic)->bus, (reg))
#define NIC_PUT(nic, reg, val)  netdev_bus_w8(&(nic)->bus, (reg), (UBYTE)(val))
#define ASIC_GET(nic, reg)      netdev_bus_ra8(&(nic)->bus, (reg))
#define ASIC_PUT(nic, reg, val) netdev_bus_wa8(&(nic)->bus, (reg), (UBYTE)(val))

/*
 * There is no timer open when a unit is probed, and a device may not call
 * Delay().  A read of a register the chip always answers is a real Zorro bus
 * cycle -- 280 ns at the fastest a Zorro II board can be, and slower on
 * everything else -- so a count is a lower bound on the microseconds.  Only
 * the reset path waits, and the chip is ready long before the wait ends.
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

/* --------------------------------------------------------- remote DMA ----- */

static VOID ne2000_readmem(NetdevNic *nic, LONG src, UBYTE *dst, UWORD amount)
{
    NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STA);

    amount = (UWORD)((amount + 1u) & ~1u);

    NIC_PUT(nic, ED_P0_RBCR0, amount);
    NIC_PUT(nic, ED_P0_RBCR1, amount >> 8);
    NIC_PUT(nic, ED_P0_RSAR0, src);
    NIC_PUT(nic, ED_P0_RSAR1, src >> 8);

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD0 | ED_CR_PAGE_0 | ED_CR_STA);

    netdev_bus_rdata(&nic->bus, dst, amount);
}

static VOID ne2000_writemem(NetdevNic *nic, const UBYTE *src, LONG dst,
                            UWORD len)
{
    UWORD maxwait = 100;

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STA);
    NIC_PUT(nic, ED_P0_ISR, ED_ISR_RDC);

    NIC_PUT(nic, ED_P0_RBCR0, len);
    NIC_PUT(nic, ED_P0_RBCR1, len >> 8);
    NIC_PUT(nic, ED_P0_RSAR0, dst);
    NIC_PUT(nic, ED_P0_RSAR1, dst >> 8);

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD1 | ED_CR_PAGE_0 | ED_CR_STA);

    netdev_bus_wdata(&nic->bus, src, len);

    if (nic->no_rdc)
        return;

    while ((NIC_GET(nic, ED_P0_ISR) & ED_ISR_RDC) != ED_ISR_RDC &&
           --maxwait != 0)
        ne_delay(nic, 1);
}

static VOID ne2000_read_hdr(NetdevNic *nic, LONG src, NetdevRing *hdr)
{
    UBYTE raw[4];

    ne2000_readmem(nic, src, raw, 4);

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

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD2 | ED_CR_PAGE_0 | ED_CR_STA);
    NIC_PUT(nic, ED_P0_ISR, ED_ISR_RDC);

    NIC_PUT(nic, ED_P0_RBCR0, len);
    NIC_PUT(nic, ED_P0_RBCR1, len >> 8);
    NIC_PUT(nic, ED_P0_RSAR0, buf);
    NIC_PUT(nic, ED_P0_RSAR1, buf >> 8);

    NIC_PUT(nic, ED_P0_CR, ED_CR_RD1 | ED_CR_PAGE_0 | ED_CR_STA);

    netdev_bus_wdata(&nic->bus, frame, (UWORD)((len + 1u) & ~1u));

    if (nic->no_rdc)
        return len;

    while ((NIC_GET(nic, ED_P0_ISR) & ED_ISR_RDC) != ED_ISR_RDC &&
           --maxwait != 0)
    {
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

    tmp = NIC_GET(nic, ED_P0_CR);
    if ((tmp & (ED_CR_RD2 | ED_CR_TXP | ED_CR_STA | ED_CR_STP)) !=
        (ED_CR_RD2 | ED_CR_STP))
        return FALSE;

    tmp = NIC_GET(nic, ED_P0_ISR);
    if ((tmp & ED_ISR_RST) != ED_ISR_RST)
        return FALSE;

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

    return (BOOL)(ne_memcmp(ne_test_pattern, test_buffer,
                            sizeof(ne_test_pattern)) == 0);
}

/*
 * Decide whether the 32-bit window is really the data port.  A card that does
 * not have one answers this with a mismatch and stays on the 16-bit path; the
 * result is what S2_GETSPECIALSTATS reports as the transfer mode, so a user
 * who is on the slow path can see that they are.
 */
static VOID ne2000_probe_wide(NetdevNic *nic)
{
    UBYTE back[NETDEV_BUS_PROBE_LEN];

    if (nic->bus.wide == NULL)
        return;

    /* Written narrow, read wide. */
    nic->bus.dmode = NETDEV_DMODE_WORD;
    ne2000_writemem(nic, ne_test_pattern, 16384, NETDEV_BUS_PROBE_LEN);
    nic->bus.dmode = NETDEV_DMODE_LONG;
    ne2000_readmem(nic, 16384, back, NETDEV_BUS_PROBE_LEN);
    if (ne_memcmp(ne_test_pattern, back, NETDEV_BUS_PROBE_LEN) != 0)
    {
        nic->bus.dmode = NETDEV_DMODE_WORD;
        return;
    }

    /* And written wide, read narrow, which is the direction that transmits. */
    nic->bus.dmode = NETDEV_DMODE_LONG;
    ne2000_writemem(nic, ne_test_pattern + 16, 16384 + 256,
                    NETDEV_BUS_PROBE_LEN - 16);
    nic->bus.dmode = NETDEV_DMODE_WORD;
    ne2000_readmem(nic, 16384 + 256, back, NETDEV_BUS_PROBE_LEN - 16);
    if (ne_memcmp(ne_test_pattern + 16, back, NETDEV_BUS_PROBE_LEN - 16) != 0)
        return;

    nic->bus.dmode = NETDEV_DMODE_LONG;
}

/* --------------------------------------------------------------- attach --- */

static LONG ne2000_attach(NetdevNic *nic)
{
    UBYTE romdata[32];
    UWORD i;

    nic->useword = 1;
    nic->no_rdc  = 0;

    if (!ne2000_detect(nic))
        return -1;

    nic->mem_start = 16384;
    nic->mem_size  = 16384;

    nic->cr_proto  = ED_CR_RD2;
    nic->rcr_proto = 0;
    nic->dcr_reg   = ED_DCR_FT1 | ED_DCR_LS | ED_DCR_WTS;

    nic->read_hdr  = ne2000_read_hdr;
    nic->ring_copy = ne2000_ring_copy;
    nic->write_buf = ne2000_write_buf;

    ne2000_probe_wide(nic);

    /*
     * Where the station address is depends on the part, not on the card, and
     * an X-Surf 100 is both: the AX88796 keeps it at AX88190_NODEID_OFFSET,
     * but a board wired as a plain NE2000 -- which is what the emulated one
     * is, and what a re-badged clone may be -- images a serial ROM into the
     * first 32 bytes of the buffer with 0x57 0x57 at the end of it.  Read the
     * ROM image, believe it if the signature is there, and only then go
     * looking in the AX88796's own space.  Guessing from the Zorro product ID
     * gets one of the two wrong.
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

    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        nic->mac[i] = nic->factory[i];

    NIC_PUT(nic, ED_P0_ISR, 0xff);

    dp8390_config(nic);
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
    dp8390_intr
};

const struct NetdevNicOps *netdev_nic_ops_for(UBYTE chip)
{
    /*
     * NETDEV_CHIP_ED -- Hydra and the ASDG LanRover -- has no core yet: those
     * boards map their packet buffer instead of reaching it through a remote
     * DMA port, so they need a second set of read_hdr/ring_copy/write_buf and
     * ED_DCR_BOS.  The rows are in the table; returning NULL keeps them out of
     * the unit numbering until the core lands, rather than enumerating a board
     * that would then fail to open.
     */
    if (chip == NETDEV_CHIP_NE2000)
        return &netdev_nic_ne2000;

    return NULL;
}
