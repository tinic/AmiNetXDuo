/*
 * anxnet.device and anxgenet.device, the card table.  One source, two
 * images: netdev_roster.h decides which rows an image carries, and a row's
 * unit pin is (its index in THAT image's table + 1) * 100, so anxgenet.device
 * has the GENET at pin 100 and anxnet.device keeps every pin it published.
 *
 * The registers are byte-swapped on the Amiga side of all of these, which is
 * why nothing in netdev_bus.c swaps anything.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_cards.h"
#include "netdev_nic.h"

#if NETDEV_HAS_CLASSIC
/*
 * The X-Surf 500's register file, transcribed from
 * wiki.icomp.de/wiki/X-Surf-500_registers and not computed: the ACA500 header
 * exposes so few address lines that no stride describes it.  0..15 are the NIC
 * file and 16..31 the ASIC block, so entry 16 is the data port and 31 the reset.
 */
static const ULONG xsurf500_regmap[32] =
{
    /* NIC, page-selected by CR as usual */
    0x0000, 0x0204, 0x0c00, 0x0e04, 0x2010, 0x2214, 0x2c10, 0x2e14,
    0x4080, 0x4284, 0x4c80, 0x4e84, 0x6090, 0x6294, 0x6c90, 0x6e94,
    /* ASIC: 16 = 16-bit data port, 17 = 8-bit data port, 31 = HWAKE/Reset */
    0x0060, 0x0264, 0x0c60, 0x0e64, 0x2070, 0x2274, 0x2c70, 0x2e74,
    0x40e0, 0x42e4, 0x4ce0, 0x4ee4, 0x60f0, 0x62f4, 0x6cf0, 0x6ef4
};

/*
 * The X-Surf's 4 KB window at board+$8000 is its private ISA bus's I/O space at
 * the row's stride of 2, so it reaches ISA ports $000..$7ff and no further.
 * Port bit 11 comes out of a write-only latch at board+$7e, bit 7, which is why
 * the PnP ADDRESS ($279) and WRITE_DATA ($a79) ports are one board address,
 * $84f2.  One logical device, so LDN 0.
 */
static const NetdevIsaPnp xsurf_pnp =
{
    0x8000,     /* the ISA I/O window                                        */
    0x007e,     /* the latch carrying ISA A11                                */
    0x80,       /* its bit 7                                                 */
    0,          /* logical device 0                                          */
    32          /* ports: the NE2000 register file, NIC block plus ASIC      */
};
#endif /* NETDEV_HAS_CLASSIC */

