/*
 * AmiNetXDuo, router advertisement processing and router solicitation
 * retransmission, driven directly rather than over a wire.
 *
 * Three things are checked here that no network can be made to produce on
 * demand:
 *
 *   1. A prefix advertised autonomous (A=1) but not on-link (L=0) forms an
 *      address.  The two flags are independent, RFC 4862 5.5.3 for A, RFC
 *      4861 6.3.4 for L, and the combination is what 3GPP and several CPE
 *      firmwares advertise.  It used to form nothing, because the autonomous
 *      test was nested inside the on-link one.
 *
 *   2. The router advertisement MTU option lowers the interface MTU, is
 *      ignored below the RFC 8200 minimum of 1280, and never raises the MTU
 *      above what the driver reported.  RFC 4861 6.3.4.
 *
 *   3. Router solicitation does not stop.  RFC 7559 replaces the fixed count
 *      with an unbounded exponential backoff and RFC 8504 5.4 makes it a MUST,
 *      so the sequence is the configured number of solicitations at the base
 *      interval and then a doubling one up to an hour, and a solicitation an
 *      hour, forever, rather than silence.
 *
 * Real, compiled from third_party/netxduo/common/src into this binary:
 * nx_icmpv6_process_ra.c and nxd_ipv6_router_solicitation_check.c, the two
 * functions under test, with nx_icmpv6_validate_ra.c,
 * nx_icmpv6_validate_options.c and nx_ipv6_util.c underneath them.
 *
 * Stubbed: everything the advertisement path calls outward, the prefix list,
 * the default router table, the neighbour cache, multicast join and
 * _nx_icmpv6_send_rs.  Each stub records what it was asked to do, which is how
 * "the prefix list was not touched" can be a check rather than an inference.
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

/* What the advertisement path did on the way out. */
static UINT  h_prefix_adds;
static UINT  h_prefix_deletes;
static ULONG h_prefix_added[4];
static ULONG h_prefix_added_length;
static UINT  h_multicast_joins;
static UINT  h_router_adds;
static UINT  h_packets_released;
static UINT  h_prefix_add_full;       /* make the next add fail */

/* What the RFC 8106 callback was handed, in order. */
#define H_RDNSS_MAX   8
static UINT  h_rdnss_count;
static ULONG h_rdnss_address[H_RDNSS_MAX][4];
static ULONG h_rdnss_lifetime[H_RDNSS_MAX];

UINT _nx_ipv6_prefix_list_add_entry(NX_IP *ip_ptr, ULONG *prefix,
                                    ULONG prefix_length, ULONG valid_lifetime)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(valid_lifetime);

    if (h_prefix_add_full)
    {
        return NX_NO_MORE_ENTRIES;
    }

    h_prefix_adds++;
    h_prefix_added[0] = prefix[0];
    h_prefix_added[1] = prefix[1];
    h_prefix_added[2] = prefix[2];
    h_prefix_added[3] = prefix[3];
    h_prefix_added_length = prefix_length;

    return NX_SUCCESS;
}

VOID _nx_ipv6_prefix_list_delete(NX_IP *ip_ptr, ULONG *prefix, INT prefix_length)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(prefix);
    NX_PARAMETER_NOT_USED(prefix_length);

    h_prefix_deletes++;
}

UINT _nx_ipv6_multicast_join(NX_IP *ip_ptr, ULONG *group, NX_INTERFACE *if_ptr)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(group);
    NX_PARAMETER_NOT_USED(if_ptr);

    h_multicast_joins++;
    return NX_SUCCESS;
}

UINT _nxd_ipv6_default_router_add_internal(NX_IP *ip_ptr, ULONG *router_addr,
                                           ULONG router_lifetime,
                                           NX_INTERFACE *if_ptr, INT router_type,
                                           NX_IPV6_DEFAULT_ROUTER_ENTRY **entry)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(router_addr);
    NX_PARAMETER_NOT_USED(router_lifetime);
    NX_PARAMETER_NOT_USED(if_ptr);
    NX_PARAMETER_NOT_USED(router_type);

    h_router_adds++;

    if (entry)
    {
        *entry = NX_NULL;
    }

    return NX_SUCCESS;
}

UINT _nxd_ipv6_default_router_delete(NX_IP *ip_ptr, NXD_ADDRESS *router_address)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(router_address);
    return NX_SUCCESS;
}

