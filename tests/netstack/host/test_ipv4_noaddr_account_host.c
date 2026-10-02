/*
 * AmiNetXDuo, IPv4 no-address receive arm accounting (N-019): a packet that is
 * not "for us" but arrives on an interface with no IP address must not
 * decrement counters it was never credited.
 *
 * The no-address arm decremented nx_ip_total_packets_delivered /
 * nx_ip_total_bytes_received unconditionally, but only UDP packets get the
 * matching increment in that arm, so any non-UDP packet (ICMP, TCP, ...)
 * wrapped both counters to ULONG_MAX on an addressless interface.  The fix
 * moves the decrement into the UDP block after the dest_port == 68 dispatch,
 * so non-UDP is net-zero (no increment, no decrement) and UDP stays net-zero
 * (increment then decrement) unless it is a dispatched DHCP message.
 *
 * Drives the real _nx_ipv4_packet_receive with a minimal NX_IP/NX_PACKET and a
 * hand-built IPv4 datagram whose destination is a unicast address the zero-IP
 * interface does not own (so the "for us" test fails and control reaches the
 * no-address arm), then asserts the counters do not wrap and the packet is
 * released exactly once.  Fails on the old source (delivered/bytes wrap),
 * passes on the fixed source.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_ipv4.h"

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

/* ---- stubs for the IPv4 receive leaf's other call-outs ---- */

static unsigned long h_releases;

VOID _nx_packet_release(NX_PACKET *packet_ptr)
{
    (void)packet_ptr;
    h_releases++;
}

USHORT _nx_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol,
                               UINT data_length, ULONG *_src_ip_addr,
                               ULONG *_dest_ip_addr)
{
    (void)packet_ptr;
    (void)protocol;
    (void)data_length;
    (void)_src_ip_addr;
    (void)_dest_ip_addr;

    /* The receive path accepts the header only when
       (~_nx_ip_checksum_compute(...) & NX_LOWER_16_MASK) == 0, i.e. the
       one's-complement sum over a valid header is all-ones. */
    return NX_LOWER_16_MASK;
}

UINT _nx_ipv4_option_process(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    (void)ip_ptr;
    (void)packet_ptr;

    return NX_TRUE;
}

UINT _nx_igmp_multicast_check(NX_IP *ip_ptr, ULONG group_address,
                              NX_INTERFACE *nx_interface)
{
    (void)ip_ptr;
    (void)group_address;
    (void)nx_interface;

    return NX_FALSE;   /* 10.0.0.5 is not a joined multicast address */
}

UINT _nx_ip_dispatch_process(NX_IP *ip_ptr, NX_PACKET *packet_ptr, UINT protocol)
{
    (void)ip_ptr;
    (void)packet_ptr;
    (void)protocol;

    return 0;
}

UINT _tx_event_flags_set(TX_EVENT_FLAGS_GROUP *group_ptr, ULONG flags_to_set,
                         UINT set_option)
{
    (void)group_ptr;
    (void)flags_to_set;
    (void)set_option;

    return TX_SUCCESS;
}

UINT _tx_thread_interrupt_disable(void)
{
    return 0;
}

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    (void)previous_posture;
}

/* Drive _nx_ipv4_packet_receive once with counters cleared, a datagram destined
   to 10.0.0.5 (a unicast the zero-IP interface does not own), and the given
   IPv4 protocol byte.  For UDP the builder adds a UDP header whose destination
   port is udp_dest_port; for other protocols it sends a bare header + 4 bytes.
   Returns the resulting counter values and release count. */
