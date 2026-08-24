/*
 * bsdsocket.library, NetStackQuery() and NetStackControl().
 *
 * The two private LVOs that let a separate Shell command ask the running
 * stack what it is doing, and tell it to change. include/aminetxduo/
 * netstatus.h has the interface. This file has the walks.
 *
 * Before these existed, every command that wanted live numbers linked its own
 * copy of NetX Duo. It got its own NX_IP with no interfaces in it and read
 * zeroes. Once src/tools/netstack_weak.c's weak stubs answered NULL, it
 * printed "the network is up, but this command cannot read it" and exited 5.
 *
 * Three rules here:
 *
 *   1. Copy, never lend. Nothing that leaves here is a pointer into the
 *      stack. The caller gets scalars in its own buffer.
 *
 *   2. One bracket, held briefly. bsd_nx_enter() adopts the calling task as a
 *      TX_THREAD, required, because nx_*_info_get() are behind
 *      NX_THREADS_ONLY_CALLER_CHECKING, and the walk does nothing but read
 *      memory and call those. No dos.library, no Wait(), no allocation.
 *
 *   3. The caller's buffer is touched only after the header checks pass, so a
 *      caller that arrived here by accident (a future vendor putting a
 *      different function at the same offset) presents the wrong magic and
 *      gets -1 with nothing written.
 *
 * There is no "ping for me" vector. nx_icmp_ping() matches an inbound echo
 * reply on the sequence number alone. nx_icmpv4_process_echo_reply.c:124
 * compares tx_thread_suspend_info against nx_icmpv4_echo_sequence_num and
 * looks at nothing else, and nx_icmpv4.h:191 says the identifier "is not used
 * as a host". Any vector over it inherits that. The raw socket path
 * (src/bsdsocket/raw.c) lets the command choose its own matching rule, is
 * already published ABI, and is what src/tools/ping.c uses.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
/* bsd_if_set_address(), shared with ConfigureInterfaceTagList(). */
#include "interfaces.h"

#include "aminetxduo/netstatus.h"
#include "aminetxduo/budget.h"
#include "aminetxduo/sana2.h"
#include "aminetxduo/config.h"
#include "aminetxduo/netstack.h"
#include "aminetxduo/events.h"

/* tx_amiga_tick_stats(), for NETSTATUS_HEALTH. */
#include "tx_amiga.h"

#ifdef AMINETXDUO_IPV6
/* ND_CACHE_STATE_*, which nx_api.h does not carry. */
#include "nx_nd_cache.h"
#endif

/*
 * The wire values in netstatus.h must be NetX Duo's own, because the socket
 * table copies them straight across. If a NetX Duo update renumbers them the
 * build stops here rather than at a user reading "ESTABLISHED" off a socket
 * that is closing.
 */
#ifdef AMINETXDUO_IPV6
_Static_assert(NETSTATUS_IP6_TENTATIVE  == NX_IPV6_ADDR_STATE_TENTATIVE,
               "IPv6 address state ABI");
_Static_assert(NETSTATUS_IP6_PREFERRED  == NX_IPV6_ADDR_STATE_PREFERRED,
               "IPv6 address state ABI");
_Static_assert(NETSTATUS_IP6_DEPRECATED == NX_IPV6_ADDR_STATE_DEPRECATED,
               "IPv6 address state ABI");
_Static_assert(NETSTATUS_IP6_VALID      == NX_IPV6_ADDR_STATE_VALID,
               "IPv6 address state ABI");

_Static_assert(NETSTATUS_ND_INCOMPLETE == ND_CACHE_STATE_INCOMPLETE,
               "neighbour state ABI");
_Static_assert(NETSTATUS_ND_REACHABLE  == ND_CACHE_STATE_REACHABLE,
               "neighbour state ABI");
_Static_assert(NETSTATUS_ND_STALE      == ND_CACHE_STATE_STALE,
               "neighbour state ABI");
_Static_assert(NETSTATUS_ND_DELAY      == ND_CACHE_STATE_DELAY,
               "neighbour state ABI");
_Static_assert(NETSTATUS_ND_PROBE      == ND_CACHE_STATE_PROBE,
               "neighbour state ABI");
_Static_assert(NETSTATUS_ND_CREATED    == ND_CACHE_STATE_CREATED,
               "neighbour state ABI");
#endif

_Static_assert(NETSTATUS_TCP_CLOSED       == NX_TCP_CLOSED,        "TCP state ABI");
_Static_assert(NETSTATUS_TCP_LISTEN       == NX_TCP_LISTEN_STATE,  "TCP state ABI");
_Static_assert(NETSTATUS_TCP_SYN_SENT     == NX_TCP_SYN_SENT,      "TCP state ABI");
_Static_assert(NETSTATUS_TCP_SYN_RECEIVED == NX_TCP_SYN_RECEIVED,  "TCP state ABI");
_Static_assert(NETSTATUS_TCP_ESTABLISHED  == NX_TCP_ESTABLISHED,   "TCP state ABI");
_Static_assert(NETSTATUS_TCP_CLOSE_WAIT   == NX_TCP_CLOSE_WAIT,    "TCP state ABI");
_Static_assert(NETSTATUS_TCP_FIN_WAIT_1   == NX_TCP_FIN_WAIT_1,    "TCP state ABI");
_Static_assert(NETSTATUS_TCP_FIN_WAIT_2   == NX_TCP_FIN_WAIT_2,    "TCP state ABI");
_Static_assert(NETSTATUS_TCP_CLOSING      == NX_TCP_CLOSING,       "TCP state ABI");
_Static_assert(NETSTATUS_TCP_TIMED_WAIT   == NX_TCP_TIMED_WAIT,    "TCP state ABI");
_Static_assert(NETSTATUS_TCP_LAST_ACK     == NX_TCP_LAST_ACK,      "TCP state ABI");

/* The header is copied by hand below. Its size is part of the ABI. */
_Static_assert(sizeof(NetStatusHeader) == 16, "NetStatusHeader ABI");

#ifdef AMINETXDUO_MDNS
/*
 * ns_fill_services() copies field for field between the two, so a width that
 * drifts silently truncates a name rather than fails to build.
 */
_Static_assert(NETSTATUS_SVC_NAME_LEN == AMI_MDNS_SVC_NAME_LEN, "service ABI");
_Static_assert(NETSTATUS_SVC_TYPE_LEN == AMI_MDNS_SVC_TYPE_LEN, "service ABI");
_Static_assert(NETSTATUS_SVC_HOST_LEN == AMI_MDNS_SVC_HOST_LEN, "service ABI");
_Static_assert(NETSTATUS_SVC_TXT_LEN  == AMI_MDNS_SVC_TXT_LEN,  "service ABI");

/* NETCTRL_MDNS_BROWSE carries a service type in the same width. */
_Static_assert(sizeof(((NetStatusControl *)0)->nsc_Name)
                   == NETSTATUS_SVC_TYPE_LEN, "service ABI");
#endif

/* ------------------------------------------------------------- plumbing, */

static VOID ns_zero(APTR mem, ULONG len)
{
    UBYTE *p = (UBYTE *)mem;

    while (len-- > 0)
        *p++ = 0;
}

static VOID ns_copy_name(char *dst, ULONG dstlen, const char *src)
{
    ULONG i = 0;

    if (dstlen == 0)
        return;

    if (src != NULL)
    {
        while (i + 1 < dstlen && src[i] != '\0')
        {
            dst[i] = src[i];
            i++;
        }
    }

    dst[i] = '\0';
}

/* A text field carried inline in a caller's fixed-size control block. */
static BOOL ns_terminated(const char *text, ULONG size)
{
    ULONG i;

    for (i = 0; i < size; i++)
    {
        if (text[i] == '\0')
            return TRUE;
    }

    return FALSE;
}

static VOID ns_mac_from_words(ULONG msw, ULONG lsw, UBYTE *mac)
{
    mac[0] = (UBYTE)((msw >> 8) & 0xff);
    mac[1] = (UBYTE)(msw & 0xff);
    mac[2] = (UBYTE)((lsw >> 24) & 0xff);
    mac[3] = (UBYTE)((lsw >> 16) & 0xff);
    mac[4] = (UBYTE)((lsw >> 8) & 0xff);
    mac[5] = (UBYTE)(lsw & 0xff);
}

/* ThreadX ticks -> milliseconds, for the records that report a duration. */
static ULONG ns_ticks_ms(ULONG ticks)
{
    const ULONG rate = (ULONG)NX_IP_PERIODIC_RATE;
    const ULONG max  = (ULONG)-1;
    ULONG       whole;
    ULONG       fraction;

    if (ticks / rate > max / 1000UL)
        return max;

    whole    = (ticks / rate) * 1000UL;
    fraction = ((ticks % rate) * 1000UL) / rate;

    return (fraction > max - whole) ? max : whole + fraction;
}

typedef struct NsWriter
{
    NetStatusHeader *hdr;
    UBYTE           *entries;
    ULONG            room;          /* entries that fit                     */
    ULONG            entry_size;
    ULONG            written;
    ULONG            available;
} NsWriter;

/*
 * How many entries of `entry_size` fit after the header, and where the first
 * one goes. `room` is 0 when the buffer holds only the header, which is a
 * legitimate way to ask "how many are there?". nsh_Available is still filled
 * in.
 */
static VOID ns_writer_init(NsWriter *w, NetStatusHeader *hdr, ULONG size,
                           UWORD type, ULONG entry_size)
{
    w->hdr        = hdr;
    w->entries    = (UBYTE *)hdr + sizeof(NetStatusHeader);
    w->entry_size = entry_size;
    w->room       = (size - sizeof(NetStatusHeader)) / entry_size;
    w->written    = 0;
    w->available  = 0;

    hdr->nsh_Type      = type;
    hdr->nsh_EntrySize = (UWORD)entry_size;
}

/*
 * The next slot to fill, or NULL when the buffer is full. `available` counts
 * regardless, so a caller with a small buffer still learns what size it
 * needs.
 */
static APTR ns_writer_next(NsWriter *w)
{
    APTR slot;

    w->available++;

    if (w->written >= w->room)
        return NULL;

    slot = w->entries + (w->written * w->entry_size);
    w->written++;
    ns_zero(slot, w->entry_size);

    return slot;
}

static VOID ns_writer_finish(NsWriter *w)
{
    w->hdr->nsh_Count     = (UWORD)w->written;
    w->hdr->nsh_Available = (UWORD)w->available;
}

/* ------------------------------------------------------------- the walks, */