UINT _nx_nd_cache_find_entry(NX_IP *ip_ptr, ULONG *dest_ip,
                             ND_CACHE_ENTRY **nd_cache_entry)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(dest_ip);
    NX_PARAMETER_NOT_USED(nd_cache_entry);
    return NX_NOT_SUCCESSFUL;
}

UINT _nx_nd_cache_add(NX_IP *ip_ptr, ULONG *dest_ip, NX_INTERFACE *if_ptr,
                      CHAR *mac, INT IsStatic, INT status,
                      NXD_IPV6_ADDRESS *iface_address, ND_CACHE_ENTRY **entry)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(dest_ip);
    NX_PARAMETER_NOT_USED(if_ptr);
    NX_PARAMETER_NOT_USED(mac);
    NX_PARAMETER_NOT_USED(IsStatic);
    NX_PARAMETER_NOT_USED(status);
    NX_PARAMETER_NOT_USED(iface_address);

    if (entry)
    {
        *entry = NX_NULL;
    }

    return NX_SUCCESS;
}

VOID _nx_icmpv6_send_queued_packets(NX_IP *ip_ptr, ND_CACHE_ENTRY *nd_entry)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(nd_entry);
}

/*
 * The destination table, which the advertisement path reaches only with Path
 * MTU Discovery enabled.  What it is asked for is recorded rather than stored:
 * the point is that the MTU option and Path MTU Discovery work from the same
 * clamped number, and this is where that can be seen.
 */
static NX_IPV6_DESTINATION_ENTRY h_dest_entry;
static UINT                      h_dest_adds;
static ULONG                     h_dest_added_mtu;
static ULONG                     h_dest_added_timeout;

UINT _nx_icmpv6_dest_table_add(NX_IP *ip_ptr, ULONG *destination_address,
                               NX_IPV6_DESTINATION_ENTRY **dest_entry_ptr, ULONG *next_hop,
                               ULONG path_mtu, ULONG mtu_timeout, NXD_IPV6_ADDRESS *ipv6_address)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(destination_address);
    NX_PARAMETER_NOT_USED(next_hop);
    NX_PARAMETER_NOT_USED(ipv6_address);

    h_dest_adds++;
    h_dest_added_mtu     = path_mtu;
    h_dest_added_timeout = mtu_timeout;

    if (dest_entry_ptr)
    {
        *dest_entry_ptr = &h_dest_entry;
    }

    return NX_SUCCESS;
}

UINT _nx_packet_release(NX_PACKET *packet_ptr)
{
    NX_PARAMETER_NOT_USED(packet_ptr);
    h_packets_released++;
    return NX_SUCCESS;
}

/* The solicitations, with the second they were sent in. */
#define H_MAX_SENDS     64

static ULONG h_now;                     /* seconds since the interface came up */
static UINT  h_send_count;
static ULONG h_send_at[H_MAX_SENDS];
static UINT  h_send_fails;              /* fail this many sends, then succeed  */

UINT _nx_icmpv6_send_rs(NX_IP *ip_ptr, UINT interface_index)
{
    NX_PARAMETER_NOT_USED(ip_ptr);
    NX_PARAMETER_NOT_USED(interface_index);

    if (h_send_fails)
    {
        h_send_fails--;
        return NX_NOT_SUCCESSFUL;
    }

    if (h_send_count < H_MAX_SENDS)
    {
        h_send_at[h_send_count] = h_now;
    }

    h_send_count++;
    return NX_SUCCESS;
}


/* ----------------------------------------------------------- the fixture -- */

static NX_IP           h_ip;
static NXD_IPV6_ADDRESS h_link_local;   /* the address the packet arrived on   */
static NX_IPV6_HEADER  h_ipv6_header;
static NX_PACKET       h_packet;
static UCHAR           h_message[256];

/* 00:80:10:49:00:01, so the modified EUI-64 is 0280:10ff:fe49:0001. */
#define H_MAC_MSW       0x00000080UL
#define H_MAC_LSW       0x10490001UL
#define H_EUI64_WORD2   0x028010FFUL
#define H_EUI64_WORD3   0xFE490001UL

#define H_LINK_MTU      1500

