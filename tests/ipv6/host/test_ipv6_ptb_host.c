/*
 * AmiNetXDuo, the ICMPv6 Packet Too Big receive path, driven directly.
 *
 * Path MTU Discovery is the one part of IPv6 where a hostile packet changes
 * how this node sends for the next ten minutes, so what it refuses matters as
 * much as what it accepts.  Neither half can be produced on a lab network on
 * demand, there is no router on it with a narrow link behind it, and nothing
 * on it will forge an error message, so both are driven here.
 *
 * What is checked, all of it RFC 8201 4:
 *
 *   1. A report of a narrower path lowers the destination's path MTU and arms
 *      the ten-minute retry, and the sweep restores the link MTU when it
 *      expires and not before.
 *
 *   2. A second report lowers it further, and a report of a WIDER path does
 *      not raise it.  A message announcing an increase is a stale packet, a
 *      forgery, or a second path, and never a reason to send bigger.
 *
 *   3. A report below the IPv6 minimum link MTU of 1280 is discarded outright
 *      rather than clamped, and leaves an existing estimate alone.  Without
 *      this one forged message pins the destination wherever the sender likes.
 *
 *   4. A report whose embedded packet was not sent from this node's address is
 *      discarded, so an off-path sender cannot create table entries for
 *      destinations this node never addressed, nor fill a four-entry table
 *      with them.
 *
 *   5. A full destination table is survived.  _nx_icmpv6_dest_table_add()
 *      returns without writing through its entry pointer at all in that case,
 *      which the handler used to dereference regardless.
 *
 * Real, compiled from third_party/netxduo/common/src into this binary:
 * nx_icmpv6_process_packet_too_big.c with the destination table underneath it
 * add, find and the periodic sweep, so the table state each check reads
 * is the table the stack would have.
 *
 * Stubbed: the neighbour cache and the two things the handler calls outward,
 * _nx_icmpv6_send_ns and _nx_packet_release.  The cache stub hands out real
 * ND_CACHE_ENTRY storage because the sweep reads the interface back through
 * it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_ipv6.h"
#include "nx_icmpv6.h"
#include "nx_nd_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ------------------------------------------------------------- harness ---- */

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


/* --------------------------------------------------------------- stubs ---- */

static TX_THREAD h_caller_thread;

TX_THREAD     *_tx_thread_current_ptr = &h_caller_thread;
TX_THREAD      _tx_timer_thread;
UINT           _tx_thread_preempt_disable;
volatile ULONG _tx_thread_system_state;

UINT _tx_thread_interrupt_disable(VOID)
{
    return 0;
}

VOID _tx_thread_interrupt_restore(UINT previous_posture)
{
    NX_PARAMETER_NOT_USED(previous_posture);
}

TX_THREAD *_tx_thread_identify(VOID)
{
    return _tx_thread_current_ptr;
}

/*
 * NX_ASSERT's failure arm, which on a real target is an endless sleep.  The
 * asserts stay compiled in, the destination table has several and they are
 * worth having under this test, so one firing has to end the run rather than
 * hang it.
 */
UINT _tx_thread_sleep(ULONG timer_ticks)
{
    NX_PARAMETER_NOT_USED(timer_ticks);

    printf("FAIL an NX_ASSERT fired\n");
    exit(1);
}

UINT _tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    NX_PARAMETER_NOT_USED(mutex_ptr);
    NX_PARAMETER_NOT_USED(wait_option);
    return TX_SUCCESS;
}

UINT _tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    NX_PARAMETER_NOT_USED(mutex_ptr);
    return TX_SUCCESS;
}

/* The neighbour cache, as much of one as the destination table needs. */
#define H_ND_ENTRIES    8

static ND_CACHE_ENTRY h_nd[H_ND_ENTRIES];
static UINT           h_nd_used;
static UINT           h_nd_full;         /* refuse the next add */

static UINT           h_packets_released;
static UINT           h_solicitations;

UINT _nx_nd_cache_find_entry(NX_IP *ip_ptr, ULONG *dest_ip, ND_CACHE_ENTRY **entry)
{
UINT i;

    NX_PARAMETER_NOT_USED(ip_ptr);

    for (i = 0; i < h_nd_used; i++)
    {
        if (CHECK_IPV6_ADDRESSES_SAME(h_nd[i].nx_nd_cache_dest_ip, dest_ip))
        {
            *entry = &h_nd[i];
            return NX_SUCCESS;
        }
    }

    return NX_NOT_SUCCESSFUL;
}

