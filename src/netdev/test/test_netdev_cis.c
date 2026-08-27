/*
 * CISTPL_CFTABLE_ENTRY, parsed against real cards' bytes.
 *
 * The vectors are not invented.  The CNet one is the CIS Amiberry replays for
 * its PCMCIA network card, which is itself a dump of a CNet CN40BC -- see the
 * table its gayle.cpp calls ne2000pcmcia[] -- so it is the exact tuple this
 * driver has been reading in every emulator run there has ever been, and the
 * exact card family the backlog says does not attach on real hardware.  The
 * rest exercise the shapes that CIS never takes: a range descriptor naming its
 * own base, a memory-only entry, a 16-bit-only entry, and a truncated one.
 *
 * A parser that only wants the I/O descriptor still has to skip the power and
 * timing descriptors byte for byte to reach it, and getting that wrong reads
 * an interrupt mask as an address.  That is what these cases are for.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "netdev_cis.h"

static int failures;

static void expect_int(const char *what, long got, long want)
{
    if (got == want)
    {
        printf("ok   %s = %ld\n", what, got);
        return;
    }

    printf("FAIL %s: got %ld, want %ld\n", what, got, want);
    failures++;
}

static void expect_hex(const char *what, unsigned long got, unsigned long want)
{
    if (got == want)
    {
        printf("ok   %s = $%04lx\n", what, got);
        return;
    }

    printf("FAIL %s: got $%04lx, want $%04lx\n", what, got, want);
    failures++;
}

/*
 * CNet CN40BC, the whole tuple body less TPL_CODE and TPL_LINK.
 *
 *   e0        TPCE_INDX: interface follows, default entry, index 32
 *   81        TPCE_IF:   wait required, interface 1 = I/O and memory
 *   1d        TPCE_FS:   one power descriptor, timing, I/O, IRQ
 *   3f        TPCE_PD:   Vnom Vmin Vmax Istatic Iavg Ipeak present
 *   55 4d 5d 06 86 46 26   those six, Ipeak two bytes because $86 continues
 *   fc        TPCE_TD:   wait present, ready and reserved absent
 *   24        the wait value
 *   65        TPCE_IO:   5 address lines, 8-bit and 16-bit, no range
 *   30 ff ff  TPCE_IR:   level mode, a mask follows, the mask
 *
 * Seventeen bytes, which is the TPL_LINK the card states.  The absence of a
 * range descriptor is the whole reason a fixed offset of $300 has worked: the
 * card decodes five address lines and leaves placement to the machine.
 */
static const UBYTE cnet_cn40bc[] =
{
    0xe0, 0x81, 0x1d, 0x3f, 0x55, 0x4d, 0x5d, 0x06, 0x86,
    0x46, 0x26, 0xfc, 0x24, 0x65, 0x30, 0xff, 0xff
};

/*
 * The same card with a range descriptor bolted on, which is the shape a card
 * that names its own base takes: TPCE_IO $8a is 10 address lines and a range,
 * then $20 is one window with a two-byte base and no length bytes, then the
 * base $0320 little-endian.  Ten decoded lines mean the card means it.
 */
static const UBYTE fixed_base_320[] =
{
    0xe0, 0x81, 0x1d, 0x3f, 0x55, 0x4d, 0x5d, 0x06, 0x86,
    0x46, 0x26, 0xfc, 0x24, 0xaa, 0x20, 0x20, 0x03, 0x30, 0xff, 0xff
};

/* An entry with no I/O space at all: TPCE_FS $01 is one power descriptor and
   nothing else.  A card whose first entry looks like this is the case the
   walk exists for. */
static const UBYTE memory_only[] =
{
    0x81, 0x00, 0x01, 0x01, 0x55
};

/* 16-bit accesses offered, 8-bit refused: TPCE_IO $45 is bit 6 without bit 5.
   Every register path here is byte-wide, so this entry is the one to take
   only when the card offers no other. */
static const UBYTE wide_only[] =
{
    0x01, 0x08, 0x45
};

/* Both width bits clear is "unspecified", which is 8-bit everywhere in the
   field and must not be read as a refusal. */
static const UBYTE width_unstated[] =
{
    0x01, 0x08, 0x05
};

/* TPCE_FS says an I/O descriptor follows and the tuple ends before it.  This
   is what a body longer than the driver's copy buffer looks like. */
static const UBYTE truncated[] =
{
    0xe0, 0x81, 0x1d, 0x3f, 0x55
};

