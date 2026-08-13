/*
 * anxnet.device, layer 3 of 3: the card table.
 *
 * Adding a card to this driver is a row in netdev_cards.c.  Nothing else in
 * the tree needs to change, and that is the whole point of the three layers:
 * the shell knows SANA-II, the chip core knows the DP8390, and only this table
 * knows that an X-Surf 100 puts its registers 4 bytes apart at board+0x800
 * while an Ariadne II puts them 2 bytes apart at board+0x600.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_CARDS_H
#define AMINETXDUO_NETDEV_CARDS_H

#include <exec/types.h>

/*
 * Chip family.  The value picks the ops table in netdev_nic.c, so a LANCE
 * (A2065, Ariadne I) is a new value here and a new ops table beside the two
 * that exist -- not a change to the shell or to any card row.
 */
#define NETDEV_CHIP_NE2000  0   /* DP8390 clone with an ASIC remote-DMA port */
#define NETDEV_CHIP_ED      1   /* DP8390 with a memory-mapped packet buffer */

/*
 * How the board tells us the interrupt was its.  A Zorro INT2 is shared, so a
 * server that answers "mine" without asking eats every other board's.
 */
#define NETDEV_IRQ_NONE     0   /* no status register: ask the chip's ISR */
#define NETDEV_IRQ_BIT7     1   /* byte at board + irq_off, bit 7 set = ours */

typedef struct NetdevCard
{
    const char *name;           /* what the user pins with, and what we print */
    UWORD       manid;
    UWORD       prodid;
    ULONG       reg_off;        /* register file, offset from the board base */
    UWORD       stride;         /* bytes between consecutive register indices */
    ULONG       wide_off;       /* 32-bit mirrored data window, 0 = none      */
    ULONG       irq_off;        /* board-level interrupt status byte          */
    UBYTE       irq_kind;       /* NETDEV_IRQ_*                               */
    UBYTE       chip;           /* NETDEV_CHIP_*                              */
    ULONG       bps;            /* S2_DEVICEQUERY line rate                   */
    UBYTE       ax88796;        /* station address at AX88796_NODEID_OFFSET   */

    /* NETDEV_CHIP_ED only: the packet buffer is mapped, not DMA'd. */
    ULONG       mem_off;
    ULONG       mem_size;
    ULONG       prom_off;       /* station address PROM, same stride          */
} NetdevCard;

extern const NetdevCard netdev_cards[];
extern const UWORD      netdev_card_count;

/* NULL when the name matches no row. */
const NetdevCard *netdev_card_by_name(const char *name);

#endif /* AMINETXDUO_NETDEV_CARDS_H */
