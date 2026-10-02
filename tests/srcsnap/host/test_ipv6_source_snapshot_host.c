/*
 * Audit N-039: an IPv6 source chosen without nx_ip_protection and used with
 * it.  _nxd_ipv6_address_delete zeroes an entry under the mutex; a UDP send or
 * a TCP connect that chose that entry before must refuse cleanly instead of
 * sending from a zeroed or reassigned address, or reaching NX_ASSERT in
 * _nx_ipv6_packet_send / the connect's NX_ASSERT(outgoing_interface), which on
 * the target sleep forever holding the mutex.
 *
 * udp_capture:   the entry is already zeroed when the send chooses it.
 * udp_window:    zeroed while the checksum runs (the unlocked stretch).
 * udp_reused:    given another address on the same interface in that stretch.
 * udp_live:      control: sent once, from the snapshot address.
 * udp_stamped_window / udp_stamped_reused: a named source, stamped on the
 *                packet by _nxd_udp_socket_source_send with a valid index,
 *                zeroed or reassigned while the checksum runs.  The stamp
 *                is the one _nxd_udp_socket_source_send sets before it
 *                delegates: the refusal leaves it as it was at
 *                _nxd_udp_socket_send entry.  It does not restore the API
 *                caller's NULL; both transfer.c callers release the packet
 *                on any refusal.
 * udp_index:     a named-source index past the table is refused (the target
 *                is built with -fsanitize=bounds trapping, so indexing past
 *                nx_ipv6_address[] stops the test).
 * tcp_window:    the connect's chosen entry is zeroed before it takes the mutex.
 * tcp_reused:    or given another address on the same interface.
 * tcp_index:     a named-source index past the table is refused.
 * tcp_live:      control: the SYN goes out, SYN_SENT, NX_IN_PROGRESS.
 *
 * On every refusal: NX_NO_INTERFACE_ADDRESS, nothing sent, no packet release,
 * the packet's prepend pointer, length and address stamp as they were at
 * _nxd_udp_socket_send entry (for the stamped arms, the stamp
 * _nxd_udp_socket_source_send set), the UDP counters unchanged, and no mutex
 * hold left.  The connect leaves the
 * socket CLOSED with no source pointer and does not suspend.  The deletion is
 * done by a stub at a fixed point that asserts the mutex is not held there:
 * the window is real, and the test does not race.
 *
 * Linked for real: nxd_udp_socket_send.c, nxd_udp_socket_source_send.c and
 * nxd_tcp_client_socket_connect.c.  Stubbed: ThreadX, the source lookup, the
 * checksum, the IP send, the SYN send and the port bind helpers.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tx_api.h"
#include "tx_thread.h"
#include "nx_api.h"
#include "nx_ip.h"
#include "nx_ipv6.h"
#include "nx_udp.h"
#include "nx_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TX_THREAD      *_tx_thread_current_ptr;
volatile ULONG  _tx_thread_system_state;

static NX_IP        ip;
static NX_INTERFACE iface;
static int          held;
static int          bad;

UINT _tx_thread_interrupt_disable(VOID)       { return 0; }
VOID _tx_thread_interrupt_restore(UINT p)     { (void) p; }
static void window(void);
static int  window_at_mutex;
static UINT mget(TX_MUTEX *m)
{
    if (m == &ip.nx_ip_protection)
    {
        /* The connect's unlocked stretch ends at its first take.  */
        if (window_at_mutex && held == 0)
        {
            window_at_mutex = 0;
            window();
        }
        held++;
    }
    return TX_SUCCESS;
}
static UINT mput(TX_MUTEX *m) { if (m == &ip.nx_ip_protection) held--; return TX_SUCCESS; }
UINT _tx_mutex_get(TX_MUTEX *m, ULONG w)      { (void) w; return mget(m); }
UINT _tx_mutex_put(TX_MUTEX *m)               { return mput(m); }
UINT _txe_mutex_get(TX_MUTEX *m, ULONG w)     { (void) w; return mget(m); }
UINT _txe_mutex_put(TX_MUTEX *m)              { return mput(m); }

/* NX_ASSERT_FAIL is tx_thread_sleep(NX_WAIT_FOREVER) in a loop.  */
static UINT assert_sleep(void)
{
    printf("FAIL NX_ASSERT fired: on the target this sleeps forever holding the mutex\n");
    exit(1);
}
UINT _tx_thread_sleep(ULONG t)  { (void) t; return assert_sleep(); }
UINT _txe_thread_sleep(ULONG t) { (void) t; return assert_sleep(); }

