/*
 * The tests for src/tools/iperfwire.c, the iperf 2 wire format and the
 * arithmetic that turns a byte count into a rate.
 *
 * Every number here was taken off iperf 2.2.1 rather than out of its source.
 * The report bytes in test_report_real() are a verbatim capture of what
 * `iperf -s -u` answered a 200-datagram run with, so a field that moves fails
 * this rather than making the tool print somebody else's number.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/tools \
 *      src/tools/test/test_iperfwire.c src/tools/iperfwire.c -o test_iperfwire
 *
 * SPDX-License-Identifier: MIT
 */

#include "iperfwire.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

/* --------------------------------------------------------------- payload --- */

static void test_pattern(void)
{
    unsigned char buf[32];
    unsigned long first;

    printf("payload pattern\n");

    memset(buf, 0xff, sizeof(buf));
    iperf_pattern_fill(buf, sizeof(buf), 0);

    CHECK(buf[0] == '0' && buf[9] == '9' && buf[10] == '0');
    CHECK(buf[31] == '1');

    /* The phase follows the position in the stream, so a receiver reading
       across two sends sees one continuous pattern. */
    iperf_pattern_fill(buf, 4, 7);
    CHECK(buf[0] == '7' && buf[1] == '8' && buf[2] == '9' && buf[3] == '0');

    /*
     * The one that matters on the wire.  An iperf 2 receiver reads the first
     * four bytes of a TCP stream as a client_hdr and switches mode on the top
     * two bits.  "0123" must leave both clear or the far end parses a
     * negotiation that never happens.  A run whose payload began with an
     * arbitrary byte would set one of them about half the time.
     */
    iperf_pattern_fill(buf, 4, 0);
    first = ((unsigned long)buf[0] << 24) | ((unsigned long)buf[1] << 16)
          | ((unsigned long)buf[2] << 8)  | (unsigned long)buf[3];
    CHECK(first == 0x30313233UL);
    CHECK((first & 0x80000000UL) == 0);
    CHECK((first & 0x40000000UL) == 0);
}

/* -------------------------------------------------------------- datagram --- */

static void test_datagram(void)
{
    unsigned char buf[64];
    int           i;
    long          id;

    printf("datagram header\n");

    memset(buf, 0xa5, sizeof(buf));
    iperf_dg_put(buf, 1, 0x6a7a4f03UL, 0x000eb333UL);

    /* Big-endian, byte at a time, because a caller can place this at an odd
       offset and a 68000 traps on a misaligned load. */
    CHECK(buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x00 && buf[3] == 0x01);
    CHECK(buf[4] == 0x6a && buf[7] == 0x03);
    CHECK(buf[8] == 0x00 && buf[11] == 0x33);

    /*
     * The 24 bytes after the header must be zero.  This is the defect in the
     * vendored nx_iperf.c: it writes the 12-byte header and leaves the rest of
     * the packet as whatever the pool buffer held, and an iperf 2.2 server
     * reads a client_hdr there.  With 0x20000000 set in that word the server
     * enters trip-time mode, reports one datagram and drops the rest of the
     * run.  Reproduced against iperf 2.2.1 before this was written.
     */
    for (i = IPERF_DG_LEN; i < IPERF_DG_TOTAL; i++)
        CHECK(buf[i] == 0);

    CHECK(iperf_dg_id(buf) == 1);
    CHECK(iperf_dg_get(buf, IPERF_DG_LEN - 1, &id) != 0);
    CHECK(iperf_dg_get(buf, IPERF_DG_LEN, &id) == 0);
    CHECK(id == 1);

    /* The end-of-test marker is the last id, negated. */
    iperf_dg_put(buf, -359, 0, 0);
    CHECK(iperf_dg_id(buf) < 0);
    CHECK(iperf_dg_id(buf) == -359);

    iperf_dg_put(buf, 0x7fffffffL, 0, 0);
    CHECK(iperf_dg_id(buf) == 0x7fffffffL);
}

/* ---------------------------------------------------------------- report --- */

