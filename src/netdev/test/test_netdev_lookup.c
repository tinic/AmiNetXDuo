/*
 * Which unit an OpenDevice() names.
 *
 * A unit number is a position in the probe order below ANXNET_UNIT_PIN and a
 * card pin, (index + 1) * 100 + instance, at or above it; CARD= in the
 * interface file is the same pin by name.  The table's order is published,
 * so a pin that lands on the wrong row is a user's interface file opening
 * somebody else's card, and the one arithmetic hazard in it -- a unit number
 * whose card index only fits in a longword being truncated onto a valid row
 * -- is exactly the kind an emulated run with one card never exercises.
 *
 * The roster is built here: three probed units, two of them the same card,
 * so the instance count has something to count.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "netdev_internal.h"

/* netdev_cards.c carries netdev_nic_ops_for(), which names every chip core.
   This test is about the roster, not the cores. */
const struct NetdevNicOps netdev_nic_ne2000;
const struct NetdevNicOps netdev_nic_ed;
const struct NetdevNicOps netdev_nic_lance;
const struct NetdevNicOps netdev_nic_el3;
const struct NetdevNicOps netdev_nic_genet;
const struct NetdevNicOps netdev_nic_zz9000;

static int failures;
static int checks;

static void expect(int ok, const char *what)
{
    checks++;
    if (ok)
        return;

    printf("FAIL %s\n", what);
    failures++;
}

static void expect_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (got != NULL && strcmp(got, want) == 0)
        return;

    printf("FAIL %s: got \"%s\", want \"%s\"\n", what,
           got != NULL ? got : "(null)", want);
    failures++;
}

/* --------------------------------------------------------------- roster --- */

static NetdevDevice dev;

static const NetdevCard *row(const char *name)
{
    const NetdevCard *c = netdev_card_by_name(name);

    if (c == NULL)
    {
        printf("FAIL no row named %s\n", name);
        failures++;
    }
    return c;
}

/* Index of a row in the table, which is what the pin's hundreds encode. */
static ULONG pin_of(const NetdevCard *c, ULONG instance)
{
    return ((ULONG)(c - netdev_cards) + 1UL) * ANXNET_UNIT_PIN + instance;
}

/* Unit 0 an A2065, unit 1 an X-Surf 100, unit 2 a second A2065. */
static void roster_three(void)
{
    memset(&dev, 0, sizeof(dev));
    dev.nd_Units[0].nu_Nic.card = row("a2065");
    dev.nd_Units[1].nu_Nic.card = row("xsurf100");
    dev.nd_Units[2].nu_Nic.card = row("a2065");
    dev.nd_UnitCount = 3;
}

/* ------------------------------------------------------ netdev_find_unit --- */

static void a_by_position(void)
{
    const char *why = "untouched";

    roster_three();

    expect(netdev_find_unit(&dev, 0, NULL, &why) == &dev.nd_Units[0],
           "unit 0 is the first probed board");
    expect(netdev_find_unit(&dev, 1, NULL, &why) == &dev.nd_Units[1],
           "unit 1 is the second");
    expect(netdev_find_unit(&dev, 2, NULL, &why) == &dev.nd_Units[2],
           "unit 2 is the third");
    expect_str("a found unit leaves why alone", why, "untouched");

    expect(netdev_find_unit(&dev, 3, NULL, &why) == NULL,
           "unit 3 of three is nothing");
    expect_str("unit 3 of three: why", why, "no such board");

    why = "untouched";
    expect(netdev_find_unit(&dev, ANXNET_UNIT_PIN - 1, NULL, &why) == NULL,
           "unit 99 is a position, not a pin");
    expect_str("unit 99: why", why, "no such board");

    dev.nd_UnitCount = 0;
    expect(netdev_find_unit(&dev, 0, NULL, &why) == NULL,
           "unit 0 of none is nothing");
    expect_str("unit 0 of none: why", why, "no such board");
}

