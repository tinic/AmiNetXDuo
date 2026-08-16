/*
 * anxnet.device, the card table.
 *
 * Sources for every number here, so the next row does not need the same
 * search:
 *
 *   X-Surf 100   NetBSD sys/arch/amiga/dev/xsh.c (XSURF100_NE_OFFSET 0x0800)
 *                and if_ne_xsh.c (amiga_bus_stride_4).  The 0x8880 window and
 *                the board interrupt byte at +0x40 are from the card, checked
 *                against Amiberry's decode: banks 0x08-0x0f and 0x88-0x8f are
 *                the byte-swapped register image, and any address in the top
 *                128 bytes of a bank is the data port again.
 *   X-Surf       if_ne_xsurf.c: base 0x8000, ISA 0x300 at stride 2 -> 0x8600.
 *   Ariadne II   if_ne_zbus.c: base 0, ISA 0x300 at stride 2 -> 0x600.
 *   Hydra/ASDG   if_ed_zbus.c: register and PROM offsets, 16K/32K buffer.
 *
 * The registers are byte-swapped on the Amiga side of all of these, which is
 * why nothing in netdev_bus.c swaps anything.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_cards.h"
#include "netdev_nic.h"

/*
 * The X-Surf 500's register file, transcribed from
 * wiki.icomp.de/wiki/X-Surf-500_registers and not computed from anything.
 *
 * The card hangs off an ACA500's 34-pin header, which exposes so few address
 * lines that the register number's bits come out scattered.  Consecutive
 * registers sit anywhere from $0204 to $6E94 apart, and no stride describes
 * it.  Offsets are from the card's base, $EE0000.
 *
 * 0..15 are the NIC file and 16..31 the ASIC block, which is the same split
 * the NE2000 core uses, so entry 16 is the 16-bit data port and entry 31 is
 * the reset.  Nothing here is derived.  A guessed offset gives a driver that
 * looks right and silently talks to the wrong register, and no emulation of
 * this card exists to catch that.
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
 * The X-Surf is a Zorro II board carrying an RTL8019AS on a private ISA bus.
 * Its 4 KB window at board+$8000 is that bus's I/O space at the row's stride
 * of 2, so it reaches ISA ports $000..$7ff and no further.  Port bit 11 comes
 * out of a write-only latch at board+$7e, bit 7.  That is why the PnP ADDRESS
 * port ($279) and WRITE_DATA port ($a79) are one board address, $84f2, with
 * the latch deciding which of the two it is.
 *
 * Derived from Amiberry's model of the card, qemuvga/ne2000.cpp: the XSURF arm
 * of toariadne2() computes `isa_addr = (flags ? 0x1000 : 0) + (addr - 0x8000)`
 * and compares it against `0x279 * 2` and `0xa79 * 2`, and the flag is set from
 * bit 7 of a byte written where `(addr & 0x80ff) == 0x007e`.  The chip's
 * registers appear only while `pnp.activated`, at `io_port * 2` in the same
 * window, 32 ports wide -- the whole NE2000 file, NIC and ASIC.
 *
 * One logical device, so LDN 0.
 */
static const NetdevIsaPnp xsurf_pnp =
{
    0x8000,     /* the ISA I/O window                                        */
    0x007e,     /* the latch carrying ISA A11                                */
    0x80,       /* its bit 7                                                 */
    0,          /* logical device 0                                          */
    32          /* ports: the NE2000 register file, NIC block plus ASIC      */
};