static void test_report_roundtrip(void)
{
    unsigned char   buf[IPERF_REPORT_LEN];
    IperfWireReport out;
    IperfWireReport in;

    printf("server report\n");

    in.flags       = IPERF_HEADER_VERSION1;
    in.bytes_hi    = 0;
    in.bytes_lo    = 295470UL;
    in.stop_sec    = 0;
    in.stop_usec   = 416644UL;
    in.lost        = 0;
    in.outoforder  = 1;
    in.datagrams   = 200;
    in.jitter_sec  = 0;
    in.jitter_usec = 2;

    iperf_report_put(buf, &in);

    /* The first sixteen bytes are the echoed datagram header, which iperf
       2.2.1 sends as zeros, so the report starts at 16 and not at 12. */
    CHECK(buf[0] == 0 && buf[15] == 0);
    CHECK(buf[16] == 0x80 && buf[17] == 0x00);

    CHECK(iperf_report_get(buf, sizeof(buf), &out) == 0);
    CHECK(out.flags == IPERF_HEADER_VERSION1);
    CHECK(out.bytes_lo == 295470UL);
    CHECK(out.stop_usec == 416644UL);
    CHECK(out.outoforder == 1);
    CHECK(out.datagrams == 200);

    /* Short is refused rather than read past. */
    CHECK(iperf_report_get(buf, 20, &out) == -1);
    CHECK(iperf_report_get(buf, 0, &out) == -1);

    /* So is a datagram that is not a report.  Without the version bit this is
       somebody else's traffic on port 5001, not an answer. */
    buf[16] = 0x00;
    CHECK(iperf_report_get(buf, sizeof(buf), &out) == -1);
}

static void test_report_real(void)
{
    /*
     * Verbatim from iperf 2.2.1's answer to a 200-datagram, 295470-byte run on
     * 2026-08-10.  Truncated after the fields this parses.  The real datagram
     * is 128 bytes and the rest is 2.2's own extensions.
     */
    static const unsigned char wire[56] = {
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0x88,0x00,0x00,0x00,                        /* flags: V1 | SEQNO64  */
        0x00,0x00,0x00,0x00,                        /* bytes, high          */
        0x00,0x04,0x82,0x2e,                        /* bytes, low = 295470  */
        0x00,0x00,0x00,0x00,                        /* stop sec             */
        0x00,0x06,0x5b,0x84,                        /* stop usec = 416644   */
        0x00,0x00,0x00,0x00,                        /* lost                 */
        0x00,0x00,0x00,0x01,                        /* out of order         */
        0x00,0x00,0x00,0xc8,                        /* datagrams = 200      */
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x02    /* jitter               */
    };
    IperfWireReport out;

    printf("a real iperf 2.2.1 report\n");

    CHECK(iperf_report_get(wire, sizeof(wire), &out) == 0);

    /* The flags word carries more than the version bit, and only that bit
       decides whether this is an answer. */
    CHECK((out.flags & IPERF_HEADER_VERSION1) != 0);
    CHECK(out.flags == 0x88000000UL);

    CHECK(out.bytes_hi == 0);
    CHECK(out.bytes_lo == 295470UL);
    CHECK(out.stop_sec == 0);
    CHECK(out.stop_usec == 416644UL);
    CHECK(out.lost == 0);
    CHECK(out.outoforder == 1);
    CHECK(out.datagrams == 200);

    /* And what the tool would print from it: 295470 bytes in 416 ms. */
    CHECK(iperf_bits_per_sec(out.bytes_hi, out.bytes_lo, 416) == 5682115UL);
}

/* ------------------------------------------------------------ arithmetic --- */