static void b_by_pin_number(void)
{
    const NetdevCard *a2065    = row("a2065");
    const NetdevCard *xsurf100 = row("xsurf100");
    const NetdevCard *xsurf    = row("xsurf");
    const char       *why      = "untouched";

    roster_three();

    expect(netdev_find_unit(&dev, pin_of(a2065, 0), NULL, &why)
           == &dev.nd_Units[0], "pin a2065 instance 0 is unit 0");
    expect(netdev_find_unit(&dev, pin_of(a2065, 1), NULL, &why)
           == &dev.nd_Units[2], "pin a2065 instance 1 skips the X-Surf");
    expect(netdev_find_unit(&dev, pin_of(xsurf100, 0), NULL, &why)
           == &dev.nd_Units[1], "pin xsurf100 instance 0 is unit 1");
    expect_str("a found pin leaves why alone", why, "untouched");

    expect(netdev_find_unit(&dev, pin_of(a2065, 2), NULL, &why) == NULL,
           "pin a2065 instance 2: only two fitted");
    expect_str("instance past the fitted ones: why", why,
               "the pinned card is not in this machine");

    why = "untouched";
    expect(netdev_find_unit(&dev, pin_of(xsurf100, 1), NULL, &why) == NULL,
           "pin xsurf100 instance 1: only one fitted");
    expect_str("second instance of a single card: why", why,
               "the pinned card is not in this machine");

    why = "untouched";
    expect(netdev_find_unit(&dev, pin_of(xsurf, 0), NULL, &why) == NULL,
           "pin xsurf: a known card that is not fitted");
    expect_str("known card not fitted: why", why,
               "the pinned card is not in this machine");

    /* One past the table. */
    why = "untouched";
    expect(netdev_find_unit(&dev, ((ULONG)netdev_card_count + 1UL)
                                  * ANXNET_UNIT_PIN, NULL, &why) == NULL,
           "pin one past the table");
    expect_str("pin past the table: why", why, "no such card type");

    /* The last row IS in the table, whether fitted or not. */
    why = "untouched";
    expect(netdev_find_unit(&dev, (ULONG)netdev_card_count
                                  * ANXNET_UNIT_PIN, NULL, &why) == NULL,
           "pin of the last row, not fitted");
    expect_str("last row not fitted: why", why,
               "the pinned card is not in this machine");

    /* Index 65536: as a UWORD that is row 0, which is fitted.  As the ULONG
       it is, it is past the table. */
    why = "untouched";
    expect(netdev_find_unit(&dev, 65537UL * ANXNET_UNIT_PIN, NULL, &why)
           == NULL, "a pin whose index only fits in a longword");
    expect_str("longword index: why", why, "no such card type");

    /* Index 65536 + 5: as a UWORD that is the fitted A2065. */
    why = "untouched";
    expect(netdev_find_unit(&dev, (65536UL + 6UL) * ANXNET_UNIT_PIN, NULL,
                            &why) == NULL,
           "a pin that truncates onto a fitted row");
    expect_str("truncating pin: why", why, "no such card type");

    expect(netdev_find_unit(&dev, 0xffffffffUL, NULL, &why) == NULL,
           "unit -1");
    expect_str("unit -1: why", why, "no such card type");
}

static void c_by_pin_name(void)
{
    const char *why = "untouched";

    roster_three();

    expect(netdev_find_unit(&dev, 0, "a2065", &why) == &dev.nd_Units[0],
           "CARD=a2065 UNIT=0 is unit 0");
    expect(netdev_find_unit(&dev, 1, "a2065", &why) == &dev.nd_Units[2],
           "CARD=a2065 UNIT=1 is the second A2065, unit 2");
    expect(netdev_find_unit(&dev, 0, "A2065", &why) == &dev.nd_Units[0],
           "the name is matched without case");
    expect(netdev_find_unit(&dev, 0, "xsurf100", &why) == &dev.nd_Units[1],
           "CARD=xsurf100 UNIT=0 is unit 1");

    /* A pin number beside a name: the name wins, the number gives the
       instance, and its hundreds are ignored. */
    expect(netdev_find_unit(&dev, 101, "a2065", &why) == &dev.nd_Units[2],
           "CARD=a2065 UNIT=101 is instance 1");
    expect(netdev_find_unit(&dev, 100, "a2065", &why) == &dev.nd_Units[0],
           "CARD=a2065 UNIT=100 is instance 0");
    expect(netdev_find_unit(&dev, 500, "xsurf100", &why) == &dev.nd_Units[1],
           "CARD=xsurf100 UNIT=500 is instance 0 of the name, not row 4");
    expect_str("a found name leaves why alone", why, "untouched");

    expect(netdev_find_unit(&dev, 2, "a2065", &why) == NULL,
           "CARD=a2065 UNIT=2: only two fitted");
    expect_str("name instance past the fitted ones: why", why,
               "the pinned card is not in this machine");

    why = "untouched";
    expect(netdev_find_unit(&dev, 0, "hydra", &why) == NULL,
           "CARD=hydra: known, not fitted");
    expect_str("name not fitted: why", why,
               "the pinned card is not in this machine");

    why = "untouched";
    expect(netdev_find_unit(&dev, 0, "nonsense", &why) == NULL,
           "CARD=nonsense");
    expect_str("unknown name: why", why, "no such card type");

    why = "untouched";
    expect(netdev_find_unit(&dev, 0, "", &why) == NULL, "CARD= empty");
    expect_str("empty name: why", why, "no such card type");
}

/* ----------------------------------------------- netdev_request_is_pcmcia --- */