static VOID h_reset(VOID)
{
    NX_INTERFACE *if_ptr;

    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_link_local, 0, sizeof(h_link_local));
    memset(&h_ipv6_header, 0, sizeof(h_ipv6_header));
    memset(&h_packet, 0, sizeof(h_packet));
    memset(h_message, 0, sizeof(h_message));

    h_prefix_adds = 0;
    h_prefix_deletes = 0;
    h_prefix_added_length = 0;
    h_multicast_joins = 0;
    h_router_adds = 0;
    h_packets_released = 0;
    h_prefix_add_full = 0;
    h_rdnss_count = 0;
    memset(h_rdnss_address, 0, sizeof(h_rdnss_address));
    memset(h_rdnss_lifetime, 0, sizeof(h_rdnss_lifetime));
    h_send_count = 0;
    h_send_fails = 0;
    h_now = 0;
    h_dest_adds = 0;
    h_dest_added_mtu = 0;
    h_dest_added_timeout = 0;
    memset(&h_dest_entry, 0, sizeof(h_dest_entry));

    if_ptr = &h_ip.nx_ip_interface[0];

    if_ptr -> nx_interface_valid                = NX_TRUE;
    if_ptr -> nx_interface_name                 = "eth0";
    if_ptr -> nx_interface_link_up              = NX_TRUE;
    if_ptr -> nx_interface_ip_mtu_size          = H_LINK_MTU;
    if_ptr -> nx_interface_physical_address_msw = H_MAC_MSW;
    if_ptr -> nx_interface_physical_address_lsw = H_MAC_LSW;
    if_ptr -> nx_interface_index                = 0;

    /* fe80::280:10ff:fe49:1, the address a router advertisement arrives on. */
    h_link_local.nxd_ipv6_address_valid    = NX_TRUE;
    h_link_local.nxd_ipv6_address_type     = NX_IP_VERSION_V6;
    h_link_local.nxd_ipv6_address_attached = if_ptr;
    h_link_local.nxd_ipv6_address_state    = NX_IPV6_ADDR_STATE_VALID;
    h_link_local.nxd_ipv6_address[0]       = 0xFE800000UL;
    h_link_local.nxd_ipv6_address[1]       = 0UL;
    h_link_local.nxd_ipv6_address[2]       = H_EUI64_WORD2;
    h_link_local.nxd_ipv6_address[3]       = H_EUI64_WORD3;

    if_ptr -> nxd_interface_ipv6_address_list_head = NX_NULL;
}

/*
 * The advertisement.  The IPv6 header is in host order, which is what
 * _nx_ipv6_packet_receive() leaves behind for the dispatcher; the ICMPv6
 * message is in network order, which is what comes off the wire, so the
 * multi-byte fields go through the same endian macros the stack uses.
 */
static VOID h_build_ra(UCHAR *message, UINT *length, USHORT router_lifetime)
{
NX_ICMPV6_RA *ra = (NX_ICMPV6_RA *)message;

    /* Source fe80::1, the router.  Destination ff02::1, all nodes. */
    h_ipv6_header.nx_ip_header_source_ip[0] = 0xFE800000UL;
    h_ipv6_header.nx_ip_header_source_ip[1] = 0UL;
    h_ipv6_header.nx_ip_header_source_ip[2] = 0UL;
    h_ipv6_header.nx_ip_header_source_ip[3] = 1UL;
    h_ipv6_header.nx_ip_header_destination_ip[0] = 0xFF020000UL;
    h_ipv6_header.nx_ip_header_destination_ip[1] = 0UL;
    h_ipv6_header.nx_ip_header_destination_ip[2] = 0UL;
    h_ipv6_header.nx_ip_header_destination_ip[3] = 1UL;

    /* Hop limit 255, which RFC 4861 requires and validate_ra enforces. */
    h_ipv6_header.nx_ip_header_word_1 = 0x000000FFUL;

    memset(message, 0, sizeof(h_message));

    ra -> nx_icmpv6_ra_icmpv6_header.nx_icmpv6_header_type = NX_ICMPV6_ROUTER_ADVERTISEMENT_TYPE;
    ra -> nx_icmpv6_ra_icmpv6_header.nx_icmpv6_header_code = 0;
    ra -> nx_icmpv6_ra_hop_limit     = 64;
    ra -> nx_icmpv6_ra_flag          = 0;
    ra -> nx_icmpv6_ra_router_lifetime = router_lifetime;
    NX_CHANGE_USHORT_ENDIAN(ra -> nx_icmpv6_ra_router_lifetime);

    *length = (UINT)sizeof(NX_ICMPV6_RA);
}

