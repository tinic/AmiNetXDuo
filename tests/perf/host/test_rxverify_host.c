/*
 * AmiNetXDuo, host check for src/net68k/n68k_rx_verify.c.
 *
 * Content-level: a frame is built by hand, the transport checksum filled in
 * the way a sender would, and both entry points asked about it -- clean, with
 * the header corrupted, with the payload corrupted, and with the length field
 * disagreeing with the buffer.  The two entries must answer identically, or
 * the fused one is accepting something the walk rejects.
 *
 * The IPv6 half also builds extension header chains, because the verifier
 * walks them and a wrong answer there is a checksum claimed over the wrong
 * bytes.  Each chain case names what the verifier is being asked to conclude:
 * an accepted one must carry the right capability bit, and a declined one
 * must carry none AND not drop the frame, since declining is a statement
 * about this file and not about the frame.
 *
 * That is a pure function of a frame in memory, so it belongs on the host
 * where it can be stepped rather than in an emulator.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "nx_api.h"
#include "net68k.h"

/* NX_ASSERT parks a real target forever; the host tier only has to link. */
UINT _tx_thread_sleep(ULONG timer_ticks)
{
    (void)timer_ticks;
    return 0;
}

static ULONG  failures;

static void ck(const char *what, int ok)
{
    if (!ok)
    {
        failures++;
        printf("FAIL %s\n", what);
    }
}

/* The ones-complement sum a sender computes, over the pseudo header and the
   segment, exactly as RFC 793 and RFC 8200 state it.  `addr` is the pair of
   addresses already in network order, `alen` their size in bytes. */
static USHORT ref_sum(const UCHAR *seg, ULONG len, const UCHAR *addr,
                      ULONG alen, ULONG proto)
{
ULONG   sum = 0;
ULONG   i;

    for (i = 0; i + 1 < alen; i += 2)
        sum += ((ULONG)addr[i] << 8) | addr[i + 1];

    sum += proto;
    sum += len;

    for (i = 0; i + 1 < len; i += 2)
        sum += ((ULONG)seg[i] << 8) | seg[i + 1];
    if (i < len)
        sum += (ULONG)seg[i] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFFUL) + (sum >> 16);

    return (USHORT)(~sum & 0xFFFFUL);
}

/*
 * What the copy hook hands the fused entry: the ones-complement longword sum
 * of what it copied, end-around carry, starting at the IP header.
 *
 * Assembled big-endian, because that is the machine the hook runs on and the
 * value it produces there is byte order dependent.  The last longword is the
 * bytes that arrived, padded with zeroes -- ami_sana2_copy_to_buff() builds it
 * that way through a byte array, so a frame whose length is not a multiple of
 * four still contributes a whole longword.  A model that dropped that tail
 * would agree with the verifier only on lengths divisible by four, which is
 * every hand-written fixture in this file and no real frame in particular.
 */
static ULONG carried_sum(const UCHAR *p, ULONG bytes)
{
ULONG   acc = 0;
ULONG   i;
ULONG   k;

    for (i = 0; i + 3 < bytes; i += 4)
    {
    ULONG w = ((ULONG)p[i] << 24) | ((ULONG)p[i + 1] << 16) |
              ((ULONG)p[i + 2] << 8) | (ULONG)p[i + 3];

        acc += w;
        if (acc < w)
            acc++;
    }

    if (i < bytes)
    {
    ULONG w = 0;

        for (k = 0; i + k < bytes; k++)
            w |= (ULONG)p[i + k] << (24 - (8 * k));

        acc += w;
        if (acc < w)
            acc++;
    }

    return acc;
}

/* ------------------------------------------------------------- fixtures -- */

/* Longword aligned: the driver guarantees it and N68K_SUM_LONGWORDS needs
   it. */
static ULONG   buf4_store[512];
static ULONG   buf6_store[512];
#define buf4   ((UCHAR *)buf4_store)
#define buf6   ((UCHAR *)buf6_store)

static const UCHAR v6src[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01
};
static const UCHAR v6dst[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02
};

static const UINT  v4_ihl     = 20;
static const ULONG v4_payload = 40;         /* 20 TCP header + 20 data */
static const ULONG v6_payload = 60;         /* 20 TCP header + 40 data */


