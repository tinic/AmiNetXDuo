/*
 * AmiNetXDuo, ICMPv4 error-message quote bound: an error message must quote at
 * most the contiguous bytes the offending datagram actually holds from its IP
 * header, not a fixed `(IHL + 2) * 4` window past its end.
 *
 * A legal minimum IPv4 datagram is a 20-byte header with no payload, so IHL = 5
 * and the fixed window is 28 bytes.  The old code copied all 28, reading 8
 * bytes past the datagram end; a 20-27 byte datagram -- a minimum header or a
 * header plus a few payload bytes -- was over-read back to the sender.  The
 * fix clamps the quote to (append_ptr - ip_header) rounded down to whole
 * ULONGs, so the `-= 4` copy loop never underflows on a non-word-aligned
 * length.  This drives the real _nx_icmpv4_send_error_message directly as an
 * error-builder contract: the 20-27 byte inputs are synthetic clamp exercises,
 * not datagrams the receive path is claimed to emit.  It asserts the quoted
 * length (nx_packet_length - sizeof(NX_ICMPV4_ERROR)).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_ipv4.h"
#include "nx_icmpv4.h"

#include <stdio.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

static void h_check(int ok, const char *what)
{
    h_checks++;

    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}

/* ---- stubs for the four calls the error builder makes ---- */

static UCHAR    h_alloc_buf[128];
static NX_PACKET h_pkt;

UINT _nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                         ULONG packet_type, ULONG wait_option)
{
    (void)pool_ptr; (void)packet_type; (void)wait_option;

    memset(&h_pkt, 0, sizeof(h_pkt));
    h_pkt.nx_packet_prepend_ptr = h_alloc_buf;
    h_pkt.nx_packet_append_ptr  = h_alloc_buf;
    *packet_ptr = &h_pkt;

    return NX_SUCCESS;
}

ULONG _nx_ip_route_find(NX_IP *ip_ptr, ULONG destination_address,
                        NX_INTERFACE **nx_ip_interface, ULONG *next_hop_address)
{
    (void)ip_ptr; (void)destination_address; (void)nx_ip_interface;

    *next_hop_address = 0;

    return 0;
}

USHORT _nx_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol,
                               UINT data_length, ULONG *_src_ip_addr,
                               ULONG *_dest_ip_addr)
{
    (void)packet_ptr; (void)protocol; (void)data_length;
    (void)_src_ip_addr; (void)_dest_ip_addr;

    return 0;
}

void _nx_ip_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                        ULONG destination_ip, ULONG type_of_service,
                        ULONG time_to_live, ULONG protocol, ULONG fragment,
                        ULONG next_hop_address)
{
    (void)ip_ptr; (void)packet_ptr; (void)destination_ip;
    (void)type_of_service; (void)time_to_live; (void)protocol;
    (void)fragment; (void)next_hop_address;
}

static void h_icmpv4_process(NX_IP *ip_ptr, NX_PACKET *packet)
{
    (void)ip_ptr; (void)packet;
}

/* Build a minimum, unicast, non-broadcast, non-fragment IPv4 datagram of
   `offending_len` bytes (header + whatever payload), hand it to the error
   builder, and return the length of the quote the builder produced. */
static ULONG h_quote(ULONG offending_len)
{
    NX_IP        ip;
    NX_INTERFACE iface;
    NX_PACKET    offending;
    ULONG        hdr[20];                 /* 80 bytes, ULONG-aligned */
    NX_IPV4_HEADER *ih = (NX_IPV4_HEADER *)hdr;

    memset(&ip, 0, sizeof(ip));
    memset(&iface, 0, sizeof(iface));
    memset(&offending, 0, sizeof(offending));
    memset(hdr, 0, sizeof(hdr));

    /* IHL = 5, no fragments, unicast source and destination. */
    ih->nx_ip_header_word_0           = (5UL << 24);
    ih->nx_ip_header_word_1           = 0;
    ih->nx_ip_header_source_ip        = 0x0A000001UL;  /* 10.0.0.1 */
    ih->nx_ip_header_destination_ip   = 0x0A000002UL;  /* 10.0.0.2 */

    iface.nx_interface_ip_address      = 0x0A00000AUL;  /* 10.0.0.10 */
    iface.nx_interface_ip_network_mask = 0xFFFFFF00UL;
    iface.nx_interface_ip_network      = 0x0A000000UL;

    ip.nx_ip_icmpv4_packet_process = h_icmpv4_process;
    ip.nx_ip_default_packet_pool   = (NX_PACKET_POOL *)&ip;   /* stub ignores */

    offending.nx_packet_ip_header   = (UCHAR *)hdr;
    offending.nx_packet_append_ptr  = (UCHAR *)hdr + offending_len;
    offending.nx_packet_address.nx_packet_interface_ptr = &iface;

    _nx_icmpv4_send_error_message(&ip, &offending,
                                  (ULONG)(NX_ICMP_DEST_UNREACHABLE_TYPE << 24), 0);

    return h_pkt.nx_packet_length - (ULONG)sizeof(NX_ICMPV4_ERROR);
}

int main(void)
{
    /* IHL = 5 makes the fixed window 28 bytes.  For every offending datagram
       length below, the quote must be min(28, length rounded down to a whole
       ULONG), so it never exceeds the contiguous bytes the datagram holds. */
    static const struct { ULONG len; ULONG quoted; } cases[] = {
        {20, 20}, {21, 20}, {22, 20}, {23, 20},   /* over-read range */
        {24, 24}, {25, 24}, {26, 24}, {27, 24},   /* over-read range */
        {28, 28}, {29, 28},                       /* the 28-byte boundary */
        {40, 28}, {64, 28},                       /* longer: full quote */
    };
    unsigned int i;

    printf("ICMPv4 error-message quote bound, direct error-builder contract\n");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        char what[80];

        snprintf(what, sizeof(what),
                 "a %lu-byte datagram quotes %lu bytes, never more than it holds",
                 (unsigned long)cases[i].len, (unsigned long)cases[i].quoted);
        h_check(h_quote(cases[i].len) == cases[i].quoted, what);
    }

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
