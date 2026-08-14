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

/*
 * Not a row yet: the PCMCIA NE2000 is at a fixed 0xA20300 (second bank
 * 0xA30300) and has no autoconfig record, so it needs a different way in than
 * walking the ConfigDev list.
 */

const NetdevCard netdev_cards[] =
{
    /* name        manid prodid reg_off stride wide_off
       chip                bps         ax  mem_off mem_size prom_off */
    { "xsurf100",  4626,   100, 0x0800,     4, 0x8880,
      NETDEV_CHIP_NE2000, 100000000UL, 1,       0,       0,       0 },

    { "xsurf",     4626,    23, 0x8600,     2,      0,
      NETDEV_CHIP_NE2000,  10000000UL, 0,       0,       0,       0 },

    { "ariadne2",  2167,   202, 0x0600,     2,      0,
      NETDEV_CHIP_NE2000,  10000000UL, 0,       0,       0,       0 },

    { "hydra",     2121,     1, 0xffe1,     2,      0,
      NETDEV_CHIP_ED,      10000000UL, 0,       0,  0x4000,  0xffc0 },

    { "lanrover",  1023,   254, 0x0001,     2,      0,
      NETDEV_CHIP_ED,      10000000UL, 0,  0x8000,  0x8000,  0x0100 },
};

const UWORD netdev_card_count =
    (UWORD)(sizeof(netdev_cards) / sizeof(netdev_cards[0]));

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
