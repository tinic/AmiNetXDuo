/*
 * bsdsocket.library -- NetStackQuery() and NetStackControl().
 *
 * The two private LVOs that let a separate Shell command ask the running
 * stack what it is doing, and tell it to change. include/aminetxduo/
 * netstatus.h has the interface; this file has the walks.
 *
 * Before these existed, every command that wanted live numbers linked its own
 * copy of NetX Duo, got its own NX_IP with no interfaces in it, and read
 * zeroes -- or, once src/tools/netstack_weak.c's weak stubs answered NULL,
 * printed "the network is up, but this command cannot read it" and exited 5.
 *
 * Three rules here:
 *
 *   1. Copy, never lend. Nothing that leaves here is a pointer into the
 *      stack; the caller gets scalars in its own buffer.
 *
 *   2. One bracket, held briefly. bsd_nx_enter() adopts the calling task as a
 *      TX_THREAD -- required, because nx_*_info_get() are behind
 *      NX_THREADS_ONLY_CALLER_CHECKING -- and the walk does nothing but read
 *      memory and call those. No dos.library, no Wait(), no allocation.
 *
 *   3. The caller's buffer is touched only after the header checks pass, so a
 *      caller that arrived here by accident (a future vendor putting a
 *      different function at the same offset) presents the wrong magic and
 *      gets -1 with nothing written.
 *
 * There is no "ping for me" vector. nx_icmp_ping() matches an inbound echo
 * reply on the sequence number alone -- nx_icmpv4_process_echo_reply.c:124
 * compares tx_thread_suspend_info against nx_icmpv4_echo_sequence_num and
 * looks at nothing else, and nx_icmpv4.h:191 says the identifier "is not used
 * as a host". A vector wrapping it would inherit that. The raw socket path
 * (src/bsdsocket/raw.c) lets the command choose its own matching rule, is
 * already published ABI, and is what src/tools/ping.c uses. See
 * docs/RESEARCH.md 21.3.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/netstatus.h"
#include "aminetxduo/sana2.h"
#include "aminetxduo/config.h"
#include "aminetxduo/netstack.h"

/*
 * The wire values in netstatus.h must be NetX Duo's own, because the socket
 * table copies them straight across. If a NetX Duo update renumbers them the
 * build stops here rather than at a user reading "ESTABLISHED" off a socket
 * that is closing.
 */
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

/* The header is copied by hand below; its size is part of the ABI. */
_Static_assert(sizeof(NetStatusHeader) == 16, "NetStatusHeader ABI");

/* ------------------------------------------------------------- plumbing -- */

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

static VOID ns_mac_from_words(ULONG msw, ULONG lsw, UBYTE *mac)
{
    mac[0] = (UBYTE)((msw >> 8) & 0xff);
    mac[1] = (UBYTE)(msw & 0xff);
    mac[2] = (UBYTE)((lsw >> 24) & 0xff);
    mac[3] = (UBYTE)((lsw >> 16) & 0xff);
    mac[4] = (UBYTE)((lsw >> 8) & 0xff);
    mac[5] = (UBYTE)(lsw & 0xff);
}

/*
 * How many entries of `entry_size` fit after the header, and where the first
 * one goes. `room` is 0 when the buffer holds only the header, which is a
 * legitimate way to ask "how many are there?" -- nsh_Available is still
 * filled in.
 */
typedef struct NsWriter
{
    NetStatusHeader *hdr;
    UBYTE           *entries;
    ULONG            room;          /* entries that fit                     */
    ULONG            entry_size;
    ULONG            written;
    ULONG            available;
} NsWriter;

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

/* ------------------------------------------------------------- the walks -- */