static VOID ns_fill_system(NX_IP *ip, NetStatusSystem *out)
{
    NX_PACKET_POOL  *pool;
    const AmiConfig *cfg     = netstack_config();
    ULONG            gateway = 0;

    out->nss_Flags          = NETSTATUS_SYS_UP;
    out->nss_InterfaceCount = (ULONG)NX_MAX_PHYSICAL_INTERFACES;

    /* The running stack's, not the disk's: DHCP can rename the machine after
       the files are read. */
    if (cfg != NULL)
        out->nss_HostSource = (ULONG)cfg->hostname_source;

#ifdef AMINETXDUO_IPV6
    out->nss_Flags |= NETSTATUS_SYS_IPV6;
#endif
#ifdef NX_ENABLE_IP_STATIC_ROUTING
    out->nss_Flags |= NETSTATUS_SYS_ROUTING;
#endif

    if (nx_ip_gateway_address_get(ip, &gateway) == NX_SUCCESS && gateway != 0)
    {
        out->nss_Gateway = gateway;
        out->nss_Flags  |= NETSTATUS_SYS_GATEWAY;
    }

#ifdef AMINETXDUO_MDNS
    {
        const char *mdns = netstack_mdns_hostname();

        out->nss_Flags |= NETSTATUS_SYS_MDNS;

        /*
         * NULL before the responder claimed a name. The flag already
         * distinguishes that from a build without mDNS, so an empty string
         * here means "not yet", not "never".
         */
        if (mdns != NULL && mdns[0] != '\0')
        {
            static const char suffix[] = ".local";
            ULONG i;
            ULONG j;

            /*
             * The responder keeps the bare label ("amiga"). ".local" is
             * appended here rather than at each display, so every consumer
             * gets the name in the form it is used in.
             */
            for (i = 0; i + 1 < (ULONG)NETSTATUS_NAME_LEN && mdns[i] != '\0';
                 i++)
            {
                out->nss_MdnsName[i] = mdns[i];
            }

            for (j = 0; suffix[j] != '\0' &&
                        i + 1 < (ULONG)NETSTATUS_NAME_LEN; j++)
            {
                out->nss_MdnsName[i++] = suffix[j];
            }

            out->nss_MdnsName[i] = '\0';
        }
    }
#endif

    pool = ip->nx_ip_default_packet_pool;
    if (pool != NX_NULL &&
        nx_packet_pool_info_get(pool, &out->nss_PoolTotal, &out->nss_PoolFree,
                                &out->nss_PoolEmptyRequests,
                                &out->nss_PoolEmptySuspensions,
                                &out->nss_PoolInvalidReleases) == NX_SUCCESS)
    {
        out->nss_PoolPayload = pool->nx_packet_pool_payload_size;
    }
}

/*
 * One row per interface, including the ones not using DHCP, so a caller can
 * tell "no DHCP on this interface" from "interface missing".
 *
 * netstack_interface_dhcp_lease() re-reads the live NX_DHCP every time, so
 * this is current rather than a copy of what was true at bring-up. It brackets
 * itself with ami_netstack_enter(), which is a no-op inside the bsd_nx_enter()
 * this already runs under. The nesting is detected, not paid for.
 */
static VOID ns_fill_dhcp(NsWriter *w)
{
    UWORD index;

    for (index = 0; index < (UWORD)NX_MAX_PHYSICAL_INTERFACES; index++)
    {
        NetStatusDhcp *out;
        AmiDhcpLease   lease;
        LONG           state;
        UWORD          i;

        out = (NetStatusDhcp *)ns_writer_next(w);
        if (out == NULL)
            continue;                   /* still count every interface */

        out->nsd_Index = index;

        state = netstack_interface_dhcp_state(index);

        /* Before the `continue` below. A caller that watches a renewal needs
           it on an interface that reports no lease this instant as much as on
           one that does. */
        out->nsd_RawState = netstack_interface_dhcp_raw_state(index);

        if (state == AMI_DHCP_BOUND)
            out->nsd_State = NETSTATUS_DHCP_BOUND;
        else if (state == AMI_DHCP_WORKING)
            out->nsd_State = NETSTATUS_DHCP_WORKING;
        else
            out->nsd_State = NETSTATUS_DHCP_OFF;

        if (out->nsd_State != NETSTATUS_DHCP_BOUND)
            continue;                   /* ns_writer_next() zeroed the rest */

        if (netstack_interface_dhcp_lease(index, &lease) != AMI_NET_OK)
        {
            /* Bound a moment ago and not now: report the state, not a lease
               of zeroes. */
            out->nsd_State = NETSTATUS_DHCP_WORKING;
            continue;
        }

        out->nsd_Address      = lease.adl_Address;
        out->nsd_NetMask      = lease.adl_NetMask;
        out->nsd_Server       = lease.adl_Server;
        out->nsd_LeaseSeconds = lease.adl_LeaseSeconds;

        out->nsd_RouterCount = lease.adl_RouterCount;
        if (out->nsd_RouterCount > (UWORD)NETSTATUS_DHCP_ADDRS)
            out->nsd_RouterCount = (UWORD)NETSTATUS_DHCP_ADDRS;
        for (i = 0; i < out->nsd_RouterCount; i++)
            out->nsd_Router[i] = lease.adl_Router[i];

        out->nsd_DnsCount = lease.adl_DnsCount;
        if (out->nsd_DnsCount > (UWORD)NETSTATUS_DHCP_ADDRS)
            out->nsd_DnsCount = (UWORD)NETSTATUS_DHCP_ADDRS;
        for (i = 0; i < out->nsd_DnsCount; i++)
            out->nsd_Dns[i] = lease.adl_Dns[i];

        out->nsd_StaticRouteCount = lease.adl_StaticRouteCount;
        if (out->nsd_StaticRouteCount > (UWORD)NETSTATUS_DHCP_ADDRS)
            out->nsd_StaticRouteCount = (UWORD)NETSTATUS_DHCP_ADDRS;
        for (i = 0; i < out->nsd_StaticRouteCount; i++)
            out->nsd_StaticRoute[i] = lease.adl_StaticRoute[i];

        ns_copy_name(out->nsd_HostName, sizeof(out->nsd_HostName),
                     lease.adl_HostName);
        ns_copy_name(out->nsd_DomainName, sizeof(out->nsd_DomainName),
                     lease.adl_DomainName);
    }
}

/*
 * The NX physical interface index is the configuration index. src/netstack
 * attaches the configured interfaces in configuration order from slot 0
 * (netstack.c:771 for the primary, :860 for the rest). NetX Duo puts the
 * loopback at NX_LOOPBACK_INTERFACE == NX_MAX_PHYSICAL_INTERFACES, past the
 * physical range this walks, so there is no off-by-one to correct for.
 *
 * NULL when the slot is not configured. NETSTATUS_IF_NAMED tells the caller
 * whether a name came from the configuration.
 */
static const AmiIfConfig *ns_config_for(UINT nx_index)
{
    /*
     * The netstack is asked, rather than the configuration array subscripted
     * by the NX index.  ns_Iface[] is filled in open order, so an interface
     * that failed to open advances the configuration index without advancing
     * the NX one, and every interface after it is then reported under its
     * neighbour's name.
     */
    return netstack_iface_config((UWORD)nx_index);
}

/*
 * The IPv6 addresses, one entry per address per interface.
 *
 * netstack_ipv6_address_get() walks the interface's own list rather than the
 * flat nx_ipv6_address[] array, which is shared between interfaces and the
 * loopback ::1 entry.  It reads memory and nothing else, so it is safe under
 * the bracket bsd_NetStackQuery() already holds.
 *
 * In a build without AMINETXDUO_IPV6 there are no addresses and the answer is
 * an empty table, which is what a caller compiled against the same header
 * expects: no entries, not an error.
 */
static VOID ns_fill_addresses6(NsWriter *w)
{
#ifdef AMINETXDUO_IPV6
    UWORD i;

    if (!netstack_ipv6_enabled())
        return;

    for (i = 0; i < (UWORD)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        UWORD slot;

        for (slot = 0; ; slot++)
        {
            ULONG              addr[4];
            ULONG              prefix = 0;
            ULONG              state = 0;
            NetStatusAddress6 *out;

            if (!netstack_ipv6_address_get(i, slot, addr, &prefix, &state))
                break;

            out = (NetStatusAddress6 *)ns_writer_next(w);
            if (out == NULL)
                continue;

            out->nsn_Interface    = i;
            out->nsn_State        = (UWORD)state;
            out->nsn_Address[0]   = addr[0];
            out->nsn_Address[1]   = addr[1];
            out->nsn_Address[2]   = addr[2];
            out->nsn_Address[3]   = addr[3];
            out->nsn_PrefixLength = prefix;
        }
    }
#else
    (VOID)w;
#endif
}

/* Which nx_ip_interface[] slot a route or a neighbour points at, 0 when it
   points nowhere. */
static UWORD ns_interface_index(NX_IP *ip, const NX_INTERFACE *nxif)
{
    UINT i;

    if (nxif == NX_NULL)
        return 0;

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        if (nxif == &ip->nx_ip_interface[i])
            return (UWORD)i;
    }

    return 0;
}

/*
 * Where IPv6 packets go, in the order _nx_ipv6_packet_send() decides it: the
 * on-link prefixes first, the default routers last.
 *
 * The on-link half has two sources and needs both, because
 * _nxd_ipv6_search_onlink() looks at both: the prefix list, which router
 * advertisements fill, and the prefix of each MANUAL address, which they do
 * not. That is the order the search uses, so the prefix list is walked first
 * here. A prefix in both, an address configured by hand on a prefix a router
 * also advertises, is one on-link route and is reported once.
 *
 * A stateless-autoconfigured address is not reported from its own prefix. An
 * advertisement can set A without L, in which case the address exists and the
 * prefix is not on link. If it did set L, the prefix list already has it.
 */
#ifdef AMINETXDUO_IPV6
static BOOL ns_prefix_listed(NX_IP *ip, const ULONG prefix[4], ULONG bits)
{
    const NX_IPV6_PREFIX_ENTRY *e;

    for (e = ip->nx_ipv6_prefix_list_ptr; e != NX_NULL;
         e = e->nx_ipv6_prefix_entry_next)
    {
        if (e->nx_ipv6_prefix_entry_prefix_length == bits &&
            e->nx_ipv6_prefix_entry_network_address[0] == prefix[0] &&
            e->nx_ipv6_prefix_entry_network_address[1] == prefix[1] &&
            e->nx_ipv6_prefix_entry_network_address[2] == prefix[2] &&
            e->nx_ipv6_prefix_entry_network_address[3] == prefix[3])
        {
            return TRUE;
        }
    }

    return FALSE;
}
#endif

