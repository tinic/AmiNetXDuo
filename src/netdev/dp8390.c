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
 * unchanged in behaviour.  dp8390reg.h beside this file is NetBSD's, verbatim.
 *
 * SPDX-License-Identifier: MIT AND BSD-2-Clause-NetBSD
 */

#include "netdev_nic.h"
#include "dp8390.h"
#include "netdev_bsdtypes.h"
#include "netdev_clock.h"
#include "netdev_verify.h"
#include "dp8390reg.h"
#include "aminetxduo/anxs2ext.h"

#include <proto/exec.h>

/* --------------------------------------------------------------- helpers -- */

#ifdef NETDEV_TIME
extern ULONG netdev_time_copy;  /* netdev_device.c folds this into its report */
extern ULONG netdev_time_null;
extern ULONG netdev_time_rx;
extern ULONG netdev_time_tx;
#endif

/*
 * Redirected by src/netdev/test/test_netdev_dp8390.c.  A DP8390's register
 * file is FOUR banks of sixteen selected by CR bits 7..6 -- PAR0 on page 1
 * and PSTART on page 0 are one register index -- so a byte array is not a
 * model of this chip, it is a model of a chip with the pages fused.  The
 * test supplies the banking; these expand to the plain bus access they
 * replace, so the target build is byte for byte what it was.
 */
#ifndef NIC_GET
#define NIC_GET(nic, reg)       netdev_bus_r8(&(nic)->bus, (reg))
#endif
#ifndef NIC_PUT
#define NIC_PUT(nic, reg, val)  netdev_bus_w8(&(nic)->bus, (reg), (UBYTE)(val))
#endif

/*
 * The chip needs settling time between a command write and the next access.
 * There is no DELAY() in a device and no timer.device open at interrupt level:
 * a read of a register the chip always answers is both the barrier and delay.
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
    /* The receive ring, in bytes: what ANXD_CMD_RX_CAPACITY answers. */
    nic->rx_capacity    =
        (ULONG)(nic->rec_page_stop - nic->rec_page_start) << ED_PAGE_SHIFT;
}

/* ----------------------------------------------------------------- halt --- */

/*
 * The poll waits for the frame in progress to finish, which at 10 Mbit is at
 * most 1214 us for a maximum-length frame.  The old 900 register reads remain
 * the floor; measured time is the bound for the case where ISR.RST never
 * arrives, which is every emulated NE2000.
 */
#define DP8390_HALT_SPINS   900u

VOID dp8390_halt(NetdevNic *nic)
{
    NetdevWait w;

    NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_0 | ED_CR_STP);
    netdev_wait_begin(&w, DP8390_STOP_WAIT_US, DP8390_HALT_SPINS);

    do
    {
        if ((NIC_GET(nic, ED_P0_ISR) & ED_ISR_RST) != 0)
            break;
        dp_pause(nic, 1);
    }
    while (!netdev_wait_done(&w));

    /*
     * STP does not deassert INT.  The chip asserts while ISR & IMR is non-zero
     * whatever the command register says, so a bit set in the window before the
     * stop would hold a level-triggered INT2 down forever, with the server
     * about to be removed.  Mask, then acknowledge.
     */
    NIC_PUT(nic, ED_P0_IMR, 0);
    NIC_PUT(nic, ED_P0_ISR, 0xff);

    nic->running = FALSE;
}

/* ------------------------------------------------- the multicast filter --- */