const NetdevCard netdev_cards[] =
{
    /* name        manid prodid reg_off stride wide_off
       chip                bps         ax  mem_off mem_size prom_off
       bus               base      odd_off  swap  regmap  oui  pnp */
    { "xsurf100",  4626,   100, 0x0800,     4, 0x8880,
      NETDEV_CHIP_NE2000, 100000000UL, 1,       0,       0,       0,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0, NULL },

    /*
     * reg_off is still the one place the register base is written down.  The
     * chip decodes nothing until netdev_isapnp.c configures it.  What that
     * file programs is derived from this number rather than carried beside
     * it: ISA port = (reg_off - pnp->io_win) / stride, which is
     * ($8600 - $8000) / 2 = $300, the port if_ne_xsurf.c names.
     *
     * The alternative is to let the PnP phase report where it put the chip
     * and write that back into the row.  That makes the register base a
     * runtime value on one row out of ten.  No reader of this table can then
     * say where the card is.
     */
    { "xsurf",     4626,    23, 0x8600,     2,      0,
      NETDEV_CHIP_NE2000,  10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0, &xsurf_pnp },

    { "ariadne2",  2167,   202, 0x0600,     2,      0,
      NETDEV_CHIP_NE2000,  10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0, NULL },

    { "hydra",     2121,     1, 0xffe1,     2,      0,
      NETDEV_CHIP_ED,      10000000UL, 0,       0,  0x4000,  0xffc0,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0, NULL },

    { "lanrover",  1023,   254, 0x0001,     2,      0,
      NETDEV_CHIP_ED,      10000000UL, 0,  0x8000,  0x8000,  0x0100,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0, NULL },

    /*
     * The two LANCE boards.  Registers are RDP then RAP, a word apart, and
     * the 32 KB SRAM behind them holds the rings, the buffers and the init
     * block.  The Ariadne crosses the SRAM byte lanes -- see lance.c -- and
     * is an Am79C960 rather than an Am7990, which changes nothing this
     * driver touches.
     */
    { "a2065",      514,   112, 0x4000,     2,      0,
      NETDEV_CHIP_LANCE,   10000000UL, 0,  0x8000,  0x8000,  0x0000,
      NETDEV_BUS_ZORRO, 0, 0, 0, NULL, 0x0080, NULL },

    { "ariadne",   2167,   201, 0x0370,     2,      0,
      NETDEV_CHIP_LANCE,   10000000UL, 0,  0x8000,  0x8000,  0x0000,
      NETDEV_BUS_ZORRO, 0, 0, 1, NULL, 0x0060, NULL },

    /*
     * The A1200/A600 PCMCIA slot.  No autoconfig record and no board base:
     * Gayle puts the card's I/O space at 0xA20000 and the card is told to
     * decode at 0x300, so the register file is 0xA20300 with the indices one
     * byte apart.  netdev_pcmcia.c does the claiming and the configuring.
     * From the chip core's side it is an NE2000 like any other.
     */
    { "pcmcia",       0,     0, 0x0300,     1,      0,
      NETDEV_CHIP_NE2000,  10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_PCMCIA, 0x00a20000UL, 0x00010000UL, 0, NULL, 0, NULL },
    /*
     * The X-Surf 500, on an ACA500 or ACA500plus.  An AX88796B, the same
     * family as the X-Surf 100, at a fixed $EE0000 with no autoconfig record.
     * It is therefore probed rather than found, and its register file is a
     * table (see above) rather than a stride.  The 16-bit data port is ASIC
     * register 0 at $EE0060.  The FIFO at $EE8440 is sixteen bytes that take a
     * movem.l, which is what wide_off names.
     *
     * Nothing emulates this card, so it has never been run.  It is written to
     * cost nothing when absent: the probe reads the chip, and attach refuses
     * if a DP8390 does not answer, as the X-Surf row does today.
     */
    { "xsurf500",     0,     0, 0x0000,     1, 0x8440,
      NETDEV_CHIP_NE2000, 100000000UL, 1,       0,       0,       0,
      NETDEV_BUS_FIXED, 0x00ee0000UL, 0, 0, xsurf500_regmap, 0, NULL },

    /*
     * The 3Com EtherLink III PCMCIA card, a 3C589 of any revision.  Same slot
     * and the same two Gayle windows as the "pcmcia" row.  What differs is the
     * chip, which is windowed rather than paged and moves frames through one
     * PIO port instead of a remote-DMA ring.
     *
     * Appended, not put beside the other PCMCIA row.  A unit pin is
     * (index + 1) * 100, so an inserted row renumbers every row after it and
     * silently repoints somebody's DEVICE= UNIT=.  The table's order is a
     * published interface, so new cards go on the end.
     *
     * manid/prodid are the CIS MANFID, 0x0101/0x0589, which is how
     * netdev_pcmcia.c tells this card from an NE2000 clone in the same slot.
     * A PCMCIA row is never matched against a ConfigDev, because the probe's
     * Zorro loop skips any row that is not NETDEV_BUS_ZORRO, so the two uses
     * of these two fields cannot collide.
     *
     * reg_off 0x0300 is an assumption, and the one thing here a hardware
     * report has to settle.  The card decodes wherever the configuration table
     * entry it was given says, and this driver writes the first entry's index
     * without parsing that entry's I/O descriptor.  It does the same for the
     * NE2000 row, where 0x300 is what cnet.device has assumed against a
     * hundred real cards.  The entry's raw bytes go into the probe record
     * (ANXDIAG_PC_CFTABLE), so one report settles it either way.
     *
     * Nothing emulates this card.  Amiberry's PCMCIA support is NE2000 only,
     * so this row has never been run against a chip.  It is written the way
     * the X-Surf 500 row above is, to cost nothing when the card is absent,
     * and the claim gives the slot straight back when no EtherLink III
     * answers.
     */
    { "3c589",   0x0101, 0x0589, 0x0300,     1,      0,
      NETDEV_CHIP_EL3,     10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_PCMCIA, 0x00a20000UL, 0x00010000UL, 0, NULL, 0, NULL },
};

const UWORD netdev_card_count =
    (UWORD)(sizeof(netdev_cards) / sizeof(netdev_cards[0]));

/*
 * Chip family -> core.  Here rather than in any of the four cores, because a
 * core that names another is a core that cannot be built without it.  NULL for a
 * family with no core: the board is then recognised and skipped instead of
 * enumerated and then failing to open, and netdev_probe() counts the skip.
 */
const struct NetdevNicOps *netdev_nic_ops_for(UBYTE chip)
{
    if (chip == NETDEV_CHIP_NE2000)
        return &netdev_nic_ne2000;
    if (chip == NETDEV_CHIP_ED)
        return &netdev_nic_ed;
    if (chip == NETDEV_CHIP_LANCE)
        return &netdev_nic_lance;
    if (chip == NETDEV_CHIP_EL3)
        return &netdev_nic_el3;

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