static VOID ns_fill_routes6(NX_IP *ip, NsWriter *w)
{
#ifdef AMINETXDUO_IPV6
    NX_IPV6_PREFIX_ENTRY *prefix;
    UINT                  i;

    if (!netstack_ipv6_enabled())
        return;

    /* Longest prefix first: _nx_ipv6_prefix_list_add_entry() keeps the list
       in that order, which is the order the search matches in. */
    for (prefix = ip->nx_ipv6_prefix_list_ptr; prefix != NX_NULL;
         prefix = prefix->nx_ipv6_prefix_entry_next)
    {
        NetStatusRoute6 *out = (NetStatusRoute6 *)ns_writer_next(w);

        if (out == NULL)
            continue;

        out->nsr6_Destination[0] = prefix->nx_ipv6_prefix_entry_network_address[0];
        out->nsr6_Destination[1] = prefix->nx_ipv6_prefix_entry_network_address[1];
        out->nsr6_Destination[2] = prefix->nx_ipv6_prefix_entry_network_address[2];
        out->nsr6_Destination[3] = prefix->nx_ipv6_prefix_entry_network_address[3];
        out->nsr6_PrefixLength   = prefix->nx_ipv6_prefix_entry_prefix_length;
        out->nsr6_Lifetime       = prefix->nx_ipv6_prefix_entry_valid_lifetime;
        out->nsr6_Flags          = NETSTATUS_RT6_UP;

        /* A lifetime nothing counts down was set by hand. An advertised
           prefix always carries the advertisement's own. */
        if (out->nsr6_Lifetime == NETSTATUS_RT6_FOREVER)
            out->nsr6_Flags |= NETSTATUS_RT6_STATIC;

        if (out->nsr6_PrefixLength == 128UL)
            out->nsr6_Flags |= NETSTATUS_RT6_HOST;
    }

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        NXD_IPV6_ADDRESS *addr =
            ip->nx_ip_interface[i].nxd_interface_ipv6_address_list_head;

        while (addr != NX_NULL)
        {
            NetStatusRoute6 *out;
            ULONG            bits;
            ULONG            network[4];
            UINT             word;

            if (!addr->nxd_ipv6_address_valid ||
                addr->nxd_ipv6_address_ConfigurationMethod !=
                    NX_IPV6_ADDRESS_MANUAL_CONFIG)
            {
                addr = addr->nxd_ipv6_address_next;
                continue;
            }

            /* fe80::/64 is on link without any list saying so, so there is no
               entry here to add or remove. */
            if (IPv6_Address_Type(addr->nxd_ipv6_address) &
                IPV6_ADDRESS_LINKLOCAL)
            {
                addr = addr->nxd_ipv6_address_next;
                continue;
            }

            bits = (ULONG)addr->nxd_ipv6_address_prefix_length;

            for (word = 0; word < 4U; word++)
            {
                ULONG left = (bits > (ULONG)(word * 32U))
                                 ? (bits - (ULONG)(word * 32U)) : 0UL;
                ULONG mask = (left >= 32UL) ? 0xFFFFFFFFUL
                           : (left == 0UL)  ? 0UL
                           : (0xFFFFFFFFUL << (32UL - left));

                network[word] = addr->nxd_ipv6_address[word] & mask;
            }

            if (ns_prefix_listed(ip, network, bits))
            {
                addr = addr->nxd_ipv6_address_next;
                continue;
            }

            out = (NetStatusRoute6 *)ns_writer_next(w);
            if (out != NULL)
            {
                out->nsr6_Destination[0] = network[0];
                out->nsr6_Destination[1] = network[1];
                out->nsr6_Destination[2] = network[2];
                out->nsr6_Destination[3] = network[3];
                out->nsr6_PrefixLength   = bits;
                out->nsr6_Lifetime       = NETSTATUS_RT6_FOREVER;
                out->nsr6_Flags          = NETSTATUS_RT6_UP;
                out->nsr6_Interface      = (UWORD)i;

                if (bits == 128UL)
                    out->nsr6_Flags |= NETSTATUS_RT6_HOST;
            }

            addr = addr->nxd_ipv6_address_next;
        }
    }

    for (i = 0; i < (UINT)NX_IPV6_DEFAULT_ROUTER_TABLE_SIZE; i++)
    {
        const NX_IPV6_DEFAULT_ROUTER_ENTRY *e = &ip->nx_ipv6_default_router_table[i];
        NetStatusRoute6                    *out;

        if ((e->nx_ipv6_default_router_entry_flag & NX_IPV6_ROUTE_TYPE_VALID) == 0)
            continue;

        out = (NetStatusRoute6 *)ns_writer_next(w);
        if (out == NULL)
            continue;

        /* ::/0, the destination is everything the lists above did not claim. */
        out->nsr6_NextHop[0] = e->nx_ipv6_default_router_entry_router_address[0];
        out->nsr6_NextHop[1] = e->nx_ipv6_default_router_entry_router_address[1];
        out->nsr6_NextHop[2] = e->nx_ipv6_default_router_entry_router_address[2];
        out->nsr6_NextHop[3] = e->nx_ipv6_default_router_entry_router_address[3];
        out->nsr6_Flags      = NETSTATUS_RT6_UP | NETSTATUS_RT6_GATEWAY;
        out->nsr6_Interface  =
            ns_interface_index(ip, e->nx_ipv6_default_router_entry_interface_ptr);

        if (e->nx_ipv6_default_router_entry_flag & NX_IPV6_ROUTE_TYPE_STATIC)
            out->nsr6_Flags |= NETSTATUS_RT6_STATIC;

        out->nsr6_Lifetime =
            (e->nx_ipv6_default_router_entry_life_time == 0xFFFFU)
                ? NETSTATUS_RT6_FOREVER
                : (ULONG)e->nx_ipv6_default_router_entry_life_time;
    }
#else
    (VOID)ip;
    (VOID)w;
#endif
}

#ifdef AMINETXDUO_IPV6
/*
 * Is this neighbour one of the machine's default routers?
 *
 * NetX Duo answers that with ND_CACHE_ENTRY.nx_nd_cache_is_router, and that
 * back pointer is only written where an RA is processed and the RA carried a
 * Source Link-Layer Address option: nx_icmpv6_process_ra.c:701 links the two
 * entries, and it is reached from the ICMPV6_OPTION_TYPE_SRC_LINK_ADDR arm
 * alone. A router that advertises without the option, or whose advertisement
 * arrived before the cache had an entry for it, is in
 * nx_ipv6_default_router_table with no back pointer at all, and the cache
 * entry the first packet through it creates is an ordinary STALE neighbour
 * for ever after. That is timing, not state: the same segment and the same
 * router read differently from one boot to the next.
 *
 * So the table is what is asked, and the back pointer only shortcuts it. The
 * flag means "it is a router for this machine" (netstatus.h), which is what
 * being in that table is.
 */
static BOOL ns_neighbour_is_router(NX_IP *ip, const ND_CACHE_ENTRY *e)
{
    UINT i;

    if (e->nx_nd_cache_is_router != NX_NULL)
        return TRUE;

    for (i = 0; i < (UINT)NX_IPV6_DEFAULT_ROUTER_TABLE_SIZE; i++)
    {
        const NX_IPV6_DEFAULT_ROUTER_ENTRY *r =
            &ip->nx_ipv6_default_router_table[i];

        if ((r->nx_ipv6_default_router_entry_flag &
             NX_IPV6_ROUTE_TYPE_VALID) == 0)
            continue;

        /* The interface too: an fe80:: router address is only unique per
           link, and two links can carry the same one. */
        if (r->nx_ipv6_default_router_entry_interface_ptr !=
            e->nx_nd_cache_interface_ptr)
            continue;

        if (r->nx_ipv6_default_router_entry_router_address[0] ==
                e->nx_nd_cache_dest_ip[0] &&
            r->nx_ipv6_default_router_entry_router_address[1] ==
                e->nx_nd_cache_dest_ip[1] &&
            r->nx_ipv6_default_router_entry_router_address[2] ==
                e->nx_nd_cache_dest_ip[2] &&
            r->nx_ipv6_default_router_entry_router_address[3] ==
                e->nx_nd_cache_dest_ip[3])
            return TRUE;
    }

    return FALSE;
}
#endif

/*
 * The neighbour cache: a flat array, unlike the ARP table's hash of circular
 * lists, so the walk is a loop over the slots.
 */
static VOID ns_fill_neighbours(NX_IP *ip, NsWriter *w)
{
#ifdef AMINETXDUO_IPV6
    UINT i;

    if (!netstack_ipv6_enabled())
        return;

    for (i = 0; i < (UINT)NX_IPV6_NEIGHBOR_CACHE_SIZE; i++)
    {
        const ND_CACHE_ENTRY *e = &ip->nx_ipv6_nd_cache[i];
        NetStatusNeighbour   *out;
        UINT                  j;

        if (e->nx_nd_cache_nd_status == ND_CACHE_STATE_INVALID)
            continue;

        out = (NetStatusNeighbour *)ns_writer_next(w);
        if (out == NULL)
            continue;

        out->nsn6_Address[0] = e->nx_nd_cache_dest_ip[0];
        out->nsn6_Address[1] = e->nx_nd_cache_dest_ip[1];
        out->nsn6_Address[2] = e->nx_nd_cache_dest_ip[2];
        out->nsn6_Address[3] = e->nx_nd_cache_dest_ip[3];
        out->nsn6_State      = (UWORD)e->nx_nd_cache_nd_status;
        out->nsn6_Queued     = (UWORD)e->nx_nd_cache_packet_waiting_queue_length;

        for (j = 0; j < (UINT)NETSTATUS_MAC_SIZE; j++)
            out->nsn6_HwAddress[j] = (UBYTE)e->nx_nd_cache_mac_addr[j];

        if (e->nx_nd_cache_is_static)
            out->nsn6_Flags |= NETSTATUS_ND_STATIC;
        if (ns_neighbour_is_router(ip, e))
            out->nsn6_Flags |= NETSTATUS_ND_ROUTER;

        out->nsn6_Interface = ns_interface_index(ip,
                                                 e->nx_nd_cache_interface_ptr);

        /*
         * nx_nd_cache_num_solicit counts down from NX_MAX_MULTICAST_SOLICIT.
         * The number a caller wants is how many times this machine asked,
         * which is the difference. The constant is only known here.
         */
        if (e->nx_nd_cache_nd_status == ND_CACHE_STATE_INCOMPLETE &&
            e->nx_nd_cache_num_solicit <= (UCHAR)NX_MAX_MULTICAST_SOLICIT)
        {
            out->nsn6_Solicitations =
                (UWORD)((UCHAR)NX_MAX_MULTICAST_SOLICIT -
                        e->nx_nd_cache_num_solicit);
        }
    }
#else
    (VOID)ip;
    (VOID)w;
#endif
}

/*
 * The destination cache: another flat array, walked the same way. The entries
 * are ordered by a use counter rather than by a clock, so the age reported is
 * a difference against nx_ipv6_destination_table_clock. It is computed here
 * because that counter is not published anywhere else.
 */
static VOID ns_fill_dest6(NX_IP *ip, NsWriter *w)
{
#ifdef AMINETXDUO_IPV6
    UINT i;

    if (!netstack_ipv6_enabled())
        return;

    for (i = 0; i < (UINT)NX_IPV6_DESTINATION_TABLE_SIZE; i++)
    {
        const NX_IPV6_DESTINATION_ENTRY *e = &ip->nx_ipv6_destination_table[i];
        const ND_CACHE_ENTRY            *nd;
        NetStatusDest6                  *out;

        if (!e->nx_ipv6_destination_entry_valid)
            continue;

        out = (NetStatusDest6 *)ns_writer_next(w);
        if (out == NULL)
            continue;

        out->nsd6_Destination[0] = e->nx_ipv6_destination_entry_destination_address[0];
        out->nsd6_Destination[1] = e->nx_ipv6_destination_entry_destination_address[1];
        out->nsd6_Destination[2] = e->nx_ipv6_destination_entry_destination_address[2];
        out->nsd6_Destination[3] = e->nx_ipv6_destination_entry_destination_address[3];

        out->nsd6_NextHop[0] = e->nx_ipv6_destination_entry_next_hop[0];
        out->nsd6_NextHop[1] = e->nx_ipv6_destination_entry_next_hop[1];
        out->nsd6_NextHop[2] = e->nx_ipv6_destination_entry_next_hop[2];
        out->nsd6_NextHop[3] = e->nx_ipv6_destination_entry_next_hop[3];

        /* Signed difference, the way nx_icmpv6_dest_table_add.c compares it,
           so a wrapped counter still orders the entries. */
        out->nsd6_Age = (ULONG)(LONG)(ip->nx_ipv6_destination_table_clock -
                                      e->nx_ipv6_destination_entry_last_used);

        out->nsd6_Capacity = (ULONG)NX_IPV6_DESTINATION_TABLE_SIZE;

#ifdef NX_ENABLE_IPV6_PATH_MTU_DISCOVERY
        out->nsd6_PathMtu = e->nx_ipv6_destination_entry_path_mtu;
#else
        out->nsd6_PathMtu = NETSTATUS_DEST6_NO_MTU;
#endif

        /* Compared word by word rather than with CHECK_IPV6_ADDRESSES_SAME:
           without NX_IPV6_UTIL_INLINE that name is a function taking two
           non-const ULONG *. */
        if (out->nsd6_Destination[0] == out->nsd6_NextHop[0] &&
            out->nsd6_Destination[1] == out->nsd6_NextHop[1] &&
            out->nsd6_Destination[2] == out->nsd6_NextHop[2] &&
            out->nsd6_Destination[3] == out->nsd6_NextHop[3])
            out->nsd6_Flags |= NETSTATUS_DEST6_ONLINK;

        /*
         * The cross-link is what says whether anything can be sent at all: a
         * destination whose next hop never resolved is exactly the case that
         * used to fail with nothing printed.
         */
        nd = e->nx_ipv6_destination_entry_nd_entry;
        if (nd != NX_NULL)
        {
            out->nsd6_NdState   = (UWORD)nd->nx_nd_cache_nd_status;
            out->nsd6_Interface = ns_interface_index(ip,
                                                     nd->nx_nd_cache_interface_ptr);

            if (nd->nx_nd_cache_is_router != NX_NULL)
                out->nsd6_Flags |= NETSTATUS_DEST6_ROUTER;
        }
    }
#else
    (VOID)ip;
    (VOID)w;
#endif
}

