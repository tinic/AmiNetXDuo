/*
 * src/bsdsocket/inet.c on the host.
 *
 * WHY THESE AND NOT A ROUND TRIP
 *
 *   inet_addr() and its relatives are the oldest part of the socket API and
 *   the part with the most disagreement between implementations, because
 *   4.2BSD accepted forms nobody would design today and every program written
 *   since relies on some of them.  "127.1" is a legal address.  "010.0.0.1" is
 *   octal on 4.4BSD and decimal on some others.  A round-trip test agrees with
 *   whatever the code does; these are written from the BSD manual page and the
 *   RFC instead, so they disagree when the code is wrong.
 *
 *   inet_addr() returns INADDR_NONE for a refusal, which is also the value of
 *   255.255.255.255, and that ambiguity is the reason inet_aton() exists.
 *   Both are checked here, because a caller that uses the first cannot tell
 *   the broadcast address from a typo and a caller that uses the second can.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <stdio.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

/*
 * A zeroed library base.  Inet_NtoA() has nowhere else to put its answer:
 * it formats into sb_NtoABuf and returns a pointer into the base, which is
 * what makes the published function safe to call from two tasks at once and
 * what makes a NULL base a crash rather than a wrong answer.
 */
static struct AmiSocketBase h_base;
#define BASE (&h_base)

/*
 * What inet.c reaches outside itself.  Stubbed rather than linked: bringing in
 * errno.c drags the whole vector table behind it. The IP text formatters and
 * parsers in src/config are linked for real. The last errno set
 * is kept because a refusal that does not say why is half a refusal.
 */
static LONG h_last_errno;

VOID bsd_set_errno(struct AmiSocketBase *base, LONG err)
{
    (VOID)base;
    h_last_errno = err;
}

VOID bsd_bcopy(CONST_APTR src, APTR dst, ULONG size)
{
    memmove(dst, src, (size_t)size);
}

VOID bsd_words_to_in6(const ULONG words[4], UBYTE bytes[16])
{
    ULONG i;

    for (i = 0; i < 16; i++)
        bytes[i] = (UBYTE)(words[i / 4] >> (24 - (i % 4) * 8));
}

VOID bsd_in6_to_words(const UBYTE bytes[16], ULONG words[4])
{
    ULONG i;

    for (i = 0; i < 16; i++)
    {
        if (i % 4 == 0)
            words[i / 4] = 0;
        words[i / 4] = (words[i / 4] << 8) | bytes[i];
    }
}

static ULONG addr_of(const char *text)
{
    return (ULONG)bsd_inet_addr((STRPTR)text, BASE);
}

/* ------------------------------------------------------------ inet_addr -- */

static void test_dotted_quad(void)
{
    printf("the four-part form\n");

    CHECK(addr_of("0.0.0.0")         == 0x00000000UL, "0.0.0.0");
    CHECK(addr_of("1.2.3.4")         == 0x01020304UL, "1.2.3.4");
    CHECK(addr_of("192.168.1.1")     == 0xC0A80101UL, "192.168.1.1");
    CHECK(addr_of("255.255.255.255") == 0xFFFFFFFFUL, "255.255.255.255");

    /* Each part is a byte, so 256 is not one. */
    CHECK(addr_of("256.0.0.1")  == INADDR_NONE, "256 in the first part");
    CHECK(addr_of("1.2.3.256")  == INADDR_NONE, "256 in the last part");
    CHECK(addr_of("1.2.3.4.5")  == INADDR_NONE, "five parts");
    CHECK(addr_of("")           == INADDR_NONE, "the empty string");
    CHECK(addr_of("1.2.3.")     == INADDR_NONE, "a trailing dot");
    CHECK(addr_of("...")        == INADDR_NONE, "dots and nothing else");
    CHECK(addr_of("1.2.3.x")    == INADDR_NONE, "a letter where a number goes");
}

/*
 * The short forms, which are not a curiosity: "ping 127.1" is in every book
 * about Unix from the 1980s and programs still pass what a user typed
 * straight to inet_addr().
 */