static VOID h_add_prefix_option(UCHAR *message, UINT *length, UCHAR flags,
                                ULONG valid_lifetime, ULONG preferred_lifetime,
                                UCHAR prefix_length)
{
NX_ICMPV6_OPTION_PREFIX *option = (NX_ICMPV6_OPTION_PREFIX *)(message + *length);

    option -> nx_icmpv6_option_prefix_type          = ICMPV6_OPTION_TYPE_PREFIX_INFO;
    option -> nx_icmpv6_option_prefix_optionlength  = 4;   /* 4 * 8 == 32 bytes */
    option -> nx_icmpv6_option_prefix_length        = prefix_length;
    option -> nx_icmpv6_option_prefix_flag          = flags;

    option -> nx_icmpv6_option_prefix_valid_lifetime = valid_lifetime;
    NX_CHANGE_ULONG_ENDIAN(option -> nx_icmpv6_option_prefix_valid_lifetime);
    option -> nx_icmpv6_option_prefix_preferred_lifetime = preferred_lifetime;
    NX_CHANGE_ULONG_ENDIAN(option -> nx_icmpv6_option_prefix_preferred_lifetime);
    option -> nx_icmpv6_option_prefix_reserved = 0;

    /* 2001:db8:1234:5678::/64 */
    option -> nx_icmpv6_option_prefix[0] = 0x20010DB8UL;
    option -> nx_icmpv6_option_prefix[1] = 0x12345678UL;
    option -> nx_icmpv6_option_prefix[2] = 0UL;
    option -> nx_icmpv6_option_prefix[3] = 0UL;
    NX_IPV6_ADDRESS_CHANGE_ENDIAN(option -> nx_icmpv6_option_prefix);

    *length += (UINT)sizeof(NX_ICMPV6_OPTION_PREFIX);
}

static VOID h_add_mtu_option(UCHAR *message, UINT *length, ULONG mtu)
{
NX_ICMPV6_OPTION_MTU *option = (NX_ICMPV6_OPTION_MTU *)(message + *length);

    option -> nx_icmpv6_option_mtu_icmpv6_header.nx_icmpv6_header_type = ICMPV6_OPTION_TYPE_MTU;
    option -> nx_icmpv6_option_mtu_icmpv6_header.nx_icmpv6_header_code = 1; /* length, 1 * 8 */
    option -> nx_icmpv6_option_mtu_icmpv6_header.nx_icmpv6_header_checksum = 0;

    option -> nx_icmpv6_option_mtu_path_mtu = mtu;
    NX_CHANGE_ULONG_ENDIAN(option -> nx_icmpv6_option_mtu_path_mtu);

    *length += (UINT)sizeof(NX_ICMPV6_OPTION_MTU);
}

/*
 * The RFC 8106 option: the header, then `count` addresses of 2001:db8::53:<n>.
 * The option length is in 8-byte units and covers both, so 1 + 2 * count.
 */
static VOID h_add_rdnss_option(UCHAR *message, UINT *length, UINT count,
                               ULONG lifetime, UCHAR option_length_override)
{
NX_ICMPV6_OPTION_RDNSS *option = (NX_ICMPV6_OPTION_RDNSS *)(message + *length);
ULONG                  *address;
UINT                    i;

    option -> nx_icmpv6_option_rdnss_type     = ICMPV6_OPTION_TYPE_RDNSS;
    option -> nx_icmpv6_option_rdnss_length   = option_length_override
                                                    ? option_length_override
                                                    : (UCHAR)(1 + (count << 1));
    option -> nx_icmpv6_option_rdnss_reserved = 0;

    option -> nx_icmpv6_option_rdnss_lifetime = lifetime;
    NX_CHANGE_ULONG_ENDIAN(option -> nx_icmpv6_option_rdnss_lifetime);

    address = (ULONG *)(message + *length + sizeof(NX_ICMPV6_OPTION_RDNSS));

    for (i = 0; i < count; i++)
    {
        address[0] = 0x20010DB8UL;
        address[1] = 0UL;
        address[2] = 0x00530000UL;
        address[3] = i + 1;
        NX_IPV6_ADDRESS_CHANGE_ENDIAN(address);
        address += 4;
    }

    *length += (UINT)(option -> nx_icmpv6_option_rdnss_length << 3);
}

/* Was the n-th server handed over 2001:db8::53:<n+1>? */
static int h_rdnss_is(UINT n, ULONG last)
{
    return (n < h_rdnss_count) &&
           (h_rdnss_address[n][0] == 0x20010DB8UL) &&
           (h_rdnss_address[n][1] == 0UL) &&
           (h_rdnss_address[n][2] == 0x00530000UL) &&
           (h_rdnss_address[n][3] == last);
}