static VOID ns_fill_interfaces(NX_IP *ip, NsWriter *w)
{
    UINT i;

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        NX_INTERFACE       *nxif = &ip->nx_ip_interface[i];
        NetStatusInterface *out  = (NetStatusInterface *)ns_writer_next(w);
        const AmiIfConfig  *cfg;
        AmiSana2If         *sana;
        AmiSana2Stats       stats;

        if (out == NULL)
            continue;

        out->nsi_Index = (UWORD)i;

        if (nxif->nx_interface_valid == 0)
            continue;

        out->nsi_Flags  |= NETSTATUS_IF_ATTACHED;
        if (nxif->nx_interface_link_up != 0)
            out->nsi_Flags |= NETSTATUS_IF_LINKUP;

        out->nsi_Address = nxif->nx_interface_ip_address;
        out->nsi_NetMask = nxif->nx_interface_ip_network_mask;
        out->nsi_MTU     = nxif->nx_interface_ip_mtu_size;

        ns_mac_from_words(nxif->nx_interface_physical_address_msw,
                          nxif->nx_interface_physical_address_lsw,
                          out->nsi_HwAddress);

        /*
         * ami_sana2_attach() parks the AmiSana2If in the interface's
         * additional link info (aminetxduo/sana2.h), so the driver's own
         * counters are reachable without a separate registry.
         */
        sana = (AmiSana2If *)nxif->nx_interface_additional_link_info;
        if (sana != NULL)
        {
            out->nsi_Flags |= NETSTATUS_IF_SANA2;
            if (ami_sana2_is_online(sana))
                out->nsi_Flags |= NETSTATUS_IF_ONLINE;

            out->nsi_Speed = ami_sana2_get_bps(sana);

            ami_sana2_get_stats(sana, &stats);
            out->nsi_PacketsIn        = stats.packets_received;
            out->nsi_PacketsOut       = stats.packets_sent;
            out->nsi_BadData          = stats.bad_data;
            out->nsi_Overruns         = stats.overruns;
            out->nsi_UnknownTypes     = stats.unknown_types;
            out->nsi_Reconfigurations = stats.reconfigurations;
            out->nsi_TxErrors         = stats.tx_errors;
            out->nsi_RxErrors         = stats.rx_errors;
            out->nsi_RxErrRunt        = stats.rx_err_runt;
            out->nsi_RxErrVerify      = stats.rx_err_verify;
            out->nsi_RxErrLength      = stats.rx_err_length;
            out->nsi_RxErrIo          = stats.rx_err_io;
            out->nsi_RxCopyHook       = stats.rx_copy_hook;
            out->nsi_RxCopySummed     = stats.rx_copy_summed;
            out->nsi_AllocFailures    = stats.alloc_failures;
        }

        if (netstack_iface_mdns((UWORD)i))
        {
            out->nsi_Flags |= NETSTATUS_IF_MDNS;
        }

        cfg = ns_config_for(i);
        if (cfg != NULL)
        {
            out->nsi_Flags |= NETSTATUS_IF_NAMED;
            ns_copy_name(out->nsi_Name, sizeof(out->nsi_Name), cfg->name);
            ns_copy_name(out->nsi_Device, sizeof(out->nsi_Device), cfg->device);
            out->nsi_Unit = cfg->unit;
        }
        else
        {
            /* NetX Duo's own name for the slot. For loopback it is the only
             * name there is. */
            ns_copy_name(out->nsi_Name, sizeof(out->nsi_Name),
                         (const char *)nxif->nx_interface_name);
        }
    }
}

static VOID ns_fill_stats(NX_IP *ip, NetStatusStats *out)
{
    if (nx_ip_info_get(ip, &out->nsx_IpPacketsSent, &out->nsx_IpBytesSent,
                       &out->nsx_IpPacketsReceived, &out->nsx_IpBytesReceived,
                       &out->nsx_IpInvalid, &out->nsx_IpReceiveDropped,
                       &out->nsx_IpChecksumErrors, &out->nsx_IpSendDropped,
                       &out->nsx_IpFragmentsSent,
                       &out->nsx_IpFragmentsReceived) == NX_SUCCESS)
        out->nsx_Have |= NETSTATUS_HAVE_IP;

    if (nx_icmp_info_get(ip, &out->nsx_IcmpPingsSent,
                         &out->nsx_IcmpPingTimeouts,
                         &out->nsx_IcmpThreadsSuspended,
                         &out->nsx_IcmpResponses,
                         &out->nsx_IcmpChecksumErrors,
                         &out->nsx_IcmpUnhandled) == NX_SUCCESS)
        out->nsx_Have |= NETSTATUS_HAVE_ICMP;

    if (nx_tcp_info_get(ip, &out->nsx_TcpPacketsSent, &out->nsx_TcpBytesSent,
                        &out->nsx_TcpPacketsReceived,
                        &out->nsx_TcpBytesReceived, &out->nsx_TcpInvalid,
                        &out->nsx_TcpReceiveDropped,
                        &out->nsx_TcpChecksumErrors, &out->nsx_TcpConnections,
                        &out->nsx_TcpDisconnections,
                        &out->nsx_TcpConnectionsDropped,
                        &out->nsx_TcpRetransmits) == NX_SUCCESS)
        out->nsx_Have |= NETSTATUS_HAVE_TCP;

    if (nx_udp_info_get(ip, &out->nsx_UdpPacketsSent, &out->nsx_UdpBytesSent,
                        &out->nsx_UdpPacketsReceived,
                        &out->nsx_UdpBytesReceived, &out->nsx_UdpInvalid,
                        &out->nsx_UdpReceiveDropped,
                        &out->nsx_UdpChecksumErrors) == NX_SUCCESS)
        out->nsx_Have |= NETSTATUS_HAVE_UDP;

    if (nx_arp_info_get(ip, &out->nsx_ArpRequestsSent,
                        &out->nsx_ArpRequestsReceived,
                        &out->nsx_ArpResponsesSent,
                        &out->nsx_ArpResponsesReceived,
                        &out->nsx_ArpDynamicEntries, &out->nsx_ArpStaticEntries,
                        &out->nsx_ArpAgedEntries,
                        &out->nsx_ArpInvalidMessages) == NX_SUCCESS)
        out->nsx_Have |= NETSTATUS_HAVE_ARP;
}

/*
 * Neither half of this touches NetX Duo, so the caller does not take the
 * baton to read it. A query that takes the baton to ask whether the baton is
 * stuck cannot answer.
 */
static VOID ns_fill_health(NetStatusHealth *out)
{
    TX_AMIGA_TICK_STATS tick;
    const AmiMemStats  *mem;

    tx_amiga_tick_stats(&tick);

    /* Before the copy, so a program that asks gets the pool as of the request
       rather than as of the last time a stack thread moved. */
    netstack_pool_sample();
    mem = ami_mem_stats();

    out->nsl_TickTicks          = tick.tx_amiga_tick_delivered;
    out->nsl_TickClipped        = tick.tx_amiga_tick_clipped;
    out->nsl_TickLost           = tick.tx_amiga_tick_lost;
    out->nsl_TickServiceUs      = tick.tx_amiga_tick_service_us;
    out->nsl_TickUptimeMs       = tx_amiga_uptime_ms(&tick);
    out->nsl_TickWorstStallMs   = tick.tx_amiga_tick_worst_stall_ms;
    out->nsl_TickWorstServiceUs = tick.tx_amiga_tick_worst_service_us;
    out->nsl_TickOverBudget     = tick.tx_amiga_tick_over_budget;
    out->nsl_TickDeferred       = tick.tx_amiga_tick_deferred;
    out->nsl_TickSkew           = tick.tx_amiga_tick_skew;
    out->nsl_TickSkewPeak       = tick.tx_amiga_tick_skew_peak;

    out->nsl_BatonLive        = ami_baton_stats.bs_Live;
    out->nsl_BatonLiveMax     = ami_baton_stats.bs_LiveMax;
    out->nsl_BatonFull        = ami_baton_stats.bs_Full;
    out->nsl_BatonTransitions = ami_baton_stats.bs_Transitions;
    out->nsl_BatonStateMax    = ami_baton_stats.bs_StateMax;
    out->nsl_BatonMoved       = ami_baton_stats.bs_BatonMoved;
    out->nsl_BatonStateShared = ami_baton_stats.bs_StateShared;

    out->nsl_AllocLive       = mem->ms_Live;
    out->nsl_AllocPeak       = mem->ms_LiveMax;
    out->nsl_AllocRefused    = mem->ms_Refused;
    out->nsl_Sockets         = mem->ms_Sockets;
    out->nsl_SocketsPeak     = mem->ms_SocketsMax;
    out->nsl_Opens           = mem->ms_Opens;
    out->nsl_PoolTotal       = mem->ms_PoolTotal;
    out->nsl_PoolFree        = mem->ms_PoolFree;
    out->nsl_PoolLow         = mem->ms_PoolLow;
    out->nsl_PoolPayload     = mem->ms_PoolPayload;
    out->nsl_PoolEmpty       = mem->ms_PoolEmpty;
    out->nsl_PoolWaited      = mem->ms_PoolWaited;
    out->nsl_PoolBadRelease  = mem->ms_PoolBadRelease;
}

/*
 * The ARP cache is a hash table of circular lists. nx_arp_active_next of the
 * last entry in a bucket points back at the bucket head, not at NX_NULL, so
 * the walk needs the head comparison below. Without it the loop spins inside
 * the bracket and holds the lock.
 */
