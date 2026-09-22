/*
 * The card table and the name list a configuration file is checked against.
 *
 * Two copies of the same fact exist and have to: netdev_cards.c carries the
 * rows, and include/aminetxduo/anxnet.h carries the names alone, because
 * src/config/config_parse.c rejects CARD=nonsense without linking the driver.
 * Drift between them is silent in both directions.  A name added to the table
 * and not the list is a card no interface file can ask for.  A name in the
 * list and not the table is a CARD= the parser accepts and the driver refuses.
 * It is therefore asserted here.
 *
 * The order matters as much as the membership: anxnet.h documents the Nth name
 * as the card UNIT = (N + 1) * 100 pins.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "aminetxduo/anxnet.h"
#include "netdev_cards.h"

/* netdev_cards.c carries netdev_nic_ops_for(), which names every chip core.
   This test is about the card table, not the cores, so they are stood in for
   rather than linked, because every one of them reaches the hardware. */
#include "netdev_nic.h"
const struct NetdevNicOps netdev_nic_ne2000;
const struct NetdevNicOps netdev_nic_ed;
const struct NetdevNicOps netdev_nic_lance;
const struct NetdevNicOps netdev_nic_el3;
const struct NetdevNicOps netdev_nic_genet;
const struct NetdevNicOps netdev_nic_zz9000;

static int failures;

static void expect_str(const char *what, const char *got, const char *want)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0)
    {
        printf("ok   %s = %s\n", what, got);
        return;
    }

    printf("FAIL %s: got %s, want %s\n", what,
           got != NULL ? got : "(null)", want != NULL ? want : "(null)");
    failures++;
}

static void expect_int(const char *what, int got, int want)
{
    if (got == want)
    {
        printf("ok   %s = %d\n", what, got);
        return;
    }

    printf("FAIL %s: got %d, want %d\n", what, got, want);
    failures++;
}

static const char *const names[] = ANXNET_CARD_NAMES;

#define NNAMES ((int)(sizeof(names) / sizeof(names[0])))

int main(void)
{
    int i;

    expect_int("ANXNET_CARD_NAMES entries", NNAMES, (int)netdev_card_count);

    for (i = 0; i < NNAMES && i < (int)netdev_card_count; i++)
    {
        char label[64];

        snprintf(label, sizeof(label), "name[%d]", i);
        expect_str(label, netdev_cards[i].name, names[i]);
    }

    /* Every name in the list resolves, and to its own row. */
    for (i = 0; i < NNAMES; i++)
    {
        char label[64];

        snprintf(label, sizeof(label), "netdev_card_by_name(%s)", names[i]);
        expect_int(label, netdev_card_by_name(names[i]) == &netdev_cards[i], 1);
    }

    /* Case does not matter: an interface file is written by hand. */
    expect_int("netdev_card_by_name(XSURF100)",
               netdev_card_by_name("XSURF100") == netdev_card_by_name("xsurf100"),
               1);
    expect_int("netdev_card_by_name(AriadneII)",
               netdev_card_by_name("Ariadne2") == netdev_card_by_name("ariadne2"),
               1);

    /* A name no row has, a prefix of one, and NULL are all misses. */
    expect_int("netdev_card_by_name(nonsense)",
               netdev_card_by_name("nonsense") == NULL, 1);
    expect_int("netdev_card_by_name(xsurf10)",
               netdev_card_by_name("xsurf10") == NULL, 1);
    expect_int("netdev_card_by_name(xsurf1000)",
               netdev_card_by_name("xsurf1000") == NULL, 1);
    expect_int("netdev_card_by_name(empty)",
               netdev_card_by_name("") == NULL, 1);
    expect_int("netdev_card_by_name(NULL)",
               netdev_card_by_name(NULL) == NULL, 1);

    /* No two rows answer to the same name, and none is empty. */
    for (i = 0; i < (int)netdev_card_count; i++)
    {
        char label[64];

        snprintf(label, sizeof(label), "row %d is the first of its name", i);
        expect_int(label, netdev_card_by_name(netdev_cards[i].name)
                          == &netdev_cards[i], 1);

        snprintf(label, sizeof(label), "row %d has a name", i);
        expect_int(label, netdev_cards[i].name[0] != '\0', 1);

        /* Gayle carries the odd 8-bit PCMCIA registers in a second window at
           +$10000 and the even ones at $A20000, both at even addresses.  A
           PCMCIA row without odd_off puts the NE2000 reset register at
           $A2031F, an odd address in the even window, which is not a register
           access on the hardware.  Amiberry decodes it anyway, so no
           emulated run would catch the row that lost it. */
        if (netdev_cards[i].bus == NETDEV_BUS_PCMCIA)
        {
            snprintf(label, sizeof(label),
                     "row %d (%s) has an odd-register window", i,
                     netdev_cards[i].name);
            expect_int(label, netdev_cards[i].odd_off != 0, 1);

            snprintf(label, sizeof(label),
                     "row %d (%s) is stride 1", i, netdev_cards[i].name);
            expect_int(label, netdev_cards[i].stride == 1, 1);
        }
    }

    /* The autoconfig match.  Every Zorro row answers to its own record and
       to nothing else, and er_Product is a byte: the row's prodid is
       compared as one, so the 3c589's 0x0589 is not 0x89 here. */
    for (i = 0; i < (int)netdev_card_count; i++)
    {
        const NetdevCard *card = &netdev_cards[i];
        char              label[64];

        if (card->bus != NETDEV_BUS_ZORRO)
            continue;

        snprintf(label, sizeof(label), "netdev_card_by_zorro(%s)", card->name);
        expect_int(label, netdev_card_by_zorro(card->manid,
                                               (UBYTE)card->prodid) == card, 1);
    }
    expect_int("netdev_card_by_zorro(xsurf100)",
               netdev_card_by_zorro(4626, 100) == netdev_card_by_name("xsurf100"),
               1);
    expect_int("netdev_card_by_zorro(xsurf)",
               netdev_card_by_zorro(4626, 23) == netdev_card_by_name("xsurf"), 1);
    expect_int("netdev_card_by_zorro(ariadne2)",
               netdev_card_by_zorro(2167, 202) == netdev_card_by_name("ariadne2"),
               1);
    expect_int("netdev_card_by_zorro(ariadne)",
               netdev_card_by_zorro(2167, 201) == netdev_card_by_name("ariadne"),
               1);
    expect_int("netdev_card_by_zorro(a2065)",
               netdev_card_by_zorro(514, 112) == netdev_card_by_name("a2065"), 1);
    /* The same manufacturer with another product, an unknown one, and the
       PCMCIA rows' MANFIDs, which are not autoconfig records. */
    expect_int("netdev_card_by_zorro(4626, 24)",
               netdev_card_by_zorro(4626, 24) == NULL, 1);
    expect_int("netdev_card_by_zorro(1, 1)",
               netdev_card_by_zorro(1, 1) == NULL, 1);
    expect_int("netdev_card_by_zorro(0, 0) is not the pcmcia row",
               netdev_card_by_zorro(0, 0) == NULL, 1);
    expect_int("netdev_card_by_zorro(0x0101, 0x89) is not the 3c589",
               netdev_card_by_zorro(0x0101, 0x89) == NULL, 1);

    if (failures != 0)
    {
        printf("netdev_cards: %d failures\n", failures);
        return 1;
    }

    printf("netdev_cards: all ok\n");

    return 0;
}