static VOID h_rdnss_notify(NX_IP *ip_ptr, UINT interface_index,
                           ULONG *dns_address, ULONG lifetime)
{
    (VOID)ip_ptr;
    (VOID)interface_index;

    if (h_rdnss_count < H_RDNSS_MAX)
    {
        COPY_IPV6_ADDRESS(dns_address, h_rdnss_address[h_rdnss_count]);
        h_rdnss_lifetime[h_rdnss_count] = lifetime;
    }

    h_rdnss_count++;
}

static VOID h_deliver(UCHAR *message, UINT length)
{
    h_packet.nx_packet_prepend_ptr = message;
    h_packet.nx_packet_append_ptr  = message + length;
    h_packet.nx_packet_length      = length;
    h_packet.nx_packet_ip_header   = (UCHAR *)&h_ipv6_header;
    h_packet.nx_packet_next        = NX_NULL;
    h_packet.nx_packet_address.nx_packet_ipv6_address_ptr = &h_link_local;

    _nx_icmpv6_process_ra(&h_ip, &h_packet);
}

/* The global address the advertisement should have produced, or NX_NULL. */
static NXD_IPV6_ADDRESS *h_formed_address(VOID)
{
NXD_IPV6_ADDRESS *entry = h_ip.nx_ip_interface[0].nxd_interface_ipv6_address_list_head;

    while (entry != NX_NULL)
    {
        if ((entry != &h_link_local) && entry -> nxd_ipv6_address_valid)
        {
            return entry;
        }

        entry = entry -> nxd_ipv6_address_next;
    }

    return NX_NULL;
}

static int h_is_expected_address(NXD_IPV6_ADDRESS *entry)
{
    return (entry != NX_NULL) &&
           (entry -> nxd_ipv6_address[0] == 0x20010DB8UL) &&
           (entry -> nxd_ipv6_address[1] == 0x12345678UL) &&
           (entry -> nxd_ipv6_address[2] == H_EUI64_WORD2) &&
           (entry -> nxd_ipv6_address[3] == H_EUI64_WORD3) &&
           (entry -> nxd_ipv6_address_prefix_length == 64);
}


/* ------------------------------------------------- the prefix information -- */

/* The two flag bits, in the option byte. */
#define H_ONLINK        0x80
#define H_AUTONOMOUS    0x40

static VOID h_prefix_case(UCHAR flags, ULONG valid_lifetime,
                          int expect_address, int expect_prefix_add,
                          const char *label)
{
UINT              length;
NXD_IPV6_ADDRESS *formed;
char              what[128];

    h_reset();
    h_build_ra(h_message, &length, 1800);
    h_add_prefix_option(h_message, &length, flags, valid_lifetime,
                        valid_lifetime, 64);
    h_deliver(h_message, length);

    formed = h_formed_address();

    snprintf(what, sizeof(what), "%s: %s an address",
             label, expect_address ? "forms" : "forms no");
    h_check(expect_address ? h_is_expected_address(formed)
                           : (formed == NX_NULL), what);

    snprintf(what, sizeof(what), "%s: %s the prefix list",
             label, expect_prefix_add ? "records in" : "does not touch");
    h_check(expect_prefix_add ? (h_prefix_adds == 1) : (h_prefix_adds == 0),
            what);

    if (expect_address)
    {
        snprintf(what, sizeof(what), "%s: joins the solicited-node group",
                 label);
        h_check(h_multicast_joins == 1, what);

        snprintf(what, sizeof(what),
                 "%s: the address starts duplicate address detection", label);
        h_check((formed != NX_NULL) &&
                (formed -> nxd_ipv6_address_state == NX_IPV6_ADDR_STATE_TENTATIVE),
                what);
    }

    snprintf(what, sizeof(what), "%s: the packet is released", label);
    h_check(h_packets_released == 1, what);
}


/* ---------------------------------------------------------- the MTU option -- */

static ULONG h_mtu_case(ULONG advertised)
{
UINT length;

    h_build_ra(h_message, &length, 1800);
    h_add_mtu_option(h_message, &length, advertised);
    h_deliver(h_message, length);

    return h_ip.nx_ip_interface[0].nx_interface_ip_mtu_size;
}


/* -------------------------------------------------------- the solicitations -- */

/*
 * Seed the interface the way nxd_ipv6_enable() does, then run the once-a-second
 * check for `seconds` seconds.
 */