static void fill_tcp(UCHAR *tcp, ULONG payload)
{
ULONG i;

    memset(tcp, 0, payload);
    tcp[0] = 0x30; tcp[1] = 0x39;            /* source port 12345 */
    tcp[2] = 0x00; tcp[3] = 0x50;            /* dest port 80 */
    tcp[12] = 0x50;                          /* data offset 5 */
    tcp[13] = 0x10;                          /* ACK */
    for (i = 20; i < payload; i++)
        tcp[i] = (UCHAR)(0xA0 + i);
}

/* Rebuild the IPv4 fixture from scratch.  `total` goes in the length field,
   which is what the "length disagrees with the buffer" case perturbs. */
static ULONG build_v4(ULONG total_field)
{
UCHAR *ip  = buf4;
UCHAR *tcp = buf4 + v4_ihl;
UCHAR  addr[8];
ULONG  total = v4_ihl + v4_payload;
ULONG  hs = 0;
ULONG  i;
USHORT sum;

    memset(buf4, 0, sizeof buf4_store);

    ip[0] = 0x45;                            /* v4, ihl 5 */
    ip[2] = (UCHAR)(total_field >> 8);
    ip[3] = (UCHAR)(total_field & 0xFF);
    ip[8] = 64;                              /* ttl */
    ip[9] = NX_PROTOCOL_TCP;
    ip[12] = 192; ip[13] = 168; ip[14] = 1; ip[15] = 1;
    ip[16] = 192; ip[17] = 168; ip[18] = 1; ip[19] = 2;

    for (i = 0; i + 1 < v4_ihl; i += 2)
        hs += ((ULONG)ip[i] << 8) | ip[i + 1];
    while (hs >> 16)
        hs = (hs & 0xFFFFUL) + (hs >> 16);
    sum = (USHORT)(~hs & 0xFFFFUL);
    ip[10] = (UCHAR)(sum >> 8);
    ip[11] = (UCHAR)(sum & 0xFF);

    fill_tcp(tcp, v4_payload);

    memcpy(addr, &ip[12], 8);
    sum = ref_sum(tcp, v4_payload, addr, 8, NX_PROTOCOL_TCP);
    tcp[16] = (UCHAR)(sum >> 8);
    tcp[17] = (UCHAR)(sum & 0xFF);

    return total;
}

static ULONG build_v6(ULONG plen_field, UCHAR next_header)
{
UCHAR *ip  = buf6;
UCHAR *tcp = buf6 + 40;
UCHAR  addr[32];
USHORT sum;

    memset(buf6, 0, sizeof buf6_store);

    ip[0] = 0x60;                            /* v6, no traffic class */
    ip[4] = (UCHAR)(plen_field >> 8);
    ip[5] = (UCHAR)(plen_field & 0xFF);
    ip[6] = next_header;
    ip[7] = 64;                              /* hop limit */
    memcpy(&ip[8],  v6src, 16);
    memcpy(&ip[24], v6dst, 16);

    fill_tcp(tcp, v6_payload);

    memcpy(addr, v6src, 16);
    memcpy(addr + 16, v6dst, 16);
    sum = ref_sum(tcp, v6_payload, addr, 32, NX_PROTOCOL_TCP);
    tcp[16] = (UCHAR)(sum >> 8);
    tcp[17] = (UCHAR)(sum & 0xFF);

    return 40 + v6_payload;
}

/* --------------------------------------------- the extension header chain -- */

/*
 * One extension header as it goes into the frame.  `type` is the next-header
 * value that names it, `bytes` its whole length, and `tail` everything from
 * byte 1 on -- the length byte included, because the unit it counts differs
 * between AH and the rest and the test is the place that must say which.
 * Byte 0 is the builder's: it is the next header, which only the chain knows.
 */
typedef struct {
    UCHAR   type;
    ULONG   bytes;
    UCHAR   tail[62];
} V6Ext;

/* The upper layer that terminates a chain. */
typedef struct {
    UCHAR   protocol;
    ULONG   bytes;
} V6Upper;

