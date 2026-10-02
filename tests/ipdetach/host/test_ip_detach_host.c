/*
 * Audit N-014 and N-015: _nx_ip_interface_detach's ND sweep and static-route
 * compaction.
 *
 * ndlock:      every _nx_nd_cache_delete_internal the detach reaches runs
 *              with nx_ip_protection held, so it cannot interleave with the
 *              IP thread's ND fast periodic update (N-014).
 * routes:      [A(K), B(K), C(0)] -> detach K leaves exactly [C] (N-015, the
 *              entry shifted into the hole was never tested).
 * routes_full: [A(K), C(0), D(0), E(0)], a full NX_IP_ROUTING_TABLE_SIZE 4
 *              table -> detach K leaves [C, D, E].  The target is built with
 *              -fsanitize=bounds trapping, so the shift's read of
 *              nx_ip_routing_table[4] stops the test (N-015).
 *
 * Linked for real: _nx_ip_interface_detach and
 * _nx_nd_cache_interface_entries_delete.  Stubbed: ThreadX (the mutex counts
 * its holder), _nx_nd_cache_delete_internal (records whether the mutex is
 * held), the ARP sweep, the SYN-cache flush and the driver.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#ifdef FEATURE_NX_IPV6
#include "nx_nd_cache.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define K 1u    /* the interface detached */

static NX_IP rig_ip;
static int   held;          /* nx_ip_protection ownership count */

static UINT mutex_get(TX_MUTEX *m)
{
    if (m == &rig_ip.nx_ip_protection)
        held++;
    return TX_SUCCESS;
}

static UINT mutex_put(TX_MUTEX *m)
{
    if (m == &rig_ip.nx_ip_protection)
    {
        if (held == 0)
        {
            printf("FAIL nx_ip_protection put while not held\n");
            exit(1);
        }
        held--;
    }
    return TX_SUCCESS;
}

UINT _tx_mutex_get(TX_MUTEX *m, ULONG w)  { (void) w; return mutex_get(m); }
UINT _tx_mutex_put(TX_MUTEX *m)           { return mutex_put(m); }
UINT _txe_mutex_get(TX_MUTEX *m, ULONG w) { (void) w; return mutex_get(m); }
UINT _txe_mutex_put(TX_MUTEX *m)          { return mutex_put(m); }

VOID _nx_arp_interface_entries_delete(NX_IP *ip_ptr, UINT index)        { (void) ip_ptr; (void) index; }
VOID _nx_tcp_socket_connection_reset(NX_TCP_SOCKET *socket_ptr)        { (void) socket_ptr; }
VOID _nx_tcp_syncache_interface_flush(NX_IP *ip_ptr, NX_INTERFACE *if_ptr,
                                      NXD_IPV6_ADDRESS *addr)
{
    (void) ip_ptr; (void) if_ptr; (void) addr;
}
UINT _nx_igmp_multicast_interface_leave_internal(NX_IP *ip_ptr, ULONG group, UINT index)
{
    (void) ip_ptr; (void) group; (void) index;
    return NX_SUCCESS;
}

#ifdef FEATURE_NX_IPV6
VOID _nx_invalidate_destination_entry(NX_IP *ip_ptr, ULONG *next_hop)  { (void) ip_ptr; (void) next_hop; }

static int nd_deletes;
static int nd_unlocked;

UINT _nx_nd_cache_delete_internal(NX_IP *ip_ptr, ND_CACHE_ENTRY *entry)
{
    (void) ip_ptr;
    nd_deletes++;
    if (held == 0)
        nd_unlocked++;
    entry -> nx_nd_cache_nd_status     = ND_CACHE_STATE_INVALID;
    return NX_SUCCESS;
}
#endif

static void rig_driver(NX_IP_DRIVER *req)
{
    req -> nx_ip_driver_status = NX_SUCCESS;
}

static void rig_reset(void)
{
    UINT i;

    memset(&rig_ip, 0, sizeof(rig_ip));
    held = 0;
    for (i = 0; i <= K; i++)
    {
        NX_INTERFACE *ifp = &rig_ip.nx_ip_interface[i];

        ifp -> nx_interface_valid             = NX_TRUE;
        ifp -> nx_interface_link_up           = NX_TRUE;
        ifp -> nx_interface_index             = (UCHAR) i;
        ifp -> nx_interface_link_driver_entry = rig_driver;
    }
}