static VOID ns_fill_arp(NX_IP *ip, NsWriter *w)
{
#ifndef NX_DISABLE_IPV4
    UINT bucket;

    for (bucket = 0; bucket < (UINT)NX_ARP_TABLE_SIZE; bucket++)
    {
        NX_ARP *head  = ip->nx_ip_arp_table[bucket];
        NX_ARP *entry = head;

        if (head == NX_NULL)
            continue;

        do
        {
            NetStatusArp *out = (NetStatusArp *)ns_writer_next(w);

            if (out != NULL)
            {
                out->nsa_Address = entry->nx_arp_ip_address;
                out->nsa_Retries = (UWORD)entry->nx_arp_retries;

                if (entry->nx_arp_route_static != 0)
                    out->nsa_Flags |= NETSTATUS_ARP_STATIC;

                ns_mac_from_words(entry->nx_arp_physical_address_msw,
                                  entry->nx_arp_physical_address_lsw,
                                  out->nsa_HwAddress);

                if (entry->nx_arp_physical_address_msw != 0 ||
                    entry->nx_arp_physical_address_lsw != 0)
                    out->nsa_Flags |= NETSTATUS_ARP_RESOLVED;

                if (entry->nx_arp_ip_interface != NX_NULL)
                {
                    UINT i;

                    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
                    {
                        if (entry->nx_arp_ip_interface == &ip->nx_ip_interface[i])
                        {
                            out->nsa_Interface = (UWORD)i;
                            break;
                        }
                    }
                }
            }

            entry = entry->nx_arp_active_next;
        }
        while (entry != NX_NULL && entry != head);
    }
#else
    (VOID)ip;
    (VOID)w;
#endif
}

/*
 * Three kinds of route, reported in the order the stack consults them:
 *
 *   1. the directly-attached prefix of each interface that has an address.
 *   2. NetX Duo's static routing table, longest prefix first. It keeps
 *      nx_ip_routing_table[] sorted by netmask descending, so the order here
 *      is the order _nx_ip_route_find() matches in.
 *   3. the default gateway (nx_ip_gateway_address), consulted last and
 *      maintained by nx_ip_gateway_address_set() in every build.
 *
 * The static table exists only when port/netxduo-amiga/inc/nx_user.h defines
 * NX_ENABLE_IP_STATIC_ROUTING. NX_IP_ROUTING_TABLE_SIZE alone is not enough.
 * NETSTATUS_SYS_ROUTING in the SYSTEM query tells a caller which kind of build
 * it is talking to, so this file compiles and reports correctly either way.
 */
static VOID ns_fill_routes(NX_IP *ip, NsWriter *w)
{
    ULONG gateway = 0;
    UINT  i;
#ifdef NX_ENABLE_IP_STATIC_ROUTING
    ULONG r;
#endif

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        NX_INTERFACE   *nxif = &ip->nx_ip_interface[i];
        NetStatusRoute *out;

        if (nxif->nx_interface_valid == 0 ||
            nxif->nx_interface_ip_address == 0)
            continue;

        out = (NetStatusRoute *)ns_writer_next(w);
        if (out == NULL)
            continue;

        out->nsr_Destination = nxif->nx_interface_ip_address &
                               nxif->nx_interface_ip_network_mask;
        out->nsr_NetMask     = nxif->nx_interface_ip_network_mask;
        out->nsr_Gateway     = 0;
        out->nsr_Interface   = (UWORD)i;
        out->nsr_Flags       = NETSTATUS_RT_UP;

        if (nxif->nx_interface_ip_network_mask == 0xffffffffUL)
            out->nsr_Flags |= NETSTATUS_RT_HOST;
    }

#ifdef NX_ENABLE_IP_STATIC_ROUTING
    for (r = 0; r < ip->nx_ip_routing_table_entry_count; r++)
    {
        const NX_IP_ROUTING_ENTRY *e   = &ip->nx_ip_routing_table[r];
        NetStatusRoute            *out = (NetStatusRoute *)ns_writer_next(w);

        if (out == NULL)
            continue;

        out->nsr_Destination = e->nx_ip_routing_dest_ip;
        out->nsr_NetMask     = e->nx_ip_routing_net_mask;
        out->nsr_Gateway     = e->nx_ip_routing_next_hop_address;
        out->nsr_Flags       = NETSTATUS_RT_UP | NETSTATUS_RT_STATIC |
                               NETSTATUS_RT_GATEWAY;
        out->nsr_Interface   = ns_interface_index(ip,
                                                  e->nx_ip_routing_entry_ip_interface);

        if (e->nx_ip_routing_net_mask == 0xffffffffUL)
            out->nsr_Flags |= NETSTATUS_RT_HOST;
    }
#endif

    if (nx_ip_gateway_address_get(ip, &gateway) == NX_SUCCESS && gateway != 0)
    {
        NetStatusRoute *out = (NetStatusRoute *)ns_writer_next(w);

        if (out != NULL)
        {
            out->nsr_Destination = 0;
            out->nsr_NetMask     = 0;
            out->nsr_Gateway     = gateway;
            out->nsr_Flags       = NETSTATUS_RT_UP | NETSTATUS_RT_GATEWAY;
            out->nsr_Interface   = ns_interface_index(ip,
                                                      ip->nx_ip_gateway_interface);
        }
    }
}

/*
 * The created-socket lists are singly linked and circular too, so the walk is
 * bounded by the count NetX Duo keeps rather than by a NULL that never comes.
 * These are the sockets of every task that uses the library, so netstat run
 * from one Shell sees the connection another Shell's `fetch` holds open.
 */
static VOID ns_fill_sockets(NX_IP *ip, NsWriter *w)
{
    ULONG n;

    {
        NX_TCP_SOCKET *sock = ip->nx_ip_tcp_created_sockets_ptr;

        for (n = 0; n < ip->nx_ip_tcp_created_sockets_count && sock != NX_NULL;
             n++)
        {
            NetStatusSocket *out = (NetStatusSocket *)ns_writer_next(w);

            if (out != NULL)
            {
                out->nso_Flags       = NETSTATUS_SOCK_TCP;
                out->nso_LocalPort   = (UWORD)sock->nx_tcp_socket_port;
                out->nso_PeerPort    = (UWORD)sock->nx_tcp_socket_connect_port;
                out->nso_State       = (UWORD)sock->nx_tcp_socket_state;
                out->nso_PeerAddress =
                    sock->nx_tcp_socket_connect_ip.nxd_ip_address.v4;
            }

            sock = sock->nx_tcp_socket_created_next;
        }
    }

    {
        NX_UDP_SOCKET *sock = ip->nx_ip_udp_created_sockets_ptr;

        for (n = 0; n < ip->nx_ip_udp_created_sockets_count && sock != NX_NULL;
             n++)
        {
            NetStatusSocket *out = (NetStatusSocket *)ns_writer_next(w);

            if (out != NULL)
            {
                out->nso_LocalPort = (UWORD)sock->nx_udp_socket_port;
                out->nso_Queued    = sock->nx_udp_socket_receive_count;
            }

            sock = sock->nx_udp_socket_created_next;
        }
    }
}

/*
 * The same TCP walk again, for the numbers that say whether a connection
 * progresses. Its own table because NetStatusSocket is 16 published bytes and
 * every consumer checks that width exactly.
 *
 * nx_tcp_socket_timeout is what remains on the retransmit timer, not the
 * interval it was armed with. That is the useful one to print. It counts down
 * for the reader, and its ceiling still shows which retransmit step the
 * connection reached.
 */
static VOID ns_fill_tcpstall(NX_IP *ip, NsWriter *w)
{
    NX_TCP_SOCKET *sock = ip->nx_ip_tcp_created_sockets_ptr;
    ULONG          n;

    for (n = 0; n < ip->nx_ip_tcp_created_sockets_count && sock != NX_NULL; n++)
    {
        NetStatusTcpStall *out = (NetStatusTcpStall *)ns_writer_next(w);

        if (out != NULL)
        {
            out->nst_LocalPort   = (UWORD)sock->nx_tcp_socket_port;
            out->nst_PeerPort    = (UWORD)sock->nx_tcp_socket_connect_port;
            out->nst_PeerAddress =
                sock->nx_tcp_socket_connect_ip.nxd_ip_address.v4;
            out->nst_Stalled     =
                ns_ticks_ms(sock->nx_tcp_socket_stall_ticks);
            out->nst_Retransmits = sock->nx_tcp_socket_timeout_retries;
            out->nst_Rto         = ns_ticks_ms(sock->nx_tcp_socket_timeout);
            out->nst_UserTimeout =
                ns_ticks_ms(sock->nx_tcp_socket_user_timeout);
        }

        sock = sock->nx_tcp_socket_created_next;
    }
}

#ifdef AMINETXDUO_MDNS
/*
 * The mDNS service cache, as of now: everything in it, of every type.
 *
 * Not the type the caller last browsed for. There is one cache and any number
 * of commands that can read it. A selector that answered "the current browse"
 * therefore answers a different question depending on what else runs. The
 * caller knows what it asked for and filters on nsv_Type, and the selector
 * keeps one stable meaning: this is the cache.
 *
 * Outside the bracket, unlike every other walk here. netstack_mdns_browse_
 * collect() takes its own, and it allocates. An NX_MDNS_SERVICE is 600-odd
 * bytes and belongs on neither this stack nor the module's. Rule 2 at the top
 * of this file says no allocation while adopted, and this is how it is kept.
 * The same call also waits for the address of a service whose SRV came without
 * one. That is a ThreadX suspension inside its own bracket, so the baton is
 * handed on, and it is bounded at two seconds for the whole walk.
 *
 * The rows are read into one array and copied out, rather than filled through
 * the NsWriter, because the count is not known until the walk ends and a
 * caller with a small buffer must still be told what it was.
 */
#define NS_SERVICE_MAX      48

static VOID ns_fill_services(NsWriter *w)
{
    AmiMdnsService *rows;
    UWORD           count;
    UWORD           available = 0;
    UWORD           i;

    rows = (AmiMdnsService *)ami_alloc(
               (ULONG)sizeof(AmiMdnsService) * NS_SERVICE_MAX);
    if (rows == NULL)
        return;

    count = netstack_mdns_browse_collect(NULL, rows, NS_SERVICE_MAX,
                                         &available);

    for (i = 0; i < count; i++)
    {
        const AmiMdnsService *in  = &rows[i];
        NetStatusService     *out = (NetStatusService *)ns_writer_next(w);

        if (out == NULL)
            continue;

        out->nsv_Index   = in->ams_Index;
        out->nsv_Port    = in->ams_Port;
        out->nsv_Address = in->ams_Address;

        if (in->ams_Name[0] != '\0')
            out->nsv_Flags |= NETSTATUS_SVC_INSTANCE;
        if (in->ams_Address != 0UL)
            out->nsv_Flags |= NETSTATUS_SVC_ADDRESS;
        if (in->ams_Text[0] != '\0')
            out->nsv_Flags |= NETSTATUS_SVC_TXT;
        if (in->ams_TextCut)
            out->nsv_Flags |= NETSTATUS_SVC_TXTCUT;
        if (in->ams_Local)
            out->nsv_Flags |= NETSTATUS_SVC_LOCAL;

        ns_copy_name(out->nsv_Name, sizeof(out->nsv_Name), in->ams_Name);
        ns_copy_name(out->nsv_Type, sizeof(out->nsv_Type), in->ams_Type);
        ns_copy_name(out->nsv_Host, sizeof(out->nsv_Host), in->ams_Host);
        ns_copy_name(out->nsv_Text, sizeof(out->nsv_Text), in->ams_Text);
    }

    /*
     * What the cache held, which is larger than `count` when the cache holds
     * more than NS_SERVICE_MAX. ns_writer_next() has counted only the rows
     * offered to it.
     */
    if (available > w->available)
        w->available = available;

    ami_free(rows);
}