static void test_short_forms(void)
{
    printf("the one, two and three part forms\n");

    /* a.b.c.d is four bytes; a.b.c makes the last part sixteen bits;
       a.b makes it twenty-four; a alone is the whole address. */
    CHECK(addr_of("127.1")     == 0x7F000001UL, "127.1 is 127.0.0.1");
    CHECK(addr_of("10.1")      == 0x0A000001UL, "10.1 is 10.0.0.1");
    CHECK(addr_of("192.168.1") == 0xC0A80001UL, "192.168.1 is 192.168.0.1");
    CHECK(addr_of("16777217")  == 0x01000001UL, "one part is the address");

    /* The widened part still has a ceiling: in a.b it holds 24 bits. */
    CHECK(addr_of("10.16777216") == INADDR_NONE, "24 bits and one over");
    CHECK(addr_of("1.2.65536")   == INADDR_NONE, "16 bits and one over");

    /* Accumulation itself has to be bounded.  Checking only the completed
       part lets these wrap through zero before the part-width check sees them. */
    CHECK(addr_of("4294967296") == INADDR_NONE, "decimal ULONG overflow");
    CHECK(addr_of("0x100000000") == INADDR_NONE, "hexadecimal ULONG overflow");
    CHECK(addr_of("040000000000") == INADDR_NONE, "octal ULONG overflow");
}

/*
 * Radix.  4.4BSD's inet_addr takes a leading 0 as octal and 0x as hex, which
 * matters because "010.1.1.1" is a different machine from "10.1.1.1" and a
 * configuration file written by hand can contain either.
 */
static void test_radix(void)
{
    printf("octal and hexadecimal parts\n");

    CHECK(addr_of("0x7f.0.0.1") == 0x7F000001UL, "0x7f is 127");
    CHECK(addr_of("0177.0.0.1") == 0x7F000001UL, "0177 is 127");
    CHECK(addr_of("0.0.0.0x10") == 0x00000010UL, "0x10 is 16");

    /* 8 and 9 are not octal digits. */
    CHECK(addr_of("08.0.0.1") == INADDR_NONE, "08 is not a number");
    CHECK(addr_of("0x.0.0.1") == INADDR_NONE, "0x with no digits");
}

static void test_component_boundaries(void)
{
    static const struct { const char *text; BOOL valid; ULONG value; } cases[] = {
        { "4294967295", TRUE, 0xFFFFFFFFUL },
        { "255.16777215", TRUE, 0xFFFFFFFFUL },
        { "255.255.65535", TRUE, 0xFFFFFFFFUL },
        { "255.255.255.255", TRUE, 0xFFFFFFFFUL },
        { "1.16777215", TRUE, 0x01FFFFFFUL },
        { "1.2.65535", TRUE, 0x0102FFFFUL },
        { "1.2.3.255", TRUE, 0x010203FFUL },
        { "0.0", TRUE, 0 }, { "0.0.0", TRUE, 0 },
        { "0377.0xffffff", TRUE, 0xFFFFFFFFUL },
        { "0xff.0377.0xffff", TRUE, 0xFFFFFFFFUL },
        { "256.1", FALSE, 0 }, { "256.1.1", FALSE, 0 },
        { "1.256.1", FALSE, 0 }, { "256.1.1.1", FALSE, 0 },
        { "1.256.1.1", FALSE, 0 }, { "1.1.256.1", FALSE, 0 },
        { "1.1.1.256", FALSE, 0 }, { "1.16777216", FALSE, 0 },
        { "1.1.65536", FALSE, 0 }, { "4294967296", FALSE, 0 },
        { "1.2.3.4.5", FALSE, 0 }, { "1.2.3.", FALSE, 0 }
    };
    unsigned i;

    printf("classic component widths and refused output preservation\n");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        struct in_addr out;
        LONG rc;

        out.s_addr = 0x12345678UL;
        rc = bsd_inet_aton((STRPTR)cases[i].text, &out, BASE);
        CHECK((rc != 0) == cases[i].valid, cases[i].text);
        CHECK(out.s_addr == (cases[i].valid ? cases[i].value : 0x12345678UL),
              "aton returns the specified classic value or keeps output untouched");
        CHECK(addr_of(cases[i].text) == (cases[i].valid ? cases[i].value : INADDR_NONE),
              "inet_addr agrees with classic component widths");
    }
}