static VOID h_solicit_for(ULONG seconds)
{
NX_INTERFACE *if_ptr = &h_ip.nx_ip_interface[0];

    if_ptr -> nx_ipv6_rtr_solicitation_max      = NX_ICMPV6_MAX_RTR_SOLICITATIONS;
    if_ptr -> nx_ipv6_rtr_solicitation_count    = NX_ICMPV6_MAX_RTR_SOLICITATIONS;
    if_ptr -> nx_ipv6_rtr_solicitation_interval = NX_ICMPV6_RTR_SOLICITATION_INTERVAL;
    if_ptr -> nx_ipv6_rtr_solicitation_timer    = NX_ICMPV6_RTR_SOLICITATION_DELAY;

    for (h_now = 1; h_now <= seconds; h_now++)
    {
        _nxd_ipv6_router_solicitation_check(&h_ip);
    }
}

/* The gap between solicitation n and n-1. */
static ULONG h_gap(UINT n)
{
    return (n == 0) ? h_send_at[0] : (h_send_at[n] - h_send_at[n - 1]);
}

/* Within the RFC 3315 randomisation of -0.1 to +0.1, plus a second of
   one-second-resolution slack at each end. */
static int h_gap_near(ULONG gap, ULONG want)
{
ULONG spread = (want / 10) + 1;

    return (gap + spread >= want) && (gap <= want + spread);
}


/* ----------------------------------------------------------------- main --- */