static void d_pcmcia_by_name(void)
{
    const NetdevCard *wanted = (const NetdevCard *)1;

    roster_three();

    expect(netdev_request_is_pcmcia(&dev, 0, "pcmcia", &wanted) == TRUE,
           "CARD=pcmcia is a slot request");
    expect(wanted == row("pcmcia"), "CARD=pcmcia wants the pcmcia row");

    expect(netdev_request_is_pcmcia(&dev, 0, "3c589", &wanted) == TRUE,
           "CARD=3c589 is a slot request");
    expect(wanted == row("3c589"), "CARD=3c589 wants the 3c589 row");

    expect(netdev_request_is_pcmcia(&dev, 0, "3CCFEM556", &wanted) == TRUE,
           "CARD=3CCFEM556 is a slot request, whatever the case");
    expect(wanted == row("3ccfem556"), "and wants its own row");

    wanted = (const NetdevCard *)1;
    expect(netdev_request_is_pcmcia(&dev, 0, "a2065", &wanted) == FALSE,
           "CARD=a2065 is not a slot request");
    expect(wanted == NULL, "a Zorro name wants nothing");

    wanted = (const NetdevCard *)1;
    expect(netdev_request_is_pcmcia(&dev, 0, "xsurf500", &wanted) == FALSE,
           "CARD=xsurf500 is a fixed row, not the slot");
    expect(wanted == NULL, "a fixed name wants nothing");

    wanted = (const NetdevCard *)1;
    expect(netdev_request_is_pcmcia(&dev, 0, "nonsense", &wanted) == FALSE,
           "CARD=nonsense is not a slot request");
    expect(wanted == NULL, "an unknown name wants nothing");

    /* The unit number is not consulted when a name is given. */
    expect(netdev_request_is_pcmcia(&dev, 3, "a2065", &wanted) == FALSE,
           "CARD=a2065 UNIT=count is still not the slot");
    expect(netdev_request_is_pcmcia(&dev, 7, "pcmcia", &wanted) == TRUE,
           "CARD=pcmcia UNIT=7 is still the slot");
}

static void e_pcmcia_by_pin_number(void)
{
    const NetdevCard *wanted = (const NetdevCard *)1;

    roster_three();

    expect(netdev_request_is_pcmcia(&dev, pin_of(row("pcmcia"), 0), NULL,
                                    &wanted) == TRUE,
           "the pcmcia row's pin is a slot request");
    expect(wanted == row("pcmcia"), "and wants that row");

    expect(netdev_request_is_pcmcia(&dev, pin_of(row("3c589"), 1), NULL,
                                    &wanted) == TRUE,
           "the 3c589 row's pin, any instance, is a slot request");
    expect(wanted == row("3c589"), "and wants that row");

    wanted = (const NetdevCard *)1;
    expect(netdev_request_is_pcmcia(&dev, pin_of(row("a2065"), 0), NULL,
                                    &wanted) == FALSE,
           "a Zorro row's pin is not");
    expect(wanted == NULL, "and wants nothing");

    wanted = (const NetdevCard *)1;
    expect(netdev_request_is_pcmcia(&dev, pin_of(row("genet"), 0), NULL,
                                    &wanted) == FALSE,
           "the device-tree row's pin is not");
    expect(wanted == NULL, "and wants nothing");

    wanted = (const NetdevCard *)1;
    expect(netdev_request_is_pcmcia(&dev, ((ULONG)netdev_card_count + 1UL)
                                          * ANXNET_UNIT_PIN, NULL, &wanted)
           == FALSE, "a pin past the table is not");
    expect(wanted == NULL, "and wants nothing");

    wanted = (const NetdevCard *)1;
    expect(netdev_request_is_pcmcia(&dev, (65536UL + 8UL) * ANXNET_UNIT_PIN,
                                    NULL, &wanted) == FALSE,
           "a pin that truncates onto the pcmcia row is not");
    expect(wanted == NULL, "and wants nothing");
}

static void f_pcmcia_by_position(void)
{
    const NetdevCard *wanted = (const NetdevCard *)1;

    roster_three();

    /* The slot is probed last, so a card inserted after boot would be the
       next unit: the one AT the count, and only that one. */
    expect(netdev_request_is_pcmcia(&dev, 3, NULL, &wanted) == TRUE,
           "unit == count could be the slot");
    expect(wanted == NULL, "by position wants no particular row");

    expect(netdev_request_is_pcmcia(&dev, 2, NULL, &wanted) == FALSE,
           "a fitted unit is not the slot retry");
    expect(netdev_request_is_pcmcia(&dev, 0, NULL, &wanted) == FALSE,
           "unit 0 of three is not");
    expect(netdev_request_is_pcmcia(&dev, 4, NULL, &wanted) == FALSE,
           "unit count + 1 is not");
    expect(netdev_request_is_pcmcia(&dev, ANXNET_UNIT_PIN - 1, NULL, &wanted)
           == FALSE, "unit 99 is not");

    dev.nd_UnitCount = 0;
    expect(netdev_request_is_pcmcia(&dev, 0, NULL, &wanted) == TRUE,
           "with nothing probed, unit 0 could be the slot");
    expect(netdev_request_is_pcmcia(&dev, 1, NULL, &wanted) == FALSE,
           "but unit 1 could not");
}

int main(void)
{
    a_by_position();
    b_by_pin_number();
    c_by_pin_name();
    d_pcmcia_by_name();
    e_pcmcia_by_pin_number();
    f_pcmcia_by_position();

    if (failures != 0)
    {
        printf("netdev_lookup: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("netdev_lookup: %d checks ok\n", checks);

    return 0;
}
