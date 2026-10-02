/*
 * Audit N-036 and N-037: the raw send's contract with the address table and
 * the route lookup.
 *
 * v6_deleted:  the caller's address index names a slot nxd_ipv6_address_delete
 *              has since zeroed.  The send must refuse it with
 *              NX_NO_INTERFACE_ADDRESS, reach no IPv6 send (the real
 *              _nx_ipv6_packet_send dereferences the slot's interface and
 *              NX_ASSERT sleeps forever holding nx_ip_protection), release the
 *              mutex and leave the packet the caller's.
 * v6_live:     a valid slot is sent from, once, with the hop limit and the
 *              slot the caller named (2c3e8601).
 * v6_find:     nxd_ip_raw_packet_send picks its source under
 *              nx_ip_protection, in the same hold as the send.
 * v4_noroute:  a destination _nx_ip_route_find cannot route is
 *              NX_IP_ADDRESS_ERROR, nothing is sent and the packet is the
 *              caller's (the IP send would have dropped and released it while
 *              the raw send said success).
 * v4_route:    a routable destination is still sent.
 * v4_noroute_delegated: the same through nxd_ip_raw_packet_send, which hands
 *              an IPv4 send to source_send with index 0 (raw.c's
 *              nxd_ip_raw_packet_send arm).
 *
 * Linked for real: nxd_ip_raw_packet_source_send.c, nxd_ip_raw_packet_send.c
 * and nxd_ipv6_raw_packet_send_internal.c.  Stubbed: ThreadX's mutex (counts
 * holds), the route lookup, the source lookup and both IP sends.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_ipv6.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NX_IP        ip;
static NX_INTERFACE iface;
static int          held;
static unsigned     hold_gen;

static UINT mutex_get(TX_MUTEX *m)
{
    if (m == &ip.nx_ip_protection && held++ == 0)
        hold_gen++;
    return TX_SUCCESS;
}
static UINT mutex_put(TX_MUTEX *m)
{
    if (m == &ip.nx_ip_protection)
        held--;
    return TX_SUCCESS;
}
UINT _tx_mutex_get(TX_MUTEX *m, ULONG w)  { (void) w; return mutex_get(m); }
UINT _tx_mutex_put(TX_MUTEX *m)           { return mutex_put(m); }
UINT _txe_mutex_get(TX_MUTEX *m, ULONG w) { (void) w; return mutex_get(m); }
UINT _txe_mutex_put(TX_MUTEX *m)          { return mutex_put(m); }

static int      v6_sends, v4_sends, released;
static NX_INTERFACE *v6_if;
static ULONG    v6_hops;
static UINT     route_status;
static int      find_held;
static unsigned find_gen, send_gen;

VOID _nx_ipv6_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr, ULONG protocol, ULONG payload_size,
                          ULONG hop_limit, ULONG traffic_class, ULONG *src_address,
                          ULONG *dest_address)
{
    (void) ip_ptr; (void) protocol; (void) payload_size; (void) traffic_class;
    (void) src_address; (void) dest_address;
    /* The real one starts if_ptr = ...->nxd_ipv6_address_attached;
       NX_ASSERT(if_ptr != NX_NULL).  */
    v6_if   = packet_ptr -> nx_packet_address.nx_packet_ipv6_address_ptr -> nxd_ipv6_address_attached;
    v6_hops = hop_limit;
    send_gen = hold_gen;
    v6_sends++;
}

UINT _nx_ip_route_find(NX_IP *ip_ptr, ULONG destination_address, NX_INTERFACE **nx_ip_interface,
                       ULONG *next_hop_address)
{
    (void) ip_ptr; (void) destination_address;
    if (route_status != NX_SUCCESS)
    {
        *next_hop_address = 0;
        return route_status;
    }
    *nx_ip_interface  = &iface;
    *next_hop_address = destination_address;
    return NX_SUCCESS;
}

VOID _nx_ip_packet_send(NX_IP *ip_ptr, NX_PACKET *packet_ptr, ULONG destination_ip,
                        ULONG type_of_service, ULONG time_to_live, ULONG protocol,
                        ULONG fragment, ULONG next_hop_address)
{
    (void) ip_ptr; (void) packet_ptr; (void) destination_ip; (void) type_of_service;
    (void) time_to_live; (void) protocol; (void) fragment;
    /* The real one drops and releases a packet with no next hop.  */
    if (next_hop_address == 0)
        released++;
    else
        v4_sends++;
}

UINT _nxd_ipv6_interface_find(NX_IP *ip_ptr, ULONG *dest_address, NXD_IPV6_ADDRESS **ipv6_addr,
                              NX_INTERFACE *if_ptr)
{
    (void) ip_ptr; (void) dest_address; (void) if_ptr;
    find_held = held;
    find_gen  = hold_gen;
    *ipv6_addr = &ip.nx_ipv6_address[1];
    return NX_SUCCESS;
}

/* NX_ASSERT_FAIL is tx_thread_sleep(NX_WAIT_FOREVER) in a loop.  */
static UINT assert_sleep(void)
{
    printf("FAIL NX_ASSERT fired: on the target this sleeps forever\n");
    exit(1);
}
UINT _tx_thread_sleep(ULONG t)  { (void) t; return assert_sleep(); }
UINT _txe_thread_sleep(ULONG t) { (void) t; return assert_sleep(); }

UINT _nx_packet_release(NX_PACKET *p)          { (void) p; released++; return NX_SUCCESS; }
UINT _nx_packet_transmit_release(NX_PACKET *p) { (void) p; released++; return NX_SUCCESS; }

static int bad;

static void check(const char *what, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    bad |= !cond;
}