/*
 * Program PAR0-5 and MAR0-7 from the state the shell keeps.  Page 1 is selected
 * around it and page 0 restored, so this is safe to call on a running chip.
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
 * This order is the National manual's and is not negotiable.
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
    nic->dma_left = 0;          /* the count just written is not ours */

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
 * The caller holds Disable(), because the remote DMA write and the interrupt
 * drain share one register file and one page selection.
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
     * The walk cannot legitimately visit more buffers than the ring has pages,
     * so that is the bound.  It catches a cycle that steps forward every time
     * and still never reaches `current`.
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
         * Forward progress is checked here rather than implied: next_packet is
         * only ever assigned hdr.next_packet, so a header that points at its
         * own page leaves the loop condition unchanged and spins forever,
         * inside the INT2 server with interrupts masked.
         */
        if (hdr.next_packet < nic->rec_page_start ||
            hdr.next_packet >= nic->rec_page_stop ||
            hdr.next_packet == nic->next_packet)
        {
            /* The ring pointers are corrupt.  Nothing short of a reset
               recovers. */
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

            if (fp == NULL && nic->rx_claim != NULL &&
                flen > NETDEV_HDR_LEN)
            {
                /*
                 * The remote-DMA form of el3.c's two-stage drain: the header
                 * from the ring first, and when the claim answers, the payload
                 * from the ring straight into the stack's packet.  The ring is
                 * random-access, so a declined claim costs one 14-byte read.
                 */
                UBYTE *hdr = (UBYTE *)nic->rxbuf;

                (VOID)nic->ring_copy(nic, src, hdr, NETDEV_HDR_LEN);

                {
                    APTR   token = NULL;
                    UBYTE  wanted = 0;
                    UBYTE *dst   = nic->rx_claim(nic->rx_arg, hdr, flen,
                                                 &token, &wanted);

                    if (dst != NULL)
                    {
                        UWORD plen   = (UWORD)(flen - NETDEV_HDR_LEN);
                        ULONG sum    = 0;
                        UBYTE summed = 0;

                        /*
                         * The payload is coming across the bus once whatever
                         * happens; the adds that ride along with it are free
                         * beside the accesses, and the walk they replace is
                         * not.  A core that declines -- a wrapped read, an
                         * 8-bit port -- leaves the frame to be summed the old
                         * way, which is what every frame on this path did.
                         */
                        if (nic->ring_copy_sum != NULL)
                            summed = (UBYTE)(nic->ring_copy_sum(
                                                 nic, src + NETDEV_HDR_LEN,
                                                 dst, plen, &sum) ? 1 : 0);

                        if (summed == 0)
                            (VOID)netdev_ring_copy_exact(
                                nic, src + NETDEV_HDR_LEN, dst, plen);

                        if (summed != 0 &&
                            (wanted & ANXD_S2_RXF_VERIFIED) != 0)
                            summed |= netdev_rx_verify(dst, plen, sum);

                        nic->rx_packets++;
                        nic->rx_claimed(nic->rx_arg, token, sum, summed);
                        goto rx_done;
                    }
                }
            }

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
rx_done:;
        }
        else
        {
            /*
             * Too long or impossibly short: skip it and carry on.  Resetting
             * the chip instead flushes the receive ring and discards every
             * transmit already reported as sent, and one 802.1Q-tagged frame
             * from any host on the LAN was enough to trigger it.
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

/* ------------------------------------------------------------ overwrite --- */

/*
 * The ring overflowed: the receiver got ahead of the drain and stopped.
 * This is the DP8390's documented recovery (National AN-874).  It does NOT
 * throw the ring away, unlike the full reset that NetBSD's dp8390_intr runs
 * on OVW and that an earlier revision of this file ran.  That reset discarded
 * every frame already received and every transmit still queued, and on a
 * machine whose ring overflows once a burst -- an A3000 (25 MHz 68030) with
 * an X-Surf 100, 42 overwrites in ten seconds -- each one was a TCP
 * retransmit timeout: 2.8 Mbit/s on a 100 Mbit link.
 *
 * Stop the chip and wait the documented minimum stop delay -- a full
 * transmit time plus guard, at least 1.6 ms (DP8390_OVW_STOP_WAIT_US), per
 * the DP8390D datasheet -- not ISR.RST: the overflow sets RST on the
 * reference core, and RST reports "reset state entered", not "transmitter
 * drained", so polling it would exit at once and skip the wait.  The wait is
 * bounded and unconditional whatever RST says: timed by the beam clock, with
 * the measured work per line as the spin floor so beam and floor end together,
 * and the DP8390_OVW_STOP_SPINS bus-cycle fallback only when the beam is down.
 *
 * The cut-off transmit is decided from the hardware, not the interrupt
 * snapshot: CR.TXP read before the stop says a frame was in flight, and the
 * caller's snapshot (already acknowledged by dp8390_intr) plus a fresh
 * post-stop ISR read say whether it completed.  Resend only when the frame
 * was in flight and neither read saw a PTX or TXE; a completion latched in
 * the stop window is left for the caller's loop to account.  Then clear the
 * remote byte count; loop the transmitter back so nothing goes out while the
 * ring is drained; start; drain; acknowledge OVW; take the loopback off;
 * resend the cut-off frame.  The caller holds the interrupt context, and the
 * wait is the price, paid only on an overwrite: unlike dp8390_halt(), which
 * exits early on ISR.RST at 1.5 ms, this wait is unconditional at 1.6 ms.
 *
 * TRUE when the drain found the ring pointers corrupt and reset the chip
 * itself: nothing below the caller's loop is valid then.
 */
static BOOL dp8390_overwrite(NetdevNic *nic, UBYTE isr)
{
    NetdevWait w;
    BOOL       resend;
    UBYTE      was_txing;
    UBYTE      after;
    ULONG      before = nic->resets;

    /* Was a frame actually in flight?  Read the transmitter bit before the
       stop, as Linux lib8390.c does; txb_inuse is a software shadow that can
       disagree with the chip. */
    was_txing = NIC_GET(nic, ED_P0_CR) & ED_CR_TXP;

    NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_0 | ED_CR_STP);
    netdev_wait_begin(&w, DP8390_OVW_STOP_WAIT_US,
                      netdev_clock_floor_spins(DP8390_OVW_STOP_WAIT_US,
                                               DP8390_OVW_STOP_SPINS));
    do
    {
        dp_pause(nic, 1);
    }
    while (!netdev_wait_done(&w));

    /* Resend only a frame that software still owns (txb_inuse), that was in
       flight (TXP), and that completed neither before the interrupt (the
       snapshot, already acknowledged by dp8390_intr) nor during the stop (the
       fresh read).  txb_inuse is a second guard against a stale high TXP
       sending an unowned frame.  A PTX or TXE latched in the stop window is
       left set for the caller's loop to account, not acknowledged here. */
    after  = NIC_GET(nic, ED_P0_ISR);
    resend = (BOOL)(nic->txb_inuse != 0 &&
                    was_txing != 0 &&
                    (isr   & (ED_ISR_PTX | ED_ISR_TXE)) == 0 &&
                    (after & (ED_ISR_PTX | ED_ISR_TXE)) == 0);

    NIC_PUT(nic, ED_P0_RBCR0, 0);
    NIC_PUT(nic, ED_P0_RBCR1, 0);
    NIC_PUT(nic, ED_P0_TCR, ED_TCR_LB0);
    NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_0 | ED_CR_STA);

    dp8390_rint(nic);
    if (nic->resets != before)
        return TRUE;

    NIC_PUT(nic, ED_P0_CR, nic->cr_proto | ED_CR_PAGE_0 | ED_CR_STA);
    dp_pause(nic, 1);
    NIC_PUT(nic, ED_P0_ISR, ED_ISR_OVW);
    NIC_PUT(nic, ED_P0_TCR, 0);

    if (resend)
    {
        /* The buffer the stop cut off is the one before txb_next_tx. */
        nic->txb_next_tx = (UWORD)((nic->txb_next_tx == 0)
                                   ? nic->txb_cnt - 1u
                                   : nic->txb_next_tx - 1u);
        dp8390_xmit(nic);
    }

    return FALSE;
}