static VOID ns_fill_system(NX_IP *ip, NetStatusSystem *out)
{
    NX_PACKET_POOL *pool;
    ULONG           gateway = 0;

    out->nss_Flags          = NETSTATUS_SYS_UP;
    out->nss_InterfaceCount = (ULONG)NX_MAX_PHYSICAL_INTERFACES;

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
         * NULL before the responder has claimed a name. The flag already
         * distinguishes that from a build without mDNS, so an empty string
         * here means "not yet", not "never".
         */
        if (mdns != NULL && mdns[0] != '\0')
        {
            static const char suffix[] = ".local";
            ULONG i;
            ULONG j;

            /*
             * The responder keeps the bare label ("amiga"). Append ".local"
             * here rather than at each display, so every consumer gets the
             * name in the form it is actually used in.
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
 * this already runs under -- the nesting is detected, not paid for.
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
            break;                      /* the caller's buffer is full */

        out->nsd_Index = index;

        state = netstack_interface_dhcp_state(index);

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
 * The NX physical interface index is the config index. src/netstack attaches
 * the configured interfaces in config order from slot 0 (netstack.c:376 for
 * the primary, :451 for the rest), and NetX Duo puts the loopback at
 * NX_LOOPBACK_INTERFACE == NX_MAX_PHYSICAL_INTERFACES, past the physical range
 * this walks, so there is no off-by-one to correct for.
 *
 * NULL when the slot is not configured; NETSTATUS_IF_NAMED tells the caller
 * whether a name came from the config.
 */
static const AmiIfConfig *ns_config_for(UINT nx_index)
{
    const AmiConfig *cfg = netstack_config();

    if (cfg == NULL)
        return NULL;

    if (nx_index >= cfg->interface_count ||
        nx_index >= (UINT)AMI_CFG_MAX_INTERFACES)
        return NULL;

    if (!cfg->interfaces[nx_index].configured)
        return NULL;

    return &cfg->interfaces[nx_index];
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
            out->nsi_AllocFailures    = stats.alloc_failures;
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
            /* NetX Duo's own name for the slot; for loopback it is the only
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
 * The ARP cache is a hash table of circular lists -- nx_arp_active_next of the
 * last entry in a bucket points back at the bucket head, not at NX_NULL -- so
 * the walk needs the head comparison below. Without it the loop spins inside
 * the bracket, holding the lock.
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
 *   1. the directly-attached prefix of each interface that has an address;
 *   2. NetX Duo's static routing table, longest prefix first -- it keeps
 *      nx_ip_routing_table[] sorted by netmask descending, so the order here
 *      is the order _nx_ip_route_find() matches in;
 *   3. the default gateway (nx_ip_gateway_address), consulted last and
 *      maintained by nx_ip_gateway_address_set() in every build.
 *
 * The static table exists only when port/netxduo-amiga/inc/nx_user.h defines
 * NX_ENABLE_IP_STATIC_ROUTING; NX_IP_ROUTING_TABLE_SIZE alone is not enough.
 * NETSTATUS_SYS_ROUTING in the SYSTEM query tells a caller which kind of build
 * it is talking to, so this file compiles and reports correctly either way.
 */
/* Which nx_ip_interface[] slot a route points at, 0 when it points nowhere. */
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
 * These are the sockets of every task using the library, so netstat run from
 * one Shell sees the connection another Shell's `fetch` has open.
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

    /*
     * The one-entry answers are written straight into the caller's buffer, so
     * the size check happens before the walk rather than inside it.
     *
     * It also makes the two unchecked ns_writer_next() results below safe:
     * `need != 0` for SYSTEM and STATS, so size >= header + entry_size, so
     * ns_writer_init() computes room >= 1 and the first slot exists. GCC's
     * -fanalyzer reports those two as "dereference of NULL 'out'" because that
     * chain goes through a division it will not reason about. A NULL test at
     * the call would be dead code implying the buffer might be too small.
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
        default:                    return bsd_fail(SocketBase, AMI_EINVAL);
    }

    if (need != 0 && size < sizeof(NetStatusHeader) + need)
        return bsd_fail(SocketBase, AMI_EINVAL);

    hdr->nsh_Type      = (UWORD)what;
    hdr->nsh_Count     = 0;
    hdr->nsh_Available = 0;
    hdr->nsh_EntrySize = 0;
    hdr->nsh_Reserved  = 0;

    ip = netstack_ip();
    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    /* Adopted from here to bsd_nx_leave(): memory reads and nx_*_info_get(). */
    switch (what)
    {
        case NETSTATUS_SYSTEM:
            ns_writer_init(&w, hdr, size, NETSTATUS_SYSTEM,
                           sizeof(NetStatusSystem));
            ns_fill_system(ip, (NetStatusSystem *)ns_writer_next(&w));
            ns_writer_finish(&w);
            break;

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

        default:    /* NETSTATUS_SOCKETS; the switch above rejected the rest */
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
     * Online/Offline go through src/netstack rather than through NetX Duo:
     * taking an interface down means stopping its SANA-II readers as well as
     * telling NetX Duo, and netstack_interface_down() is the only thing that
     * knows about both. It brackets itself, so it is called outside ours.
     */
    switch (op)
    {
        case NETCTRL_INTERFACE_UP:
            return (netstack_interface_up(ctl->nsc_Index) == AMI_NET_OK)
                       ? 0 : bsd_fail(SocketBase, AMI_ENXIO);

        case NETCTRL_INTERFACE_DOWN:
            return (netstack_interface_down(ctl->nsc_Index) == AMI_NET_OK)
                       ? 0 : bsd_fail(SocketBase, AMI_ENXIO);

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
             * an address may be cached statically or dynamically, and this
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

        default:
            rc = bsd_fail(SocketBase, AMI_EINVAL);
            break;
    }

    bsd_nx_leave(SocketBase);

    return rc;
}