const NetdevCard netdev_cards[] =
{
    /* name        manid prodid reg_off stride wide_off
       chip                bps         ax  mem_off mem_size prom_off
       bus               base      odd_off  swap  regmap  oui  pnp */
#if NETDEV_HAS_CLASSIC
    { "xsurf100",  4626,   100, 0x0800,     4, 0x8880,
      NETDEV_CHIP_NE2000, 100000000UL, 1,       0,       0,       0,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0, NULL, NULL },

    /*
     * reg_off is the one place the register base is written down.  The chip
     * decodes nothing until netdev_isapnp.c configures it, and what that file
     * programs is derived from this number rather than carried beside it:
     * ISA port = (reg_off - pnp->io_win) / stride == $300.
     */
    { "xsurf",     4626,    23, 0x8600,     2,      0,
      NETDEV_CHIP_NE2000,  10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0, &xsurf_pnp, NULL },

    { "ariadne2",  2167,   202, 0x0600,     2,      0,
      NETDEV_CHIP_NE2000,  10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0, NULL, NULL },

    { "hydra",     2121,     1, 0xffe1,     2,      0,
      NETDEV_CHIP_ED,      10000000UL, 0,       0,  0x4000,  0xffc0,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0, NULL, NULL },

    { "lanrover",  1023,   254, 0x0001,     2,      0,
      NETDEV_CHIP_ED,      10000000UL, 0,  0x8000,  0x8000,  0x0100,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0, NULL, NULL },

    /*
     * The two LANCE boards.  Registers are RDP then RAP, a word apart, and the
     * 32 KB SRAM behind them holds the rings, the buffers and the init block.
     * The Ariadne crosses the SRAM byte lanes -- see lance.c.
     */
    { "a2065",      514,   112, 0x4000,     2,      0,
      NETDEV_CHIP_LANCE,   10000000UL, 0,  0x8000,  0x8000,  0x0000,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0x0080, NULL, NULL },

    { "ariadne",   2167,   201, 0x0370,     2,      0,
      NETDEV_CHIP_LANCE,   10000000UL, 0,  0x8000,  0x8000,  0x0000,
      NETDEV_BUS_ZORRO, 0, 0, 1, NULL, 0x0060, NULL, NULL },

    /*
     * The A1200/A600 PCMCIA slot.  No autoconfig record and no board base:
     * Gayle puts the card's I/O space at 0xA20000 and the card is told to decode
     * at 0x300, so the register file is 0xA20300 with indices one byte apart.
     */
    { "pcmcia",       0,     0, 0x0300,     1,      0,
      NETDEV_CHIP_NE2000,  10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_PCMCIA, 0x00a20000UL, 0x00010000UL, 0, NULL, 0, NULL, NULL },
    /*
     * The X-Surf 500, on an ACA500 or ACA500plus.  An AX88796B at a fixed
     * $EE0000 with no autoconfig record, so it is probed rather than found and
     * its register file is a table rather than a stride.  The FIFO at $EE8440
     * is sixteen bytes that take a movem.l, which is what wide_off names.
     *
     * UNVERIFIED, AND IT IS NOT GOING TO BE.  Every number in this row comes
     * from the iComp wiki.  No X-Surf 500 and no ACA500 is on this network, no
     * emulator models the board, and `tests/tools/run-hwcard.sh -C xsurf500`
     * says so and skips rather than crediting a run against another card.  A
     * row nobody can test is not the same as a row that is wrong, and this one
     * costs nothing while it sits here; if a board ever turns up, that harness
     * is what proves it.
     */
    { "xsurf500",     0,     0, 0x0000,     1, 0x8440,
      NETDEV_CHIP_NE2000, 100000000UL, 1,       0,       0,       0,
      NETDEV_BUS_FIXED, 0x00ee0000UL, 0, 0, xsurf500_regmap, 0, NULL, NULL },

    /*
     * The 3Com EtherLink III PCMCIA card, a 3C589 of any revision.  manid/prodid
     * are the CIS MANFID, which is how netdev_pcmcia.c tells this card from an
     * NE2000 clone in the same slot; a PCMCIA row is never matched against a
     * ConfigDev.  Appended, never inserted: a unit pin is (index + 1) * 100, so
     * the table's order is a published interface.  reg_off 0x0300 is an
     * assumption, and the entry's raw bytes go into the probe record.
     */
    { "3c589",   0x0101, 0x0589, 0x0300,     1,      0,
      NETDEV_CHIP_EL3,     10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_PCMCIA, 0x00a20000UL, 0x00010000UL, 0, NULL, 0, NULL, NULL },

    /*
     * The two Megahertz/3Com LAN+modem combo cards the CIS walk reaches.
     * Function 0 of each is an EtherLink III -- the tuples they state say so,
     * and tests/../test_netdev_cis.c carries their real per-function chains --
     * so the core is the one the 3c589 above uses.  Without a row of their own
     * netdev_card_by_cis() handed them to the NE2000 fallback, which is not
     * what is in them, and the walk that had already parsed them correctly
     * ended in a card that would not come up.
     *
     * reg_off is the same assumption the 3c589 row makes and matters less
     * here: netdev_pcmcia.c takes the offset the card's own CISTPL_CFTABLE_ENTRY
     * states over this number, and both of these state one.
     *
     * UNVERIFIED ON HARDWARE.  Neither card is on this network.  The CIS
     * parse is covered by test_netdev_cis.c against their real tuples, and
     * that is the whole of what is proven; whether an EtherLink III core
     * drives function 0 once it is configured is not, and `run-hwcard.sh`
     * against one of these cards is what would say.  Same standing as the
     * xsurf500 row above, and the same reason it costs nothing to carry.
     */
    { "3ccfem556", 0x0101, 0x0556, 0x0300,   1,      0,
      NETDEV_CHIP_EL3,     10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_PCMCIA, 0x00a20000UL, 0x00010000UL, 0, NULL, 0, NULL, NULL },
    { "3cxem556",  0x0101, 0x0035, 0x0300,   1,      0,
      NETDEV_CHIP_EL3,     10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_PCMCIA, 0x00a20000UL, 0x00010000UL, 0, NULL, 0, NULL, NULL },

#endif /* NETDEV_HAS_CLASSIC */

#if NETDEV_HAS_DTREE
    /*
     * The Raspberry Pi 4 / CM4's own Ethernet, reached through a PiStorm32
     * running Emu68.  No autoconfig record and no fixed address: Emu68's
     * device tree names the register window, the interrupt and the station
     * address, and netdev_dtree.c reads all three.  reg_off 0 and stride 4
     * describe the 32-bit little-endian register file for the bus record;
     * the core reads it through its own accessor and swaps.  Appended, like
     * every row, because the pin number (index + 1) * 100 is published.
     */
    { "genet",        0,     0, 0x0000,     4,      0,
      NETDEV_CHIP_GENET, 1000000000UL, 0,       0,       0,       0,
      NETDEV_BUS_DTREE, 0, 0, 0, NULL, 0, NULL, "brcm,bcm2711-genet-v5" },
#endif /* NETDEV_HAS_DTREE */

#if NETDEV_HAS_ZZ9000
    /*
     * The MNT ZZ9000's Ethernet: the Zynq's GEM, driven by the card's ARM
     * firmware, presented to the 68k as a register block and two frame
     * windows in the board's first 64 KB (zz9000.c has the map).  The Zorro
     * III record is product 4, the Zorro II one product 3; the same firmware
     * behind both.  reg_off 0 and stride 2 describe the 16-bit register
     * file for the bus record; the core reads it through its own accessor.
     * bps is 100 Mbit/s, what MNT's own driver reports: the GEM negotiates
     * a gigabit, but the stack reads a gigabit link with a slow handshake
     * as a long path and lets the window outgrow the card's ring
     * (bsdsocket_window.h, ami_bsd_tcp_window_burst_bound), and this
     * machine's handshake is slow because this machine is.  Measured: a
     * 396 KB window put 275 retransmissions into ten seconds.
     * Appended after the GENET, like every row: the pin (index + 1) * 100
     * and the ANXNET_CARD_NAMES position are published.
     */
    { "zz9000",    0x6d6e,     4, 0x0000,     2,      0,
      NETDEV_CHIP_ZZ9000, 100000000UL, 0,       0,       0,       0,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0, NULL, NULL },
    { "zz9000z2",  0x6d6e,     3, 0x0000,     2,      0,
      NETDEV_CHIP_ZZ9000, 100000000UL, 0,       0,       0,       0,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0, NULL, NULL },
#endif /* NETDEV_HAS_ZZ9000 */
};