/* ------------------------------------------------------------ interrupt --- */

/*
 * Drain all of it: the loop re-reads ISR until it reads zero.  The line is
 * level-triggered, and a bit left set is an interrupt that never ends.
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
         * One write clears every bit set here.  An AX88190 or AX88790 needs the
         * acknowledge retried until it takes; no such part is in the card table
         * and no retry loop is implemented.  See the note in ne2000.c.
        */
        NIC_PUT(nic, ED_P0_ISR, isr);

        /* Recover the receive ring before completing a transmit.  With PTX
           and OVW asserted together, completing first starts the next queued
           buffer and overwrite recovery immediately stops it again.  PTX
           describes the buffer that already completed, so the recovery code
           quite correctly does not resend -- and the newly started buffer is
           then stranded.  Handling OVW first leaves no new transmit for the
           stop to cut off; the completion below starts it after the chip and
           receive ring are live again. */
        if ((isr & ED_ISR_OVW) != 0)
        {
            nic->overruns++;
            if (dp8390_overwrite(nic, isr))
                return TRUE;            /* reset: the file is fresh */
        }

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
            nic->tx_completed++;

            if (--nic->txb_inuse != 0)
                dp8390_xmit(nic);
        }

        if ((isr & ED_ISR_OVW) == 0 &&
            (isr & (ED_ISR_PRX | ED_ISR_RXE)) != 0)
        {
            if ((isr & ED_ISR_RXE) != 0)
                nic->rx_errors++;
            dp8390_rint(nic);
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
         * A board that has been pulled reads 0xff forever, and this loop runs at
         * interrupt level where nothing else in the machine does.  Give up and
         * return: the line is level-triggered, so real work re-enters.
         */
        if (--rounds == 0)
            break;
    }

    return TRUE;
}
