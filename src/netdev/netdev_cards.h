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
 * Chip family.  The value picks the ops table in netdev_nic_ops_for(), at the
 * foot of netdev_cards.c, so a LANCE (A2065, Ariadne I) is a new value here
 * and a new ops table beside the two that exist -- not a change to the shell
 * or to any card row.
 */
#define NETDEV_CHIP_NE2000  0   /* DP8390 clone with an ASIC remote-DMA port */
#define NETDEV_CHIP_ED      1   /* DP8390 with a memory-mapped packet buffer */
#define NETDEV_CHIP_LANCE   2   /* Am7990/Am79C960: bus master, rings in RAM  */
#define NETDEV_CHIP_EL3     3   /* 3Com EtherLink III: windowed, PIO FIFO     */

/*
 * How the board tells us the interrupt was its.  A Zorro INT2 is shared, so a
 * server that answers "mine" without asking eats every other board's.
 */
#define NETDEV_IRQ_NONE     0   /* no status register: ask the chip's ISR */

/*
 * How the board is found.  Everything with an autoconfig record comes off the
 * ConfigDev list; the PCMCIA slot has no such record and lives at a fixed
 * address behind Gayle, so it is claimed through card.resource instead.
 */
#define NETDEV_BUS_ZORRO    0
#define NETDEV_BUS_PCMCIA   1
#define NETDEV_BUS_FIXED    2   /* a fixed address, no autoconfig, no claim */

/*
 * A CARD WHOSE CHIP IS BEHIND AN ISA PLUG AND PLAY BRIDGE.
 *
 * The X-Surf is a Zorro II board carrying an RTL8019AS on a private ISA bus,
 * and that chip decodes nothing at all until ISA PnP has assigned it an I/O
 * base and activated it.  Until then the board's window reads as a floating
 * bus and the card looks absent.  netdev_isapnp.c runs the sequence; this is
 * the four numbers it needs, and NULL on every row that has no bridge.
 *
 * io_win + port * stride is where an ISA port lands in the board window, and
 * the window is not wide enough for the whole 12-bit port space: the X-Surf's
 * is 4 KB at stride 2, so it carries ports 0..0x7ff and the twelfth address
 * line comes out of a latch on the board.  hi_reg/hi_bit is that latch, and
 * it is why the PnP ADDRESS port (0x279) and WRITE_DATA port (0xa79) are the
 * same board address with one bit of state between them.
 */
typedef struct NetdevIsaPnp
{
    ULONG   io_win;             /* board offset of ISA port 0                */
    ULONG   hi_reg;             /* board offset of the latch holding A11     */
    UBYTE   hi_bit;             /* which bit of that latch A11 is            */
    UBYTE   ldn;                /* logical device to configure, 0 on one-
                                   function cards                            */
    UWORD   ports;              /* how many ports the device decodes         */
} NetdevIsaPnp;

typedef struct NetdevCard
{
    const char *name;           /* what the user pins with, and what we print */
    UWORD       manid;
    UWORD       prodid;
    ULONG       reg_off;        /* register file, offset from the board base */
    UWORD       stride;         /* bytes between consecutive register indices */
    ULONG       wide_off;       /* 32-bit mirrored data window, 0 = none      */
    UBYTE       chip;           /* NETDEV_CHIP_*                              */
    ULONG       bps;            /* S2_DEVICEQUERY line rate                   */
    UBYTE       ax88796;        /* station address at AX88796_NODEID_OFFSET   */

    /* NETDEV_CHIP_ED only: the packet buffer is mapped, not DMA'd. */
    ULONG       mem_off;
    ULONG       mem_size;
    ULONG       prom_off;       /* station address PROM, same stride          */

    /* Named by every row: -Werror=missing-field-initializers does not accept
       a trailing field that defaults, and a card row is exactly the place
       where a silently-defaulted field would be worth catching. */
    UBYTE       bus;            /* NETDEV_BUS_*                               */
    ULONG       base;           /* NETDEV_BUS_PCMCIA: the fixed window base   */
    ULONG       odd_off;        /* odd-register window, offset from base; 0 =
                                   the register file is contiguous            */
    UBYTE       lance_swap;     /* the board crosses the SRAM byte lanes, so
                                   descriptor words are written pre-swapped   */
    /*
     * A register file whose addresses are not base + reg * stride.  32 entries
     * when set: 0..15 are the NIC registers, 16..31 the ASIC block, which is
     * how the NE2000 core already thinks of them (ASIC 0 is the data port,
     * ASIC 15 the reset).  NULL for every card whose file is evenly spaced.
     */
    const ULONG *regmap;
    UWORD       serial_oui;     /* non-zero: the station address is this OUI
                                   prefix plus the autoconfig serial number,
                                   which is where Commodore put it -- there is
                                   no address PROM in the board window        */
    /*
     * The ISA Plug and Play bridge in front of the chip, or NULL when the
     * register file decodes as soon as the board is mapped -- which is every
     * row but the X-Surf.  The port the chip is told to decode at is derived
     * from reg_off, so this adds no second place for the register base to be
     * written down.
     */
    const NetdevIsaPnp *pnp;
} NetdevCard;

extern const NetdevCard netdev_cards[];
extern const UWORD      netdev_card_count;

/* NULL when the name matches no row. */
const NetdevCard *netdev_card_by_name(const char *name);

/*
 * netdev_isapnp.c: bring the chip out from behind an ISA PnP bridge, so that
 * it decodes at board + card->reg_off.  TRUE when there is no bridge on this
 * row -- the common case -- and TRUE when the sequence left a DP8390
 * answering there.  Every step is recorded in the probe record either way.
 */
BOOL netdev_isapnp_configure(const NetdevCard *card, APTR board);

/*
 * Which PCMCIA row drives the card whose CIS says (manf, prod).
 *
 * A PCMCIA row's manid/prodid are its CISTPL_MANFID, not an autoconfig
 * record; the slot has no autoconfig record to carry one.  An exact match
 * wins, and a row with 0/0 is the catch-all -- the NE2000 clones, which are a
 * hundred manufacturer IDs for one chip and cannot be enumerated.  NULL when
 * the table has neither, which cannot happen while the catch-all row exists
 * and is the answer if it ever stops existing.
 */
const NetdevCard *netdev_card_by_cis(UWORD manf, UWORD prod);

#endif /* AMINETXDUO_NETDEV_CARDS_H */