UINT _nx_nd_cache_add_entry(NX_IP *ip_ptr, ULONG *dest_ip,
                            NXD_IPV6_ADDRESS *nxd_address, ND_CACHE_ENTRY **entry)
{
ND_CACHE_ENTRY *new_entry;

    NX_PARAMETER_NOT_USED(ip_ptr);

    if (h_nd_full || (h_nd_used == H_ND_ENTRIES))
    {
        *entry = NX_NULL;
        return NX_NOT_SUCCESSFUL;
    }

    new_entry = &h_nd[h_nd_used++];

    memset(new_entry, 0, sizeof(*new_entry));
    COPY_IPV6_ADDRESS(dest_ip, new_entry -> nx_nd_cache_dest_ip);
    new_entry -> nx_nd_cache_nd_status       = ND_CACHE_STATE_CREATED;
    new_entry -> nx_nd_cache_interface_ptr   = nxd_address -> nxd_ipv6_address_attached;
    new_entry -> nx_nd_cache_outgoing_address = nxd_address;

    *entry = new_entry;
    return NX_SUCCESS;
}

VOID _nx_icmpv6_send_ns(NX_IP *ip_ptr, ULONG *neighbor_IP_address, INT send_slla,
                        NXD_IPV6_ADDRESS *outgoing_address, INT sendUnicast,
                        ND_CACHE_ENTRY *NDCacheEntry)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(neighbor_IP_address);
    NX_PARAMETER_NOT_USED(send_slla);
    NX_PARAMETER_NOT_USED(outgoing_address);
    NX_PARAMETER_NOT_USED(sendUnicast);
    NX_PARAMETER_NOT_USED(NDCacheEntry);

    h_solicitations++;
}

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    NX_PARAMETER_NOT_USED(packet_ptr);
    h_packets_released++;
    return NX_SUCCESS;
}


/* ----------------------------------------------------------- the fixture -- */

static NX_IP            h_ip;
static NXD_IPV6_ADDRESS h_local;         /* the address the error arrives on */
static NX_IPV6_HEADER   h_outer;         /* the error message's own header   */
static NX_PACKET        h_packet;
static UCHAR            h_message[128];

#define H_MAC_MSW       0x00000080UL
#define H_MAC_LSW       0x10490001UL

#define H_LINK_MTU      1500

/* fe80::280:10ff:fe49:1, this node. */
#define H_LOCAL_0       0xFE800000UL
#define H_LOCAL_1       0UL
#define H_LOCAL_2       0x028010FFUL
#define H_LOCAL_3       0xFE490001UL

/* fe80::1, the router that returned the error. */
#define H_ROUTER_3      1UL

static VOID h_reset(VOID)
{
NX_INTERFACE *if_ptr;

    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_local, 0, sizeof(h_local));
    memset(&h_outer, 0, sizeof(h_outer));
    memset(&h_packet, 0, sizeof(h_packet));
    memset(h_message, 0, sizeof(h_message));
    memset(h_nd, 0, sizeof(h_nd));

    h_nd_used = 0;
    h_nd_full = 0;
    h_packets_released = 0;
    h_solicitations = 0;

    if_ptr = &h_ip.nx_ip_interface[0];

    if_ptr -> nx_interface_valid                = NX_TRUE;
    if_ptr -> nx_interface_name                 = "eth0";
    if_ptr -> nx_interface_link_up              = NX_TRUE;
    if_ptr -> nx_interface_ip_mtu_size          = H_LINK_MTU;
    if_ptr -> nx_interface_physical_address_msw = H_MAC_MSW;
    if_ptr -> nx_interface_physical_address_lsw = H_MAC_LSW;
    if_ptr -> nx_interface_index                = 0;

    h_local.nxd_ipv6_address_valid    = NX_TRUE;
    h_local.nxd_ipv6_address_type     = NX_IP_VERSION_V6;
    h_local.nxd_ipv6_address_attached = if_ptr;
    h_local.nxd_ipv6_address_state    = NX_IPV6_ADDR_STATE_VALID;
    h_local.nxd_ipv6_address[0]       = H_LOCAL_0;
    h_local.nxd_ipv6_address[1]       = H_LOCAL_1;
    h_local.nxd_ipv6_address[2]       = H_LOCAL_2;
    h_local.nxd_ipv6_address[3]       = H_LOCAL_3;

    /* The destination table is only live once ICMPv6 has been enabled. */
    h_ip.nx_ipv6_destination_table_size = 0;
    h_ip.nx_destination_table_periodic_update = _nx_icmpv6_destination_table_periodic_update;
}

