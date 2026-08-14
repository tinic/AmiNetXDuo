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
 * AmiNetXDuo: adapted from NetBSD sys/dev/ic/dp8390.c (rev 1.101) and
 * dp8390var.h.  The register programming, the ring geometry, the receive-ring
 * walk, the interrupt drain and the multicast hash are NetBSD's and are
 * unchanged in behaviour.  What was replaced:
 *
 *   bus_space_*      -> netdev_bus.h, which knows the per-card register stride
 *   struct ifnet     -> a frame callback; there is no network stack here
 *   struct mbuf      -> one linear staging buffer per unit
 *   ether_multi list -> the shell's reference-counted list, hashed by the
 *                       same ether_crc32_be, and this is the defect the
 *                       driver exists to fix: every X-Surf and X-Surf 100
 *                       rejects the join before the hash is ever reached
 *   splnet/callout   -> Disable()/Enable(); the receive drain runs in the
 *                       interrupt server, so nothing schedules
 *   printf/log       -> counters, read back through S2_GETSPECIALSTATS
 *
 * dp8390reg.h beside this file is NetBSD's, verbatim.
 *
 * SPDX-License-Identifier: MIT AND BSD-2-Clause-NetBSD
 */

#include "netdev_nic.h"
#include "dp8390.h"
#include "netdev_bsdtypes.h"
#include "dp8390reg.h"

#include <proto/exec.h>

/* --------------------------------------------------------------- helpers -- */

#ifdef NETDEV_TIME
extern ULONG netdev_time_copy;  /* netdev_device.c folds this into its report */
extern ULONG netdev_time_null;
extern ULONG netdev_time_rx;
extern ULONG netdev_time_tx;
#endif

#define NIC_GET(nic, reg)       netdev_bus_r8(&(nic)->bus, (reg))
#define NIC_PUT(nic, reg, val)  netdev_bus_w8(&(nic)->bus, (reg), (UBYTE)(val))

/*
 * The chip needs settling time between a command write and the next access.
 * There is no DELAY() in a device, and no timer.device open at interrupt
 * level: a read of a register the chip always answers is both the barrier and
 * the delay, and it is what the Zorro bus cycle costs anyway.
 */
static VOID dp_pause(NetdevNic *nic, UWORD ticks)
{
    while (ticks-- != 0)
        (VOID)NIC_GET(nic, ED_P0_CR);
}

/* ------------------------------------------------------------- geometry --- */

VOID dp8390_config(NetdevNic *nic)
{
    if (nic->mem_size < 16384)
        nic->txb_cnt = 1;
    else if (nic->mem_size < 8192 * 3)
        nic->txb_cnt = 2;
    else
        nic->txb_cnt = 3;

    nic->tx_page_start  = (UWORD)(nic->mem_start >> ED_PAGE_SHIFT);
    nic->rec_page_start =
        (UWORD)(nic->tx_page_start + nic->txb_cnt * ED_TXBUF_SIZE);
    nic->rec_page_stop  =
        (UWORD)(nic->tx_page_start + (nic->mem_size >> ED_PAGE_SHIFT));
    nic->mem_ring       =
        nic->mem_start + ((LONG)(nic->txb_cnt * ED_TXBUF_SIZE) << ED_PAGE_SHIFT);
    nic->mem_end        = nic->mem_start + nic->mem_size;
}

/* ----------------------------------------------------------------- halt --- */

/*
 * What the poll waits for is the frame in progress finishing, which at 10 Mbit
 * is at most 1214 us for a maximum-length frame.  The old bound of 5000 was a
 * count with no time behind it, and it is reached EVERY time on an emulated
 * NE2000, which never asserts ISR.RST: two register accesses per iteration at
 * 0.82 us each measured 8.1-8.4 ms with interrupts disabled, on the offline,
 * expunge and chip-reset paths.  900 iterations is that same worst-case frame
 * time and a margin.  A real chip does assert RST and leaves long before the
 * bound, so this is not a shorter wait for it -- it is a bound that means
 * something for the case where the bit never arrives.
 */