/* ------------------------------------------------------------ inet_aton -- */

static void test_aton_reports_broadcast(void)
{
    printf("inet_aton tells the broadcast address from a refusal\n");

    struct in_addr a;

    a.s_addr = 0;
    CHECK(bsd_inet_aton((STRPTR)"255.255.255.255", &a, BASE) != 0,
          "255.255.255.255 is accepted");
    CHECK(a.s_addr == 0xFFFFFFFFUL, "and is the all-ones address");

    /* The same value out of inet_addr() means "no", which is the whole
       reason this function exists. */
    CHECK(addr_of("255.255.255.255") == INADDR_NONE,
          "inet_addr cannot distinguish it");

    a.s_addr = 0x12345678UL;
    CHECK(bsd_inet_aton((STRPTR)"not an address", &a, BASE) == 0,
          "a refusal is reported");
    CHECK(a.s_addr == 0x12345678UL,
          "and the caller's buffer is left alone");
}

/* --------------------------------------------------------- inet_network -- */

static void test_network_parts(void)
{
    printf("inet_network packs one byte per component\n");

    CHECK(bsd_inet_network((STRPTR)"192.168.1", BASE) == 0x00C0A801UL,
          "three components are packed");
    CHECK(bsd_inet_network((STRPTR)"256", BASE) == INADDR_NONE,
          "an oversized single component is refused");
    CHECK(bsd_inet_network((STRPTR)"10.511", BASE) == INADDR_NONE,
          "an oversized trailing component is refused");
}

/* ------------------------------------------------------------ Inet_NtoA -- */

static void test_ntoa(void)
{
    printf("Inet_NtoA\n");

    CHECK(strcmp((const char *)bsd_Inet_NtoA(0x00000000UL, BASE),
                 "0.0.0.0") == 0, "0.0.0.0");
    CHECK(strcmp((const char *)bsd_Inet_NtoA(0xC0A80101UL, BASE),
                 "192.168.1.1") == 0, "192.168.1.1");
    CHECK(strcmp((const char *)bsd_Inet_NtoA(0xFFFFFFFFUL, BASE),
                 "255.255.255.255") == 0, "255.255.255.255");
    CHECK(strcmp((const char *)bsd_Inet_NtoA(0x7F000001UL, BASE),
                 "127.0.0.1") == 0, "127.0.0.1");
}

/* ------------------------------------------------- the classful helpers -- */

/*
 * NetOf, LnaOf and MakeAddr predate CIDR and answer by the class of the
 * address, which is what their callers still expect: they are published in the
 * NDK and a program that uses them is not asking about a netmask.
 */
static void test_classful(void)
{
    printf("NetOf, LnaOf and MakeAddr\n");

    /* Class A: 1 byte of network. */
    CHECK(bsd_Inet_NetOf(0x0A010203UL, BASE) == 0x0AUL, "class A network");
    CHECK(bsd_Inet_LnaOf(0x0A010203UL, BASE) == 0x010203UL, "class A local");

    /* Class B: 2 bytes. */
    CHECK(bsd_Inet_NetOf(0x81020304UL, BASE) == 0x8102UL, "class B network");
    CHECK(bsd_Inet_LnaOf(0x81020304UL, BASE) == 0x0304UL, "class B local");

    /* Class C: 3 bytes. */
    CHECK(bsd_Inet_NetOf(0xC0A80105UL, BASE) == 0xC0A801UL, "class C network");
    CHECK(bsd_Inet_LnaOf(0xC0A80105UL, BASE) == 0x05UL, "class C local");

    /* And back again. */
    CHECK(bsd_Inet_MakeAddr(0xC0A801UL, 0x05UL, BASE) == 0xC0A80105UL,
          "class C round trip");
    CHECK(bsd_Inet_MakeAddr(0x0AUL, 0x010203UL, BASE) == 0x0A010203UL,
          "class A round trip");
}