/*
 * A Packet Too Big.  The outer header is in host order, which is what
 * _nx_ipv6_packet_receive() leaves for the dispatcher.  The message body,
 * the ICMPv6 header with the MTU in it, then as much of the offending packet
 * as fits, is in network order, which is what came off the wire.
 *
 * `sent_from` is the source of the offending packet, which for anything this
 * node really sent is this node's own address.
 */
static VOID h_deliver_ptb(ULONG mtu, ULONG *destination, ULONG *sent_from)
{
NX_ICMPV6_OPTION_MTU *header = (NX_ICMPV6_OPTION_MTU *)h_message;
NX_IPV6_HEADER       *inner  = (NX_IPV6_HEADER *)(h_message + sizeof(NX_ICMPV6_OPTION_MTU));
UINT                  length = (UINT)(sizeof(NX_ICMPV6_OPTION_MTU) + sizeof(NX_IPV6_HEADER));

    memset(h_message, 0, sizeof(h_message));

    /* Source fe80::1, the router.  Destination is this node. */
    h_outer.nx_ip_header_source_ip[0] = H_LOCAL_0;
    h_outer.nx_ip_header_source_ip[1] = 0UL;
    h_outer.nx_ip_header_source_ip[2] = 0UL;
    h_outer.nx_ip_header_source_ip[3] = H_ROUTER_3;
    h_outer.nx_ip_header_destination_ip[0] = H_LOCAL_0;
    h_outer.nx_ip_header_destination_ip[1] = H_LOCAL_1;
    h_outer.nx_ip_header_destination_ip[2] = H_LOCAL_2;
    h_outer.nx_ip_header_destination_ip[3] = H_LOCAL_3;
    h_outer.nx_ip_header_word_1 = 0x000000FFUL;

    header -> nx_icmpv6_option_mtu_icmpv6_header.nx_icmpv6_header_type = NX_ICMPV6_PACKET_TOO_BIG_TYPE;
    header -> nx_icmpv6_option_mtu_icmpv6_header.nx_icmpv6_header_code = 0;
    header -> nx_icmpv6_option_mtu_icmpv6_header.nx_icmpv6_header_checksum = 0;
    header -> nx_icmpv6_option_mtu_path_mtu = mtu;
    NX_CHANGE_ULONG_ENDIAN(header -> nx_icmpv6_option_mtu_path_mtu);

    COPY_IPV6_ADDRESS(sent_from, inner -> nx_ip_header_source_ip);
    COPY_IPV6_ADDRESS(destination, inner -> nx_ip_header_destination_ip);
    NX_IPV6_ADDRESS_CHANGE_ENDIAN(inner -> nx_ip_header_source_ip);
    NX_IPV6_ADDRESS_CHANGE_ENDIAN(inner -> nx_ip_header_destination_ip);

    h_packet.nx_packet_prepend_ptr = h_message;
    h_packet.nx_packet_append_ptr  = h_message + length;
    h_packet.nx_packet_length      = length;
    h_packet.nx_packet_ip_header   = (UCHAR *)&h_outer;
    h_packet.nx_packet_next        = NX_NULL;
    h_packet.nx_packet_address.nx_packet_ipv6_address_ptr = &h_local;

    (VOID)_nx_icmpv6_process_packet_too_big(&h_ip, &h_packet);
}

/* The destination table entry for an address, or NX_NULL. */
static NX_IPV6_DESTINATION_ENTRY *h_entry(ULONG *destination)
{
UINT i;

    for (i = 0; i < NX_IPV6_DESTINATION_TABLE_SIZE; i++)
    {
        if (h_ip.nx_ipv6_destination_table[i].nx_ipv6_destination_entry_valid &&
            CHECK_IPV6_ADDRESSES_SAME(h_ip.nx_ipv6_destination_table[i].nx_ipv6_destination_entry_destination_address,
                                      destination))
        {
            return &h_ip.nx_ipv6_destination_table[i];
        }
    }

    return NX_NULL;
}

