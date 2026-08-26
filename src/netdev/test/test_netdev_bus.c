/*
 * The bus accessors, and in particular cnet16's GETODD, on the host.
 *
 * The rule on a little-endian host is test_netdev_ed.c's: a claim about a word
 * access is stated as a word value, and a claim about a byte access is stated
 * as a byte at a byte address.  Neither is a claim about the host's byte order.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "netdev_bus.h"

static int failures;

static void ok(const char *what, int cond)
{
    if (cond)
    {
        printf("ok   %s\n", what);
        return;
    }

    printf("FAIL %s\n", what);
    failures++;
}

static void expect_u8(const char *what, unsigned got, unsigned want)
{
    if (got == want)
    {
        printf("ok   %s = 0x%02x\n", what, got);
        return;
    }

    printf("FAIL %s: got 0x%02x, want 0x%02x\n", what, got, want);
    failures++;
}

/* ------------------------------------------------------------ the windows -- */

/*
 * Gayle's two PCMCIA I/O windows, at stride 1.  The even one holds registers
 * 0..31 and is declared as words, because that is how GETODD reaches it.  The
 * odd one is only ever addressed a byte at a time, so it is bytes.  In the
 * machine they are 0xA20300 and 0xA30300-1.  Here they are two arrays and the
 * arithmetic is identical.
 */
#define REGS        32

static UWORD even_w[REGS / 2];
static UBYTE odd_b[REGS];

static UBYTE *even_b(void) { return (UBYTE *)even_w; }

static void windows_fill(void)
{
    UWORD i;

    /* One distinguishable value per word of the even window, so a read that
       lands one word early or late is a different answer and not a lucky
       match.  The two halves differ, so keeping the wrong one is visible. */
    for (i = 0; i < REGS / 2; i++)
        even_w[i] = (UWORD)(((0xe0u + i) << 8) | (0x10u + i));

    /* And one per byte of the odd window. */
    for (i = 0; i < REGS; i++)
        odd_b[i] = (UBYTE)(0x40u + i);
}

static void bus_pcmcia(NetdevBus *bus)
{
    windows_fill();
    netdev_bus_setup(bus, even_b(), 1, NULL);
    netdev_bus_split(bus, odd_b);
}

/* ------------------------------------------------------- the split window -- */

/*
 * Without GETODD.  This is the shipped path and it must not move: an odd
 * register is a byte at index reg-1 of the second window, which is where
 * cnet.device puts it (cnet.i:29 -- `odd = $a30300-1`, so nic_isr = 7+odd is
 * the even address $a30306).
 */
static void test_split(void)
{
    NetdevBus bus;

    bus_pcmcia(&bus);

    expect_u8("split: register 0 is byte 0 of the even window",
              netdev_bus_r8(&bus, 0), even_b()[0]);
    expect_u8("split: register 6 is byte 6 of the even window",
              netdev_bus_r8(&bus, 6), even_b()[6]);
    expect_u8("split: register 7 (ISR) is byte 6 of the odd window",
              netdev_bus_r8(&bus, 7), odd_b[6]);
    expect_u8("split: register 3 (BNRY) is byte 2 of the odd window",
              netdev_bus_r8(&bus, 3), odd_b[2]);

    /* ASIC register 15 is whole-file register 31, and odd. */
    expect_u8("split: ASIC 15 (reset) is byte 30 of the odd window",
              netdev_bus_ra8(&bus, 15), odd_b[30]);
    expect_u8("split: ASIC 0 (data port) is byte 16 of the even window",
              netdev_bus_ra8(&bus, 0), even_b()[16]);
}

/* -------------------------------------------------------------- GETODD ---- */