/*
 * Build an IPv6 frame with `count` extension headers ahead of `upper`.
 *
 * `plen_delta` is added to the payload length field after everything else is
 * computed, which is how a frame that lies about its own length is made.
 * Returns the frame length, which is what actually sits in the buffer.
 */
static ULONG build_v6x(const V6Ext *chain, UINT count, V6Upper upper,
                       long plen_delta)
{
UCHAR  *ip = buf6;
UCHAR  *up;
UCHAR   addr[32];
ULONG   extlen = 0;
ULONG   plen;
ULONG   at;
UINT    i;
USHORT  sum;

    memset(buf6, 0, sizeof buf6_store);

    for (i = 0; i < count; i++)
        extlen += chain[i].bytes;

    plen = extlen + upper.bytes;

    ip[0] = 0x60;
    ip[4] = (UCHAR)(((plen + (ULONG)plen_delta) >> 8) & 0xFF);
    ip[5] = (UCHAR)((plen + (ULONG)plen_delta) & 0xFF);
    ip[6] = (count != 0) ? chain[0].type : upper.protocol;
    ip[7] = 64;
    memcpy(&ip[8],  v6src, 16);
    memcpy(&ip[24], v6dst, 16);

    at = 40;
    for (i = 0; i < count; i++)
    {
        ip[at] = (UCHAR)((i + 1 < count) ? chain[i + 1].type
                                         : upper.protocol);
        memcpy(&ip[at + 1], chain[i].tail, chain[i].bytes - 1);
        at += chain[i].bytes;
    }

    up = &ip[at];

    memcpy(addr, v6src, 16);
    memcpy(addr + 16, v6dst, 16);

    if (upper.protocol == NX_PROTOCOL_ICMPV6)
    {
    ULONG j;

        /* An echo request.  Filled in here and not by the caller afterwards,
           because the checksum below has to cover it. */
        up[0] = 128;                        /* echo request */
        up[1] = 0;
        up[4] = 0x12; up[5] = 0x34;         /* identifier */
        up[6] = 0x00; up[7] = 0x01;         /* sequence */
        for (j = 8; j < upper.bytes; j++)
            up[j] = (UCHAR)(0x40 + j);
    }
    else if (upper.protocol == NX_PROTOCOL_UDP)
    {
    ULONG j;

        up[0] = 0x30; up[1] = 0x39;         /* source port 12345 */
        up[2] = 0x00; up[3] = 0x35;         /* dest port 53 */
        up[4] = (UCHAR)(upper.bytes >> 8);
        up[5] = (UCHAR)(upper.bytes & 0xFF);
        for (j = 8; j < upper.bytes; j++)
            up[j] = (UCHAR)(0x70 + j);
    }
    else
    {
        fill_tcp(up, upper.bytes);
    }

    sum = ref_sum(up, upper.bytes, addr, 32, upper.protocol);

    if (upper.protocol == NX_PROTOCOL_ICMPV6)
    {
        up[2] = (UCHAR)(sum >> 8);
        up[3] = (UCHAR)(sum & 0xFF);
    }
    else if (upper.protocol == NX_PROTOCOL_UDP)
    {
        /* A computed zero goes on the wire as all ones: zero means "no
           checksum", which the verifier declines rather than checks. */
        if (sum == 0)
            sum = 0xFFFF;
        up[6] = (UCHAR)(sum >> 8);
        up[7] = (UCHAR)(sum & 0xFF);
    }
    else
    {
        up[16] = (UCHAR)(sum >> 8);
        up[17] = (UCHAR)(sum & 0xFF);
    }

    return 40 + extlen + upper.bytes;
}

/* An eight-byte TLV header -- hop-by-hop or destination options -- padded out
   with PadN so the options walk ends exactly on the header's end. */
static V6Ext tlv8(UCHAR type)
{
V6Ext   e;

    memset(&e, 0, sizeof e);
    e.type    = type;
    e.bytes   = 8;
    e.tail[0] = 0;                          /* length: (0 + 1) * 8 = 8 */
    e.tail[1] = 1;                          /* PadN */
    e.tail[2] = 4;                          /*   of four zero bytes */

    return e;
}

/* ---------------------------------------------------------- the compare -- */

/*
 * Run one frame through both entries and require the same verdict.  `copied`
 * is what the copy hook would report, which is the whole frame as it sits in
 * the buffer -- not the length field, which may be lying.
 */