/* What the deletion stub does to slot 1, and when.  */
enum { NONE, ZERO, REUSE };
static int  window_action;
static int  window_held = -1;

static void slot1_make(void)
{
    memset(&ip.nx_ipv6_address[1], 0, sizeof(ip.nx_ipv6_address[1]));
    ip.nx_ipv6_address[1].nxd_ipv6_address_valid    = NX_TRUE;
    ip.nx_ipv6_address[1].nxd_ipv6_address_state    = NX_IPV6_ADDR_STATE_VALID;
    ip.nx_ipv6_address[1].nxd_ipv6_address_attached = &iface;
    ip.nx_ipv6_address[1].nxd_ipv6_address_index    = 1;
    ip.nx_ipv6_address[1].nxd_ipv6_address[0]       = 0x20010db8UL;
    ip.nx_ipv6_address[1].nxd_ipv6_address[3]       = 0x11UL;
}

static void window(void)
{
    if (window_action == NONE)
        return;
    window_held = held;
    if (window_action == ZERO)
    {
        /* What nxd_ipv6_address_delete.c leaves.  */
        memset(&ip.nx_ipv6_address[1], 0, sizeof(ip.nx_ipv6_address[1]));
        ip.nx_ipv6_address[1].nxd_ipv6_address_index = 1;
    }
    else
    {
        /* Deleted and set again for another address on the same interface.  */
        ip.nx_ipv6_address[1].nxd_ipv6_address[3] = 0x22UL;
    }
    window_action = NONE;
}

UINT _nxd_ipv6_interface_find(NX_IP *ip_ptr, ULONG *dest_address, NXD_IPV6_ADDRESS **ipv6_addr,
                              NX_INTERFACE *if_ptr)
{
    (void) ip_ptr; (void) dest_address; (void) if_ptr;
    *ipv6_addr = &ip.nx_ipv6_address[1];
    return NX_SUCCESS;
}

USHORT _nx_ip_checksum_compute(NX_PACKET *packet_ptr, ULONG protocol, UINT data_length,
                               ULONG *src_ip_addr, ULONG *dest_ip_addr)
{
    (void) packet_ptr; (void) protocol; (void) data_length; (void) src_ip_addr; (void) dest_ip_addr;
    window();
    return 0x1234;
}

static int   sends;
static ULONG sent_src3;
VOID _nx_ipv6_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr, ULONG protocol, ULONG payload_size,
                          ULONG hop_limit, ULONG traffic_class, ULONG *src_address, ULONG *dest_address)
{
    (void) ip_ptr; (void) protocol; (void) payload_size; (void) hop_limit; (void) traffic_class;
    (void) dest_address;
    /* The real one: if_ptr = ...->nxd_ipv6_address_attached; NX_ASSERT(if_ptr).  */
    if (packet_ptr -> nx_packet_address.nx_packet_ipv6_address_ptr -> nxd_ipv6_address_attached == NX_NULL)
        assert_sleep();
    sent_src3 = src_address[3];
    sends++;
}
VOID _nx_ip_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr, ULONG d, ULONG t, ULONG l, ULONG p, ULONG f, ULONG n)
{
    (void) ip_ptr; (void) packet_ptr; (void) d; (void) t; (void) l; (void) p; (void) f; (void) n;
    sends++;
}
UINT _nx_ip_route_find(NX_IP *ip_ptr, ULONG d, NX_INTERFACE **i, ULONG *n)
{
    (void) ip_ptr; (void) d; (void) i; (void) n;
    return NX_IP_ADDRESS_ERROR;
}

static int released;
UINT _nx_packet_release(NX_PACKET *p)          { (void) p; released++; return NX_SUCCESS; }
UINT _nx_packet_transmit_release(NX_PACKET *p) { (void) p; released++; return NX_SUCCESS; }

/* The connect, past the recheck: the SYN goes out and a NX_NO_WAIT connect
   returns NX_IN_PROGRESS.  */
