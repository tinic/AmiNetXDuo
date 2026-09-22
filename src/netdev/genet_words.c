/*
 * anxgenet.device: the register words attach and init compose (genet_words.h).
 *
 * SPDX-License-Identifier: MIT
 */

#include "genet_words.h"
#include "genetreg.h"

/* --------------------------------------------------------------- attach --- */

ULONG genet_rev_major(ULONG rev)
{
    ULONG maj = GENET_SYS_REV_MAJOR(rev);

    if (maj == 0)
        maj = 1;
    else if (maj == 5 || maj == 6)
        maj--;
    return maj;
}

/* --------------------------------------------------------------- filter --- */

BOOL genet_mdf_fits(const NetdevMcast *table, UWORD max)
{
    UWORD n = 2;
    UWORD i;

    for (i = 0; i < max; i++)
        if (table[i].refs != 0)
            n++;
    return (BOOL)(n <= GENET_MAX_MDF_FILTER);
}

ULONG genet_mdf_ctrl_word(UWORD slots)
{
    /* Slot k is enabled by bit (16 - k): the top `slots` bits of 17. */
    return ((1UL << GENET_MAX_MDF_FILTER) - 1UL) &
           ~((1UL << (GENET_MAX_MDF_FILTER - slots)) - 1UL);
}

/* ------------------------------------------------------------------ GIC --- */

ULONG genet_gicd_bank_offset(ULONG irq)
{
    return (irq >> 5) << 2;
}

ULONG genet_gicd_bit(ULONG irq)
{
    return 1UL << (irq & 31UL);
}

BOOL genet_gicd_is_gic400(ULONG iidr)
{
    return (BOOL)((iidr & GICD_IIDR_MASK) == GICD_IIDR_GIC400);
}