#define DP8390_HALT_POLLS   900

VOID dp8390_halt(NetdevNic *nic)
{
    UWORD n = DP8390_HALT_POLLS;

    NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_0 | ED_CR_STP);

    while ((NIC_GET(nic, ED_P0_ISR) & ED_ISR_RST) == 0 && --n != 0)
        dp_pause(nic, 1);

    /*
     * STP does not deassert INT.  The chip asserts while ISR & IMR is
     * non-zero whatever the command register says, and dp8390_intr() returns
     * without acknowledging anything once running is clear -- so a bit set in
     * the window before the stop would hold a level-triggered INT2 down
     * forever, with the server about to be removed.  Mask, then acknowledge.
     */
    NIC_PUT(nic, ED_P0_IMR, 0);
    NIC_PUT(nic, ED_P0_ISR, 0xff);

    nic->running = FALSE;
}

/* ------------------------------------------------- the multicast filter --- */

/*
 * Program PAR0-5 and MAR0-7 from the state the shell keeps.  Page 1 is
 * selected around it and page 0 restored, so this is safe to call on a
 * running chip, which is what every S2_ADDMULTICASTADDRESS does.
 */
VOID dp8390_setfilter(NetdevNic *nic)
{
    UBYTE run = (UBYTE)(nic->running ? ED_CR_STA : ED_CR_STP);
    UWORD i;
    UBYTE rcr;

    NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_1 | run);
    dp_pause(nic, 1);

    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        NIC_PUT(nic, ED_P1_PAR0 + i, nic->mac[i]);
    for (i = 0; i < 8; i++)
        NIC_PUT(nic, ED_P1_MAR0 + i, nic->mar[i]);

    NIC_PUT(nic, ED_P1_CR, nic->cr_proto | ED_CR_PAGE_0 | run);
    dp_pause(nic, 1);

    rcr = (UBYTE)(ED_RCR_AB | ED_RCR_AM | nic->rcr_proto);
    if (nic->promisc)
        rcr |= ED_RCR_PRO | ED_RCR_AR | ED_RCR_SEP;
    NIC_PUT(nic, ED_P0_RCR, rcr);
}

/* ----------------------------------------------------------------- init --- */

/*
 * NetBSD's comment applies unchanged: this order is the National manual's and
 * is not negotiable.
 */
LONG dp8390_init(NetdevNic *nic)
{
    UWORD i;

    nic->txb_inuse   = 0;
    nic->txb_new     = 0;
    nic->txb_next_tx = 0;

    NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_0 | ED_CR_STP);
    dp_pause(nic, 1);

    if ((nic->dcr_reg & ED_DCR_LS) != 0)
        NIC_PUT(nic, ED_P0_DCR, nic->dcr_reg);
    else
        NIC_PUT(nic, ED_P0_DCR, ED_DCR_FT1 | ED_DCR_LS);

    NIC_PUT(nic, ED_P0_RBCR0, 0);
    NIC_PUT(nic, ED_P0_RBCR1, 0);

    NIC_PUT(nic, ED_P0_RCR, ED_RCR_MON | nic->rcr_proto);
    NIC_PUT(nic, ED_P0_TCR, ED_TCR_LB0);

    NIC_PUT(nic, ED_P0_BNRY, nic->rec_page_start);
    NIC_PUT(nic, ED_P0_PSTART, nic->rec_page_start);
    NIC_PUT(nic, ED_P0_PSTOP, nic->rec_page_stop);

    NIC_PUT(nic, ED_P0_IMR,
            ED_IMR_PRXE | ED_IMR_PTXE | ED_IMR_RXEE | ED_IMR_TXEE |
            ED_IMR_OVWE);
    NIC_PUT(nic, ED_P0_ISR, 0xff);

    NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_1 | ED_CR_STP);
    dp_pause(nic, 1);

    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        NIC_PUT(nic, ED_P1_PAR0 + i, nic->mac[i]);
    for (i = 0; i < 8; i++)
        NIC_PUT(nic, ED_P1_MAR0 + i, nic->mar[i]);

    nic->next_packet = (UWORD)(nic->rec_page_start + 1);
    NIC_PUT(nic, ED_P1_CURR, nic->next_packet);

    NIC_PUT(nic, ED_P1_CR, nic->cr_proto | ED_CR_PAGE_0 | ED_CR_STP);
    dp_pause(nic, 1);

    {
        UBYTE rcr = (UBYTE)(ED_RCR_AB | ED_RCR_AM | nic->rcr_proto);

        if (nic->promisc)
            rcr |= ED_RCR_PRO | ED_RCR_AR | ED_RCR_SEP;
        NIC_PUT(nic, ED_P0_RCR, rcr);
    }

    NIC_PUT(nic, ED_P0_TCR, 0);
    NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_0 | ED_CR_STA);

    nic->running = TRUE;

    return 0;
}