static int syns;
VOID _nx_tcp_packet_send_syn(NX_TCP_SOCKET *s, ULONG seq) { (void) s; (void) seq; syns++; }
VOID _nx_tcp_socket_thread_suspend(TX_THREAD **h, VOID (*c)(TX_THREAD * NX_CLEANUP_PARAMETER), NX_TCP_SOCKET *s,
                                   TX_MUTEX *m, ULONG w)
{
    (void) h; (void) c; (void) s; (void) m; (void) w;
    printf("FAIL the connect suspended\n");
    bad = 1;
}
VOID _nx_tcp_connect_cleanup(TX_THREAD *t NX_CLEANUP_PARAMETER) { (void) t; NX_CLEANUP_EXTENSION }
ULONG _tx_time_get(VOID) { return 1000; }
VOID  _nx_tcp_socket_receive_queue_flush(NX_TCP_SOCKET *s) { (void) s; }
ULONG _nx_tcp_transmit_timer_rate = 1;

static void check(const char *what, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    bad |= !cond;
}

static NX_UDP_SOCKET udp;
static NX_TCP_SOCKET tcp;
static NX_PACKET     pkt;
static UCHAR         buf[256];
static NXD_ADDRESS   dest;

static void setup(void)
{
    memset(&ip, 0, sizeof(ip));
    memset(&iface, 0, sizeof(iface));
    memset(&udp, 0, sizeof(udp));
    memset(&tcp, 0, sizeof(tcp));
    memset(&pkt, 0, sizeof(pkt));
    held = sends = released = syns = 0;
    window_action = NONE;
    window_at_mutex = 0;
    window_held = -1;
    iface.nx_interface_valid    = NX_TRUE;
    iface.nx_interface_link_up  = NX_TRUE;
    iface.nx_interface_ip_mtu_size = 1500;
    slot1_make();
    ip.nx_ipv6_hop_limit = 64;

    udp.nx_udp_socket_id         = NX_UDP_ID;
    udp.nx_udp_socket_ip_ptr     = &ip;
    udp.nx_udp_socket_bound_next = &udp;
    udp.nx_udp_socket_port       = 5353;
    udp.nx_udp_socket_time_to_live = 64;

    pkt.nx_packet_data_start  = buf;
    pkt.nx_packet_data_end    = buf + sizeof(buf);
    pkt.nx_packet_prepend_ptr = buf + 128;
    pkt.nx_packet_append_ptr  = buf + 128 + 20;
    pkt.nx_packet_length      = 20;

    memset(&dest, 0, sizeof(dest));
    dest.nxd_ip_version = NX_IP_VERSION_V6;
    dest.nxd_ip_address.v6[0] = 0x20010db8UL;
    dest.nxd_ip_address.v6[3] = 0x99UL;
}

/* A refused UDP send gives everything back.  */
static void udp_refused(UINT status, NXD_IPV6_ADDRESS *stamp_in)
{
    check("refused with NX_NO_INTERFACE_ADDRESS", status == NX_NO_INTERFACE_ADDRESS);
    check("nothing sent, no packet released", sends == 0 && released == 0);
    check("prepend pointer and length as at _nxd_udp_socket_send entry",
          pkt.nx_packet_prepend_ptr == buf + 128 && pkt.nx_packet_length == 20);
    check("the address stamp as at _nxd_udp_socket_send entry",
          pkt.nx_packet_address.nx_packet_ipv6_address_ptr == stamp_in);
    check("UDP counters unchanged",
          ip.nx_ip_udp_packets_sent == 0 && ip.nx_ip_udp_bytes_sent == 0 &&
          udp.nx_udp_socket_packets_sent == 0 && udp.nx_udp_socket_bytes_sent == 0);
    check("no mutex hold left", held == 0);
}

static void tcp_setup(void)
{
    tcp.nx_tcp_socket_id         = NX_TCP_ID;
    tcp.nx_tcp_socket_ip_ptr     = &ip;
    tcp.nx_tcp_socket_bound_next = &tcp;
    tcp.nx_tcp_socket_state      = NX_TCP_CLOSED;
    tcp.nx_tcp_socket_client_type = NX_TRUE;
    tcp.nx_tcp_socket_rx_window_default = 8192;
    tcp.nx_tcp_socket_mss = 1440;
}

