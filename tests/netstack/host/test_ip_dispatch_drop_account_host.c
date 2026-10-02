/*
 * AmiNetXDuo, IP dispatcher drop accounting: every packet counted by its
 * front door must be un-counted exactly once when the dispatcher drops it.
 * Two drop paths in _nx_ip_dispatch_process skipped the decrement of
 * nx_ip_total_packets_delivered / nx_ip_total_bytes_received (and the
 * increment of nx_ip_receive_packets_dropped):
 *
 *   1. next-header 50 (ESP) with IPsec off -- the case's #else arm did
 *      `return(1)` and never reached the accounting.
 *   2. an IPv6 extension header that ends exactly at nx_packet_append_ptr --
 *      the advance guard is a strict `<`, so equality sets drop_packet inside
 *      the `if (!drop_packet)` block, whose `else` (the accounting) is then
 *      skipped and the loop exits.
 *
 * This drives the real _nx_ip_dispatch_process with a minimal NX_IP/NX_PACKET
 * and the counters pre-set to what the front door would have counted (one
 * packet, N payload bytes), then asserts the dispatcher rolls them back.  It
 * fails on the old source (counters stay incremented) and passes on the fixed
 * source (delivered/bytes return to zero, dropped increments once).
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_ipv6.h"
#include "nx_icmpv6.h"

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

/* ---- stubs for the dispatcher's other option arms (never reached) ---- */

UINT _nx_ipv6_process_routing_option(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    (void)ip_ptr;
    (void)packet_ptr;
    return NX_NOT_SUCCESSFUL;
}

UINT _nx_ipv6_process_fragment_option(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    (void)ip_ptr;
    (void)packet_ptr;
    return NX_NOT_SUCCESSFUL;
}

UINT _nx_ipv6_option_error(NX_IP *ip_ptr, NX_PACKET *packet_ptr,
                           UCHAR option_type, UINT offset)
{
    (void)ip_ptr;
    (void)packet_ptr;
    (void)option_type;
    (void)offset;
    return NX_SUCCESS;
}

VOID _nx_icmpv6_send_error_message(NX_IP *ip_ptr, NX_PACKET *offending_packet,
                                   ULONG word1, ULONG error_pointer)
{
    (void)ip_ptr;
    (void)offending_packet;
    (void)word1;
    (void)error_pointer;
}

VOID _nx_icmpv4_send_error_message(NX_IP *ip_ptr, NX_PACKET *offending_packet,
                                   ULONG word1, ULONG error_pointer)
{
    (void)ip_ptr;
    (void)offending_packet;
    (void)word1;
    (void)error_pointer;
}

/* ---- a release counter: the dispatcher (IPsec off) must not release ---- */

static unsigned long h_releases;

VOID _nx_packet_release(NX_PACKET *packet_ptr)
{
    (void)packet_ptr;
    h_releases++;
}

/* Drive the dispatcher once with counters pre-set as the front door left
   them, and a packet whose option area is `payload` bytes after the IP header.
   Returns the dispatcher's status and leaves the counters for the caller to
   inspect. */
static UINT h_dispatch(NX_IP *ip_ptr, NX_PACKET *packet_ptr, UINT protocol)
{
    ip_ptr->nx_ip_total_packets_delivered   = 1;
    ip_ptr->nx_ip_total_bytes_received      = packet_ptr->nx_packet_length;
    ip_ptr->nx_ip_receive_packets_dropped   = 0;

    return _nx_ip_dispatch_process(ip_ptr, packet_ptr, protocol);
}

/* Assemble a packet whose IPv6 option area is the 8-octet hop-by-hop header
   `{next=No-Next-Header, HdrExtLen=0, Pad1 x6}`, which ends exactly at
   append_ptr.  The handler accepts it (equality passes its own `<` sanity
   check), then the dispatcher's advance guard drops it. */
static void h_build_option_packet(NX_PACKET *packet_ptr, UCHAR *buf)
{
    UCHAR *hop = buf + sizeof(NX_IPV6_HEADER);

    memset(buf, 0, sizeof(NX_IPV6_HEADER) + 8);
    hop[0] = 59;   /* Next Header: No Next Header */
    hop[1] = 0;    /* Hdr Ext Len = 0 -> (0 + 1) * 8 = 8 octets */

    memset(packet_ptr, 0, sizeof(*packet_ptr));
    packet_ptr->nx_packet_ip_header        = buf;
    packet_ptr->nx_packet_prepend_ptr      = hop;
    packet_ptr->nx_packet_append_ptr       = hop + 8;
    packet_ptr->nx_packet_length           = 8;
    packet_ptr->nx_packet_ip_version       = NX_IP_VERSION_V6;
    packet_ptr->nx_packet_option_state     = 0;
    packet_ptr->nx_packet_destination_header = 0;
    packet_ptr->nx_packet_option_offset    = 0;
}

int main(void)
{
    NX_IP     ip;
    NX_PACKET packet;
    UCHAR     buf[sizeof(NX_IPV6_HEADER) + 8];
    UINT      rc;

    printf("IP dispatcher drop accounting, direct contract\n");
    memset(&ip, 0, sizeof(ip));

    /* Case 1: extension header ending exactly at append_ptr. */
    h_build_option_packet(&packet, buf);
    rc = h_dispatch(&ip, &packet, NX_PROTOCOL_NEXT_HEADER_HOP_BY_HOP);

    h_check(rc == 1, "exact-length extension header must be dropped");
    h_check(ip.nx_ip_total_packets_delivered == 0,
            "delivered must return to zero after an exact-length drop");
    h_check(ip.nx_ip_total_bytes_received == 0,
            "bytes must return to zero after an exact-length drop");
    h_check(ip.nx_ip_receive_packets_dropped == 1,
            "dropped must increment once after an exact-length drop");

    /* Case 2: next-header 50 (ESP) with IPsec off. */
    memset(&packet, 0, sizeof(packet));
    packet.nx_packet_length = 8;
    packet.nx_packet_ip_version = NX_IP_VERSION_V6;
    rc = h_dispatch(&ip, &packet, NX_PROTOCOL_NEXT_HEADER_ENCAP_SECURITY);

    h_check(rc == 1, "ESP with IPsec off must be dropped");
    h_check(ip.nx_ip_total_packets_delivered == 0,
            "delivered must return to zero after an ESP drop");
    h_check(ip.nx_ip_total_bytes_received == 0,
            "bytes must return to zero after an ESP drop");
    h_check(ip.nx_ip_receive_packets_dropped == 1,
            "dropped must increment once after an ESP drop");

    /* The dispatcher performs no release with IPsec off; the single release
       is the caller's, unchanged by this fix. */
    h_check(h_releases == 0, "dispatcher must not release with IPsec off");

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