static void both(const char *what, UCHAR *base, ULONG frame, ULONG copied,
                 UINT want_drop, ULONG want_bits)
{
NX_PACKET   packet;
ULONG       walk_flags;
ULONG       fused_flags;
UINT        walk_drop  = 99;
UINT        fused_drop = 99;
char        label[128];

    memset(&packet, 0, sizeof packet);
    packet.nx_packet_prepend_ptr = base;
    packet.nx_packet_append_ptr  = base + frame;
    packet.nx_packet_length      = frame;
    packet.nx_packet_data_start  = base;
    packet.nx_packet_data_end    = base + 2048;

    walk_flags = n68k_rx_verify(&packet, &walk_drop);

    memset(&packet, 0, sizeof packet);
    packet.nx_packet_prepend_ptr = base;
    packet.nx_packet_append_ptr  = base + frame;
    packet.nx_packet_length      = frame;
    packet.nx_packet_data_start  = base;
    packet.nx_packet_data_end    = base + 2048;

    fused_flags = n68k_rx_verify_sum(&packet, carried_sum(base, copied),
                                     copied, &fused_drop);

    printf("%-34s walk=0x%08lx/%u fused=0x%08lx/%u\n", what,
           (unsigned long)walk_flags, (unsigned)walk_drop,
           (unsigned long)fused_flags, (unsigned)fused_drop);

    snprintf(label, sizeof label, "%s: walk drop", what);
    ck(label, walk_drop == want_drop);
    snprintf(label, sizeof label, "%s: fused drop", what);
    ck(label, fused_drop == want_drop);
    snprintf(label, sizeof label, "%s: entries agree on drop", what);
    ck(label, walk_drop == fused_drop);

    snprintf(label, sizeof label, "%s: walk bits", what);
    ck(label, walk_flags == want_bits);
    snprintf(label, sizeof label, "%s: fused bits", what);
    ck(label, fused_flags == want_bits);
}

/* ------------------------------------------------------------- the fuzz -- */

static ULONG    fz_state = 0x12345678UL;

static ULONG fz_next(void)
{
    fz_state = (fz_state * 1103515245UL + 12345UL) & 0xFFFFFFFFUL;

    return ((fz_state >> 8) & 0xFFFFFFUL);
}

/*
 * One random chain, against the two properties that matter and nothing else.
 *
 * THE TEST DOES NOT RE-DERIVE THE ACCEPT RULE.  Encoding "hop-by-hop is
 * walked, ESP is not" here would compare the implementation with a copy of
 * itself and pass on a shared misreading.  What it knows instead is the truth
 * about the frame, because it built it: where the transport header is, what
 * protocol it is, and whether the checksum on it is right.  So:
 *
 *   both entry points must answer identically
 *   a dropped frame must really carry a bad checksum
 *   a cleared frame must really carry a good one, under the right protocol
 *
 * A declined frame asserts nothing, which is the point: declining is a
 * statement about this file and not about the frame.
 */