static void tcp_refused(UINT status)
{
    check("connect refused with NX_NO_INTERFACE_ADDRESS", status == NX_NO_INTERFACE_ADDRESS);
    check("the socket is still CLOSED, no source pointer, no SYN",
          tcp.nx_tcp_socket_state == NX_TCP_CLOSED && tcp.nx_tcp_socket_ipv6_addr == NX_NULL && syns == 0);
    check("no mutex hold left", held == 0);
}

int main(int argc, char **argv)
{
    UINT        status;
    const char *a;

    if (argc != 2)
        return 2;
    a = argv[1];
    setup();

    if (strcmp(a, "udp_capture") == 0)
    {
        memset(&ip.nx_ipv6_address[1], 0, sizeof(ip.nx_ipv6_address[1]));
        ip.nx_ipv6_address[1].nxd_ipv6_address_index = 1;
        status = _nxd_udp_socket_send(&udp, &pkt, &dest, 53);
        udp_refused(status, NX_NULL);
    }
    else if (strcmp(a, "udp_window") == 0 || strcmp(a, "udp_reused") == 0)
    {
        window_action = (a[4] == 'w') ? ZERO : REUSE;
        status = _nxd_udp_socket_send(&udp, &pkt, &dest, 53);
        printf("     window ran with mutex depth %d\n", window_held);
        check("the deletion stub ran in an unlocked stretch", window_held == 0);
        udp_refused(status, NX_NULL);
    }
    else if (strcmp(a, "udp_stamped_window") == 0 || strcmp(a, "udp_stamped_reused") == 0)
    {
        window_action = (a[12] == 'w') ? ZERO : REUSE;
        status = _nxd_udp_socket_source_send(&udp, &pkt, &dest, 53, 1);
        printf("     window ran with mutex depth %d\n", window_held);
        check("the deletion stub ran in an unlocked stretch", window_held == 0);
        udp_refused(status, &ip.nx_ipv6_address[1]);
    }
    else if (strcmp(a, "udp_live") == 0)
    {
        status = _nxd_udp_socket_send(&udp, &pkt, &dest, 53);
        check("a live source is sent from, once", status == NX_SUCCESS && sends == 1 && sent_src3 == 0x11UL);
        check("counters advanced once",
              ip.nx_ip_udp_packets_sent == 1 && udp.nx_udp_socket_packets_sent == 1);
        check("no mutex hold left, no release", held == 0 && released == 0);
    }
    else if (strcmp(a, "udp_index") == 0)
    {
        status = _nxd_udp_socket_source_send(&udp, &pkt, &dest, 53,
                                             (UINT)(sizeof(ip.nx_ipv6_address) / sizeof(ip.nx_ipv6_address[0]) + 3));
        udp_refused(status, NX_NULL);
    }
    else if (strcmp(a, "tcp_window") == 0 || strcmp(a, "tcp_reused") == 0)
    {
        tcp_setup();
        /* Chosen without the mutex; deleted just before the connect takes it.  */
        window_action   = (a[4] == 'w') ? ZERO : REUSE;
        window_at_mutex = 1;
        status = _nxd_tcp_client_socket_connect_internal(&tcp, &dest, 80, NX_TCP_SOURCE_ADDRESS_ANY, NX_NO_WAIT);
        printf("     window ran with mutex depth %d\n", window_held);
        check("the deletion stub ran in an unlocked stretch", window_held == 0);
        tcp_refused(status);
    }
    else if (strcmp(a, "tcp_live") == 0)
    {
        tcp_setup();
        status = _nxd_tcp_client_socket_connect_internal(&tcp, &dest, 80, NX_TCP_SOURCE_ADDRESS_ANY, NX_NO_WAIT);
        check("a live source connects: SYN sent, SYN_SENT, NX_IN_PROGRESS",
              status == NX_IN_PROGRESS && syns == 1 && tcp.nx_tcp_socket_state == NX_TCP_SYN_SENT &&
              tcp.nx_tcp_socket_ipv6_addr == &ip.nx_ipv6_address[1]);
        check("no mutex hold left", held == 0);
    }
    else if (strcmp(a, "tcp_index") == 0)
    {
        tcp_setup();
        status = _nxd_tcp_client_socket_connect_internal(&tcp, &dest, 80,
                                                         (UINT)(sizeof(ip.nx_ipv6_address) / sizeof(ip.nx_ipv6_address[0]) + 3),
                                                         NX_NO_WAIT);
        tcp_refused(status);
    }
    else
    {
        return 2;
    }
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad;
}
