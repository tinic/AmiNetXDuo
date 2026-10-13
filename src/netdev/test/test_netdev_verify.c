/*
 * The shared driver-side IPv4/IPv6 checksum verdict, against packets built
 * independently byte by byte on the host.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "netdev_verify.h"
#include "aminetxduo/anxs2ext.h"

static int checks;
static int failures;

static void expect(const char *what, int ok)
{
    checks++;
    if (!ok)
    {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void put16(UBYTE *p, UWORD v)
{
    p[0] = (UBYTE)(v >> 8);
    p[1] = (UBYTE)v;
}

static void put32(UBYTE *p, ULONG v)
{
    put16(p, (UWORD)(v >> 16));
    put16(p + 2, (UWORD)v);
}

static ULONG add16(ULONG sum, UWORD word)
{
    sum += word;
    return (sum & 0xffffUL) + (sum >> 16);
}

static ULONG bytes_sum(ULONG sum, const UBYTE *p, UWORD len)
{
    while (len >= 2)
    {
        sum = add16(sum, (UWORD)(((UWORD)p[0] << 8) | p[1]));
        p += 2;
        len = (UWORD)(len - 2);
    }
    if (len != 0)
        sum = add16(sum, (UWORD)((UWORD)p[0] << 8));
    return sum;
}

static UWORD finish_sum(ULONG sum)
{
    sum = (sum & 0xffffUL) + (sum >> 16);
    sum = (sum & 0xffffUL) + (sum >> 16);
    return (UWORD)~sum;
}

static ULONG packet_sum(const UBYTE *p, UWORD len)
{
    return bytes_sum(0, p, len);
}

static UWORD transport4_sum(const UBYTE *ip, UWORD tlen, UBYTE proto)
{
    ULONG sum = bytes_sum(0, ip + 12, 8);

    sum = add16(sum, proto);
    sum = add16(sum, tlen);
    sum = bytes_sum(sum, ip + 20, tlen);
    return finish_sum(sum);
}

static UWORD transport6_sum(const UBYTE *ip, UWORD tlen, UBYTE next)
{
    ULONG sum = bytes_sum(0, ip + 8, 32);

    sum = add16(sum, tlen);             /* upper length word is zero */
    sum = add16(sum, next);
    sum = bytes_sum(sum, ip + 40, tlen);
    return finish_sum(sum);
}

static UWORD make_ipv4_tcp(UBYTE *ip)
{
    const UWORD len = 44;

    memset(ip, 0, len);
    ip[0] = 0x45;
    put16(ip + 2, len);
    put16(ip + 4, 0x1234);
    put16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = 6;
    ip[12] = 192; ip[13] = 0; ip[14] = 2; ip[15] = 1;
    ip[16] = 198; ip[17] = 51; ip[18] = 100; ip[19] = 2;
    put16(ip + 20, 1234);
    put16(ip + 22, 4321);
    put32(ip + 24, 0x01020304UL);
    put32(ip + 28, 0x11223344UL);
    ip[32] = 0x50;
    ip[33] = 0x10;
    put16(ip + 34, 4096);
    ip[40] = 0xde; ip[41] = 0xad; ip[42] = 0xbe; ip[43] = 0xef;
    put16(ip + 10, finish_sum(bytes_sum(0, ip, 20)));
    put16(ip + 36, transport4_sum(ip, (UWORD)(len - 20), 6));
    return len;
}

static UWORD make_ipv4_udp(UBYTE *ip)
{
    const UWORD len = 32;

    memset(ip, 0, len);
    ip[0] = 0x45;
    put16(ip + 2, len);
    ip[8] = 32;
    ip[9] = 17;
    ip[12] = 10; ip[15] = 1;
    ip[16] = 10; ip[19] = 2;
    put16(ip + 20, 53);
    put16(ip + 22, 49152);
    put16(ip + 24, 12);
    ip[28] = 1; ip[29] = 2; ip[30] = 3; ip[31] = 4;
    put16(ip + 10, finish_sum(bytes_sum(0, ip, 20)));
    put16(ip + 26, transport4_sum(ip, 12, 17));
    return len;
}