static void test_rate(void)
{
    printf("rate arithmetic\n");

    /* A measurement with no duration has no rate. */
    CHECK(iperf_bits_per_sec(0, 1000, 0) == 0);

    CHECK(iperf_bits_per_sec(0, 1000UL, 1000UL) == 8000UL);
    CHECK(iperf_bits_per_sec(0, 1250000UL, 1000UL) == 10000000UL);

    /*
     * The figures that matter here are Amiga ones, and they are what the
     * vendored engine gets wrong.  nx_iperf computes
     * bytes / ticks * rate / 125000 in integer, which divides first:
     *
     *   an A1200 at 400 KB/s   -> reports 3 Mbit/s for 3.2768
     *   an A600 at 40 KB/s     -> reports 0
     *
     * Ours is in bits per second throughout and the formatting is where the
     * unit is chosen.
     */
    CHECK(iperf_bits_per_sec(0, 4000000UL, 10000UL) == 3200000UL);
    CHECK(iperf_bits_per_sec(0, 400000UL, 10000UL) == 320000UL);
    CHECK(iperf_bits_per_sec(0, 40000UL, 10000UL) == 32000UL);

    /* Rounded to nearest, not down. */
    CHECK(iperf_bits_per_sec(0, 1000UL, 3UL) == 2666667UL);

    /*
     * Past 4 GB the count is two words.  8 GB in 10 s is 6.87 Gbit/s, which
     * does not fit in the unsigned long this returns, so it saturates rather
     * than wrapping to 2.58 Gbit/s.  No Amiga reaches it.  A host running the
     * same code over loopback does, and a wrapped figure there would be read
     * as a regression.
     */
    CHECK(iperf_bits_per_sec(2, 0, 10000UL) == 0xffffffffUL);

    /* And the value just under the ceiling is exact, not clamped. */
    CHECK(iperf_bits_per_sec(0, 536870911UL, 1000UL) == 4294967288UL);

    /* 512 MB in 10 s, which fits, and is the shape of a real fast run. */
    CHECK(iperf_bits_per_sec(0, 536870912UL, 10000UL) == 429496730UL);
}

static void test_add64(void)
{
    unsigned long hi = 0;
    unsigned long lo = 0;

    printf("64-bit counting\n");

    iperf_add64(&hi, &lo, 4000000000UL);
    CHECK(hi == 0 && lo == 4000000000UL);

    /* The carry.  A ten-second run on a fast host passes 4 GB, and without
       this the byte count wraps and the rate reads as nearly zero. */
    iperf_add64(&hi, &lo, 400000000UL);
    CHECK(hi == 1);
    CHECK(lo == 4400000000UL - 4294967296UL);

    hi = 0;
    lo = 0xffffffffUL;
    iperf_add64(&hi, &lo, 1);
    CHECK(hi == 1 && lo == 0);
}

/* ----------------------------------------------------------- formatting --- */

static void test_format(void)
{
    char buf[32];

    printf("formatting\n");

    iperf_format_rate(buf, sizeof(buf), 3200000UL);
    CHECK(strcmp(buf, "3.20 Mbit/s") == 0);

    iperf_format_rate(buf, sizeof(buf), 320000UL);
    CHECK(strcmp(buf, "320.00 Kbit/s") == 0);

    iperf_format_rate(buf, sizeof(buf), 999UL);
    CHECK(strcmp(buf, "999.00 bit/s") == 0);

    iperf_format_rate(buf, sizeof(buf), 0UL);
    CHECK(strcmp(buf, "0.00 bit/s") == 0);

    /* The rounding carry.  .995 and up must move the whole part rather than
       print "2.100". */
    iperf_format_rate(buf, sizeof(buf), 1999995UL);
    CHECK(strcmp(buf, "2.00 Mbit/s") == 0);

    iperf_format_bytes(buf, sizeof(buf), 0, 527730UL);
    CHECK(strcmp(buf, "515.36 KBytes") == 0);

    iperf_format_bytes(buf, sizeof(buf), 0, 100UL);
    CHECK(strcmp(buf, "100.00 Bytes") == 0);

    iperf_format_bytes(buf, sizeof(buf), 2, 0);
    CHECK(strcmp(buf, "8192.00 MBytes") == 0);

    /* Never writes past the end, and always terminates. */
    memset(buf, 'x', sizeof(buf));
    iperf_format_rate(buf, 6, 3200000UL);
    CHECK(strlen(buf) < 6);
    CHECK(buf[5] == '\0' || buf[strlen(buf)] == '\0');
}

/* --------------------------------------------------------------- limits --- */

