/*
 * src/bsdsocket/oob.c on the host: the urgent-data packet filter.
 *
 * oob.c is compiled into this target with -Dstatic= so bsd_oob_ip_filter()
 * and its mark record are reachable, and with -ffunction-sections +
 * gc-sections so bsd_oob_send() (whose NetX packet externs are not scripted
 * here) is dropped.  This test drives the filter directly over a synthetic
 * IPv4/TCP buffer.
 *
 * N-074: the filter is armed with tx_sequence, and a pure ACK carries that
 * same sequence with no payload, so a (sport, dport, seq) match alone is not
 * enough -- marking the ACK would set URG over zero bytes.  The filter must
 * require at least one payload byte.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

static unsigned long o_checks;
static unsigned long o_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        o_checks++;                                                           \
        if (!(cond)) {                                                        \
            o_failures++;                                                     \
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

/* oob.c's mark record and filter, made extern by -Dstatic=. */
struct BsdOobMark
{
    BOOL  om_Active;
    UINT  om_LocalPort;
    UINT  om_PeerPort;
    ULONG om_Sequence;
    BOOL  om_Offload;
};

extern struct BsdOobMark bsd_oob_mark;
extern UINT bsd_oob_ip_filter(VOID *ip_header_ptr, UINT direction);

#define IP_HDR  20
#define TCP_HDR 20

/*
 * Build an IPv4/TCP segment.  `total_len` is the IP total-length field; the
 * buffer itself is a full 64 bytes so the filter never reads a stale byte
 * whichever header field it inspects.  sport/dport/seq are the connection the
 * mark is armed for.  data offset is 5 (a 20-byte TCP header) unless the
 * caller overrides it.
 */
static void o_packet(UBYTE *p, ULONG total_len, UWORD sport, UWORD dport,
                     ULONG seq)
{
    UBYTE *tcp = p + IP_HDR;

    memset(p, 0, 64);

    p[0] = 0x45;                            /* IPv4, IHL 5 (= 20 bytes) */
    p[2] = (UBYTE)(total_len >> 8);
    p[3] = (UBYTE)total_len;
    p[9] = IPPROTO_TCP;

    tcp[0]  = (UBYTE)(sport >> 8);
    tcp[1]  = (UBYTE)sport;
    tcp[2]  = (UBYTE)(dport >> 8);
    tcp[3]  = (UBYTE)dport;
    tcp[4]  = (UBYTE)(seq >> 24);
    tcp[5]  = (UBYTE)(seq >> 16);
    tcp[6]  = (UBYTE)(seq >> 8);
    tcp[7]  = (UBYTE)seq;
    tcp[12] = 0x50;                         /* data offset 5, no flags */
    tcp[16] = 0xFF;                         /* sentinel checksum the update
                                               moves off */
    tcp[17] = 0xFF;
}

static void o_arm(UWORD sport, UWORD dport, ULONG seq)
{
    bsd_oob_mark.om_Active    = TRUE;
    bsd_oob_mark.om_LocalPort = sport;
    bsd_oob_mark.om_PeerPort  = dport;
    bsd_oob_mark.om_Sequence  = seq;
    bsd_oob_mark.om_Offload   = FALSE;
}

int main(void)
{
    UBYTE  p[64];
    UBYTE *tcp = p + IP_HDR;

    printf("oob.c host checks\n\n");

    /*
     * A matching segment that carries one payload byte is the urgent byte's
     * own segment: URG is set, urgent pointer 1, and the RFC 1624 checksum
     * update moves the checksum off its sentinel.
     */
    o_arm(1234, 80, 0x01020304UL);
    o_packet(p, 41, 1234, 80, 0x01020304UL);
    CHECK(bsd_oob_ip_filter(p, NX_IP_PACKET_OUT) == NX_SUCCESS,
          "the filter never drops (returns NX_SUCCESS)");
    CHECK((tcp[13] & 0x20) != 0,
          "a data segment with the armed (sport,dport,seq) gets URG");
    CHECK(tcp[18] == 0 && tcp[19] == 1,
          "with urgent pointer 1 (one past the urgent byte)");
    CHECK(tcp[16] != 0xFF || tcp[17] != 0xFF,
          "and the checksum is updated for the URG + pointer change");

    /*
     * N-074: a pure ACK carries seq == tx_sequence but no payload.  The match
     * still succeeds, but there is nothing to mark -- and marking it would put
     * URG over a zero-length segment.  The filter must leave it alone.
     */
    o_arm(1234, 80, 0x01020304UL);
    o_packet(p, 40, 1234, 80, 0x01020304UL);
    CHECK(bsd_oob_ip_filter(p, NX_IP_PACKET_OUT) == NX_SUCCESS,
          "a zero-payload ACK is not dropped");
    CHECK((tcp[13] & 0x20) == 0,
          "and is NOT marked URG (N-074: a payload is required)");
    CHECK(tcp[18] == 0 && tcp[19] == 0,
          "leaving the urgent pointer untouched");

    /*
     * A data offset that overclaims (60 bytes of TCP header inside a 40-byte
     * total length) has no valid payload either; the bound must not underflow.
     */
    o_arm(1234, 80, 0x01020304UL);
    o_packet(p, 40, 1234, 80, 0x01020304UL);
    tcp[12] = 0xF0;                         /* data offset 15 -> 60 bytes */
    CHECK(bsd_oob_ip_filter(p, NX_IP_PACKET_OUT) == NX_SUCCESS,
          "an overclaiming data offset is not dropped");
    CHECK((tcp[13] & 0x20) == 0,
          "and is NOT marked URG (total length shorter than the header)");

    /* A mismatched sequence is a different segment and must be left alone. */
    o_arm(1234, 80, 0x01020304UL);
    o_packet(p, 41, 1234, 80, 0x01020305UL);
    CHECK(bsd_oob_ip_filter(p, NX_IP_PACKET_OUT) == NX_SUCCESS,
          "a mismatched segment is not dropped");
    CHECK((tcp[13] & 0x20) == 0,
          "and is NOT marked URG (the match gate runs first)");

    printf("\n%lu checks, %lu failures\n", o_checks, o_failures);

    return (o_failures == 0) ? 0 : 1;
}
