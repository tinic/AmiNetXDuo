/*
 * anxnet.device, the card table.
 *
 * Sources for every number here, so a seventh row does not need the archaeology
 * again:
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

const NetdevCard netdev_cards[] =
{
    /* name        manid prodid reg_off stride wide_off
       chip                bps         ax  mem_off mem_size prom_off
       bus               base      odd_off  swap */
    { "xsurf100",  4626,   100, 0x0800,     4, 0x8880,
      NETDEV_CHIP_NE2000, 100000000UL, 1,       0,       0,       0,
      NETDEV_BUS_ZORRO, 0, 0, 0 },

    { "xsurf",     4626,    23, 0x8600,     2,      0,
      NETDEV_CHIP_NE2000,  10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_ZORRO, 0, 0, 0 },

    { "ariadne2",  2167,   202, 0x0600,     2,      0,
      NETDEV_CHIP_NE2000,  10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_ZORRO, 0, 0, 0 },

    { "hydra",     2121,     1, 0xffe1,     2,      0,
      NETDEV_CHIP_ED,      10000000UL, 0,       0,  0x4000,  0xffc0,
      NETDEV_BUS_ZORRO, 0, 0, 0 },

    { "lanrover",  1023,   254, 0x0001,     2,      0,
      NETDEV_CHIP_ED,      10000000UL, 0,  0x8000,  0x8000,  0x0100,
      NETDEV_BUS_ZORRO, 0, 0, 0 },

    /*
     * The two LANCE boards.  Registers are RDP then RAP, a word apart, and
     * the 32 KB SRAM behind them holds the rings, the buffers and the init
     * block.  The Ariadne crosses the SRAM byte lanes -- see lance.c -- and
     * is an Am79C960 rather than an Am7990, which changes nothing this
     * driver touches.
     */
    { "a2065",      514,   112, 0x4000,     2,      0,
      NETDEV_CHIP_LANCE,   10000000UL, 0,  0x8000,  0x8000,  0x0000,
      NETDEV_BUS_ZORRO, 0, 0, 0 },

    { "ariadne",   2167,   201, 0x0370,     2,      0,
      NETDEV_CHIP_LANCE,   10000000UL, 0,  0x8000,  0x8000,  0x0000,
      NETDEV_BUS_ZORRO, 0, 0, 1 },

    /*
     * The A1200/A600 PCMCIA slot.  No autoconfig record and no board base:
     * Gayle puts the card's I/O space at 0xA20000 and the card is told to
     * decode at 0x300, so the register file is 0xA20300 with the indices one
     * byte apart.  netdev_pcmcia.c does the claiming and the configuring;
     * from the chip core's side it is an NE2000 like any other.
     */
    { "pcmcia",       0,     0, 0x0300,     1,      0,
      NETDEV_CHIP_NE2000,  10000000UL, 0,       0,       0,       0,
      NETDEV_BUS_PCMCIA, 0x00a20000UL, 0x00010000UL, 0 },
};

const UWORD netdev_card_count =
    (UWORD)(sizeof(netdev_cards) / sizeof(netdev_cards[0]));

/*
 * Chip family -> core.  Here rather than in either core, because a core that
 * names the other one is a core that cannot be built without it.  NULL for a
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