int main(void)
{
    NetdevCisEntry e;

    /* ---- the CNet CN40BC, byte for byte the emulator's card ---- */
    expect_int("cn40bc parses",
               netdev_cis_cftable(cnet_cn40bc, (UWORD)sizeof(cnet_cn40bc), &e),
               1);
    expect_int("cn40bc index", e.index, 32);
    expect_int("cn40bc is the default entry",
               (e.flags & NETDEV_CIS_DEFAULT) != 0, 1);
    expect_int("cn40bc interface is I/O", e.iface, NETDEV_CIS_IF_IO);
    expect_int("cn40bc address lines", e.io_lines, 5);
    expect_int("cn40bc windows", e.io_nwin, 1);
    expect_hex("cn40bc window base", e.io_base, 0x0000);
    expect_int("cn40bc window length", e.io_len, 32);
    expect_int("cn40bc offers 8-bit", (e.flags & NETDEV_CIS_IO8) != 0, 1);
    expect_int("cn40bc offers 16-bit", (e.flags & NETDEV_CIS_IO16) != 0, 1);
    expect_int("cn40bc offers a level interrupt",
               (e.flags & NETDEV_CIS_IRQ_LEVEL) != 0, 1);
    expect_int("cn40bc is usable", netdev_cis_usable(&e), 1);
    expect_int("cn40bc scores best", netdev_cis_score(&e),
               NETDEV_CIS_SCORE_BEST);
    /* The one that matters: the walk must not move a card that already works
       off the offset it works at. */
    expect_hex("cn40bc keeps the assumed offset",
               netdev_cis_io_off(&e, 0x0300), 0x0300);

    /* ---- a card that names its own base ---- */
    expect_int("fixed-base parses",
               netdev_cis_cftable(fixed_base_320,
                                  (UWORD)sizeof(fixed_base_320), &e), 1);
    expect_int("fixed-base address lines", e.io_lines, 10);
    expect_int("fixed-base windows", e.io_nwin, 1);
    expect_hex("fixed-base window base", e.io_base, 0x0320);
    expect_int("fixed-base window length", e.io_len, 1);
    /* Ten decoded lines and a base of $320: $300 misses it. */
    expect_hex("fixed-base overrides the assumption",
               netdev_cis_io_off(&e, 0x0300), 0x0320);
    /* One byte of window is not a register file. */
    expect_int("fixed-base one-byte window is unusable",
               netdev_cis_usable(&e), 0);

    /* ---- a memory-only entry ---- */
    expect_int("memory-only parses",
               netdev_cis_cftable(memory_only, (UWORD)sizeof(memory_only), &e),
               1);
    expect_int("memory-only has no I/O space",
               (e.flags & NETDEV_CIS_HAS_IO) != 0, 0);
    expect_int("memory-only is unusable", netdev_cis_usable(&e), 0);
    expect_int("memory-only scores nothing", netdev_cis_score(&e),
               NETDEV_CIS_SCORE_NONE);
    expect_hex("memory-only keeps the assumption",
               netdev_cis_io_off(&e, 0x0300), 0x0300);

    /* ---- widths ---- */
    expect_int("wide-only parses",
               netdev_cis_cftable(wide_only, (UWORD)sizeof(wide_only), &e), 1);
    expect_int("wide-only address lines", e.io_lines, 5);
    expect_int("wide-only is usable", netdev_cis_usable(&e), 1);
    expect_int("wide-only scores below best", netdev_cis_score(&e),
               NETDEV_CIS_SCORE_WIDE);

    expect_int("unstated width parses",
               netdev_cis_cftable(width_unstated,
                                  (UWORD)sizeof(width_unstated), &e), 1);
    expect_int("unstated width scores best", netdev_cis_score(&e),
               NETDEV_CIS_SCORE_BEST);

    /* ---- a body that ends early ---- */
    expect_int("truncated is refused",
               netdev_cis_cftable(truncated, (UWORD)sizeof(truncated), &e), 0);
    expect_int("an empty body is refused",
               netdev_cis_cftable(cnet_cn40bc, 0, &e), 0);
    expect_int("a null body is refused", netdev_cis_cftable(NULL, 17, &e), 0);

    /* A refused parse still leaves a zeroed entry, so a caller that ignores
       the return value cannot act on a stale window. */
    expect_int("a refused parse leaves no I/O window", e.io_nwin, 0);

    if (failures != 0)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }

    printf("all cases pass\n");
    return 0;
}
