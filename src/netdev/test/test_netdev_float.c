/*
 * The float guard: an empty Gayle socket must not read back as a DP8390.
 *
 * The keeper mock is the whole point.  A byte array cannot model it -- an
 * array answers every address from its own storage, which is exactly what an
 * empty socket does not have -- so the accessors are redirected instead.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include <exec/types.h>

/* Redirect before the header: the mock is a bus, not memory. */
#define NETDEV_CR_PUT(p, v)     bus_put((p), (UBYTE)(v))
#define NETDEV_CR_GET(p)        bus_get(p)

static void  bus_put(volatile UBYTE *p, UBYTE v);
static UBYTE bus_get(volatile UBYTE *p);

#include "netdev_float.h"

/* ---------------------------------------------------------------- mocks -- */

enum BusKind
{
    BUS_CHIP,       /* a real DP8390: CR latches, other registers are theirs */
    BUS_CLONE,      /* the same, with the START bit stuck (FA411)           */
    BUS_KEEPER,     /* an empty socket behind Gayle: one latch, echoes      */
    BUS_PULLUP,     /* nothing decoding, lines pulled high                  */
    BUS_DEAD        /* nothing decoding, lines low                          */
};

#define BUS_WINDOW  64

static enum BusKind bus_kind;
static UBYTE        bus_mem[BUS_WINDOW];
static UBYTE        bus_latch;
static UBYTE        bus_base[BUS_WINDOW];   /* addresses only, never read */
static ULONG        bus_writes;

static ULONG bus_index(volatile UBYTE *p)
{
    return (ULONG)((const volatile UBYTE *)p - bus_base);
}

static void bus_put(volatile UBYTE *p, UBYTE v)
{
    ULONG i = bus_index(p);

    bus_writes++;
    bus_latch = v;                  /* every kind: the bus carried this byte */

    if (bus_kind == BUS_CHIP || bus_kind == BUS_CLONE)
    {
        if (i < (ULONG)BUS_WINDOW)
            bus_mem[i] = v;
    }
}

static UBYTE bus_get(volatile UBYTE *p)
{
    ULONG i = bus_index(p);

    switch (bus_kind)
    {
    case BUS_CHIP:
        return i < (ULONG)BUS_WINDOW ? bus_mem[i] : 0xffu;
    case BUS_CLONE:
        return (UBYTE)((i < (ULONG)BUS_WINDOW ? bus_mem[i] : 0xffu) | 0x02u);
    case BUS_KEEPER:
        return bus_latch;           /* the last value driven, whoever drove it */
    case BUS_PULLUP:
        return 0xffu;
    case BUS_DEAD:
    default:
        return 0x00u;
    }
}

static void bus_reset(enum BusKind kind)
{
    ULONG i;

    bus_kind   = kind;
    bus_latch  = 0x00u;
    bus_writes = 0;
    for (i = 0; i < (ULONG)BUS_WINDOW; i++)
        bus_mem[i] = 0x00u;
}

/* The probe as it stood before the guard, so the mock is shown to bite. */
static BOOL old_cr_answers(volatile UBYTE *nic)
{
    UBYTE v;

    bus_put(nic, 0x21u);
    v = bus_get(nic);

    return (BOOL)((v & (UBYTE)~0x02u) == 0x21u);
}

/* ---------------------------------------------------------------- tests -- */

static int failures;

static void expect(const char *what, int got, int want)
{
    if (got == want)
    {
        printf("ok   %s = %d\n", what, got);
        return;
    }

    printf("FAIL %s: got %d, want %d\n", what, got, want);
    failures++;
}

static void expect_byte(const char *what, UBYTE got, UBYTE want)
{
    if (got == want)
    {
        printf("ok   %s = $%02x\n", what, (unsigned)got);
        return;
    }

    printf("FAIL %s: got $%02x, want $%02x\n", what, (unsigned)got,
           (unsigned)want);
    failures++;
}

static BOOL answers(enum BusKind kind, UWORD stride, UBYTE *cr)
{
    bus_reset(kind);

    return netdev_cr_answers(bus_base, stride, cr);
}

int main(void)
{
    UBYTE cr = 0;

    /* THE BUG.  The old probe calls the empty socket a card; the guard does
       not.  Both statements are asserted, so neither half can rot alone. */
    bus_reset(BUS_KEEPER);
    expect("old probe, echoing bus: card seen (the bug)",
           old_cr_answers(bus_base) != 0, 1);

    expect("guarded probe, echoing bus: NO CARD",
           answers(BUS_KEEPER, 1, &cr) != 0, 0);
    expect_byte("and the recorded CR is the decoy, not the probe's own $21",
                cr, (UBYTE)ANXDIAG_CR_DECOY);

    /* A real part still answers, at both strides the two callers pass:
       netdev_cards.c gives the pcmcia row 1 and the xsurf row 2. */
    expect("DP8390, stride 1 (pcmcia): card", answers(BUS_CHIP, 1, &cr) != 0, 1);
    expect_byte("CR reads back stopped", cr, (UBYTE)NETDEV_CR_STOPPED);

    expect("DP8390, stride 2 (xsurf): card", answers(BUS_CHIP, 2, &cr) != 0, 1);
    expect_byte("CR reads back stopped", cr, (UBYTE)NETDEV_CR_STOPPED);

    expect("clone with START stuck: card", answers(BUS_CLONE, 1, &cr) != 0, 1);
    expect_byte("and reads $23", cr, 0x23u);

    expect("floating high: NO CARD", answers(BUS_PULLUP, 1, &cr) != 0, 0);
    expect("nothing decoding: NO CARD", answers(BUS_DEAD, 1, &cr) != 0, 0);

    /* The decoy must land on another register, or a real CR is overwritten
       and the guard would reject the card it was meant to keep. */
    bus_reset(BUS_CHIP);
    (void)netdev_cr_answers(bus_base, 2, &cr);
    expect("decoy went to RBCR0, not CR", (int)bus_writes, 2);
    expect_byte("RBCR0 holds the decoy",
                bus_mem[NETDEV_CR_DECOY_REG * 2], (UBYTE)ANXDIAG_CR_DECOY);
    expect_byte("CR still holds stopped", bus_mem[0], (UBYTE)NETDEV_CR_STOPPED);

    printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL", failures);

    return failures == 0 ? 0 : 1;
}