static UWORD make_ipv6_tcp(UBYTE *ip)
{
    const UWORD plen = 24;
    const UWORD len  = (UWORD)(40 + plen);
    UWORD i;

    memset(ip, 0, len);
    ip[0] = 0x60;
    put16(ip + 4, plen);
    ip[6] = 6;
    ip[7] = 64;
    for (i = 0; i < 16; i++)
    {
        ip[8 + i]  = (UBYTE)i;
        ip[24 + i] = (UBYTE)(0x80u + i);
    }
    put16(ip + 40, 22);
    put16(ip + 42, 6000);
    put32(ip + 44, 0x55667788UL);
    put32(ip + 48, 0x99aabbccUL);
    ip[52] = 0x50;
    ip[53] = 0x18;
    put16(ip + 54, 8192);
    ip[60] = 5; ip[61] = 6; ip[62] = 7; ip[63] = 8;
    put16(ip + 56, transport6_sum(ip, plen, 6));
    return len;
}

static void test_ipv4(void)
{
    UBYTE ip[64];
    UWORD len = make_ipv4_tcp(ip);
    expect("valid IPv4/TCP is verified",
           netdev_rx_verify4(ip, len, packet_sum(ip, len)) ==
               ANXD_S2_RXF_VERIFIED);
    expect("GEM TCP verdict certifies a structurally valid packet",
           netdev_rx_trust4(ip, len, 2) == ANXD_S2_RXF_VERIFIED);
    expect("GEM UDP verdict cannot certify TCP",
           netdev_rx_trust4(ip, len, 3) == 0);
    expect("version dispatcher accepts IPv4",
           netdev_rx_verify(ip, len, packet_sum(ip, len)) ==
               ANXD_S2_RXF_VERIFIED);

    ip[43] ^= 1;
    expect("bad IPv4 transport checksum is refused",
           netdev_rx_verify(ip, len, packet_sum(ip, len)) == 0);
    len = make_ipv4_tcp(ip);
    ip[0] = 0x46;
    expect("IPv4 options are refused",
           netdev_rx_verify4(ip, len, packet_sum(ip, len)) == 0);
    expect("hardware verdict still refuses IPv4 options",
           netdev_rx_trust4(ip, len, 2) == 0);
    len = make_ipv4_tcp(ip);
    ip[6] |= 0x20;
    expect("IPv4 fragments are refused",
           netdev_rx_verify4(ip, len, packet_sum(ip, len)) == 0);
    expect("hardware verdict still refuses fragments",
           netdev_rx_trust4(ip, len, 2) == 0);
    len = make_ipv4_tcp(ip);
    expect("Ethernet padding beyond IPv4 total length is refused",
           netdev_rx_verify4(ip, (UWORD)(len + 4), packet_sum(ip, len)) == 0);
    expect("hardware verdict still refuses Ethernet padding",
           netdev_rx_trust4(ip, (UWORD)(len + 4), 2) == 0);

    len = make_ipv4_udp(ip);
    expect("valid IPv4/UDP is verified",
           netdev_rx_verify(ip, len, packet_sum(ip, len)) ==
               ANXD_S2_RXF_VERIFIED);
    expect("GEM UDP verdict certifies a structurally valid packet",
           netdev_rx_trust4(ip, len, 3) == ANXD_S2_RXF_VERIFIED);
    expect("IP-only GEM verdict is not enough",
           netdev_rx_trust4(ip, len, 1) == 0);

    /* The hardware sum pads an odd final byte in the low address/high-order
       half of its word.  Keep that case in the contract: many small UDP
       protocols do not happen to have an even payload. */
    put16(ip + 2, 33);
    put16(ip + 24, 13);
    ip[32] = 5;
    put16(ip + 10, 0);
    put16(ip + 26, 0);
    put16(ip + 10, finish_sum(bytes_sum(0, ip, 20)));
    put16(ip + 26, transport4_sum(ip, 13, 17));
    len = 33;
    expect("odd-length IPv4/UDP is verified",
           netdev_rx_verify(ip, len, packet_sum(ip, len)) ==
               ANXD_S2_RXF_VERIFIED);

    len = make_ipv4_udp(ip);
    ip[26] = 0;
    ip[27] = 0;
    expect("IPv4 UDP without a checksum is not certified",
           netdev_rx_verify(ip, len, packet_sum(ip, len)) == 0);
    expect("hardware verdict cannot certify absent UDP checksum",
           netdev_rx_trust4(ip, len, 3) == 0);
}

