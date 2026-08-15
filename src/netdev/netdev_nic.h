/*
 * anxnet.device, layer 2 of 3: the chip core interface.
 *
 * The SANA-II shell above never names a register.  It calls attach/init/stop/
 * tx/setfilter/intr and is handed whole Ethernet frames back through a
 * callback.  Everything DP8390 lives behind that, so a second core is another
 * NetdevNicOps table and no change at all above this line.
 *
 * That has now been paid for once: ed.c is the second core, for the Hydra and
 * the ASDG LAN Rover, and it changed nothing in this file except adding its
 * name to the list at the bottom.  It shares dp8390.c whole and differs only
 * in the three buffer-access pointers below.  A third core -- an Am7990 LANCE
 * for the A2065 and Ariadne I, which are not in this family and are not
 * implemented here -- would share none of dp8390.c and still fit.
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

/*
 * The receive staging buffer takes anything the ring can hold, not just an
 * untagged MTU: an 802.1Q frame is 1518 bytes and a jumbo-ish clone can be
 * longer still, and the alternative to accepting it is resetting the chip.
 */
#define NETDEV_RXBUF_MAX    2048

/* How many times one interrupt may go round before it gives the machine back. */
#define NETDEV_DRAIN_MAX    32

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

    /* Even, and stated rather than inherited from what precedes them:
       netdev_device.c copies both as a longword and a word, which is an
       address error on a 68000 if a field reorder ever lands them odd. */
    UBYTE               factory[NETDEV_ADDR_LEN] __attribute__((aligned(2)));
    UBYTE               mac[NETDEV_ADDR_LEN] __attribute__((aligned(2)));
    UBYTE               mar[8];         /* the multicast hash, host order */
    BOOL                promisc;
    BOOL                running;

    /* DP8390 ring state, the names are NetBSD's. */
    /*
     * Where the chip's remote-DMA pointer is, and how much of the burst is
     * left.  A read of the 4-byte ring header leaves the pointer exactly at
     * the frame body, so the body's own RSAR/RBCR programming is writing the
     * chip's own position back to it -- six register accesses, and on the
     * PCMCIA card a scalar register access costs 8.3 us against 0.5 for a
     * word through the data port.  dma_left is 0 whenever the position is not
     * to be trusted.
     */
    LONG                dma_pos;
    UWORD               dma_left;

    LONG                mem_start;
    LONG                mem_end;
    LONG                mem_size;
    LONG                mem_ring;
    UWORD               txb_cnt;
    ULONG               serial;     /* the board's autoconfig serial number */
    UWORD               txb_inuse;

    /* LANCE ring cursors.  The DP8390 cores do not use them: their ring is
       the chip's own page walk, not a descriptor list we index. */
    UWORD               rx_next;
    UWORD               tx_next;
    UWORD               tx_done;
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
    UBYTE               pad_proto;

    /*
     * How the packet buffer is reached.  NE2000 fills these with its remote
     * DMA; a shared-memory DP8390 board (Hydra, LanRover) fills them with
     * moves through its mapped window, and that is the whole of the
     * difference between the two.
     */
    VOID  (*read_hdr)(NetdevNic *nic, LONG src, NetdevRing *hdr);
    LONG  (*ring_copy)(NetdevNic *nic, LONG src, UBYTE *dst, UWORD amount);

    /*
     * A pointer straight into the card's own buffer, or NULL when the frame
     * has to be staged.  Set only by cores whose buffer is memory-mapped:
     * the receive path then makes ONE pass over the frame -- the opener's
     * CopyToBuff reads the card directly -- instead of copying it to rxbuf
     * and having CopyToBuff copy it again.  hydra.device is 6 KB with no
     * bulk copy loop in it at all, which is how it does the same thing.
     *
     * NULL for a wrapped frame, and NULL for every port-driven core, where
     * there is no address to hand out.
     */
    const volatile UBYTE *(*frame_at)(NetdevNic *nic, LONG src, UWORD len);

    /*
     * Where to frame the NEXT transmit, or NULL to use the unit's staging
     * buffer.  A core whose transmit buffer is CPU-addressable returns it and
     * the opener's CopyFrom writes the card directly -- one pass instead of
     * building in RAM and copying it across.  The profile priced that copy at
     * 223 us of a 273 us deficit against a2065.device.
     *
     * NULL for the DP8390 cores: an NE2000's buffer is behind a port, and the
     * Hydra and LAN Rover map a buffer whose byte lanes are not separately
     * selectable, so CopyFrom writing bytes into it would corrupt both halves.
     */
    UBYTE *(*tx_at)(NetdevNic *nic);
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

    /*
     * What had to be done to the station address before the chip could be
     * given one.  All three are 0 or 1 and all three are reported: a driver
     * that quietly repaired or invented a hardware address is exactly the
     * thing that costs an afternoon later.
     */
    ULONG               mac_group_fix;  /* group bit cleared out of the PROM  */
    ULONG               mac_from_cis;   /* PROM blank, address taken from CIS */
    ULONG               mac_derived;    /* PROM blank, address derived here   */

    /* One frame at a time comes out of the ring, into here. */
    ULONG               rxbuf[(NETDEV_RXBUF_MAX + 7) / 4];
};

/* The two cores. netdev_nic_ops_for() returns NULL for a chip with no core. */
extern const struct NetdevNicOps netdev_nic_ne2000;
extern const struct NetdevNicOps netdev_nic_ed;
extern const struct NetdevNicOps netdev_nic_lance;

const struct NetdevNicOps *netdev_nic_ops_for(UBYTE chip);

#endif /* AMINETXDUO_NETDEV_NIC_H */