static void test_getodd(void)
{
    NetdevBus bus;

    bus_pcmcia(&bus);
    ok("getodd is off until something turns it on", bus.getodd == 0);
    ok("a stride-1 split bus accepts it", netdev_bus_set_getodd(&bus) != FALSE);
    ok("and the flag is set", bus.getodd != 0);

    /*
     * Register 7 comes from the word at even-window offset 6, which is
     * even_w[3], and it is the low half that is kept.  cnet's GETODD is
     * `move.w reg-1,-(sp) / move.b 1(sp),d`, the second byte of the word,
     * which on a 68k is the byte at the odd address.
     */
    expect_u8("getodd: register 7 is the low half of the word at offset 6",
              netdev_bus_r8(&bus, 7), (UBYTE)even_w[3]);
    expect_u8("getodd: register 3 is the low half of the word at offset 2",
              netdev_bus_r8(&bus, 3), (UBYTE)even_w[1]);
    expect_u8("getodd: register 15 is the low half of the word at offset 14",
              netdev_bus_r8(&bus, 15), (UBYTE)even_w[7]);

    /* The word really is the one at reg-1 and not the one at reg+1, and it is
       the low half that survives the narrowing and not the high one.  Both
       halves of every word differ by construction, so these two are real
       claims and neither depends on the host's byte order. */
    ok("getodd: register 7 is not the word beside it",
       netdev_bus_r8(&bus, 7) != (UBYTE)even_w[4]);
    ok("getodd: register 7 keeps the low half, not the high half",
       netdev_bus_r8(&bus, 7) != (UBYTE)(even_w[3] >> 8));
    ok("getodd: ASIC 15 keeps the low half, not the high half",
       netdev_bus_ra8(&bus, 15) != (UBYTE)(even_w[15] >> 8));

    /* Even registers are untouched: still a byte, still the even window. */
    expect_u8("getodd: register 0 is still a byte read",
              netdev_bus_r8(&bus, 0), even_b()[0]);
    expect_u8("getodd: register 6 is still a byte read",
              netdev_bus_r8(&bus, 6), even_b()[6]);

    /* ASIC 15 is whole-file 31: the word at offset 30, low half. */
    expect_u8("getodd: ASIC 15 is the low half of the word at offset 30",
              netdev_bus_ra8(&bus, 15), (UBYTE)even_w[15]);
    expect_u8("getodd: ASIC 0 is still a byte read",
              netdev_bus_ra8(&bus, 0), even_b()[16]);

    /*
     * Writes do not change.  cnet's trick is read-only, and its source has no
     * write form of GETODD, so an odd register is still written as a byte into
     * the odd window.  bus->odd must still be set for that to have anywhere to
     * go.
     */
    netdev_bus_w8(&bus, 7, 0x5a);
    expect_u8("getodd: a write to register 7 still lands in the odd window",
              odd_b[6], 0x5a);
    ok("getodd: and did not touch the even window", even_w[3] != 0x5a5a);

    netdev_bus_wa8(&bus, 15, 0xa5);
    expect_u8("getodd: a write to ASIC 15 still lands in the odd window",
              odd_b[30], 0xa5);
}

/* ----------------------------------------------------------- the refusals -- */

/*
 * The arithmetic is only valid at stride 1.  At stride 2 the word at
 * (reg-1)*2 spans register reg-1 and the pad byte beside it, so a bus that
 * turned this on would read every odd register, silently, as padding.
 */
static void test_refused(void)
{
    static const ULONG map[32] = { 0 };
    NetdevBus bus;

    windows_fill();

    netdev_bus_setup(&bus, even_b(), 2, NULL);
    netdev_bus_split(&bus, odd_b);
    ok("stride 2 refuses getodd", netdev_bus_set_getodd(&bus) == FALSE);
    ok("stride 2 leaves the flag clear", bus.getodd == 0);

    netdev_bus_setup(&bus, even_b(), 4, NULL);
    netdev_bus_split(&bus, odd_b);
    ok("stride 4 refuses getodd", netdev_bus_set_getodd(&bus) == FALSE);

    /* A contiguous register file has no second window, and a card whose odd
       registers are plain bytes beside the even ones is not the case this
       exists for. */
    netdev_bus_setup(&bus, even_b(), 1, NULL);
    ok("a bus with no odd window refuses getodd",
       netdev_bus_set_getodd(&bus) == FALSE);

    /* A scatter table has no arithmetic relation between neighbours. */
    netdev_bus_setup(&bus, even_b(), 1, NULL);
    netdev_bus_split(&bus, odd_b);
    netdev_bus_regmap(&bus, map, even_b());
    ok("a regmap bus refuses getodd", netdev_bus_set_getodd(&bus) == FALSE);
}

int main(void)
{
    test_split();
    test_getodd();
    test_refused();

    printf("%s\n", failures == 0 ? "PASS" : "FAIL");

    return failures == 0 ? 0 : 1;
}