/*
 * The service type a browse was asked for, terminated whatever the caller sent.
 * Empty is the DNS-SD meta-query and comes back NULL, which is what the module
 * wants for it. The module checks anything else itself, because it has to
 * assemble "<type>.local" and fails on what it cannot.
 */
static const char *ns_service_type(const NetStatusControl *ctl,
                                   char buf[NETSTATUS_SVC_TYPE_LEN])
{
    UWORD i = 0;

    while (i + 1 < NETSTATUS_SVC_TYPE_LEN && ctl->nsc_Name[i] != '\0')
    {
        buf[i] = ctl->nsc_Name[i];
        i++;
    }
    buf[i] = '\0';

    return (i == 0) ? NULL : buf;
}
#endif /* AMINETXDUO_MDNS */

/* ---------------------------------------------------------- NetStackQuery */

/*
 * The header check, done before anything is written, so a caller that landed
 * here by accident presents the wrong magic and leaves with its buffer
 * untouched. That is why the magic sits in the buffer's first four bytes.
 */
static LONG ns_header_ok(const NetStatusHeader *hdr, ULONG size)
{
    if (hdr == NULL || size < sizeof(NetStatusHeader))
        return 0;
    if (hdr->nsh_Magic != AMI_NETSTATUS_MAGIC)
        return 0;
    if (hdr->nsh_Version != (UWORD)AMI_NETSTATUS_VERSION)
        return 0;

    return 1;
}

LONG bsd_NetStackQuery(register ULONG magic __asm("d0"),
                       register ULONG what __asm("d1"),
                       register APTR buffer __asm("a0"),
                       register ULONG size __asm("d2"),
                       register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NetStatusHeader *hdr = (NetStatusHeader *)buffer;
    NX_IP           *ip;
    NsWriter         w;
    ULONG            need;

    if (magic != AMI_NETSTATUS_MAGIC)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (!ns_header_ok(hdr, size))
        return bsd_fail(SocketBase, AMI_EINVAL);

    /* Every walk below reports live configuration, and NETSTATUS_SYSTEM reports
       the host name a lease can rename.  DHCP and router advertisements record
       that on NetX tasks and hand it to a caller task; this is one.  Once per
       query, ahead of the walk, rather than inside every netstack_config(). */
    netstack_dns_absorb_pending();

    /*
     * The one-entry answers are written straight into the caller's buffer, so
     * the size check happens before the walk rather than inside it.
     *
     * It also makes the unchecked ns_writer_next() results below safe.
     * `need != 0` for SYSTEM, STATS and HEALTH, so size >= header +
     * entry_size, so ns_writer_init() computes room >= 1 and the first slot
     * exists. GCC's -fanalyzer reports those two as "dereference of NULL
     * 'out'" because that chain goes through a division it does not reason
     * about. A NULL test at the call is dead code that implies the buffer can
     * be too small.
     */
    switch (what)
    {
        case NETSTATUS_SYSTEM:      need = sizeof(NetStatusSystem);  break;
        case NETSTATUS_STATS:       need = sizeof(NetStatusStats);   break;
        case NETSTATUS_INTERFACES:  need = 0;                        break;
        case NETSTATUS_ARP:         need = 0;                        break;
        case NETSTATUS_ROUTES:      need = 0;                        break;
        case NETSTATUS_SOCKETS:     need = 0;                        break;
        case NETSTATUS_DHCP:        need = 0;                        break;
        case NETSTATUS_ADDRESSES6:  need = 0;                        break;
        case NETSTATUS_ROUTES6:     need = 0;                        break;
        case NETSTATUS_NEIGHBOURS:  need = 0;                        break;
        case NETSTATUS_HEALTH:      need = sizeof(NetStatusHealth);  break;
        case NETSTATUS_SERVICES:    need = 0;                        break;
        case NETSTATUS_OPENERS:     need = 0;                        break;
        case NETSTATUS_TCPSTALL:    need = 0;                        break;
        case NETSTATUS_DEST6:       need = 0;                        break;
        case NETSTATUS_EVENTS:      need = 0;                        break;
        case NETSTATUS_RXBUDGET:    need = sizeof(NetStatusRxBudget); break;
        default:                    return bsd_fail(SocketBase, AMI_EINVAL);
    }

    if (need != 0 && size < sizeof(NetStatusHeader) + need)
        return bsd_fail(SocketBase, AMI_EINVAL);

    hdr->nsh_Type      = (UWORD)what;
    hdr->nsh_Count     = 0;
    hdr->nsh_Available = 0;
    hdr->nsh_EntrySize = 0;
    hdr->nsh_Reserved  = 0;

    /*
     * Before the stack is looked for, and without the baton, for the reason
     * NETSTATUS_HEALTH is: the ring is the library's own memory. It is also
     * the only way this selector is worth anything -- the events a reader
     * wants are the ones a teardown left behind, and by then netstack_ip()
     * answers NULL and the bracket has nothing to enter.
     */
    if (what == NETSTATUS_EVENTS)
    {
        ULONG held = 0;

        ns_writer_init(&w, hdr, size, NETSTATUS_EVENTS,
                       sizeof(NetStatusEvent));
        w.written = ami_event_snapshot((NetStatusEvent *)w.entries, w.room,
                                       &held);
        /* What the ring holds, not what the machine did: nse_Seq carries
           that, and a reader with a buffer too small still learns the size
           it needs. */
        w.available = held;
        ns_writer_finish(&w);
        return (LONG)hdr->nsh_Count;
    }

    /*
     * Like HEALTH: the budget is the library's own memory, not NetX Duo's,
     * so no baton and no stack lookup.  A build without the probe answers
     * all-zero counts through the same shape, which is what lets one tool
     * serve both builds and say which it met.
     */
    if (what == NETSTATUS_RXBUDGET)
    {
        NetStatusRxBudget *out;

        ns_writer_init(&w, hdr, size, NETSTATUS_RXBUDGET,
                       sizeof(NetStatusRxBudget));
        out = (NetStatusRxBudget *)ns_writer_next(&w);
        if (out != NULL)
        {
            out->nrb_EClockRate = ami_eclock_rate();
#ifdef AMINETXDUO_RXPROBE
            /* Field for field rather than one memcpy: the two structs agree
               today, and this keeps a divergence a compile error tomorrow. */
            out->nrb_Drain.nbl_Count  = ami_budget.drain.count;
            out->nrb_Drain.nbl_Sum    = ami_budget.drain.sum;
            out->nrb_Drain.nbl_Max    = ami_budget.drain.max;
            out->nrb_Baton.nbl_Count  = ami_budget.baton.count;
            out->nrb_Baton.nbl_Sum    = ami_budget.baton.sum;
            out->nrb_Baton.nbl_Max    = ami_budget.baton.max;
            out->nrb_Settle.nbl_Count = ami_budget.settle.count;
            out->nrb_Settle.nbl_Sum   = ami_budget.settle.sum;
            out->nrb_Settle.nbl_Max   = ami_budget.settle.max;
            out->nrb_Fetch.nbl_Count  = ami_budget.fetch.count;
            out->nrb_Fetch.nbl_Sum    = ami_budget.fetch.sum;
            out->nrb_Fetch.nbl_Max    = ami_budget.fetch.max;
            out->nrb_Ack.nbl_Count    = ami_budget.ack.count;
            out->nrb_Ack.nbl_Sum      = ami_budget.ack.sum;
            out->nrb_Ack.nbl_Max      = ami_budget.ack.max;
            {
                UWORD i;

                for (i = 0; i < NETSTATUS_BUDGET_BUCKETS; i++)
                {
                    out->nrb_Drain.nbl_Hist[i]  = ami_budget.drain.hist[i];
                    out->nrb_Baton.nbl_Hist[i]  = ami_budget.baton.hist[i];
                    out->nrb_Settle.nbl_Hist[i] = ami_budget.settle.hist[i];
                    out->nrb_Fetch.nbl_Hist[i]  = ami_budget.fetch.hist[i];
                    out->nrb_Ack.nbl_Hist[i]    = ami_budget.ack.hist[i];
                }
            }
#endif
        }
        ns_writer_finish(&w);
        return (LONG)hdr->nsh_Count;
    }

    /* Answered before the stack is looked for, and without the baton: it reads
       the tick task and the bracket's own counters, not NetX Duo. */
    if (what == NETSTATUS_HEALTH)
    {
        ns_writer_init(&w, hdr, size, NETSTATUS_HEALTH,
                       sizeof(NetStatusHealth));
        ns_fill_health((NetStatusHealth *)ns_writer_next(&w));
        ns_writer_finish(&w);
        return (LONG)hdr->nsh_Count;
    }

    /*
     * Also outside the bracket, for the reason ns_fill_services() gives: it
     * takes its own and allocates inside it.
     */
    if (what == NETSTATUS_SERVICES)
    {
        ns_writer_init(&w, hdr, size, NETSTATUS_SERVICES,
                       sizeof(NetStatusService));
#ifdef AMINETXDUO_MDNS
        ns_fill_services(&w);
#endif
        ns_writer_finish(&w);
        return (LONG)hdr->nsh_Count;
    }

    /*
     * Outside the bracket as well, and outside the stack check below. Who has
     * the library open is a fact about the library, not about NetX Duo, and
     * the caller most likely to ask is one that has to decide whether the
     * stack can be shut down. The walk belongs to library.c, which owns the
     * list and its semaphore, so the writer is filled from the outside here.
     */
    if (what == NETSTATUS_OPENERS)
    {
        LONG avail = 0;
        LONG n;

        ns_writer_init(&w, hdr, size, NETSTATUS_OPENERS,
                       sizeof(NetStatusOpener));
        n = bsd_openers_list(SocketBase, (NetStatusOpener *)w.entries,
                             (LONG)w.room, &avail);
        w.written   = (n > 0) ? (ULONG)n : 0;
        w.available = (avail > 0) ? (ULONG)avail : 0;
        ns_writer_finish(&w);
        return (LONG)hdr->nsh_Count;
    }

    ip = netstack_ip();
    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    /* Adopted from here to bsd_nx_leave(): memory reads and nx_*_info_get(). */
    switch (what)
    {
        case NETSTATUS_SYSTEM:
        {
            NetStatusSystem *sys;
            LONG             openers = 0;

            ns_writer_init(&w, hdr, size, NETSTATUS_SYSTEM,
                           sizeof(NetStatusSystem));
            sys = (NetStatusSystem *)ns_writer_next(&w);
            ns_fill_system(ip, sys);

            /* Counted rather than listed: a NULL table with no room asks
               bsd_openers_list() for the number and nothing else. */
            (VOID)bsd_openers_list(SocketBase, NULL, 0, &openers);
            sys->nss_Openers = (openers > 0) ? (ULONG)openers : 0;
            sys->nss_OpenCnt = bsd_open_count(SocketBase);

            ns_writer_finish(&w);
            break;
        }

        case NETSTATUS_INTERFACES:
            ns_writer_init(&w, hdr, size, NETSTATUS_INTERFACES,
                           sizeof(NetStatusInterface));
            ns_fill_interfaces(ip, &w);
            ns_writer_finish(&w);
            break;

        case NETSTATUS_STATS:
            ns_writer_init(&w, hdr, size, NETSTATUS_STATS,
                           sizeof(NetStatusStats));
            ns_fill_stats(ip, (NetStatusStats *)ns_writer_next(&w));
            ns_writer_finish(&w);
            break;

        case NETSTATUS_ARP:
            ns_writer_init(&w, hdr, size, NETSTATUS_ARP, sizeof(NetStatusArp));
            ns_fill_arp(ip, &w);
            ns_writer_finish(&w);
            break;

        case NETSTATUS_ROUTES:
            ns_writer_init(&w, hdr, size, NETSTATUS_ROUTES,
                           sizeof(NetStatusRoute));
            ns_fill_routes(ip, &w);
            ns_writer_finish(&w);
            break;

        case NETSTATUS_DHCP:
            ns_writer_init(&w, hdr, size, NETSTATUS_DHCP,
                           sizeof(NetStatusDhcp));
            ns_fill_dhcp(&w);
            ns_writer_finish(&w);
            break;

        case NETSTATUS_ADDRESSES6:
            ns_writer_init(&w, hdr, size, NETSTATUS_ADDRESSES6,
                           sizeof(NetStatusAddress6));
            ns_fill_addresses6(&w);
            ns_writer_finish(&w);
            break;

        case NETSTATUS_ROUTES6:
            ns_writer_init(&w, hdr, size, NETSTATUS_ROUTES6,
                           sizeof(NetStatusRoute6));
            ns_fill_routes6(ip, &w);
            ns_writer_finish(&w);
            break;

        case NETSTATUS_NEIGHBOURS:
            ns_writer_init(&w, hdr, size, NETSTATUS_NEIGHBOURS,
                           sizeof(NetStatusNeighbour));
            ns_fill_neighbours(ip, &w);
            ns_writer_finish(&w);
            break;

        case NETSTATUS_TCPSTALL:
            ns_writer_init(&w, hdr, size, NETSTATUS_TCPSTALL,
                           sizeof(NetStatusTcpStall));
            ns_fill_tcpstall(ip, &w);
            ns_writer_finish(&w);
            break;

        case NETSTATUS_DEST6:
            ns_writer_init(&w, hdr, size, NETSTATUS_DEST6,
                           sizeof(NetStatusDest6));
            ns_fill_dest6(ip, &w);
            ns_writer_finish(&w);
            break;

        default:    /* NETSTATUS_SOCKETS. The switch above rejected the rest */
            ns_writer_init(&w, hdr, size, NETSTATUS_SOCKETS,
                           sizeof(NetStatusSocket));
            ns_fill_sockets(ip, &w);
            ns_writer_finish(&w);
            break;
    }

    bsd_nx_leave(SocketBase);

    return (LONG)hdr->nsh_Count;
}