/* ------------------------------------------------------------- ntop/pton -- */

static void test_pton_v4(void)
{
    printf("inet_pton, which is stricter than inet_addr on purpose\n");

    ULONG out = 0;

    CHECK(bsd_inet_pton(AF_INET, (STRPTR)"192.168.1.1", &out, BASE) == 1,
          "a four-part address is accepted");

    /*
     * RFC 3493 4.3: inet_pton takes the four-part form and nothing else.  The
     * short forms and the radix prefixes that inet_addr must accept are
     * exactly what a program using inet_pton is trying to avoid, because
     * "0177.0.0.1" reaching a firewall rule as 127.0.0.1 is how an allowlist
     * is bypassed.
     */
    CHECK(bsd_inet_pton(AF_INET, (STRPTR)"127.1", &out, BASE) == 0,
          "the short form is refused");
    CHECK(bsd_inet_pton(AF_INET, (STRPTR)"0177.0.0.1", &out, BASE) == 0,
          "an octal part is refused");
    CHECK(bsd_inet_pton(AF_INET, (STRPTR)"0x7f.0.0.1", &out, BASE) == 0,
          "a hexadecimal part is refused");
    CHECK(bsd_inet_pton(AF_INET, (STRPTR)"1.2.3.4.5", &out, BASE) == 0,
          "five parts are refused");
}

static void test_ntop_v4(void)
{
    printf("inet_ntop\n");

    char  buf[32];
    ULONG addr = 0xC0A80101UL;

    memset(buf, 0, sizeof(buf));
    CHECK(bsd_inet_ntop(AF_INET, &addr, (STRPTR)buf, (LONG)sizeof(buf), BASE)
              != NULL, "a big enough buffer is filled");
    CHECK(strcmp(buf, "192.168.1.1") == 0, "and holds the address");

    /* RFC 3493 4.4: too small is a refusal, not a truncation, because a
       truncated address is a different address. */
    CHECK(bsd_inet_ntop(AF_INET, &addr, (STRPTR)buf, 4, BASE) == NULL,
          "a buffer that cannot hold it is refused");

    h_last_errno = 0;
    CHECK(bsd_inet_ntop(AF_UNIX, &addr, (STRPTR)buf, (LONG)sizeof(buf), BASE)
              == NULL, "a family it does not know is refused");
    CHECK(h_last_errno != 0, "and says why in errno");
}

#ifdef AMINETXDUO_IPV6
static void test_ntop_v6_bounds(void)
{
    static const struct
    {
        UBYTE bytes[16];
        const char *text;
    } cases[] = {
        { { 0 }, "::" },
        { { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 }, "::1" },
        { { 0,0,0,0,0,0,0,0,0,0,0xff,0xff,192,168,1,1 },
          "::ffff:192.168.1.1" },
        { { 0,0,0,0,0,0,0,0,0,0,0,0,1,2,3,4 }, "::1.2.3.4" },
        { { 0x20,1,0xd,0xb8,0,0,0,0,0,1,0,0,0,0,0,1 },
          "2001:db8::1:0:0:1" },
        { { 0x20,1,0,0,0,0,0,1,0,0,0,0,0,0,0,0 }, "2001:0:0:1::" },
        { { 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
            0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff },
          "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff" }
    };
    ULONG i;

    printf("inet_ntop IPv6 output and refusal boundaries\n");
    for (i = 0; i < (ULONG)(sizeof(cases) / sizeof(cases[0])); i++)
    {
        ULONG length = (ULONG)strlen(cases[i].text);
        LONG size;

        for (size = -1; size <= AMI_CFG_IP6_STRLEN; size++)
        {
            UBYTE guarded[AMI_CFG_IP6_STRLEN + 2];
            STRPTR result;
            ULONG untouched;
            ULONG j;

            memset(guarded, 0xa5, sizeof(guarded));
            h_last_errno = 0;
            result = bsd_inet_ntop(AF_INET6, (APTR)cases[i].bytes,
                                   (STRPTR)(guarded + 1), size, BASE);
            CHECK(guarded[0] == 0xa5, "IPv6 output preserves leading canary");
            if (size > (LONG)length)
            {
                CHECK(result == (STRPTR)(guarded + 1), "IPv6 returns caller buffer");
                CHECK(strcmp((const char *)guarded + 1, cases[i].text) == 0,
                      "IPv6 matches fixed RFC output");
                CHECK(h_last_errno == 0, "IPv6 success leaves errno alone");
                untouched = length + 2;
            }
            else
            {
                CHECK(result == NULL && h_last_errno == AMI_ENOSPC,
                      "IPv6 insufficient size reports ENOSPC");
                untouched = 1;
            }
            for (j = untouched; j < (ULONG)sizeof(guarded); j++)
                CHECK(guarded[j] == 0xa5, "IPv6 touches only successful text and NUL");
        }
    }
}
#endif