/* A peer address, 2001:db8::<n>. */
static VOID h_peer(ULONG *address, ULONG n)
{
    address[0] = 0x20010DB8UL;
    address[1] = 0UL;
    address[2] = 0UL;
    address[3] = n;
}

/* Run the once-a-second sweep for `seconds`. */
static VOID h_age(ULONG seconds)
{
ULONG i;

    for (i = 0; i < seconds; i++)
    {
        _nx_icmpv6_destination_table_periodic_update(&h_ip);
    }
}


/* ---------------------------------------------------------------- tests --- */

/* A narrower path is discovered, and the ten-minute retry restores it. */
static VOID test_narrow_path(VOID)
{
ULONG                      peer[4];
NX_IPV6_DESTINATION_ENTRY *entry;

    h_reset();
    h_peer(peer, 1);

    h_deliver_ptb(1400, peer, h_local.nxd_ipv6_address);

    entry = h_entry(peer);
    h_check(entry != NX_NULL, "a narrower path creates a destination entry");

    if (entry == NX_NULL)
    {
        return;
    }

    h_check(entry -> nx_ipv6_destination_entry_path_mtu == 1400,
            "the reported MTU becomes the path MTU");
    h_check(entry -> nx_ipv6_destination_entry_MTU_timer_tick ==
            NX_PATH_MTU_INCREASE_WAIT_INTERVAL_TICKS,
            "the retry interval is armed");
    h_check(h_packets_released == 1, "the message is released");

    /* One second short of the interval it is still narrow. */
    h_age(NX_PATH_MTU_INCREASE_WAIT_INTERVAL - 1);

    h_check(entry -> nx_ipv6_destination_entry_path_mtu == 1400,
            "the path MTU is held for the whole interval");

    h_age(1);

    h_check(entry -> nx_ipv6_destination_entry_path_mtu == H_LINK_MTU,
            "the sweep retries the link MTU when the interval expires");
    h_check(entry -> nx_ipv6_destination_entry_MTU_timer_tick == NX_WAIT_FOREVER,
            "an entry back at the link MTU stops being aged");
}

/* Lower, yes.  Higher, never. */
static VOID test_never_increases(VOID)
{
ULONG                      peer[4];
NX_IPV6_DESTINATION_ENTRY *entry;

    h_reset();
    h_peer(peer, 2);

    h_deliver_ptb(1400, peer, h_local.nxd_ipv6_address);
    h_deliver_ptb(1300, peer, h_local.nxd_ipv6_address);

    entry = h_entry(peer);
    h_check(entry != NX_NULL, "the entry survives a second report");

    if (entry == NX_NULL)
    {
        return;
    }

    h_check(entry -> nx_ipv6_destination_entry_path_mtu == 1300,
            "a second, narrower report lowers the path MTU again");

    h_deliver_ptb(1450, peer, h_local.nxd_ipv6_address);

    h_check(entry -> nx_ipv6_destination_entry_path_mtu == 1300,
            "a report of a wider path does not raise the path MTU");

    /* Nor does one above the link MTU, which cannot be acted on at all. */
    h_deliver_ptb(9000, peer, h_local.nxd_ipv6_address);

    h_check(entry -> nx_ipv6_destination_entry_path_mtu == 1300,
            "a report above the link MTU changes nothing");
}