static void test_limits(void)
{
    printf("bounded runs\n");

    /* The ordinary shapes are accepted. */
    CHECK(iperf_limits_check(10, 0, 4096, 5001) == NULL);
    CHECK(iperf_limits_check(0, 1024, 1470, 5001) == NULL);
    CHECK(iperf_limits_check(IPERF_MAX_SECONDS, 0, 64, 1) == NULL);

    /*
     * The gate.  A run with neither a duration nor a size would go until
     * something stopped it, printing progress the whole time, and a caller
     * that redirected that to a file would have an unbounded writer.  A wedged
     * iperf 2.2.1 wrote 72 GB of interval reports into a shell redirect on the
     * test host while this was being written.  Ours cannot be asked to.
     */
    CHECK(iperf_limits_check(0, 0, 4096, 5001) != NULL);
    CHECK(iperf_limits_check(IPERF_MAX_SECONDS + 1, 0, 4096, 5001) != NULL);
    CHECK(iperf_limits_check(0x7fffffffUL, 0, 4096, 5001) != NULL);

    /* And the two ways of saying when to stop are exclusive, so neither is
       quietly ignored in favour of the other. */
    CHECK(iperf_limits_check(10, 1024, 4096, 5001) != NULL);

    /* A byte target is bounded too. */
    CHECK(iperf_limits_check(0, 4UL * 1024UL * 1024UL, 4096, 5001) == NULL);
    CHECK(iperf_limits_check(0, 4UL * 1024UL * 1024UL + 1, 4096, 5001) != NULL);

    /* Ports, and buffers this will not send. */
    CHECK(iperf_limits_check(10, 0, 4096, 0) != NULL);
    CHECK(iperf_limits_check(10, 0, 4096, 65536) != NULL);
    CHECK(iperf_limits_check(10, 0, IPERF_BUF_MIN - 1, 5001) != NULL);
    CHECK(iperf_limits_check(10, 0, IPERF_BUF_MAX + 1, 5001) != NULL);

    /*
     * The output budget follows from the ceiling, at one progress line a
     * second for at most an hour.  A ceiling that moves fails this.
     */
    CHECK(IPERF_MAX_LINES == IPERF_MAX_SECONDS);
    CHECK(IPERF_MAX_SECONDS <= 3600UL);
}

static void test_slice_budget(void)
{
    printf("the bound that does not consult the clock\n");

    /*
     * Everything else here decides it has finished by comparing two clock
     * readings, so a clock that stops advancing is a run that never ends.
     * That is the failure the host-side iperf 2.2.1 had.  A timing call failed
     * and it spun for seventeen hours.  This is the second bound, and it
     * counts slices instead of milliseconds.
     */
    CHECK(iperf_slice_budget(10, 0, 4096) == 100000UL);
    CHECK(iperf_slice_budget(1, 0, 4096) == 20000UL);   /* the floor */

    /* Monotonic in the duration, so a longer run never gets a smaller
       budget. */
    CHECK(iperf_slice_budget(60, 0, 4096) > iperf_slice_budget(10, 0, 4096));
    CHECK(iperf_slice_budget(IPERF_MAX_SECONDS, 0, 4096)
          > iperf_slice_budget(600, 0, 4096));

    /* Generous by a wide margin.  A slice does at most 64 sends or waits
       20 ms, so a real second is a few hundred slices.  This must never fire
       on a slow machine, only on a stopped clock. */
    CHECK(iperf_slice_budget(10, 0, 4096) > 10UL * 1000UL);

    /* A byte target is bounded by sends rather than by seconds, and still
       gets a budget rather than none. */
    CHECK(iperf_slice_budget(0, 64, 4096) >= 20000UL);
    CHECK(iperf_slice_budget(0, 4UL * 1024UL * 1024UL, 4096)
          == 104857700UL);

    /* The largest accepted transfer with the smallest buffer needs more
       slices than ULONG can count. Saturating preserves the fail-safe instead
       of wrapping it into an early abort. */
    CHECK(iperf_slice_budget(0, 4UL * 1024UL * 1024UL, IPERF_BUF_MIN)
          == 0xffffffffUL);

    /* Never zero, whatever it is asked, because zero would fire at once. */
    CHECK(iperf_slice_budget(0, 0, 0) > 0);
    CHECK(iperf_slice_budget(0, 1, 64) > 0);
}

int main(void)
{
    test_limits();
    test_slice_budget();
    test_pattern();
    test_datagram();
    test_report_roundtrip();
    test_report_real();
    test_rate();
    test_add64();
    test_format();

    printf("%d checks, %d failures\n", checks, failures);

    return (failures == 0) ? 0 : 1;
}
