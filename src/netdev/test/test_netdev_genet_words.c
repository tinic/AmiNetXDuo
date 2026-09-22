/*
 * What the GENET core decides at attach and init (genet_words.h), pinned.
 *
 * Each of these was an expression beside the register access that used it:
 * the revision that admits a chip, whether the multicast table fits the
 * seventeen filter slots and the enable mask that follows, and the GIC-400
 * distributor's bank and bit for the line the workaround clears.  The values
 * below are what the MAC and the GIC must be handed.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "genet_words.h"
#include "genetreg.h"

static int failures;
static int checks;

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    checks++;
    if (got == want)
        return;

    printf("FAIL %s: got 0x%lx (%lu), want 0x%lx (%lu)\n",
           what, got, got, want, want);
    failures++;
}

/* --------------------------------------------------------------- attach --- */

static void a_rev(void)
{
    expect_u32("rev: a Pi 4 reads 6, which is 5", genet_rev_major(0x06000000UL), 5);
    expect_u32("rev: 5 reads as 4", genet_rev_major(0x05000000UL), 4);
    expect_u32("rev: 0 is 1", genet_rev_major(0), 1);
    expect_u32("rev: 4 is 4", genet_rev_major(0x04000000UL), 4);
    expect_u32("rev: 7 is 7", genet_rev_major(0x07000000UL), 7);
    expect_u32("rev: only the low four bits of the byte",
               genet_rev_major(0x16000000UL), 5);
    expect_u32("rev: the minor field does not count", genet_rev_major(0x0600ffffUL), 5);
}

/* ---------------------------------------------------------------- filter --- */

static void b_mdf_fits(void)
{
    NetdevMcast table[NETDEV_MCAST_MAX];
    UWORD       i;

    memset(table, 0, sizeof(table));
    expect_u32("fits: no table", genet_mdf_fits(table, 0), TRUE);
    expect_u32("fits: an empty table", genet_mdf_fits(table, NETDEV_MCAST_MAX), TRUE);

    for (i = 0; i < 15; i++)
        table[i].refs = 1;
    expect_u32("fits: fifteen and the two fixed slots are seventeen",
               genet_mdf_fits(table, NETDEV_MCAST_MAX), TRUE);
    table[15].refs = 3;
    expect_u32("fits: sixteen do not", genet_mdf_fits(table, NETDEV_MCAST_MAX), FALSE);
    expect_u32("fits: only the rows within max count",
               genet_mdf_fits(table, 15), TRUE);

    memset(table, 0, sizeof(table));
    for (i = 0; i < NETDEV_MCAST_MAX; i += 2)
        table[i].refs = 7;              /* sixteen, scattered */
    expect_u32("fits: sixteen scattered do not",
               genet_mdf_fits(table, NETDEV_MCAST_MAX), FALSE);
    table[30].refs = 0;
    expect_u32("fits: fifteen scattered do", genet_mdf_fits(table, NETDEV_MCAST_MAX), TRUE);
}

static void c_mdf_ctrl(void)
{
    expect_u32("mdf ctrl: no slots", genet_mdf_ctrl_word(0), 0);
    expect_u32("mdf ctrl: slot 0 is bit 16", genet_mdf_ctrl_word(1), 0x10000UL);
    expect_u32("mdf ctrl: broadcast and us", genet_mdf_ctrl_word(2), 0x18000UL);
    expect_u32("mdf ctrl: three", genet_mdf_ctrl_word(3), 0x1c000UL);
    expect_u32("mdf ctrl: sixteen", genet_mdf_ctrl_word(16), 0x1fffeUL);
    expect_u32("mdf ctrl: all seventeen", genet_mdf_ctrl_word(17), 0x1ffffUL);
    expect_u32("mdf ctrl: the table holds seventeen", GENET_MAX_MDF_FILTER, 17);
}

/* ------------------------------------------------------------------ GIC --- */

static void d_gicd(void)
{
    /* The Pi 4's GENET lines are SPIs 157 and 158, interrupt IDs 189 and 190. */
    expect_u32("gicd bank: 189", genet_gicd_bank_offset(189), 0x14);
    expect_u32("gicd bank: 190", genet_gicd_bank_offset(190), 0x14);
    expect_u32("gicd bank: 0", genet_gicd_bank_offset(0), 0);
    expect_u32("gicd bank: 31", genet_gicd_bank_offset(31), 0);
    expect_u32("gicd bank: 32", genet_gicd_bank_offset(32), 4);
    expect_u32("gicd bank: 64", genet_gicd_bank_offset(64), 8);

    expect_u32("gicd bit: 189", genet_gicd_bit(189), 0x20000000UL);
    expect_u32("gicd bit: 190", genet_gicd_bit(190), 0x40000000UL);
    expect_u32("gicd bit: 0", genet_gicd_bit(0), 1);
    expect_u32("gicd bit: 31", genet_gicd_bit(31), 0x80000000UL);
    expect_u32("gicd bit: 32", genet_gicd_bit(32), 1);

    expect_u32("iidr: a GIC-400", genet_gicd_is_gic400(0x0200043BUL), TRUE);
    expect_u32("iidr: revision bits do not count",
               genet_gicd_is_gic400(0x0200143BUL), TRUE);
    expect_u32("iidr: another implementer", genet_gicd_is_gic400(0x0200043CUL), FALSE);
    expect_u32("iidr: another product", genet_gicd_is_gic400(0x0000043BUL), FALSE);
    expect_u32("iidr: bit 28 is the product", genet_gicd_is_gic400(0x1200043BUL), FALSE);
    expect_u32("iidr: zero", genet_gicd_is_gic400(0), FALSE);

    expect_u32("gicd: ICPENDR", GICD_ICPENDR, 0x280);
    expect_u32("gicd: ICACTIVER", GICD_ICACTIVER, 0x380);
}

int main(void)
{
    a_rev();
    b_mdf_fits();
    c_mdf_ctrl();
    d_gicd();

    if (failures != 0)
    {
        printf("netdev_genet_words: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("netdev_genet_words: %d checks ok\n", checks);

    return 0;
}
