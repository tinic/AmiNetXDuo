/*
 * AmiNetXDuo, host check for src/net68k/n68k_rx_verify.c.
 *
 * Content-level: a frame is built by hand, the transport checksum filled in
 * the way a sender would, and both entry points asked about it -- clean, with
 * the header corrupted, with the payload corrupted, and with the length field
 * disagreeing with the buffer.  The two entries must answer identically, or
 * the fused one is accepting something the walk rejects.
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

/* What the copy hook hands the fused entry: the ones-complement longword sum
   of what it copied, end-around carry, starting at the IP header. */
static ULONG carried_sum(const UCHAR *p, ULONG bytes)
{
ULONG   acc = 0;
ULONG   i;

    for (i = 0; i + 3 < bytes; i += 4)
    {
    ULONG w = ((ULONG)p[i] << 24) | ((ULONG)p[i + 1] << 16) |
              ((ULONG)p[i + 2] << 8) | (ULONG)p[i + 3];

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

    /* An extension header is refused outright: the transport header is not at
       offset 40 and this file does not walk the chain. */
    frame = build_v6(v6_payload, 44);        /* fragment header */
    both("v6 extension header refused", buf6, frame, frame, NX_FALSE, 0UL);

    frame = build_v6(v6_payload, 0);         /* hop-by-hop */
    both("v6 hop-by-hop refused", buf6, frame, frame, NX_FALSE, 0UL);

    /* Ethernet padding: the copy carried more than the datagram, so the fused
       lane must fall back rather than fold the padding in. */
    frame = build_v6(v6_payload, NX_PROTOCOL_TCP);
    both("v6 padded frame", buf6, frame + 8, frame + 8, NX_FALSE, v6_bits);

#endif /* FEATURE_NX_IPV6 */

    printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures ? 1 : 0;
}