static void test_format_octets_and_bounds(void)
{
    static const struct { ULONG address; const char *text; } cases[] = {
        { 0, "0.0.0.0" }, { 0x090A6364UL, "9.10.99.100" },
        { 0xFFFFFFFFUL, "255.255.255.255" }
    };
    unsigned v, i;

    for (v = 0; v <= 255; v++)
    {
        unsigned b = (v + 1) & 255, c = (v + 99) & 255, d = (v + 100) & 255;
        ULONG address = ((ULONG)v << 24) | ((ULONG)b << 16) |
                        ((ULONG)c << 8) | (ULONG)d;
        char expected[16], output[16];

        snprintf(expected, sizeof(expected), "%u.%u.%u.%u", v, b, c, d);
        CHECK(bsd_Inet_NtoA(address, BASE) == (STRPTR)BASE->sb_NtoABuf &&
              strcmp(BASE->sb_NtoABuf, expected) == 0,
              "every octet value formats through the caller's private buffer");
        CHECK(bsd_inet_ntop(AF_INET, &address, (STRPTR)output, (LONG)sizeof(output), BASE) == (STRPTR)output &&
              strcmp(output, expected) == 0,
              "inet_ntop formats every octet value without leading zeroes");
    }
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        LONG size;
        for (size = -1; size <= 16; size++)
        {
            char guarded[18], original[sizeof(guarded)];
            STRPTR result;
            size_t length = strlen(cases[i].text), j;
            ULONG address = cases[i].address;

            memset(guarded, 0x5A, sizeof(guarded));
            memcpy(original, guarded, sizeof(guarded));
            h_last_errno = 0;
            result = bsd_inet_ntop(AF_INET, &address, (STRPTR)(guarded + 1), size, BASE);
            if (size <= (LONG)length)
            {
                CHECK(result == NULL && h_last_errno == AMI_ENOSPC,
                      "a short or negative buffer size is refused with ENOSPC");
                CHECK(memcmp(guarded, original, sizeof(guarded)) == 0,
                      "a refused conversion does not write any output byte");
            }
            else
            {
                CHECK(result == (STRPTR)(guarded + 1) && strcmp((const char *)result, cases[i].text) == 0,
                      "exact room including NUL succeeds with the original spelling");
                CHECK(guarded[0] == 0x5A, "conversion leaves its leading guard intact");
                for (j = length + 2; j < sizeof(guarded); j++)
                    CHECK(guarded[j] == 0x5A,
                          "conversion writes no bytes beyond its terminating NUL");
            }
        }
    }
}

int main(void)
{
    printf("AmiNetXDuo, src/bsdsocket/inet.c on the host\n\n");

    test_dotted_quad();
    test_short_forms();
    test_component_boundaries();
    test_radix();
    test_aton_reports_broadcast();
    test_network_parts();
    test_ntoa();
    test_classful();
    test_pton_v4();
    test_ntop_v4();
    test_format_octets_and_bounds();
#ifdef AMINETXDUO_IPV6
    test_ntop_v6_bounds();
#endif

    printf("\n%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