/* -------------------------------------------------------- NetStackControl */

static LONG ns_map_status(struct AmiSocketBase *SocketBase, UINT status)
{
    if (status == NX_SUCCESS)
        return 0;

    switch (status)
    {
        case NX_NOT_ENABLED:        return bsd_fail(SocketBase, AMI_ENOSYS);
        /* No table to hold it, as against a table that is full. IPv6 answers
           this to a prefix given a next hop. */
        case NX_NOT_SUPPORTED:      return bsd_fail(SocketBase, AMI_ENOSYS);
        case NX_DUPLICATED_ENTRY:   return bsd_fail(SocketBase, AMI_EEXIST);
        case NX_ENTRY_NOT_FOUND:    return bsd_fail(SocketBase, AMI_ENOENT);
        case NX_NO_MORE_ENTRIES:    return bsd_fail(SocketBase, AMI_ENOBUFS);
        case NX_IP_ADDRESS_ERROR:   return bsd_fail(SocketBase, AMI_EINVAL);
        case NX_INVALID_INTERFACE:  return bsd_fail(SocketBase, AMI_ENXIO);
        /* Static routing table full. NX_IP_ROUTING_TABLE_SIZE is 4, so a user
           can hit this without a bug being involved. */
        case NX_OVERFLOW:           return bsd_fail(SocketBase, AMI_ENOBUFS);
        default:                    return bsd_fail(SocketBase, AMI_EINVAL);
    }
}