const UWORD netdev_card_count =
    (UWORD)(sizeof(netdev_cards) / sizeof(netdev_cards[0]));

/*
 * Chip family -> core.  Here rather than in any of the six cores, because a
 * core that names another cannot be built without it.  NULL for a family with
 * no core: the board is recognised and skipped rather than enumerated.
 */
const struct NetdevNicOps *netdev_nic_ops_for(UBYTE chip)
{
#if NETDEV_HAS_CLASSIC
    if (chip == NETDEV_CHIP_NE2000)
        return &netdev_nic_ne2000;
    if (chip == NETDEV_CHIP_ED)
        return &netdev_nic_ed;
    if (chip == NETDEV_CHIP_LANCE)
        return &netdev_nic_lance;
    if (chip == NETDEV_CHIP_EL3)
        return &netdev_nic_el3;
#endif
#if NETDEV_HAS_DTREE
    if (chip == NETDEV_CHIP_GENET)
        return &netdev_nic_genet;
#endif
#if NETDEV_HAS_ZZ9000
    if (chip == NETDEV_CHIP_ZZ9000)
        return &netdev_nic_zz9000;
#endif

    return NULL;
}

const NetdevCard *netdev_card_by_cis(UWORD manf, UWORD prod)
{
    const NetdevCard *fallback = NULL;
    UWORD             i;

    for (i = 0; i < netdev_card_count; i++)
    {
        const NetdevCard *card = &netdev_cards[i];

        if (card->bus != NETDEV_BUS_PCMCIA)
            continue;

        if (card->manid == 0 && card->prodid == 0)
        {
            fallback = card;
            continue;
        }

        if (card->manid == manf && card->prodid == prod)
            return card;
    }

    return fallback;
}

const NetdevCard *netdev_card_by_zorro(UWORD manid, UBYTE prodid)
{
    UWORD i;

    for (i = 0; i < netdev_card_count; i++)
    {
        /* A PCMCIA row's manid/prodid are its CIS MANFID, not an
           autoconfig record, so it must never match a board here. */
        if (netdev_cards[i].bus != NETDEV_BUS_ZORRO)
            continue;

        if (netdev_cards[i].manid == manid &&
            (UBYTE)netdev_cards[i].prodid == prodid)
            return &netdev_cards[i];
    }

    return NULL;
}

static int card_streq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = *a++;
        char cb = *b++;

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + ('a' - 'A'));
        if (ca != cb)
            return 0;
    }

    return (*a == '\0' && *b == '\0');
}

const NetdevCard *netdev_card_by_name(const char *name)
{
    UWORD i;

    if (name == NULL)
        return NULL;

    for (i = 0; i < netdev_card_count; i++)
    {
        if (card_streq(netdev_cards[i].name, name))
            return &netdev_cards[i];
    }

    return NULL;
}