static void test_ipv6(void)
{
    UBYTE ip[80];
    UWORD len = make_ipv6_tcp(ip);
    expect("valid IPv6/TCP is verified",
           netdev_rx_verify6(ip, len, packet_sum(ip, len)) ==
               ANXD_S2_RXF_VERIFIED);
    expect("version dispatcher accepts IPv6",
           netdev_rx_verify(ip, len, packet_sum(ip, len)) ==
               ANXD_S2_RXF_VERIFIED);

    ip[63] ^= 1;
    expect("bad IPv6 transport checksum is refused",
           netdev_rx_verify(ip, len, packet_sum(ip, len)) == 0);
    len = make_ipv6_tcp(ip);
    ip[6] = 0;                          /* hop-by-hop extension */
    expect("IPv6 extension headers are refused",
           netdev_rx_verify6(ip, len, packet_sum(ip, len)) == 0);
    len = make_ipv6_tcp(ip);
    expect("bytes beyond the IPv6 payload length are refused",
           netdev_rx_verify6(ip, (UWORD)(len + 2), packet_sum(ip, len)) == 0);
}

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#define GUARD_PAGE 1

/* The first byte of an inaccessible page, after a writable one: a frame of
   n bytes at (result - n) ends exactly where reading faults. */
static UBYTE *guard_frame_end(void)
{
    long   pg = sysconf(_SC_PAGESIZE);
    UBYTE *m;

    if (pg <= 0)
        return NULL;
    m = (UBYTE *)mmap(NULL, (size_t)pg * 2, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == (UBYTE *)MAP_FAILED)
        return NULL;
    if (mprotect(m + pg, (size_t)pg, PROT_NONE) != 0)
        return NULL;
    return m + pg;
}
#else
#define GUARD_PAGE 0
static UBYTE *guard_frame_end(void) { return NULL; }
#endif

/*
 * F-314: a payload length of 65516 plus the 40-byte header wrapped to 20 in
 * sixteen bits and matched a 20-byte frame.  Bytes 8-9 balance the sum so the
 * checksum passes too; the UDP case also takes its length and checksum from
 * past the end of the frame.
 */
static void test_ipv6_short(void)
{
    UBYTE ip[80];
    UWORD tlen = 0xffecu;               /* 65516 + 40 == 20 in a UWORD */
    UWORD plen = 20;

    memset(ip, 0, sizeof(ip));
    ip[0] = 0x60;
    put16(ip + 4, tlen);
    ip[6] = 6;
    put16(ip + 8, finish_sum(add16(tlen, 6)));
    expect("a 20-byte IPv6/TCP frame claiming 65516 is refused",
           netdev_rx_verify6(ip, plen, packet_sum(ip, plen)) == 0);
    expect("and by the version dispatcher",
           netdev_rx_verify(ip, plen, packet_sum(ip, plen)) == 0);

    memset(ip, 0, sizeof(ip));
    ip[0] = 0x60;
    put16(ip + 4, tlen);
    ip[6] = 17;
    put16(ip + 8, finish_sum(add16(tlen, 17)));
    put16(ip + 44, tlen);               /* beyond the frame */
    put16(ip + 46, 1);
    expect("a 20-byte IPv6/UDP frame claiming 65516 is refused",
           netdev_rx_verify6(ip, plen, packet_sum(ip, plen)) == 0);

    {   /* Every length short of the header, whatever the claim, with the
           frame ending at an unmapped page where one can be had: a read of
           any byte past plen faults instead of being masked by the array. */
        UBYTE *page = guard_frame_end();
        UWORD  n;
        int    refused = 1;

        for (n = 0; n < 40; n++)
        {
            UBYTE *f = (page != NULL) ? page - n : ip;

            memset(ip, 0, sizeof(ip));
            ip[0] = 0x60;
            put16(ip + 4, (UWORD)(n - 40));
            ip[6] = 6;
            if (n >= 10)
                put16(ip + 8, finish_sum(add16((UWORD)(n - 40), 6)));
            if (f != ip)
                memcpy(f, ip, n);
            if (netdev_rx_verify6(f, n, packet_sum(f, n)) != 0)
                refused = 0;
        }
        expect("no frame shorter than the IPv6 header is certified", refused);
        expect("the short-frame sweep ran against a guard page",
               page != NULL || !GUARD_PAGE);
    }

    /* The exact fit still verifies. */
    {
        UWORD len = make_ipv6_tcp(ip);

        expect("a whole IPv6/TCP frame still verifies",
               netdev_rx_verify6(ip, len, packet_sum(ip, len)) ==
                   ANXD_S2_RXF_VERIFIED);
    }
}