int main(void)
{
UINT  i;
ULONG mtu;
char  what[128];

    printf("AmiNetXDuo -- IPv6 router advertisement and solicitation\n");

    /* rand() is NX_RAND on this host and the backoff randomisation uses it.
       Seeded so a failure can be reproduced. */
    srand(1);

    /* --- (1) the prefix information option's two flags are independent --- */

    /* On-link and autonomous, which always worked. */
    h_prefix_case(H_ONLINK | H_AUTONOMOUS, 3600, 1, 1, "A=1 L=1");

    /* Autonomous only.  This is the one that formed nothing. */
    h_prefix_case(H_AUTONOMOUS, 3600, 1, 0, "A=1 L=0");

    /* On-link only: a prefix to route directly to, and no address from it. */
    h_prefix_case(H_ONLINK, 3600, 0, 1, "A=0 L=1");

    /* Neither flag: nothing at all. */
    h_prefix_case(0, 3600, 0, 0, "A=0 L=0");

    /* A zero valid lifetime forms no address, RFC 4862 5.5.3(c), whether or
       not the prefix was also advertised on-link. */
    h_prefix_case(H_AUTONOMOUS, 0, 0, 0, "A=1 L=0 lifetime 0");

    {
    UINT              length;
    NXD_IPV6_ADDRESS *formed;

        h_reset();
        h_build_ra(h_message, &length, 1800);
        h_add_prefix_option(h_message, &length, H_ONLINK | H_AUTONOMOUS, 0, 0, 64);
        h_deliver(h_message, length);

        h_check(h_formed_address() == NX_NULL,
                "A=1 L=1 lifetime 0: forms no address");
        h_check(h_prefix_deletes == 1,
                "A=1 L=1 lifetime 0: removes the prefix list entry");

        /* A prefix length that leaves no room for the interface identifier. */
        h_reset();
        h_build_ra(h_message, &length, 1800);
        h_add_prefix_option(h_message, &length, H_AUTONOMOUS, 3600, 3600, 48);
        h_deliver(h_message, length);

        h_check(h_formed_address() == NX_NULL,
                "A=1 prefix /48: forms no address");

        /* An on-link prefix the prefix list will not hold forms no address
           either, which is what this code did before the flags were split. */
        h_reset();
        h_prefix_add_full = 1;
        h_build_ra(h_message, &length, 1800);
        h_add_prefix_option(h_message, &length, H_ONLINK | H_AUTONOMOUS,
                            3600, 3600, 64);
        h_deliver(h_message, length);

        h_check(h_formed_address() == NX_NULL,
                "A=1 L=1 with a full prefix list: forms no address");

        /* And a prefix that was never offered on-link is not affected by the
           prefix list's capacity at all. */
        h_reset();
        h_prefix_add_full = 1;
        h_build_ra(h_message, &length, 1800);
        h_add_prefix_option(h_message, &length, H_AUTONOMOUS, 3600, 3600, 64);
        h_deliver(h_message, length);

        formed = h_formed_address();
        h_check(h_is_expected_address(formed),
                "A=1 L=0 with a full prefix list: forms an address");
    }

    /* ------------------------- (2) the MTU option ------------------------- */

    h_reset();
    mtu = h_mtu_case(1400);
    h_check(mtu == 1400, "MTU option 1400 lowers the interface MTU");

    h_reset();
    mtu = h_mtu_case(9000);
    h_check(mtu == H_LINK_MTU,
            "MTU option 9000 does not raise the interface above the link MTU");

    h_reset();
    mtu = h_mtu_case(1000);
    h_check(mtu == H_LINK_MTU,
            "MTU option below the IPv6 minimum is ignored");

    h_reset();
    mtu = h_mtu_case(NX_MINIMUM_IPV6_PATH_MTU);
    h_check(mtu == (ULONG)NX_MINIMUM_IPV6_PATH_MTU,
            "MTU option at the IPv6 minimum is taken");

    /*
     * And with Path MTU Discovery on, the router's own destination entry is
     * written from that same clamped number rather than from what was
     * advertised.  9000 became the link MTU above; the table has to agree, or
     * the stack would send 9000-byte packets at a router it had just decided
     * was 1500.
     */
    h_reset();
    (VOID)h_mtu_case(9000);
    h_check(h_dest_adds == 1, "the MTU option writes one destination entry");
    h_check(h_dest_added_mtu == H_LINK_MTU,
            "the destination entry takes the clamped MTU, not the advertised one");
    h_check(h_dest_added_timeout == NX_WAIT_FOREVER,
            "an MTU from a router advertisement is not aged out");

    /* ---------------------- (3) router solicitation ---------------------- */

    h_reset();
    h_solicit_for(24 * 60 * 60);        /* a day */

    h_check(h_send_count > NX_ICMPV6_MAX_RTR_SOLICITATIONS,
            "solicitation does not stop after NX_ICMPV6_MAX_RTR_SOLICITATIONS");

    h_check(h_send_at[0] == (ULONG)NX_ICMPV6_RTR_SOLICITATION_DELAY,
            "the first solicitation goes out after the initial delay");

    /* The configured number of solicitations at the base interval. */
    for (i = 1; i < (UINT)NX_ICMPV6_MAX_RTR_SOLICITATIONS; i++)
    {
        snprintf(what, sizeof(what),
                 "solicitation %u is one base interval after the last", i + 1);
        h_check(h_gap(i) == (ULONG)NX_ICMPV6_RTR_SOLICITATION_INTERVAL, what);
    }

    /* Then the interval doubles, RFC 7559 section 2 over RFC 3315 section 14. */
    {
    ULONG want = (ULONG)NX_ICMPV6_RTR_SOLICITATION_INTERVAL;

        for (i = (UINT)NX_ICMPV6_MAX_RTR_SOLICITATIONS;
             (i < h_send_count) && (i < H_MAX_SENDS); i++)
        {
            want <<= 1;

            if (want > (ULONG)NX_ICMPV6_MAX_RTR_SOLICITATION_INTERVAL)
            {
                want = (ULONG)NX_ICMPV6_MAX_RTR_SOLICITATION_INTERVAL;
            }

            snprintf(what, sizeof(what),
                     "solicitation %u waits about %lu seconds, not %lu",
                     i + 1, (unsigned long)want, (unsigned long)h_gap(i));
            h_check(h_gap_near(h_gap(i), want), what);
        }

        h_check(want == (ULONG)NX_ICMPV6_MAX_RTR_SOLICITATION_INTERVAL,
                "the interval reaches its ceiling within a day");
    }

    /* A failed first send is retried the very next second, unchanged. */
    h_reset();
    h_send_fails = 2;
    h_solicit_for(60);
    h_check(h_send_at[0] == ((ULONG)NX_ICMPV6_RTR_SOLICITATION_DELAY + 2),
            "a first solicitation that could not be sent is retried each second");

    /* And an advertisement stops the whole thing. */
    {
    UINT length;

        h_reset();
        h_ip.nx_ip_interface[0].nx_ipv6_rtr_solicitation_max      = NX_ICMPV6_MAX_RTR_SOLICITATIONS;
        h_ip.nx_ip_interface[0].nx_ipv6_rtr_solicitation_count    = NX_ICMPV6_MAX_RTR_SOLICITATIONS;
        h_ip.nx_ip_interface[0].nx_ipv6_rtr_solicitation_interval = NX_ICMPV6_RTR_SOLICITATION_INTERVAL;
        h_ip.nx_ip_interface[0].nx_ipv6_rtr_solicitation_timer    = NX_ICMPV6_RTR_SOLICITATION_DELAY;

        for (h_now = 1; h_now <= 10; h_now++)
        {
            _nxd_ipv6_router_solicitation_check(&h_ip);
        }

        h_check(h_send_count > 0, "solicitation started");

        h_build_ra(h_message, &length, 1800);
        h_add_prefix_option(h_message, &length, H_ONLINK | H_AUTONOMOUS,
                            3600, 3600, 64);
        h_deliver(h_message, length);

        h_check(h_ip.nx_ip_interface[0].nx_ipv6_rtr_solicitation_count == 0,
                "a router advertisement stops the solicitations");

        {
        UINT before = h_send_count;

            for (; h_now <= 4000; h_now++)
            {
                _nxd_ipv6_router_solicitation_check(&h_ip);
            }

            h_check(h_send_count == before,
                    "and nothing is sent after it");
        }
    }

    /* --- (4) the recursive DNS server option, RFC 8106 --- */

    /*
     * On an IPv6-only link this is the resolver's only route in: no DHCPv6 is
     * built and the name_resolution file takes a dotted quad.  Before this the
     * option fell through the walk's if/else-if chain with everything else the
     * stack does not parse.
     */
    {
    UINT length;

        /* Two servers alongside a prefix, which is what a router sends. */
        h_reset();
        h_ip.nx_ipv6_rdnss_notify = h_rdnss_notify;
        h_build_ra(h_message, &length, 1800);
        h_add_prefix_option(h_message, &length, H_ONLINK | H_AUTONOMOUS,
                            3600, 3600, 64);
        h_add_rdnss_option(h_message, &length, 2, 600, 0);
        h_deliver(h_message, length);

        h_check(h_rdnss_count == 2, "both advertised name servers are reported");
        h_check(h_rdnss_is(0, 1) && h_rdnss_is(1, 2),
                "in the order the option carried them, in host order");
        h_check(h_rdnss_lifetime[0] == 600 && h_rdnss_lifetime[1] == 600,
                "each with the option's lifetime");
        h_check(h_is_expected_address(h_formed_address()),
                "and the prefix in the same advertisement still forms an address");

        /* A lifetime of zero is a withdrawal and is still reported as one. */
        h_reset();
        h_ip.nx_ipv6_rdnss_notify = h_rdnss_notify;
        h_build_ra(h_message, &length, 1800);
        h_add_rdnss_option(h_message, &length, 1, 0, 0);
        h_deliver(h_message, length);

        h_check(h_rdnss_count == 1, "a zero lifetime is reported, not skipped");
        h_check(h_rdnss_lifetime[0] == 0, "with the lifetime it carried");

        /* An option carrying no address at all: the header is the whole thing. */
        h_reset();
        h_ip.nx_ipv6_rdnss_notify = h_rdnss_notify;
        h_build_ra(h_message, &length, 1800);
        h_add_rdnss_option(h_message, &length, 0, 600, 0);
        h_add_prefix_option(h_message, &length, H_ONLINK | H_AUTONOMOUS,
                            3600, 3600, 64);
        h_deliver(h_message, length);

        h_check(h_rdnss_count == 0, "an option with no server reports none");
        h_check(h_is_expected_address(h_formed_address()),
                "and does not stop the option after it being processed");

        /*
         * A length that claims more addresses than the option holds.  The walk
         * is bounded by the length field either way, so what matters is that
         * the count taken from it never reads past the option: 5 units is one
         * header and two addresses, and two is what may be reported.
         */
        h_reset();
        h_ip.nx_ipv6_rdnss_notify = h_rdnss_notify;
        h_build_ra(h_message, &length, 1800);
        h_add_rdnss_option(h_message, &length, 2, 600, 5);
        h_deliver(h_message, length);

        h_check(h_rdnss_count == 2,
                "the server count comes from the option length");

        /* No callback installed: the option is skipped and nothing crashes. */
        h_reset();
        h_build_ra(h_message, &length, 1800);
        h_add_rdnss_option(h_message, &length, 2, 600, 0);
        h_add_prefix_option(h_message, &length, H_ONLINK | H_AUTONOMOUS,
                            3600, 3600, 64);
        h_deliver(h_message, length);

        h_check(h_rdnss_count == 0, "with no callback nothing is reported");
        h_check(h_is_expected_address(h_formed_address()),
                "and the rest of the advertisement is processed anyway");
    }

    printf("%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
