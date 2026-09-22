/*
 * The sums under the NETDEV_TIME beam clock and the serial hex trace.
 *
 * The clock is two chip reads and a subtraction, and the subtraction has been
 * wrong twice: once adding 65,536 at the end of every PAL field, which charged
 * 12.7 ms of invented time to whichever span was open, and once telling the
 * vpos carry from the field end by size, which dropped every span over half
 * a field -- the longest ones.  Both were found by reading a report whose
 * rows contradicted each other, months after the instrument was believed.
 * These are the values the repaired arithmetic must give, pinned.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "netdev_trace.h"

static int failures;
static int checks;

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    checks++;
    if (got == want)
        return;

    printf("FAIL %s: got %lu, want %lu\n", what, got, want);
    failures++;
}

static void expect_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want) == 0)
        return;

    printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    failures++;
}

/* ------------------------------------------------------------ the text --- */

static void a_hex8(void)
{
    char buf[12];

    memset(buf, 'x', sizeof(buf));
    netdev_trace_hex8(buf, 0xdeadbeefUL);
    expect_str("hex8 deadbeef", buf, "deadbeef\r\n");
    expect_u32("hex8 writes 11 bytes and no more", (unsigned long)buf[11], 'x');
    expect_u32("hex8 length", (unsigned long)strlen(buf), NETDEV_TRACE_HEX8_LEN - 1);

    netdev_trace_hex8(buf, 0);
    expect_str("hex8 zero is eight digits", buf, "00000000\r\n");

    netdev_trace_hex8(buf, 0xffffffffUL);
    expect_str("hex8 all ones", buf, "ffffffff\r\n");

    netdev_trace_hex8(buf, 0x00000a5cUL);
    expect_str("hex8 leading zeros kept", buf, "00000a5c\r\n");

    netdev_trace_hex8(buf, 0x00a20300UL);
    expect_str("hex8 a PCMCIA base", buf, "00a20300\r\n");
}

/* ----------------------------------------------------------- the clock --- */

static void b_pack(void)
{
    /* VHPOSR carries vpos bits 7..0 in its high byte and hpos in its low;
       VPOSR bit 0 is vpos bit 8. */
    expect_u32("pack line 0 clock 0", netdev_beam_pack(0, 0x0000), 0);
    expect_u32("pack line 0 clock 226", netdev_beam_pack(0, 0x00e2), 226);
    expect_u32("pack line 1 clock 0", netdev_beam_pack(0, 0x0100), 256);
    expect_u32("pack line 255 clock 0", netdev_beam_pack(0, 0xff00), 65280);
    expect_u32("pack line 256 clock 0: the ninth bit",
               netdev_beam_pack(1, 0x0000), 65536);
    expect_u32("pack line 308 clock 18", netdev_beam_pack(1, 0x3412), 78866);
    expect_u32("pack the last PAL line", netdev_beam_pack(1, 0x38e2),
               312UL * 256UL + 226UL);
    /* Only bit 0 of the high word is vpos; the caller masks it, and the
       packer shifts whatever it is given. */
    expect_u32("pack hi=1 is one line of 256", netdev_beam_pack(1, 0) -
               netdev_beam_pack(0, 0), 65536);
}

static void c_since_forward(void)
{
    BOOL w = TRUE;

    expect_u32("same instant", netdev_beam_since(100, 100, 80099, &w), 0);
    expect_u32("same instant is not a wrap", (unsigned long)w, FALSE);

    expect_u32("100 units later", netdev_beam_since(65300, 65400, 80099, &w),
               100);
    expect_u32("forward is not a wrap", (unsigned long)w, FALSE);

    /* vpos 255 -> 256: the low byte carries, the ninth bit does not fall
       back to zero, and this is NOT a discontinuity any more. */
    expect_u32("the vpos carry", netdev_beam_since(255UL * 256UL + 200UL,
                                                   256UL * 256UL + 10UL,
                                                   80099, &w), 66);
    expect_u32("the vpos carry is not a wrap", (unsigned long)w, FALSE);

    /* A 13 ms interrupt span, over half the old range: forward is forward
       whatever its size. */
    expect_u32("a long span", netdev_beam_since(1000, 53924, 80099, &w),
               52924);
    expect_u32("a long span is not a wrap", (unsigned long)w, FALSE);

    /* From zero to the top of the field. */
    expect_u32("the whole field", netdev_beam_since(0, 80099, 80099, &w),
               80099);
}

static void d_since_wrapped(void)
{
    BOOL w = FALSE;

    /* The end of a PAL field: 313 lines learned, vpos 312 hpos 56 to vpos 0
       hpos 3.  The old line returned about 51,200 here. */
    expect_u32("PAL field end", netdev_beam_since(312UL * 256UL + 56UL, 3,
                                                  312UL * 256UL + 227UL, &w),
               175);
    expect_u32("PAL field end is a wrap", (unsigned long)w, TRUE);

    /* NTSC: 262 lines seen, so the learned top is 261*256+227 = 67043 and
       the floor is what applies. */
    expect_u32("NTSC field end", netdev_beam_since(67000, 20, 67043, &w),
               NETDEV_BEAM_FIELD_MIN + 20 - 67000);
    expect_u32("NTSC field end is a wrap", (unsigned long)w, TRUE);

    /* Before the clock has seen a field: floored at NTSC's height. */
    expect_u32("nothing learned yet", netdev_beam_since(65300, 100, 0, &w),
               1872);
    expect_u32("field_top just under the floor",
               netdev_beam_since(65300, 100, NETDEV_BEAM_FIELD_MIN - 2, &w),
               1872);
    expect_u32("field_top one under the floor",
               netdev_beam_since(65300, 100, NETDEV_BEAM_FIELD_MIN - 1, &w),
               1872);
    expect_u32("field_top at the floor",
               netdev_beam_since(65300, 100, NETDEV_BEAM_FIELD_MIN, &w), 1873);

    /* A backwards step of one unit is still the field, and the answer is the
       whole field less one. */
    expect_u32("one unit back", netdev_beam_since(10, 9, 80099, &w), 80099);
    expect_u32("one unit back is a wrap", (unsigned long)w, TRUE);

    /* A span longer than half a field that straddles the end is repaired
       by the field, not dropped: t0 late, t1 well into the next field. */
    expect_u32("a long straddling span",
               netdev_beam_since(70000, 60000, 80099, &w),
               80100 + 60000 - 70000);
    expect_u32("a long straddling span is a wrap", (unsigned long)w, TRUE);
}

static void e_constants(void)
{
    expect_u32("unit numerator", NETDEV_BEAM_UNIT_NUM, 227);
    expect_u32("unit denominator", NETDEV_BEAM_UNIT_DEN, 256);
    expect_u32("field floor is 262 NTSC lines", NETDEV_BEAM_FIELD_MIN, 67072);
    expect_u32("field floor is under a PAL field",
               (unsigned long)(NETDEV_BEAM_FIELD_MIN < 313UL * 256UL), 1);
}

int main(void)
{
    a_hex8();
    b_pack();
    c_since_forward();
    d_since_wrapped();
    e_constants();

    if (failures != 0)
    {
        printf("netdev_trace: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("netdev_trace: %d checks ok\n", checks);

    return 0;
}
