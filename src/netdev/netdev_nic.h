/*
 * anxnet.device, layer 2 of 3: the chip core interface.
 *
 * The SANA-II shell above never names a register.  It calls attach/init/stop/
 * tx/setfilter/intr and is handed whole Ethernet frames back through a
 * callback.  Everything DP8390 lives behind that, so a second core -- an
 * Am7990 LANCE for the A2065 and Ariadne I, which are not in this family and
 * are not implemented here -- is another NetdevNicOps table and no change at
 * all above this line.
 *
 * What the seam has to carry for a LANCE to fit later, and does:
 *   - the frame callback takes a complete frame, header included, so a core
 *     that DMAs into host memory hands over a pointer and a core that reads a
 *     FIFO hands over its staging buffer;
 *   - the filter is a 64-bit hash plus a promiscuous flag, which is what both
 *     chips actually implement;
 *   - tx takes one linear frame, because that is the only shape either chip
 *     can be given one in.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_NIC_H
#define AMINETXDUO_NETDEV_NIC_H

#include <exec/types.h>

#include "netdev_bus.h"
#include "netdev_cards.h"
#include "netdev_mcaf.h"

#define NETDEV_HDR_LEN      14
#define NETDEV_MTU          1500
#define NETDEV_FRAME_MAX    (NETDEV_HDR_LEN + NETDEV_MTU)   /* no FCS on RX */
#define NETDEV_FRAME_MIN    60

typedef struct NetdevNic NetdevNic;

typedef VOID (*NetdevRxFn)(APTR arg, const UBYTE *frame, UWORD len);

/*
 * The 4-byte header the DP8390 writes in front of every received frame.
 * count is little-endian on the wire side of the chip and is byte-swapped by
 * whichever buffer reader produced it, so by the time the ring walk sees it
 * the field is host order.
 */
typedef struct NetdevRing
{
    UBYTE   rsr;
    UBYTE   next_packet;
    UWORD   count;
} NetdevRing;

struct NetdevNicOps
{
    /* Identify the chip and read the factory station address. 0 = present. */
    LONG  (*attach)(NetdevNic *nic);
    /* Program the chip from mac/mar/promisc and start it. 0 = running. */
    LONG  (*init)(NetdevNic *nic);
    VOID  (*stop)(NetdevNic *nic);
    /* One linear frame, header included, no FCS. 0 = queued to the chip. */
    LONG  (*tx)(NetdevNic *nic, const UBYTE *frame, UWORD len);
    /* Push mac/mar/promisc into the chip without disturbing the ring. */
    VOID  (*setfilter)(NetdevNic *nic);
    /* Drain the ring and every ISR bit. TRUE if this board had work. */
    BOOL  (*intr)(NetdevNic *nic);
};

struct NetdevNic
{
    NetdevBus           bus;
    const NetdevCard   *card;
    const struct NetdevNicOps *ops;
    volatile UBYTE     *board;          /* the Zorro board base */

    NetdevRxFn          rx;
    APTR                rx_arg;

    UBYTE               factory[NETDEV_ADDR_LEN];
    UBYTE               mac[NETDEV_ADDR_LEN];
    UBYTE               mar[8];         /* the multicast hash, host order */
    BOOL                promisc;
    BOOL                running;

    /* DP8390 ring state, the names are NetBSD's. */
    LONG                mem_start;
    LONG                mem_end;
    LONG                mem_size;
    LONG                mem_ring;
    UWORD               txb_cnt;
    UWORD               txb_inuse;
    UWORD               txb_new;
    UWORD               txb_next_tx;
    UWORD               txb_len[3];
    UWORD               tx_page_start;
    UWORD               rec_page_start;
    UWORD               rec_page_stop;
    UWORD               next_packet;
    UBYTE               cr_proto;
    UBYTE               rcr_proto;
    UBYTE               dcr_reg;
    UBYTE               useword;
    UBYTE               ax_workaround;  /* AX88190 ISR acknowledge quirk */
    UBYTE               no_rdc;         /* AX88796 has no ISR.RDC        */

    /*
     * How the packet buffer is reached.  NE2000 fills these with its remote
     * DMA; a shared-memory DP8390 board (Hydra, LanRover) fills them with
     * moves through its mapped window, and that is the whole of the
     * difference between the two.
     */
    VOID  (*read_hdr)(NetdevNic *nic, LONG src, NetdevRing *hdr);
    LONG  (*ring_copy)(NetdevNic *nic, LONG src, UBYTE *dst, UWORD amount);
    UWORD (*write_buf)(NetdevNic *nic, const UBYTE *frame, UWORD len,
                       LONG buf);

    /* Counters the shell reports through S2_GETGLOBALSTATS / SPECIALSTATS. */
    ULONG               rx_packets;
    ULONG               tx_packets;
    ULONG               rx_errors;
    ULONG               tx_errors;
    ULONG               overruns;
    ULONG               collisions;
    ULONG               resets;

    /* One frame at a time comes out of the ring, into here. */
    ULONG               rxbuf[(NETDEV_FRAME_MAX + 7) / 4];
};

/* The two cores. netdev_nic_ops_for() returns NULL for a chip with no core. */
extern const struct NetdevNicOps netdev_nic_ne2000;

const struct NetdevNicOps *netdev_nic_ops_for(UBYTE chip);

#endif /* AMINETXDUO_NETDEV_NIC_H */
