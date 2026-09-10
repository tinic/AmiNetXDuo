/*
 * The SHIPPING LANCE register accessors, which no other test runs.
 *
 * test_netdev_lance.c defines LANCE_CSR_GET/PUT before including lance.c, so
 * everything under that file's `#ifndef LANCE_CSR_GET` -- le_rap(), le_rdp(),
 * le_csr_get(), le_csr_put() and the RAP cache -- is compiled out of it.  The
 * code the device actually runs against an A2065 had no coverage at all.
 * This file includes lance.c WITHOUT the override, against the same kind of
 * byte array standing in for the board window.
 *
 * What it pins down: RAP is write-only, so every CSR access used to set it,
 * and a CSR access is two Zorro transactions where one will do.  In the
 * steady state every access is CSR0 -- the interrupt handler's read,
 * acknowledge and re-read, and lance_tx's demand write -- so the port already
 * holds what the next access wants.  A reset leaves RAP undefined, which is
 * why the reset paths store LE_RAP_UNKNOWN, and that is the half a cache gets
 * wrong when it gets anything wrong.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#include "netdev_nic.h"
#include "lancereg.h"

/* No LANCE_CSR_GET/PUT here: that is the entire point of this file. */
#include "lance.c"

/* lance_tx() links against it; nothing here calls it.  Same stub as
   test_netdev_lance.c keeps, and honest for a later transmit fixture. */
VOID n68k_copy_longs(volatile void *to, const volatile void *from, ULONG longs)
{
    volatile ULONG       *dst = (volatile ULONG *)to;
    const volatile ULONG *src = (const volatile ULONG *)from;

    while (longs-- != 0)
        *dst++ = *src++;
}

#define REG_OFF     0x4000U         /* anywhere clear of the ring memory */
#define RAP_POISON  0xEEEEU

static union
{
    ULONG align;
    UBYTE bytes[REG_OFF + 16];
} board;

static NetdevCard card;
static NetdevNic  nic;
static int        failures;

static volatile UWORD *rap_cell(VOID)
{
    return (volatile UWORD *)(volatile void *)(board.bytes + REG_OFF + 2);
}

static VOID rap_poison(VOID)      { *rap_cell() = RAP_POISON; }
static UWORD rap_read(VOID)       { return *rap_cell(); }

static VOID expect(const char *what, ULONG got, ULONG want)
{
    if (got == want)
    {
        printf("ok   %s = 0x%04lx\n", what, (unsigned long)got);
        return;
    }

    printf("FAIL %s = 0x%04lx, want 0x%04lx\n",
           what, (unsigned long)got, (unsigned long)want);
    failures++;
}

static VOID fixture(VOID)
{
    memset(&board, 0, sizeof(board));
    memset(&card, 0, sizeof(card));
    memset(&nic, 0, sizeof(nic));

    card.reg_off    = REG_OFF;
    card.mem_off    = 0;
    card.mem_size   = REG_OFF;
    card.lance_swap = 0;            /* identity le_swap, so values read back */

    nic.board   = board.bytes;
    nic.card    = &card;
    nic.le_rap  = LE_RAP_UNKNOWN;
}

int main(VOID)
{
    printf("anxnet.device LANCE register port\n");

    /* A cold port selects on the first access, whatever it is asked for. */
    fixture();
    rap_poison();
    le_csr_put(&nic, LE_CSR0, 0x1234);
    expect("cold access selects CSR0", rap_read(), LE_CSR0);

    /* The second access to the same register must NOT touch RAP.  This is
       the whole saving: it is the interrupt handler's second and third CSR0
       access on every frame. */
    rap_poison();
    le_csr_put(&nic, LE_CSR0, 0x5678);
    expect("repeat access leaves RAP alone", rap_read(), RAP_POISON);

    /* A different register still selects. */
    rap_poison();
    (VOID)le_csr_get(&nic, LE_CSR1);
    expect("a different register selects", rap_read(), LE_CSR1);

    /* ...and the data port carried the value, so the cache did not eat the
       access itself. */
    expect("cached select still reaches RDP",
           (ULONG)*(volatile UWORD *)(volatile void *)(board.bytes + REG_OFF),
           0x5678);

    /* THE HALF THAT MATTERS.  A reset leaves RAP undefined, so lance_halt()
       must select again even though the cache says CSR0 is already there.
       Without the invalidation this write is suppressed and the STOP lands
       in whatever register the port happens to hold. */
    le_csr_put(&nic, LE_CSR0, 0);        /* cache now says CSR0 */
    rap_poison();
    lance_halt(&nic);
    expect("halt selects again after invalidating", rap_read(), LE_CSR0);

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASS", failures);

    return failures ? 1 : 0;
}