static void test_tx_checksum(void)
{
    UBYTE frame[96];
    UBYTE *ip = frame + 14;
    UWORD len;
    UWORD offset = 0xffffu;

    memset(frame, 0, sizeof(frame));
    frame[12] = 0x08;
    frame[13] = 0x00;
    len = (UWORD)(14 + make_ipv4_tcp(ip));
    expect("TX TCP checksum field is located after Ethernet and IPv4",
           netdev_tx_csum4(frame, len, ANXD_S2_TXF_TCP, &offset) ==
               ANXD_S2_TXF_TCP && offset == 50);
    expect("TX TCP offload is not used unless it was negotiated",
           netdev_tx_csum4(frame, len, ANXD_S2_TXF_UDP, &offset) == 0);

    offset = 0xffffu;
    expect("short TCP frame refuses without changing the output",
           netdev_tx_csum4(frame, 53, ANXD_S2_TXF_TCP, &offset) == 0 &&
           offset == 0xffffu);
    ip[32] = 0x40;
    expect("short TCP header is refused", netdev_tx_csum4(
               frame, len, ANXD_S2_TXF_TCP, &offset) == 0);
    ip[32] = 0x50;
    ip[6] = 0x40;                       /* DF is allowed */
    expect("TX TCP with DF and padding still uses the fixed offset",
           netdev_tx_csum4(frame, 80, ANXD_S2_TXF_TCP, &offset) ==
               ANXD_S2_TXF_TCP && offset == 50);
    ip[7] = 1;
    expect("fragment offset is refused", netdev_tx_csum4(
               frame, len, ANXD_S2_TXF_TCP, &offset) == 0);
    len = (UWORD)(14 + make_ipv4_tcp(ip));
    memmove(ip + 24, ip + 20, len - 34);
    memset(ip + 20, 0, 4);
    ip[0] = 0x46;
    ip[3] += 4;
    len += 4;
    expect("TCP with IPv4 options retains the parsed offset",
           netdev_tx_csum4(frame, len, ANXD_S2_TXF_TCP, &offset) ==
               ANXD_S2_TXF_TCP && offset == 54);

    len = (UWORD)(14 + make_ipv4_udp(ip));
    expect("TX UDP checksum field is located from the parsed IHL",
           netdev_tx_csum4(frame, len, ANXD_S2_TXF_UDP, &offset) ==
               ANXD_S2_TXF_UDP && offset == 40);
    expect("Ethernet padding is accepted on transmit",
           netdev_tx_csum4(frame, 60, ANXD_S2_TXF_UDP, &offset) ==
               ANXD_S2_TXF_UDP);

    len = (UWORD)(14 + make_ipv4_tcp(ip));
    ip[6] = 0x20;
    expect("TX fragments are refused", netdev_tx_csum4(
               frame, len, ANXD_S2_TXF_TCP, &offset) == 0);
    len = (UWORD)(14 + make_ipv4_tcp(ip));
    frame[12] = 0x86;
    frame[13] = 0xdd;
    expect("TX IPv6 is not mistaken for GEM IPv4 full offload",
           netdev_tx_csum4(frame, len, ANXD_S2_TXF_TCP, &offset) == 0);
}

/* ANX-005: verify4/trust4 read the total length at ip+2 before checking
   that the frame holds an IPv4 header at all.  Every length 0..19,
   starting 0x45 and ending at an inaccessible page, must be refused without
   reading past its end. */
static void test_ipv4_short(void)
{
    UBYTE *end = guard_frame_end();
    UWORD  n;
    int    refused = 1;

    if (end == NULL)
    {
        expect("IPv4 short frames: no guard page on this host", GUARD_PAGE == 0);
        return;
    }
    /* Zero bytes: the pointer is the inaccessible page itself. */
    if (netdev_rx_verify4(end, 0, 0) != 0 || netdev_rx_trust4(end, 0, 2) != 0)
        refused = 0;
    for (n = 1; n < 20; n++)
    {
        UBYTE *ip = end - n;

        memset(ip, 0, n);
        ip[0] = 0x45;
        if (netdev_rx_verify4(ip, n, 0) != 0 ||
            netdev_rx_trust4(ip, n, 2) != 0)
            refused = 0;
    }
    expect("IPv4 frames shorter than a header are refused in bounds", refused);
}

int main(void)
{
    test_ipv4();
    test_ipv4_short();
    test_ipv6();
    test_ipv6_short();
    test_tx_checksum();
    printf("%d checks, %d failures, %s\n", checks, failures,
           failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