VOID dp8390_reset(NetdevNic *nic)
{
    nic->resets++;
    dp8390_halt(nic);
    (VOID)dp8390_init(nic);
}

/* ------------------------------------------------------------- transmit --- */

static VOID dp8390_xmit(NetdevNic *nic)
{
    UWORD len = nic->txb_len[nic->txb_next_tx];

    NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_0 | ED_CR_STA);
    dp_pause(nic, 1);

    NIC_PUT(nic, ED_P0_TPSR,
            nic->tx_page_start + nic->txb_next_tx * ED_TXBUF_SIZE);
    NIC_PUT(nic, ED_P0_TBCR0, len);
    NIC_PUT(nic, ED_P0_TBCR1, len >> 8);

    NIC_PUT(nic, ED_P0_CR,
            nic->cr_proto | ED_CR_PAGE_0 | ED_CR_TXP | ED_CR_STA);

    if (++nic->txb_next_tx == nic->txb_cnt)
        nic->txb_next_tx = 0;
}

/*
 * One frame into a free transmit buffer, and start the chip if it is idle.
 * The caller holds Disable(): the remote DMA write and the interrupt drain
 * share one register file and one page selection.
 */
LONG dp8390_tx(NetdevNic *nic, const UBYTE *frame, UWORD len)
{
    LONG  buf;
    UWORD sent;

    if (!nic->running)
        return DP8390_TX_OFFLINE;
    if (nic->txb_inuse >= nic->txb_cnt)
        return DP8390_TX_BUSY;

    buf = nic->mem_start +
          ((LONG)(nic->txb_new * ED_TXBUF_SIZE) << ED_PAGE_SHIFT);

    sent = nic->write_buf(nic, frame, len, buf);
    if (sent == 0)
        return DP8390_TX_FAILED;

    nic->txb_len[nic->txb_new] = sent;
    if (++nic->txb_new == nic->txb_cnt)
        nic->txb_new = 0;
    if (nic->txb_inuse++ == 0)
        dp8390_xmit(nic);

    return 0;
}

/* -------------------------------------------------------------- receive --- */

