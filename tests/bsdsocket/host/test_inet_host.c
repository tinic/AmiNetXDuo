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
 * errno.c drags the whole vector table behind it, and the IPv6 text parser
 * lives in src/config, which has a host test of its own.  The last errno set
 * is kept because a refusal that does not say why is half a refusal.
 */
static LONG h_last_errno;

VOID bsd_set_errno(struct AmiSocketBase *base, LONG err)
{
    (VOID)base;
    h_last_errno = err;
}

VOID bsd_bcopy(const APTR src, APTR dst, ULONG size)
{
    memmove(dst, src, (size_t)size);
}

BOOL ami_config_parse_ip6(const char *text, ULONG out[AMI_CFG_IP6_WORDS],
                          ULONG *prefix_out)
{
    (VOID)text; (VOID)out; (VOID)prefix_out;
    return FALSE;                   /* the v6 cases are not exercised here */
}

VOID bsd_words_to_in6(const ULONG words[4], UBYTE bytes[16])
{
    (VOID)words; (VOID)bytes;
}

VOID bsd_in6_to_words(const UBYTE bytes[16], ULONG words[4])
{
    (VOID)bytes; (VOID)words;
}

VOID ami_config_format_ip6(const ULONG addr[AMI_CFG_IP6_WORDS], char *out,
                           ULONG outlen)
{
    (VOID)addr;
    if (outlen > 0UL)
        out[0] = '\0';
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

int main(void)
{
    printf("AmiNetXDuo, src/bsdsocket/inet.c on the host\n\n");

    test_dotted_quad();
    test_short_forms();
    test_radix();
    test_aton_reports_broadcast();
    test_ntoa();
    test_classful();
    test_pton_v4();
    test_ntop_v4();

    printf("\n%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
