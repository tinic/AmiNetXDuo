/*
 * The card table and the name list a config file is checked against.
 *
 * Two copies of the same fact exist and have to: netdev_cards.c carries the
 * rows, and include/aminetxduo/anxnet.h carries the names alone, because
 * src/config/config_parse.c rejects CARD=nonsense without linking the driver.
 * Drift between them is silent in both directions -- a name added to the table
 * and not the list is a card no interface file can ask for, a name in the list
 * and not the table is a CARD= the parser accepts and the driver refuses -- so
 * it is asserted here.
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

/* netdev_cards.c carries netdev_nic_ops_for(), which names the two chip cores.
   This test is about the card table, not the cores, so they are stood in for
   rather than linked -- ed.c and ne2000.c reach the hardware. */
#include "netdev_nic.h"
const struct NetdevNicOps netdev_nic_ne2000;
const struct NetdevNicOps netdev_nic_ed;
const struct NetdevNicOps netdev_nic_lance;
const struct NetdevNicOps netdev_nic_el3;

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
           access on the hardware -- and Amiberry decodes it anyway, so no
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

    if (failures != 0)
    {
        printf("netdev_cards: %d failures\n", failures);
        return 1;
    }

    printf("netdev_cards: all ok\n");

    return 0;
}