static VOID dp8390_rint(NetdevNic *nic)
{
    NetdevRing hdr;
    LONG       packet_ptr;
    UWORD      len;
    UBYTE      boundary;
    UBYTE      current;
    UBYTE      nlen;

    UWORD rounds = NETDEV_DRAIN_MAX;
    UWORD steps;

 loop:
    NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_1 | ED_CR_STA);
    dp_pause(nic, 1);

    current = NIC_GET(nic, ED_P1_CURR);
    if (nic->next_packet == current)
        return;

    NIC_PUT(nic, ED_P1_CR, nic->cr_proto | ED_CR_PAGE_0 | ED_CR_STA);
    dp_pause(nic, 1);

    /*
     * The walk cannot legitimately visit more buffers than the ring has
     * pages, so that is the bound.  Belt as well as braces: the header check
     * below already guarantees forward progress, and this catches a cycle
     * that steps forward every time and still never reaches `current`.
     */
    steps = (UWORD)(nic->rec_page_stop - nic->rec_page_start);

    do
    {
        packet_ptr = nic->mem_ring +
            ((LONG)(nic->next_packet - nic->rec_page_start) << ED_PAGE_SHIFT);

        nic->read_hdr(nic, packet_ptr, &hdr);
        len = hdr.count;

        /*
         * Old chips sometimes duplicate the low byte of the length into the
         * high byte, so the length is recomputed from the next-packet
         * pointer and only the low byte of count is trusted.
         */
        if (hdr.next_packet >= nic->next_packet)
            nlen = (UBYTE)(hdr.next_packet - nic->next_packet);
        else
            nlen = (UBYTE)((hdr.next_packet - nic->rec_page_start) +
                           (nic->rec_page_stop - nic->next_packet));
        --nlen;
        if ((len & ED_PAGE_MASK) + sizeof(NetdevRing) > ED_PAGE_SIZE)
            --nlen;
        len = (UWORD)((len & ED_PAGE_MASK) | (nlen << ED_PAGE_SHIFT));

        /*
         * FORWARD PROGRESS, and it has to be checked here rather than implied.
         *
         * next_packet is only ever assigned hdr.next_packet, so a header that
         * points at its own page leaves the loop condition unchanged and this
         * spins forever -- inside the INT2 server, with interrupts masked,
         * which is a dead machine and not a dropped frame.
         *
         * NetBSD is protected from that input by accident: a self-referencing
         * header underflows nlen, the length comes out >= 0xFE00, and its
         * `len <= MCLBYTES` test sends it down the reset-and-return arm.  When
         * the over-length arm here stopped resetting -- so that one 802.1Q
         * frame could not flush the ring -- that exit went with it.  So the
         * pathological headers are named explicitly instead.
         */
        if (hdr.next_packet < nic->rec_page_start ||
            hdr.next_packet >= nic->rec_page_stop ||
            hdr.next_packet == nic->next_packet)
        {
            /* The ring pointers are corrupt; nothing short of a reset. */
            nic->rx_errors++;
            dp8390_reset(nic);
            return;
        }

        if (len > sizeof(NetdevRing) && len <= NETDEV_RXBUF_MAX)
        {
            UWORD        flen = (UWORD)(len - sizeof(NetdevRing));
            LONG         src  = packet_ptr + (LONG)sizeof(NetdevRing);
            const UBYTE *fp   = NULL;

            /* A mapped buffer needs no staging: hand the frame up where it
               lies and the opener's CopyToBuff reads the card once, instead
               of this copying it to rxbuf for CopyToBuff to copy again. */
            if (nic->frame_at != NULL)
                fp = (const UBYTE *)nic->frame_at(nic, src, flen);

            if (fp == NULL)
            {
#ifdef NETDEV_TIME
                UWORD vh0 = *(volatile UWORD *)0xdff006;
                UWORD vh1;
                ULONG t0, t1;
#endif
                (VOID)nic->ring_copy(nic, src, (UBYTE *)nic->rxbuf, flen);
                fp = (const UBYTE *)nic->rxbuf;
#ifdef NETDEV_TIME
                vh1 = *(volatile UWORD *)0xdff006;
                t0  = (ULONG)((vh0 >> 8) & 0xff) * 227UL + (vh0 & 0xff);
                t1  = (ULONG)((vh1 >> 8) & 0xff) * 227UL + (vh1 & 0xff);
                netdev_time_copy += (t1 >= t0) ? (t1 - t0)
                                               : (256UL * 227UL + t1 - t0);
#endif
            }

            nic->rx_packets++;
            if (nic->rx != NULL)
                nic->rx(nic->rx_arg, fp, flen);
        }
        else
        {
            /*
             * Too long or impossibly short: skip it and carry on.  Upstream
             * accepts anything up to a cluster and this used to reset the
             * chip instead, which flushes the receive ring and discards
             * every transmit already reported as sent -- one 802.1Q-tagged
             * frame from any host on the LAN was enough.
             */
            nic->rx_errors++;
        }

        nic->next_packet = hdr.next_packet;

        boundary = (UBYTE)(nic->next_packet - 1);
        if (boundary < nic->rec_page_start)
            boundary = (UBYTE)(nic->rec_page_stop - 1);
        NIC_PUT(nic, ED_P0_BNRY, boundary);

        if (--steps == 0)
        {
            nic->rx_errors++;
            dp8390_reset(nic);
            return;
        }
    }
    while (nic->next_packet != current);

    if (--rounds != 0)
        goto loop;
}