static void h_run(UCHAR protocol_byte, UINT udp_dest_port,
                  unsigned long *out_delivered, unsigned long *out_bytes,
                  unsigned long *out_invalid, unsigned long *out_dropped,
                  unsigned long *out_releases)
{
    NX_IP        ip;
    NX_INTERFACE iface;
    NX_PACKET    packet;
    UCHAR        buf[32];
    ULONG        total = (protocol_byte == 17) ? 28UL : 24UL;

    memset(&ip, 0, sizeof(ip));
    memset(&iface, 0, sizeof(iface));
    memset(&packet, 0, sizeof(packet));
    memset(buf, 0, sizeof(buf));

    /* word_0: version 4, IHL 5, total length = `total`. */
    buf[0] = 0x45;
    buf[2] = (UCHAR)(total >> 8);
    buf[3] = (UCHAR)(total & 0xFF);
    /* word_1: identification 0, no fragments (already 0). */
    /* word_2: TTL 64, protocol, checksum 0 (stub reports it valid). */
    buf[8] = 0x40;
    buf[9] = protocol_byte;
    /* source 10.0.0.1, destination 10.0.0.5. */
    buf[12] = 0x0A; buf[13] = 0x00; buf[14] = 0x00; buf[15] = 0x01;
    buf[16] = 0x0A; buf[17] = 0x00; buf[18] = 0x00; buf[19] = 0x05;

    if (protocol_byte == 17)
    {
        /* UDP header after the 20-byte IP header: source 1024, dest port. */
        buf[20] = 0x04; buf[21] = 0x00;
        buf[22] = (UCHAR)(udp_dest_port >> 8);
        buf[23] = (UCHAR)(udp_dest_port & 0xFF);
    }

    iface.nx_interface_ip_address      = 0;
    iface.nx_interface_ip_network_mask = 0xFFFFFF00UL;
    iface.nx_interface_ip_network      = 0x0A000000UL;

    packet.nx_packet_prepend_ptr = buf;
    packet.nx_packet_append_ptr  = buf + total;
    packet.nx_packet_length      = total;
    packet.nx_packet_ip_header   = buf;
    packet.nx_packet_address.nx_packet_interface_ptr = &iface;

    ip.nx_ip_total_packets_delivered = 0;
    ip.nx_ip_total_bytes_received    = 0;
    ip.nx_ip_invalid_receive_address = 0;
    ip.nx_ip_receive_packets_dropped = 0;

    h_releases = 0;

    _nx_ipv4_packet_receive(&ip, &packet);

    *out_delivered = ip.nx_ip_total_packets_delivered;
    *out_bytes     = ip.nx_ip_total_bytes_received;
    *out_invalid   = ip.nx_ip_invalid_receive_address;
    *out_dropped   = ip.nx_ip_receive_packets_dropped;
    *out_releases  = h_releases;
}

int main(void)
{
    unsigned long delivered, bytes, invalid, dropped, releases;

    printf("IPv4 no-address receive arm accounting, direct contract\n");

    /* Non-UDP (ICMP): the arm must not decrement counters it never incremented. */
    h_run(1, 0, &delivered, &bytes, &invalid, &dropped, &releases);
    h_check(delivered == 0, "ICMP on zero-IP interface leaves delivered at zero");
    h_check(bytes == 0, "ICMP on zero-IP interface leaves bytes at zero");
    h_check(invalid == 1, "ICMP on zero-IP interface counts one invalid address");
    h_check(dropped == 1, "ICMP on zero-IP interface counts one drop");
    h_check(releases == 1, "ICMP on zero-IP interface releases exactly once");

    /* Non-UDP (TCP): protocol-agnostic, same result. */
    h_run(6, 0, &delivered, &bytes, &invalid, &dropped, &releases);
    h_check(delivered == 0, "TCP on zero-IP interface leaves delivered at zero");
    h_check(bytes == 0, "TCP on zero-IP interface leaves bytes at zero");
    h_check(invalid == 1, "TCP on zero-IP interface counts one invalid address");
    h_check(dropped == 1, "TCP on zero-IP interface counts one drop");
    h_check(releases == 1, "TCP on zero-IP interface releases exactly once");

    /* UDP non-DHCP (port 53): net-zero (increment then decrement) is preserved. */
    h_run(17, 53, &delivered, &bytes, &invalid, &dropped, &releases);
    h_check(delivered == 0, "UDP/53 on zero-IP interface leaves delivered at zero");
    h_check(bytes == 0, "UDP/53 on zero-IP interface leaves bytes at zero");
    h_check(invalid == 1, "UDP/53 on zero-IP interface counts one invalid address");
    h_check(dropped == 1, "UDP/53 on zero-IP interface counts one drop");
    h_check(releases == 1, "UDP/53 on zero-IP interface releases exactly once");

    printf("%lu checks, %lu failures, %s\n",
           h_checks, h_failures, (h_failures == 0UL) ? "PASS" : "FAIL");

    return (h_failures == 0UL) ? 0 : 1;
}
