/*
 * AmiNetXDuo, host check for src/net68k/n68k_rx_verify.c.
 *
 * The drill says clean frames round trip and corrupt ones are not dropped,
 * which means the verifier is answering "verified" for a payload it should
 * reject.  That is a pure function of an IPv4 frame in memory, so it belongs
 * on the host where it can be stepped rather than in an emulator.
 *
 * A frame is built by hand, the transport checksum filled in the way a sender
 * would, and the verifier asked about it before and after one byte is
 * flipped.
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
   segment, exactly as RFC 793 states it. */
static USHORT ref_sum(const UCHAR *seg, ULONG len, ULONG src, ULONG dst,
                      ULONG proto)
{
ULONG   sum = 0;
ULONG   i;

    sum += (src >> 16) & 0xFFFFUL;
    sum += src & 0xFFFFUL;
    sum += (dst >> 16) & 0xFFFFUL;
    sum += dst & 0xFFFFUL;
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

int main(void)
{
static UCHAR    buf[256];
NX_PACKET       packet;
ULONG           src = 0xC0A80101UL;         /* 192.168.1.1 */
ULONG           dst = 0xC0A80102UL;         /* 192.168.1.2 */
UINT            ihl = 20;
UINT            payload = 40;               /* 20 TCP header + 20 data */
ULONG           total = ihl + payload;
UCHAR          *ip = buf;
UCHAR          *tcp = buf + ihl;
USHORT          sum;
ULONG           flags;
UINT            drop;
ULONG           i;

    memset(buf, 0, sizeof buf);
    memset(&packet, 0, sizeof packet);

    /* ---- an IPv4 header ------------------------------------------------- */
    ip[0] = 0x45;                            /* v4, ihl 5 */
    ip[2] = (UCHAR)(total >> 8);
    ip[3] = (UCHAR)(total & 0xFF);
    ip[8] = 64;                              /* ttl */
    ip[9] = NX_PROTOCOL_TCP;
    ip[12] = 192; ip[13] = 168; ip[14] = 1; ip[15] = 1;
    ip[16] = 192; ip[17] = 168; ip[18] = 1; ip[19] = 2;

    /* header checksum */
    {
    ULONG hs = 0;
        for (i = 0; i + 1 < ihl; i += 2)
            hs += ((ULONG)ip[i] << 8) | ip[i + 1];
        while (hs >> 16)
            hs = (hs & 0xFFFFUL) + (hs >> 16);
        sum = (USHORT)(~hs & 0xFFFFUL);
        ip[10] = (UCHAR)(sum >> 8);
        ip[11] = (UCHAR)(sum & 0xFF);
    }

    /* ---- a TCP segment --------------------------------------------------- */
    tcp[0] = 0x30; tcp[1] = 0x39;            /* source port 12345 */
    tcp[2] = 0x00; tcp[3] = 0x50;            /* dest port 80 */
    tcp[12] = 0x50;                          /* data offset 5 */
    tcp[13] = 0x10;                          /* ACK */
    for (i = 20; i < payload; i++)
        tcp[i] = (UCHAR)(0xA0 + i);

    sum = ref_sum(tcp, payload, src, dst, NX_PROTOCOL_TCP);
    tcp[16] = (UCHAR)(sum >> 8);
    tcp[17] = (UCHAR)(sum & 0xFF);

    packet.nx_packet_prepend_ptr = ip;
    packet.nx_packet_append_ptr  = ip + total;
    packet.nx_packet_length      = total;
    packet.nx_packet_data_start  = buf;
    packet.nx_packet_data_end    = buf + sizeof buf;

    /* ---- clean ----------------------------------------------------------- */
    drop = 99;
    flags = n68k_rx_verify(&packet, &drop);
    printf("clean:  flags=0x%08lx drop=%u\n", (unsigned long)flags,
           (unsigned)drop);
    ck("clean frame is not dropped", drop == NX_FALSE);
    ck("clean frame claims IPv4",
       (flags & NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM) != 0);
    ck("clean frame claims TCP",
       (flags & NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM) != 0);

    /* ---- one payload byte flipped ---------------------------------------- */
    tcp[25] ^= 0xFF;

    packet.nx_packet_prepend_ptr = ip;
    packet.nx_packet_append_ptr  = ip + total;
    packet.nx_packet_length      = total;

    drop = 99;
    flags = n68k_rx_verify(&packet, &drop);
    printf("corrupt: flags=0x%08lx drop=%u\n", (unsigned long)flags,
           (unsigned)drop);
    ck("corrupt frame IS dropped", drop == NX_TRUE);
    ck("corrupt frame claims nothing", flags == 0UL);

    printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures ? 1 : 0;
}