/* The floor: below 1280 the message is discarded, not clamped. */
static VOID test_minimum_mtu_floor(VOID)
{
ULONG                      peer[4];
NX_IPV6_DESTINATION_ENTRY *entry;
ULONG                      invalid_before;

    h_reset();
    h_peer(peer, 3);

    /* On a destination with no entry at all, nothing is created. */
    invalid_before = h_ip.nx_ip_icmp_invalid_packets;
    h_deliver_ptb(68, peer, h_local.nxd_ipv6_address);

    h_check(h_entry(peer) == NX_NULL,
            "a report below the minimum link MTU creates no entry");
    h_check(h_ip.nx_ip_icmp_invalid_packets == invalid_before + 1,
            "a report below the minimum link MTU is counted invalid");
    h_check(h_packets_released == 1, "the discarded message is released");

    /* Nor does it disturb an estimate already established. */
    h_deliver_ptb(1400, peer, h_local.nxd_ipv6_address);
    h_deliver_ptb(576, peer, h_local.nxd_ipv6_address);
    h_deliver_ptb(0, peer, h_local.nxd_ipv6_address);
    h_deliver_ptb(1279, peer, h_local.nxd_ipv6_address);

    entry = h_entry(peer);
    h_check(entry != NX_NULL, "the entry from the valid report is still there");

    if (entry != NX_NULL)
    {
        h_check(entry -> nx_ipv6_destination_entry_path_mtu == 1400,
                "reports below the minimum leave an existing estimate alone");
    }

    /* 1280 exactly is the boundary and is legal. */
    h_deliver_ptb(NX_MINIMUM_IPV6_PATH_MTU, peer, h_local.nxd_ipv6_address);

    entry = h_entry(peer);

    if (entry != NX_NULL)
    {
        h_check(entry -> nx_ipv6_destination_entry_path_mtu == NX_MINIMUM_IPV6_PATH_MTU,
                "the minimum link MTU itself is accepted");
    }
}

/* A report about traffic this node never sent. */
static VOID test_payload_not_ours(VOID)
{
ULONG peer[4];
ULONG someone_else[4];
ULONG invalid_before;

    h_reset();
    h_peer(peer, 4);
    h_peer(someone_else, 0x999);

    invalid_before = h_ip.nx_ip_icmp_invalid_packets;
    h_deliver_ptb(1400, peer, someone_else);

    h_check(h_entry(peer) == NX_NULL,
            "an error about somebody else's packet creates no entry");
    h_check(h_ip.nx_ipv6_destination_table_size == 0,
            "and does not consume a destination table slot");
    h_check(h_ip.nx_ip_icmp_invalid_packets == invalid_before + 1,
            "and is counted invalid");
    h_check(h_packets_released == 1, "and is released");
}

/* A full destination table, which used to be where a destination stopped being
   reachable at all: _nx_icmpv6_dest_table_add() refused, _nx_ipv6_packet_send()
   released the packet, and nothing ever freed a slot again. */
static VOID test_destination_table_full(VOID)
{
ULONG peer[4];
ULONG oldest[4];
ULONG i;

    h_reset();

    for (i = 0; i < NX_IPV6_DESTINATION_TABLE_SIZE; i++)
    {
        h_peer(peer, 0x100 + i);
        h_deliver_ptb(1400, peer, h_local.nxd_ipv6_address);
    }

    h_check(h_ip.nx_ipv6_destination_table_size == NX_IPV6_DESTINATION_TABLE_SIZE,
            "the destination table fills");

    /* One more destination than the table holds.  Reachable from the network. */
    h_peer(oldest, 0x100);
    h_peer(peer, 0x200);
    h_deliver_ptb(1400, peer, h_local.nxd_ipv6_address);

    h_check(h_entry(peer) != NX_NULL,
            "a destination that does not fit takes the place of another");
    h_check(h_entry(oldest) == NX_NULL,
            "and the one it takes is the least recently used");
    h_check(h_ip.nx_ipv6_destination_table_size == NX_IPV6_DESTINATION_TABLE_SIZE,
            "and the table is not corrupted");

    /* Touched, so it is no longer the oldest, and the next new destination has
       to displace the one after it instead. */
    h_peer(peer, 0x101);
    h_deliver_ptb(1400, peer, h_local.nxd_ipv6_address);
    h_peer(peer, 0x201);
    h_deliver_ptb(1400, peer, h_local.nxd_ipv6_address);

    h_peer(peer, 0x101);
    h_check(h_entry(peer) != NX_NULL,
            "a destination in use is not the one given up");
    h_peer(peer, 0x102);
    h_check(h_entry(peer) == NX_NULL,
            "the one untouched for longest is");

    /* And the other half of it: the ND cache refuses, so the entry is NX_NULL. */
    h_reset();
    h_peer(peer, 0x300);
    h_nd_full = 1;
    h_deliver_ptb(1400, peer, h_local.nxd_ipv6_address);

    h_check(h_solicitations == 0,
            "no neighbour solicitation is sent for an entry that was not created");
}


int main(void)
{
    test_narrow_path();
    test_never_increases();
    test_minimum_mtu_floor();
    test_payload_not_ours();
    test_destination_table_full();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return h_failures ? 1 : 0;
}