LONG bsd_NetStackControl(register ULONG magic __asm("d0"),
                         register ULONG op __asm("d1"),
                         register APTR arg __asm("a0"),
                         register ULONG size __asm("d2"),
                         register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NetStatusControl *ctl = (NetStatusControl *)arg;
    NX_IP            *ip;
    LONG              rc;
    UINT              status;

    if (magic != AMI_NETSTATUS_MAGIC)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (ctl == NULL || size < sizeof(NetStatusControl))
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (ctl->nsc_Magic != AMI_NETSTATUS_MAGIC ||
        ctl->nsc_Version != (UWORD)AMI_NETSTATUS_VERSION)
        return bsd_fail(SocketBase, AMI_EINVAL);

    /*
     * Online/Offline go through src/netstack rather than through NetX Duo. An
     * interface goes down when its SANA-II readers stop and NetX Duo is told,
     * and netstack_interface_down() is the only thing that knows about both.
     * It brackets itself, so it is called outside ours.
     */
    switch (op)
    {
        case NETCTRL_INTERFACE_UP:
            return (netstack_interface_up(ctl->nsc_Index) == AMI_NET_OK)
                       ? 0 : bsd_fail(SocketBase, AMI_ENXIO);

        case NETCTRL_INTERFACE_DOWN:
            return (netstack_interface_down(ctl->nsc_Index) == AMI_NET_OK)
                       ? 0 : bsd_fail(SocketBase, AMI_ENXIO);

        /*
         * Removal, which is the same call RemoveInterface() makes and for the
         * same reason it is out here: half the work is NetX Duo's and half is
         * the SANA-II device.
         *
         * The two refusals are told apart because they mean opposite things to
         * the caller. EBUSY is a decision the caller can overrule with
         * NETCTRL_F_FORCE. The other is a device that did not give its read
         * requests back, where nothing was freed and a retry is the only
         * option.
         */
        case NETCTRL_INTERFACE_REMOVE:
        {
            LONG err = netstack_interface_remove(
                           ctl->nsc_Index,
                           (ctl->nsc_Flags & NETCTRL_F_FORCE) ? TRUE : FALSE);

            if (err == AMI_NET_OK)
                return 0;

            return bsd_fail(SocketBase,
                            (err == AMI_NET_ERR_BUSY) ? AMI_EBUSY : AMI_ENXIO);
        }

        /*
         * The other direction, and the one AddNetInterface makes: an interface
         * named by its file in DEVS:NetInterfaces, brought up the way a boot
         * brings it up. The file is read here rather than passed in because
         * this argument block has no room for a parsed interface, and because
         * a re-add that reads the same file the first add read is what makes
         * the two the same interface.
         *
         * Out here with the rest of them: it opens a SANA-II device and reads
         * a file, neither of which must happen inside the bracket.
         */
        case NETCTRL_INTERFACE_ADD:
        {
            AmiIfConfig cfg;
            LONG        err;

            if (!ns_terminated(ctl->nsc_Name, sizeof(ctl->nsc_Name)) ||
                ctl->nsc_Name[0] == '\0')
                return bsd_fail(SocketBase, AMI_EINVAL);

            if (ami_config_load_interface(ctl->nsc_Name, &cfg) != AMI_CFG_OK)
                return bsd_fail(SocketBase, AMI_ENOENT);

            err = netstack_interface_start(&cfg, NULL);

            switch (err)
            {
                case AMI_NET_OK:         return 0;
                case AMI_NET_ERR_NODEV:  return bsd_fail(SocketBase, AMI_ENXIO);
                case AMI_NET_ERR_DEVBAD: return bsd_fail(SocketBase, AMI_EIO);
                case AMI_NET_ERR_NOMEM:  return bsd_fail(SocketBase, AMI_ENOBUFS);
                /* A name the stack already has, as against no slot left to put
                   it in: NX_MAX_PHYSICAL_INTERFACES is 2, so a user can reach
                   the second without there being a bug. */
                case AMI_NET_ERR_CONFIG: return bsd_fail(SocketBase, AMI_EEXIST);
                default:                 return bsd_fail(SocketBase, AMI_ENOSPC);
            }
        }

        /*
         * Re-address a running interface, which used to need a removal and an
         * add in a pair and so a device close, a device open and every
         * connection on the interface reset.
         *
         * Out here rather than in the bracketed switch below because
         * bsd_if_set_address() takes the bracket itself. That is the same
         * function ConfigureInterfaceTagList() calls, deliberately: the rule
         * for a mask that was not given lives in one place, and
         * tests/tools/run-ifreadd.sh is what says it is the right rule.
         */
        case NETCTRL_INTERFACE_CONFIGURE:
        {
            NX_IP *cip = netstack_ip();
            LONG   rc2;

            if (cip == NULL)
                return bsd_fail(SocketBase, AMI_ENETDOWN);

            if (ctl->nsc_Index >= (UWORD)NX_MAX_PHYSICAL_INTERFACES ||
                cip->nx_ip_interface[ctl->nsc_Index].nx_interface_valid == 0)
                return bsd_fail(SocketBase, AMI_ENXIO);

            if ((ctl->nsc_Flags & (NETCTRL_F_ADDRESS | NETCTRL_F_NETMASK)) != 0 &&
                bsd_if_set_address(
                    SocketBase, (LONG)ctl->nsc_Index,
                    (ctl->nsc_Flags & NETCTRL_F_ADDRESS) ? TRUE : FALSE,
                    ctl->nsc_Destination,
                    (ctl->nsc_Flags & NETCTRL_F_NETMASK) ? TRUE : FALSE,
                    ctl->nsc_NetMask) != 0)
                return -1;

            if ((ctl->nsc_Flags & NETCTRL_F_GATEWAY) == 0)
                return 0;

            /*
             * Last, and in its own bracket. nx_ip_gateway_address_set()
             * refuses a next hop that is not on some interface's subnet, and
             * the subnet it needs is usually the one just written above.
             */
            if (bsd_nx_enter(SocketBase) != 0)
                return bsd_fail(SocketBase, AMI_ENETDOWN);

            status = (ctl->nsc_Gateway != 0)
                         ? nx_ip_gateway_address_set(cip, ctl->nsc_Gateway)
                         : nx_ip_gateway_address_clear(cip);
            rc2 = ns_map_status(SocketBase, status);

            bsd_nx_leave(SocketBase);

            return rc2;
        }

        /*
         * The DHCP three, aimed at one interface. Out here with the rest of
         * the src/netstack calls: each takes the ThreadX bracket itself, and
         * the one NX_DHCP is the netstack's, not NetX Duo's to reach from
         * inside ours.
         *
         * None of them waits for a lease. The caller has the Process.
         */
        case NETCTRL_DHCP_START:
        case NETCTRL_DHCP_RENEW:
        case NETCTRL_DHCP_RELEASE:
        {
            LONG err;

            if (ctl->nsc_Index >= (UWORD)NX_MAX_PHYSICAL_INTERFACES)
                return bsd_fail(SocketBase, AMI_ENXIO);

            if (op == NETCTRL_DHCP_START)
            {
                /* NETCTRL_F_ADDRESS makes nsc_Destination the address to ask
                   the server for. A wish, not a demand: DISCOVER is still
                   sent, so a server that disagrees offers something else. */
                err = netstack_interface_dhcp_start(
                          ctl->nsc_Index,
                          (ctl->nsc_Flags & NETCTRL_F_ADDRESS)
                              ? ctl->nsc_Destination : 0UL);

                if (err == AMI_NET_ERR_BUSY)
                    return bsd_fail(SocketBase, AMI_EBUSY);
            }
            else
            {
                /*
                 * Both of these act on a lease, so both refuse an interface
                 * that has none rather than act on a guess at what was meant.
                 * ENOTCONN is the one errno that says "there is nothing here
                 * to act on" without a claim that the interface or the
                 * operation is wrong.
                 */
                if (netstack_interface_dhcp_state(ctl->nsc_Index) !=
                        AMI_DHCP_BOUND)
                    return bsd_fail(SocketBase, AMI_ENOTCONN);

                err = (op == NETCTRL_DHCP_RENEW)
                          ? netstack_interface_dhcp_renew(ctl->nsc_Index)
                          : netstack_interface_dhcp_stop(ctl->nsc_Index, TRUE);
            }

            if (err == AMI_NET_OK)
                return 0;

            /* No stack, no client, or an interface the client was never
               enabled on: all of them are "there is no DHCP here". */
            return bsd_fail(SocketBase, AMI_ENETDOWN);
        }

        /*
         * The machine's name. Out here for the reason the rest of these are:
         * netstack_hostname_offer() takes the bracket itself, and it needs to,
         * because the DHCP thread reads the buffer it writes.
         *
         * It answers on a stack with no interfaces up, which is deliberate: a
         * machine is named before it is addressed, and the name is what the
         * first DHCP request will announce.
         */
        case NETCTRL_HOSTNAME_SET:
        {
            LONG err;

            if (!ns_terminated(ctl->nsc_HostName,
                               sizeof(ctl->nsc_HostName)) ||
                ctl->nsc_HostName[0] == '\0')
                return bsd_fail(SocketBase, AMI_EINVAL);

            err = netstack_hostname_offer((UWORD)AMI_HOSTNAME_ENV,
                                          ctl->nsc_HostName);

            if (err == AMI_NET_OK)
                return 0;

            /*
             * EPERM and not EINVAL: the name is fine and the caller is not
             * allowed to use it, because something that outranks ENV:HOSTNAME
             * named this machine already. The caller reads NETSTATUS_SYSTEM's
             * nss_HostSource to say which.
             */
            return bsd_fail(SocketBase,
                            (err == AMI_NET_ERR_CONFIG) ? AMI_EPERM
                                                        : AMI_ENETDOWN);
        }

        /*
         * Out here because it touches nothing but the master base's own
         * counters, and because it has to answer on a stack whose interfaces
         * are all still coming up: it is the first thing AddNetInterface asks
         * for, before it has waited for an address.
         */
        case NETCTRL_STACK_HOLD:
            return (bsd_stack_hold(SocketBase) == 0)
                       ? 0 : bsd_fail(SocketBase, AMI_ENETDOWN);

        /*
         * The shutdown pair, out here with it and for the same reason: both
         * touch the master base's own list and counters and nothing below
         * them, and both have to answer on a stack whose interfaces are
         * already down, which is the state NetShutdown asks in.
         */
        case NETCTRL_STACK_NOTIFY:
        {
            ULONG signalled = 0;

            if (bsd_stack_notify(SocketBase, &signalled) != 0)
                return bsd_fail(SocketBase, AMI_ENETDOWN);

            ctl->nsc_Count = signalled;
            return 0;
        }

        /* EBUSY and not ENETDOWN: the stack is up, and the refusal is that
           this caller is the last thing holding it up. */
        case NETCTRL_STACK_RELEASE:
            return (bsd_stack_unhold(SocketBase) == 0)
                       ? 0 : bsd_fail(SocketBase, AMI_EBUSY);

        /*
         * The responder on one interface. Out here with the rest of the
         * src/netstack calls: netstack_iface_mdns_set() takes the bracket
         * itself, and it has to, because a request to switch the responder on
         * can create it, and the module then registers an address-change
         * notifier on the NX_IP.
         *
         * It does not wait for probing. A name is claimed about a second
         * later. The flag NETSTATUS_INTERFACES reports is true from the moment
         * the interface is enabled, and NETSTATUS_SYSTEM's nss_MdnsName is
         * what says the claim went through.
         */
        case NETCTRL_INTERFACE_MDNS:
        {
#ifdef AMINETXDUO_MDNS
            LONG st;

            if (ctl->nsc_Index >= (UWORD)NX_MAX_PHYSICAL_INTERFACES)
                return bsd_fail(SocketBase, AMI_ENXIO);

            st = netstack_iface_mdns_set(
                     ctl->nsc_Index,
                     (ctl->nsc_Flags & NETCTRL_F_MDNS) ? TRUE : FALSE);

            if (st == AMI_NET_OK)
                return 0;

            if (st == AMI_NET_ERR_NOMEM)
                return bsd_fail(SocketBase, AMI_ENOMEM);

            /* ENXIO rather than ENETDOWN for AMI_NET_ERR_STATE: this call
               reaches src/netstack only through an interface, so "no such
               interface" is the reading a caller can act on, and a stack that
               is down has no interfaces either. */
            return bsd_fail(SocketBase,
                            (st == AMI_NET_ERR_KERNEL) ? AMI_EIO : AMI_ENXIO);
#else
            return bsd_fail(SocketBase, AMI_ENOSYS);
#endif
        }

        /*
         * The browse pair, outside the bracket for the same reason: both take
         * their own, and neither goes near an NX_IP, so neither needs the
         * netstack_ip() check the rest of this function opens with, and both
         * answer on a stack whose interfaces are all down.
         */
        case NETCTRL_MDNS_BROWSE:
        case NETCTRL_MDNS_BROWSE_STOP:
        {
#ifdef AMINETXDUO_MDNS
            char        buf[NETSTATUS_SVC_TYPE_LEN];
            const char *type = ns_service_type(ctl, buf);
            LONG        st;

            st = (op == NETCTRL_MDNS_BROWSE)
                     ? netstack_mdns_browse_start(type)
                     : netstack_mdns_browse_stop(type);

            if (st == AMI_NET_OK)
                return 0;

            /* No responder running, against a query the module did not take
               or, for STOP, one that was not there to retire. */
            return bsd_fail(SocketBase,
                            (st == AMI_NET_ERR_STATE) ? AMI_ENETDOWN
                                                      : AMI_EINVAL);
#else
            return bsd_fail(SocketBase, AMI_ENOSYS);
#endif
        }

        default:
            break;
    }

    ip = netstack_ip();
    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    switch (op)
    {
        case NETCTRL_GATEWAY_SET:
            status = nx_ip_gateway_address_set(ip, ctl->nsc_Gateway);
            rc = ns_map_status(SocketBase, status);
            break;

        case NETCTRL_GATEWAY_CLEAR:
            status = nx_ip_gateway_address_clear(ip);
            rc = ns_map_status(SocketBase, status);
            break;

        case NETCTRL_ROUTE_ADD:
#ifdef NX_ENABLE_IP_STATIC_ROUTING
            status = nx_ip_static_route_add(ip, ctl->nsc_Destination,
                                            ctl->nsc_NetMask,
                                            ctl->nsc_Gateway);
            rc = ns_map_status(SocketBase, status);
#else
            /*
             * NX_ENABLE_IP_STATIC_ROUTING is not defined in
             * port/netxduo-amiga/inc/nx_user.h, so there is no routing table
             * to add to. NETSTATUS_SYS_ROUTING lets a command check before it
             * asks.
             */
            rc = bsd_fail(SocketBase, AMI_ENOSYS);
#endif
            break;

        case NETCTRL_ROUTE_DELETE:
#ifdef NX_ENABLE_IP_STATIC_ROUTING
            status = nx_ip_static_route_delete(ip, ctl->nsc_Destination,
                                               ctl->nsc_NetMask);
            rc = ns_map_status(SocketBase, status);
#else
            rc = bsd_fail(SocketBase, AMI_ENOSYS);
#endif
            break;

        case NETCTRL_ARP_ADD:
            status = nx_arp_static_entry_create(ip, ctl->nsc_Destination,
                        ((ULONG)ctl->nsc_HwAddress[0] << 8) |
                         (ULONG)ctl->nsc_HwAddress[1],
                        ((ULONG)ctl->nsc_HwAddress[2] << 24) |
                        ((ULONG)ctl->nsc_HwAddress[3] << 16) |
                        ((ULONG)ctl->nsc_HwAddress[4] << 8) |
                         (ULONG)ctl->nsc_HwAddress[5]);
            rc = ns_map_status(SocketBase, status);
            break;

        case NETCTRL_ARP_DELETE:
            /*
             * nx_arp_entry_delete() rather than nx_arp_static_entry_delete():
             * an address can be cached statically or dynamically, and this
             * call removes either. The static-only call needs the hardware
             * address as well and fails on a dynamic entry.
             */
            status = nx_arp_entry_delete(ip, ctl->nsc_Destination);
            rc = ns_map_status(SocketBase, status);
            break;

        case NETCTRL_ARP_FLUSH:
            status = nx_arp_dynamic_entries_invalidate(ip);
            rc = ns_map_status(SocketBase, status);
            break;

        /*
         * The IPv6 four go through src/netstack rather than straight to NetX
         * Duo, for the reason INTERFACE_UP does: a route is configured at
         * bring-up from GATEWAY6 as well as from a command, and one function
         * is what keeps the two from disagreeing about the router lifetime or
         * about flushing the destination cache. In a build without IPv6 they
         * are not compiled and answer ENOSYS.
         */
        case NETCTRL_ROUTE6_ADD:
#ifdef AMINETXDUO_IPV6
            status = netstack_ipv6_route_add(ctl->nsc_Destination6,
                                             ctl->nsc_PrefixLength,
                                             ctl->nsc_Gateway6,
                                             ctl->nsc_Index);
            rc = ns_map_status(SocketBase, status);
#else
            rc = bsd_fail(SocketBase, AMI_ENOSYS);
#endif
            break;

        case NETCTRL_ROUTE6_DELETE:
#ifdef AMINETXDUO_IPV6
            status = netstack_ipv6_route_delete(ctl->nsc_Destination6,
                                                ctl->nsc_PrefixLength,
                                                ctl->nsc_Gateway6);
            rc = ns_map_status(SocketBase, status);
#else
            rc = bsd_fail(SocketBase, AMI_ENOSYS);
#endif
            break;

        case NETCTRL_ND_ADD:
#ifdef AMINETXDUO_IPV6
            status = nxd_nd_cache_entry_set(ip, ctl->nsc_Destination6,
                                            (UINT)ctl->nsc_Index,
                                            (CHAR *)ctl->nsc_HwAddress);
            rc = ns_map_status(SocketBase, status);
#else
            rc = bsd_fail(SocketBase, AMI_ENOSYS);
#endif
            break;

        case NETCTRL_ND_DELETE:
#ifdef AMINETXDUO_IPV6
            status = nxd_nd_cache_entry_delete(ip, ctl->nsc_Destination6);
            rc = ns_map_status(SocketBase, status);
#else
            rc = bsd_fail(SocketBase, AMI_ENOSYS);
#endif
            break;

        default:
            rc = bsd_fail(SocketBase, AMI_EINVAL);
            break;
    }

    bsd_nx_leave(SocketBase);

    return rc;
}