/* ------------------------------------------------------------ interrupt --- */

/*
 * Drain the WHOLE of it.  The loop re-reads ISR until it reads zero, which is
 * the part a driver that services one bit per interrupt gets wrong: the line
 * is level-triggered, and a bit left set is an interrupt that never ends.
 */
BOOL dp8390_intr(NetdevNic *nic)
{
    UWORD rounds = NETDEV_DRAIN_MAX;
    UBYTE isr;

    if (!nic->running)
        return FALSE;

    NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_0 | ED_CR_STA);
    dp_pause(nic, 1);

    isr = NIC_GET(nic, ED_P0_ISR);
    if (isr == 0)
    {
#ifdef NETDEV_TIME
        netdev_time_null++;
#endif
        return FALSE;
    }

#ifdef NETDEV_TIME
    if ((isr & (ED_ISR_PRX | ED_ISR_RXE | ED_ISR_OVW)) != 0)
        netdev_time_rx++;
    if ((isr & (ED_ISR_PTX | ED_ISR_TXE)) != 0)
        netdev_time_tx++;
#endif

    for (;;)
    {
        /*
         * One write clears every bit set here.  An AX88190 or AX88790 needs
         * the acknowledge retried until it takes; no such part is in the card
         * table, and the retry loop lived here behind a flag nothing ever set,
         * which read as support for a chip that is not supported.  See the
         * note in ne2000.c for what a row for one has to add.
         */
        NIC_PUT(nic, ED_P0_ISR, isr);

        if ((isr & (ED_ISR_PTX | ED_ISR_TXE)) != 0 && nic->txb_inuse != 0)
        {
            UBYTE collisions = (UBYTE)(NIC_GET(nic, ED_P0_NCR) & 0x0f);

            if ((isr & ED_ISR_TXE) != 0)
            {
                if ((NIC_GET(nic, ED_P0_TSR) & ED_TSR_ABT) != 0 &&
                    collisions == 0)
                    collisions = 16;
                nic->tx_errors++;
            }
            else
            {
                (VOID)NIC_GET(nic, ED_P0_TSR);
                nic->tx_packets++;
            }

            nic->collisions += collisions;

            if (--nic->txb_inuse != 0)
                dp8390_xmit(nic);
        }

        if ((isr & (ED_ISR_PRX | ED_ISR_RXE | ED_ISR_OVW)) != 0)
        {
            if ((isr & ED_ISR_OVW) != 0)
            {
                nic->overruns++;
                dp8390_reset(nic);
            }
            else
            {
                if ((isr & ED_ISR_RXE) != 0)
                    nic->rx_errors++;
                dp8390_rint(nic);
            }
        }

        NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_0 | ED_CR_STA);
        dp_pause(nic, 1);

        /* Reading the tally counters is what clears ED_ISR_CNT on old parts. */
        if ((isr & ED_ISR_CNT) != 0)
        {
            (VOID)NIC_GET(nic, ED_P0_CNTR0);
            (VOID)NIC_GET(nic, ED_P0_CNTR1);
            (VOID)NIC_GET(nic, ED_P0_CNTR2);
        }

        isr = NIC_GET(nic, ED_P0_ISR);
        if (isr == 0)
            break;

        /*
         * A board that has been pulled reads 0xff forever, and this loop runs
         * at interrupt level where nothing else in the machine does.  Give up
         * and return: the line is level-triggered, so real work re-enters.
         */
        if (--rounds == 0)
            break;
    }

    return TRUE;
}