static NX_PACKET   pkt;
static NXD_ADDRESS dest6, dest4;

static void reset(void)
{
    memset(&ip, 0, sizeof(ip));
    memset(&pkt, 0, sizeof(pkt));
    held = 0;
    v6_sends = v4_sends = released = 0;
    v6_if = NX_NULL;
    route_status = NX_SUCCESS;
    find_held = -1;
    ip.nx_ipv6_hop_limit = 255;
    ip.nx_ipv6_address[1].nxd_ipv6_address_index    = 1;
    ip.nx_ipv6_address[1].nxd_ipv6_address_valid    = NX_TRUE;
    ip.nx_ipv6_address[1].nxd_ipv6_address_state    = NX_IPV6_ADDR_STATE_VALID;
    ip.nx_ipv6_address[1].nxd_ipv6_address_attached = &iface;
    ip.nx_ipv6_address[1].nxd_ipv6_address[0]       = 0x20010db8UL;
    ip.nx_ipv6_address[1].nxd_ipv6_address[3]       = 0x88UL;
}

static void delete_slot1(void)
{
    /* What nxd_ipv6_address_delete.c leaves: the slot zeroed, its index put back.  */
    memset(&ip.nx_ipv6_address[1], 0, sizeof(ip.nx_ipv6_address[1]));
    ip.nx_ipv6_address[1].nxd_ipv6_address_index = 1;
}

int main(int argc, char **argv)
{
    UINT status;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s v6_deleted|v6_live|v6_find|v4_noroute|v4_noroute_delegated|v4_route\n", argv[0]);
        return 2;
    }
    memset(&dest6, 0, sizeof(dest6));
    dest6.nxd_ip_version = NX_IP_VERSION_V6;
    dest6.nxd_ip_address.v6[0] = 0x20010db8UL;
    dest6.nxd_ip_address.v6[3] = 2;
    memset(&dest4, 0, sizeof(dest4));
    dest4.nxd_ip_version = NX_IP_VERSION_V4;
    dest4.nxd_ip_address.v4 = 0xc0000207UL;
    reset();

    if (strcmp(argv[1], "v6_deleted") == 0)
    {
        delete_slot1();
        status = _nxd_ip_raw_packet_source_send(&ip, &pkt, &dest6, 1, 17, 9, 0);
        printf("     status 0x%x, IPv6 sends %d (with interface %s), released %d, held %d\n",
               status, v6_sends, v6_sends ? (v6_if ? "set" : "NULL: the real NX_ASSERT spins") : "-",
               released, held);
        check("a deleted source slot is NX_NO_INTERFACE_ADDRESS", status == NX_NO_INTERFACE_ADDRESS);
        check("nothing reaches the IPv6 send", v6_sends == 0);
        check("the mutex is released and the packet is the caller's", held == 0 && released == 0);
    }
    else if (strcmp(argv[1], "v6_live") == 0)
    {
        status = _nxd_ip_raw_packet_source_send(&ip, &pkt, &dest6, 1, 17, 9, 0);
        check("a live source slot is sent from once",
              status == NX_SUCCESS && v6_sends == 1 && v6_if == &iface &&
              pkt.nx_packet_address.nx_packet_ipv6_address_ptr == &ip.nx_ipv6_address[1]);
        check("with the caller's hop limit", v6_hops == 9);
        check("the mutex is released", held == 0);
    }
    else if (strcmp(argv[1], "v6_find") == 0)
    {
        status = _nxd_ip_raw_packet_send(&ip, &pkt, &dest6, 17, 0, 0);
        printf("     source found with depth %d (hold %u), sent in hold %u\n", find_held, find_gen, send_gen);
        check("the send succeeds from the slot found", status == NX_SUCCESS && v6_sends == 1 && v6_if == &iface);
        check("the source is found under nx_ip_protection, in the send's hold",
              find_held > 0 && find_gen == send_gen);
        check("the default hop limit applies", v6_hops == 255);
        check("the mutex is released", held == 0);
    }
    else if (strcmp(argv[1], "v4_noroute") == 0)
    {
        route_status = NX_IP_ADDRESS_ERROR;
        status = _nxd_ip_raw_packet_source_send(&ip, &pkt, &dest4, 0, 253, 64, 0);
        printf("     status 0x%x, IPv4 sends %d, released by the stack %d, held %d\n",
               status, v4_sends, released, held);
        check("an unroutable destination is NX_IP_ADDRESS_ERROR", status == NX_IP_ADDRESS_ERROR);
        check("nothing is sent and the stack did not release the packet", v4_sends == 0 && released == 0);
        check("the mutex is released", held == 0);
    }
    else if (strcmp(argv[1], "v4_noroute_delegated") == 0)
    {
        route_status = NX_IP_ADDRESS_ERROR;
        status = _nxd_ip_raw_packet_send(&ip, &pkt, &dest4, 253, 64, 0);
        printf("     status 0x%x, IPv4 sends %d, released by the stack %d, held %d\n",
               status, v4_sends, released, held);
        check("an unroutable destination through nxd_ip_raw_packet_send is NX_IP_ADDRESS_ERROR",
              status == NX_IP_ADDRESS_ERROR);
        check("nothing is sent and the stack did not release the packet", v4_sends == 0 && released == 0);
        check("the mutex is released", held == 0);
    }
    else if (strcmp(argv[1], "v4_route") == 0)
    {
        status = _nxd_ip_raw_packet_source_send(&ip, &pkt, &dest4, 0, 253, 64, 0);
        check("a routable destination is sent", status == NX_SUCCESS && v4_sends == 1 && released == 0);
        check("the mutex is released", held == 0);
    }
    else
    {
        return 2;
    }
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad;
}