static void fuzz_one(unsigned long round)
{
V6Ext       chain[6];
V6Upper     upper;
NX_PACKET   packet;
ULONG       extlen = 0;
ULONG       frame;
ULONG       at;
ULONG       walk_flags;
ULONG       fused_flags;
ULONG       want;
UINT        walk_drop  = 99;
UINT        fused_drop = 99;
UINT        count;
UINT        i;
int         corrupt;
char        label[64];

    count = (UINT)(fz_next() % 5UL);

    for (i = 0; i < count; i++)
    {
    ULONG kind = fz_next() % 10UL;

        memset(&chain[i], 0, sizeof chain[i]);

        switch (kind)
        {
        case 0:                             /* hop-by-hop, 8 or 16 bytes */
        case 1:
            chain[i] = tlv8(0);
            if ((fz_next() & 1UL) != 0UL)
            {
                chain[i].bytes   = 16;
                chain[i].tail[0] = 1;
                chain[i].tail[2] = 12;      /* PadN over the rest */
            }
            break;

        case 2:                             /* destination options */
            chain[i] = tlv8(60);
            break;

        case 3:                             /* with a home address option */
            chain[i].type    = 60;
            chain[i].bytes   = 24;
            chain[i].tail[0] = 2;
            chain[i].tail[1] = 201;
            chain[i].tail[2] = 16;
            chain[i].tail[19] = 1;
            chain[i].tail[20] = 1;
            break;

        case 4:                             /* routing */
            chain[i].type    = 43;
            chain[i].bytes   = 8;
            chain[i].tail[1] = 4;
            chain[i].tail[2] = (UCHAR)(fz_next() & 3UL);   /* segments left */
            break;

        case 5:                             /* fragment */
        case 6:
            chain[i].type    = 44;
            chain[i].bytes   = 8;
            chain[i].tail[1] = (UCHAR)(fz_next() & 0x07UL);
            chain[i].tail[2] = (UCHAR)(fz_next() & 0xFFUL);
            break;

        case 7:                             /* authentication */
            chain[i].type    = 51;
            chain[i].bytes   = 24;
            chain[i].tail[0] = 4;           /* (4 + 2) * 4 */
            break;

        case 8:                             /* ESP */
            chain[i].type  = 50;
            chain[i].bytes = 8;
            break;

        default:                            /* no next header */
            chain[i].type  = 59;
            chain[i].bytes = 8;
            break;
        }

        extlen += chain[i].bytes;
    }

    switch (fz_next() % 3UL)
    {
    case 0:  upper.protocol = NX_PROTOCOL_TCP;    upper.bytes = 20 + (fz_next() % 40UL); break;
    case 1:  upper.protocol = NX_PROTOCOL_UDP;    upper.bytes =  8 + (fz_next() % 40UL); break;
    default: upper.protocol = NX_PROTOCOL_ICMPV6; upper.bytes =  8 + (fz_next() % 40UL); break;
    }

    upper.bytes &= ~1UL;                    /* keep ref_sum's odd tail out of it */
    if (upper.protocol == NX_PROTOCOL_TCP && upper.bytes < 20)
        upper.bytes = 20;
    if (upper.bytes < 8)
        upper.bytes = 8;

    frame = build_v6x(chain, count, upper, 0);
    at    = 40 + extlen;

    /* Corrupt a transport byte, never an extension header one: the transport
       checksum does not cover those, so a frame corrupted there is still a
       good frame and the assertion below would be wrong about it. */
    corrupt = ((fz_next() & 3UL) == 0UL) ? 1 : 0;
    if (corrupt)
        buf6[at + (fz_next() % upper.bytes)] ^= 0xFF;

    memset(&packet, 0, sizeof packet);
    packet.nx_packet_prepend_ptr = buf6;
    packet.nx_packet_append_ptr  = buf6 + frame;
    packet.nx_packet_length      = frame;
    packet.nx_packet_data_start  = buf6;
    packet.nx_packet_data_end    = buf6 + 2048;
    walk_flags = n68k_rx_verify(&packet, &walk_drop);

    memset(&packet, 0, sizeof packet);
    packet.nx_packet_prepend_ptr = buf6;
    packet.nx_packet_append_ptr  = buf6 + frame;
    packet.nx_packet_length      = frame;
    packet.nx_packet_data_start  = buf6;
    packet.nx_packet_data_end    = buf6 + 2048;
    fused_flags = n68k_rx_verify_sum(&packet, carried_sum(buf6, frame), frame,
                                     &fused_drop);

    snprintf(label, sizeof label, "fuzz %lu: entries agree", round);
    ck(label, walk_flags == fused_flags && walk_drop == fused_drop);

    snprintf(label, sizeof label, "fuzz %lu: dropped a good frame", round);
    ck(label, (walk_drop == NX_FALSE) || (corrupt != 0));

    switch (upper.protocol)
    {
    case NX_PROTOCOL_TCP:
        want = NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM;    break;
    case NX_PROTOCOL_UDP:
        want = NX_INTERFACE_CAPABILITY_UDP_RX_CHECKSUM;    break;
    default:
        want = NX_INTERFACE_CAPABILITY_ICMPV6_RX_CHECKSUM; break;
    }

    snprintf(label, sizeof label, "fuzz %lu: cleared a bad frame", round);
    ck(label, (walk_flags == 0UL) || (corrupt == 0));

    snprintf(label, sizeof label, "fuzz %lu: named the right protocol", round);
    ck(label, (walk_flags == 0UL) || (walk_flags == want));
}