static int arm_ndlock(void)
{
#ifdef FEATURE_NX_IPV6
    int bad;

    rig_reset();
    nd_deletes  = 0;
    nd_unlocked = 0;
    rig_ip.nx_ipv6_nd_cache[0].nx_nd_cache_interface_ptr = &rig_ip.nx_ip_interface[K];
    rig_ip.nx_ipv6_nd_cache[0].nx_nd_cache_nd_status     = ND_CACHE_STATE_INCOMPLETE;
    rig_ip.nx_ipv6_nd_cache[1].nx_nd_cache_interface_ptr = &rig_ip.nx_ip_interface[0];
    rig_ip.nx_ipv6_nd_cache[1].nx_nd_cache_nd_status     = ND_CACHE_STATE_REACHABLE;
    rig_ip.nx_ipv6_nd_cache[2].nx_nd_cache_interface_ptr = &rig_ip.nx_ip_interface[K];
    rig_ip.nx_ipv6_nd_cache[2].nx_nd_cache_nd_status     = ND_CACHE_STATE_PROBE;

    if (_nx_ip_interface_detach(&rig_ip, K) != NX_SUCCESS)
    {
        printf("FAIL detach\n");
        return 1;
    }

    printf("%s ND entries deleted for the detached interface: %d (want 2)\n",
           nd_deletes == 2 ? "ok  " : "FAIL", nd_deletes);
    printf("%s ND deletes run without nx_ip_protection: %d (want 0)\n",
           nd_unlocked == 0 ? "ok  " : "FAIL", nd_unlocked);
    printf("%s nx_ip_protection released at the end: held %d (want 0)\n",
           held == 0 ? "ok  " : "FAIL", held);
    bad = nd_deletes != 2 || nd_unlocked != 0 || held != 0;
    return bad;
#else
    printf("ok   IPv6 not built: no ND sweep\n");
    return 0;
#endif
}

#ifdef NX_ENABLE_IP_STATIC_ROUTING
static void route_set(UINT slot, ULONG dest, UINT iface)
{
    NX_IP_ROUTING_ENTRY *r = &rig_ip.nx_ip_routing_table[slot];

    r -> nx_ip_routing_dest_ip            = dest;
    r -> nx_ip_routing_net_mask           = 0xffffff00UL;
    r -> nx_ip_routing_next_hop_address   = dest | 1UL;
    r -> nx_ip_routing_entry_ip_interface = &rig_ip.nx_ip_interface[iface];
}

/* After the detach the table must be exactly `want`, in order, all on
   interface 0, with the slots past the count cleared.  */
static int routes_check(const ULONG *want, UINT n)
{
    UINT i;
    int  bad = 0;

    if (rig_ip.nx_ip_routing_table_entry_count != n)
    {
        printf("FAIL entry count %lu (want %u)\n",
               (unsigned long) rig_ip.nx_ip_routing_table_entry_count, n);
        bad = 1;
    }
    for (i = 0; i < NX_IP_ROUTING_TABLE_SIZE; i++)
    {
        NX_IP_ROUTING_ENTRY *r = &rig_ip.nx_ip_routing_table[i];

        if (r -> nx_ip_routing_entry_ip_interface == &rig_ip.nx_ip_interface[K])
        {
            printf("FAIL slot %u (dest 0x%08lx) still names the detached interface\n",
                   i, (unsigned long) r -> nx_ip_routing_dest_ip);
            bad = 1;
        }
        if (i < n && r -> nx_ip_routing_dest_ip != want[i])
        {
            printf("FAIL slot %u dest 0x%08lx (want 0x%08lx)\n",
                   i, (unsigned long) r -> nx_ip_routing_dest_ip, (unsigned long) want[i]);
            bad = 1;
        }
        if (i >= n && (r -> nx_ip_routing_dest_ip != 0
                       || r -> nx_ip_routing_entry_ip_interface != NX_NULL))
        {
            printf("FAIL slot %u past the count is not cleared\n", i);
            bad = 1;
        }
    }
    if (!bad)
        printf("ok   %u surviving route(s), none on the detached interface\n", n);
    return bad;
}
#endif

static int arm_routes(int full)
{
#ifdef NX_ENABLE_IP_STATIC_ROUTING
    static const ULONG want_adj[]  = { 0x0a000300UL };
    static const ULONG want_full[] = { 0x0a000300UL, 0x0a000400UL, 0x0a000500UL };

    rig_reset();
    if (full)
    {
        route_set(0, 0x0a000100UL, K);
        route_set(1, 0x0a000300UL, 0);
        route_set(2, 0x0a000400UL, 0);
        route_set(3, 0x0a000500UL, 0);
        rig_ip.nx_ip_routing_table_entry_count = NX_IP_ROUTING_TABLE_SIZE;
    }
    else
    {
        route_set(0, 0x0a000100UL, K);
        route_set(1, 0x0a000200UL, K);
        route_set(2, 0x0a000300UL, 0);
        rig_ip.nx_ip_routing_table_entry_count = 3;
    }
    fflush(stdout);

    if (_nx_ip_interface_detach(&rig_ip, K) != NX_SUCCESS)
    {
        printf("FAIL detach\n");
        return 1;
    }
    return full ? routes_check(want_full, 3) : routes_check(want_adj, 1);
#else
    (void) full;
    printf("FAIL NX_ENABLE_IP_STATIC_ROUTING is off: the port enables it\n");
    return 1;
#endif
}

int main(int argc, char **argv)
{
    int bad;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s ndlock|routes|routes_full\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "ndlock") == 0)
        bad = arm_ndlock();
    else if (strcmp(argv[1], "routes") == 0)
        bad = arm_routes(0);
    else if (strcmp(argv[1], "routes_full") == 0)
        bad = arm_routes(1);
    else
        return 2;
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad;
}