int main(void)
{
ULONG   frame;
ULONG   v4_bits = NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM |
                  NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM;
#ifdef FEATURE_NX_IPV6
ULONG   v6_bits = NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM;
#endif

    /* ---- IPv4 ------------------------------------------------------------ */

    frame = build_v4(v4_ihl + v4_payload);
    both("v4 clean", buf4, frame, frame, NX_FALSE, v4_bits);

    frame = build_v4(v4_ihl + v4_payload);
    buf4[8] ^= 0xFF;                         /* ttl: inside the header sum */
    both("v4 header corrupt", buf4, frame, frame, NX_TRUE, 0UL);

    frame = build_v4(v4_ihl + v4_payload);
    buf4[v4_ihl + 25] ^= 0xFF;
    both("v4 payload corrupt", buf4, frame, frame, NX_TRUE, 0UL);

    /* The length field claims more than the buffer holds.  Neither entry may
       drop it and neither may claim the transport; the header still checks. */
    frame = build_v4(v4_ihl + v4_payload + 100);
    both("v4 length overclaims", buf4, v4_ihl + v4_payload,
         v4_ihl + v4_payload, NX_FALSE,
         NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM);

#ifdef FEATURE_NX_IPV6

    /* ---- IPv6 ------------------------------------------------------------ */

    frame = build_v6(v6_payload, NX_PROTOCOL_TCP);
    both("v6 clean", buf6, frame, frame, NX_FALSE, v6_bits);

    /* No IPv6 header checksum exists, so a corrupted address is caught by the
       pseudo header and nowhere else.  It must still be caught. */
    frame = build_v6(v6_payload, NX_PROTOCOL_TCP);
    buf6[9] ^= 0xFF;
    both("v6 header corrupt", buf6, frame, frame, NX_TRUE, 0UL);

    frame = build_v6(v6_payload, NX_PROTOCOL_TCP);
    buf6[40 + 25] ^= 0xFF;
    both("v6 payload corrupt", buf6, frame, frame, NX_TRUE, 0UL);

    frame = build_v6(v6_payload + 100, NX_PROTOCOL_TCP);
    both("v6 length overclaims", buf6, 40 + v6_payload, 40 + v6_payload,
         NX_FALSE, 0UL);

    /* Ethernet padding: the copy carried more than the datagram, so the fused
       lane must fall back rather than fold the padding in. */
    frame = build_v6(v6_payload, NX_PROTOCOL_TCP);
    both("v6 padded frame", buf6, frame + 8, frame + 8, NX_FALSE, v6_bits);

    /* ---- the extension header chain -------------------------------------- */

    {
    V6Upper tcp  = { NX_PROTOCOL_TCP, 0 };
    V6Upper icmp = { NX_PROTOCOL_ICMPV6, 0 };
    V6Ext   chain[10];
    ULONG   icmp_bits = NX_INTERFACE_CAPABILITY_ICMPV6_RX_CHECKSUM;
    UINT    i;

        tcp.bytes  = v6_payload;
        icmp.bytes = 24;

        /* Hop-by-hop: nothing in it moves the pseudo header, so it is walked
           and the transport behind it is verified. */
        chain[0] = tlv8(0);
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 hop-by-hop walked", buf6, frame, frame, NX_FALSE, v6_bits);

        /* And the same frame with its payload corrupted must still drop: a
           walked chain must not turn a bad checksum into a good one. */
        chain[0] = tlv8(0);
        frame = build_v6x(chain, 1, tcp, 0);
        buf6[48 + 25] ^= 0xFF;
        both("v6 hop-by-hop, payload corrupt", buf6, frame, frame,
             NX_TRUE, 0UL);

        /* Destination options, padding only. */
        chain[0] = tlv8(60);
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 dest options walked", buf6, frame, frame, NX_FALSE, v6_bits);

        /*
         * Destination options carrying a home address option.  RFC 6275 says
         * the pseudo header then carries the home address, not the source
         * address in the frame, so the only right answer is to decline --
         * and NOT to drop, which is what a verifier that summed it anyway
         * would end up doing.
         */
        memset(&chain[0], 0, sizeof chain[0]);
        chain[0].type    = 60;
        chain[0].bytes   = 24;
        chain[0].tail[0] = 2;                /* (2 + 1) * 8 = 24 */
        chain[0].tail[1] = 201;              /* Home Address */
        chain[0].tail[2] = 16;
        for (i = 0; i < 16; i++)
            chain[0].tail[3 + i] = (UCHAR)(0x20 + i);
        chain[0].tail[19] = 1;               /* PadN */
        chain[0].tail[20] = 1;
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 home address option declined", buf6, frame, frame,
             NX_FALSE, 0UL);

        /* An option whose length runs off the end of its header. */
        memset(&chain[0], 0, sizeof chain[0]);
        chain[0].type    = 60;
        chain[0].bytes   = 8;
        chain[0].tail[0] = 0;
        chain[0].tail[1] = 1;                /* PadN */
        chain[0].tail[2] = 40;               /*   claiming forty bytes */
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 option length lies", buf6, frame, frame, NX_FALSE, 0UL);

        /* Routing with no segments left: this host is the destination the
           sender summed, so the chain is walked. */
        memset(&chain[0], 0, sizeof chain[0]);
        chain[0].type    = 43;
        chain[0].bytes   = 8;
        chain[0].tail[0] = 0;
        chain[0].tail[1] = 4;                /* routing type */
        chain[0].tail[2] = 0;                /* segments left */
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 routing, no segments left", buf6, frame, frame,
             NX_FALSE, v6_bits);

        /* With segments left the destination in the frame is a waypoint and
           the sender summed a different address. */
        chain[0].tail[2] = 2;
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 routing, segments left", buf6, frame, frame, NX_FALSE, 0UL);

        /*
         * An atomic fragment: offset zero, no more fragments, so this frame
         * is the whole datagram and its checksum covers exactly it.  tail[0]
         * is the header's reserved byte, tail[1] and tail[2] the offset and
         * flags, tail[3] onward the identification.
         */
        memset(&chain[0], 0, sizeof chain[0]);
        chain[0].type    = 44;
        chain[0].bytes   = 8;
        chain[0].tail[1] = 0x00;             /* offset 0 */
        chain[0].tail[2] = 0x00;             /*   and no more fragments */
        chain[0].tail[3] = 0x11;             /* identification */
        chain[0].tail[4] = 0x22;
        chain[0].tail[5] = 0x33;
        chain[0].tail[6] = 0x44;
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 atomic fragment walked", buf6, frame, frame,
             NX_FALSE, v6_bits);

        /* The reserved bits between the offset and the flag must not turn an
           atomic fragment into a fragmented one. */
        chain[0].tail[2] = 0x06;             /* both reserved bits set */
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 atomic fragment, reserved set", buf6, frame, frame,
             NX_FALSE, v6_bits);

        /* The first fragment of a real one: the checksum covers the whole
           reassembled datagram, which one frame cannot see. */
        chain[0].tail[2] = 0x01;             /* more fragments */
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 first fragment declined", buf6, frame, frame, NX_FALSE, 0UL);

        /* And a later one: offset 185, the second frame of a 1480-byte
           first. */
        chain[0].tail[1] = 0x05;             /* 185 << 3 = 0x05C8 */
        chain[0].tail[2] = 0xC8;
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 later fragment declined", buf6, frame, frame, NX_FALSE, 0UL);

        /* Authentication: the transport checksum is an ordinary one, and the
           header's length is in four-byte units, which is the arithmetic this
           case is here to pin. */
        memset(&chain[0], 0, sizeof chain[0]);
        chain[0].type    = 51;
        chain[0].bytes   = 24;
        chain[0].tail[0] = 4;                /* (4 + 2) * 4 = 24 */
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 authentication header walked", buf6, frame, frame,
             NX_FALSE, v6_bits);

        /* ESP: the transport header is inside the encrypted payload. */
        memset(&chain[0], 0, sizeof chain[0]);
        chain[0].type  = 50;
        chain[0].bytes = 8;
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 ESP declined", buf6, frame, frame, NX_FALSE, 0UL);

        /* No next header: there is no transport to check. */
        memset(&chain[0], 0, sizeof chain[0]);
        chain[0].type  = 59;
        chain[0].bytes = 8;
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 no-next-header declined", buf6, frame, frame, NX_FALSE, 0UL);

        /* The mobility header is an upper layer this file does not check. */
        tcp.protocol = 135;
        frame = build_v6x(chain, 0, tcp, 0);
        both("v6 mobility header declined", buf6, frame, frame, NX_FALSE, 0UL);
        tcp.protocol = NX_PROTOCOL_TCP;

        /* Three headers in a row. */
        chain[0] = tlv8(0);
        memset(&chain[1], 0, sizeof chain[1]);
        chain[1].type    = 43;
        chain[1].bytes   = 8;
        chain[1].tail[1] = 4;
        chain[2] = tlv8(60);
        frame = build_v6x(chain, 3, tcp, 0);
        both("v6 three headers walked", buf6, frame, frame, NX_FALSE, v6_bits);

        /* Nine, which is past the bound this file walks. */
        for (i = 0; i < 9; i++)
            chain[i] = tlv8((UCHAR)((i == 0) ? 0 : 60));
        frame = build_v6x(chain, 9, tcp, 0);
        both("v6 chain too long declined", buf6, frame, frame, NX_FALSE, 0UL);

        /* A chain the payload length cannot hold: the header claims 72 bytes
           and the datagram has room for eight. */
        memset(&chain[0], 0, sizeof chain[0]);
        chain[0].type    = 0;
        chain[0].bytes   = 8;
        chain[0].tail[0] = 8;                /* (8 + 1) * 8 = 72 */
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 truncated chain declined", buf6, frame, frame, NX_FALSE, 0UL);

        /* ---- ICMPv6, which the stack has a capability bit for ------------ */

        frame = build_v6x(chain, 0, icmp, 0);
        both("v6 icmpv6 echo", buf6, frame, frame, NX_FALSE, icmp_bits);

        frame = build_v6x(chain, 0, icmp, 0);
        buf6[40 + 12] ^= 0xFF;
        both("v6 icmpv6 payload corrupt", buf6, frame, frame, NX_TRUE, 0UL);

        /* A neighbour solicitation, which is what the fused path exists for:
           it is the message an IPv6 host sees most of. */
        icmp.bytes = 32;                     /* 8 + target + 8 of option */
        frame = build_v6x(chain, 0, icmp, 0);
        buf6[40] = 135;                      /* neighbour solicitation */
        {
        UCHAR  addr[32];
        USHORT sum;

            memcpy(addr, v6src, 16);
            memcpy(addr + 16, v6dst, 16);
            buf6[42] = 0; buf6[43] = 0;
            sum = ref_sum(&buf6[40], icmp.bytes, addr, 32, NX_PROTOCOL_ICMPV6);
            buf6[42] = (UCHAR)(sum >> 8);
            buf6[43] = (UCHAR)(sum & 0xFF);
        }
        both("v6 neighbour solicitation", buf6, frame, frame,
             NX_FALSE, icmp_bits);

        /* A hop-by-hop header ahead of ICMPv6, which is what a multicast
           listener report looks like. */
        icmp.bytes = 24;
        chain[0] = tlv8(0);
        chain[0].tail[1] = 5;                /* router alert */
        chain[0].tail[2] = 2;
        frame = build_v6x(chain, 1, icmp, 0);
        both("v6 hop-by-hop then icmpv6", buf6, frame, frame,
             NX_FALSE, icmp_bits);

        /* An extension header and Ethernet padding together: the fused lane
           must fall back to the walk, not fold the padding into the sum. */
        chain[0] = tlv8(0);
        frame = build_v6x(chain, 1, tcp, 0);
        both("v6 hop-by-hop, padded frame", buf6, frame + 6, frame + 6,
             NX_FALSE, v6_bits);
    }

#endif /* FEATURE_NX_IPV6 */

#ifdef FEATURE_NX_IPV6
    {
    unsigned long r;

        for (r = 0; r < 20000UL; r++)
            fuzz_one(r);

        printf("v6 chain fuzz                      %lu rounds\n", r);
    }
#endif

    printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures ? 1 : 0;
}
