/*
 * bsdsocket.library -- NetStackQuery() and NetStackControl().
 *
 * The two private LVOs that let a separate Shell command ask the RUNNING
 * stack what it is doing, and tell it to change. include/aminetxduo/
 * netstatus.h has the contract and the reasoning; this file has the walks.
 *
 * WHAT MADE THESE NECESSARY
 *
 *   Every command that wanted live numbers linked its own copy of NetX Duo,
 *   got its own NX_IP with no interfaces in it, and read zeroes -- or, once
 *   src/tools/netstack_weak.c's weak stubs answered NULL, printed "the
 *   network is up, but this command cannot read it" and exited 5. That
 *   message reads like a pass, which is why it survived a release.
 *
 * THREE RULES THIS FILE FOLLOWS
 *
 *   1. COPY, NEVER LEND. Nothing that leaves here is a pointer into the
 *      stack. The caller gets scalars in its own buffer, and what it does
 *      with them afterwards cannot fault.
 *
 *   2. ONE BRACKET, HELD BRIEFLY. bsd_nx_enter() adopts the calling task as a
 *      TX_THREAD -- required, because nx_*_info_get() are behind
 *      NX_THREADS_ONLY_CALLER_CHECKING -- and the walk does nothing but read
 *      memory and call those. No dos.library, no Wait(), no allocation.
 *
 *   3. THE CALLER'S BUFFER IS TOUCHED ONLY AFTER THE HEADER CHECKS PASS. A
 *      caller that arrived here by accident (some future vendor putting a
 *      different function at the same offset) presents the wrong magic and
 *      gets -1 with nothing written.
 *
 * WHAT IS NOT HERE, AND WHY
 *
 *   There is no "ping for me" vector. It was designed and rejected:
 *   nx_icmp_ping() matches an inbound echo reply on the SEQUENCE NUMBER
 *   ALONE -- nx_icmpv4_process_echo_reply.c:124 compares
 *   tx_thread_suspend_info against nx_icmpv4_echo_sequence_num and looks at
 *   nothing else, and nx_icmpv4.h:191 says outright that the identifier "is
 *   not used as a host". A vector wrapping it would inherit that. The raw
 *   socket path (src/bsdsocket/raw.c) lets the command choose its own
 *   matching rule, is already published ABI, and is what src/tools/ping.c
 *   uses. See docs/RESEARCH.md 21.3.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/netstatus.h"
#include "aminetxduo/sana2.h"
#include "aminetxduo/config.h"

/*
 * The wire values in netstatus.h must be NetX Duo's own, because that is what
 * the socket table copies straight across. If a NetX Duo update ever
 * renumbers them the build stops here rather than at a user reading
 * "ESTABLISHED" off a socket that is closing.
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
 * legitimate way to ask "how many are there?" -- nsh_Available still comes
 * back filled in.
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
 * regardless, so a caller with a small buffer still learns how big one it
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
 * The NX physical interface index IS the config index. src/netstack attaches
 * the configured interfaces in config order from slot 0
 * (netstack.c:376 for the primary, :451 for the rest), and NetX Duo puts the
 * loopback at NX_LOOPBACK_INTERFACE == NX_MAX_PHYSICAL_INTERFACES, past the
 * physical range this walks -- so there is no off-by-one here, which is worth
 * saying because there looks as though there should be.
 *
 * NULL when the slot is not configured; NETSTATUS_IF_NAMED is how the caller
 * tells a name that came from the config from one that did not.
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
         * additional link info (aminetxduo/sana2.h), which is how the driver's
         * own counters are reached without a separate registry.
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
            /* NetX Duo's own name for the slot, which for loopback is the
             * only name there is. */
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
 * The ARP cache is a hash table of CIRCULAR lists -- nx_arp_active_next of the
 * last entry in a bucket points back at the bucket head, not at NX_NULL -- so
 * every walk needs the head comparison below. Getting that wrong does not
 * fail; it spins, inside the bracket, with the baton held.
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
 * ROUTES, and the honest answer about them.
 *
 * NX_ENABLE_IP_STATIC_ROUTING is NOT defined in
 * port/netxduo-amiga/inc/nx_user.h, so nx_ip_static_route_add() and the
 * nx_ip_routing_table[] it fills are not compiled into this stack at all --
 * NX_IP_ROUTING_TABLE_SIZE *is* set there, which reads as though they were.
 *
 * What exists without it is exactly two kinds of route, and both are real:
 * the directly-attached prefix of each interface that has an address, and the
 * default gateway (nx_ip_gateway_address, which nx_ip_gateway_address_set()
 * maintains in every build). Those are what a machine on one Ethernet
 * actually has, so this reports them and sets nsh_Count accordingly, rather
 * than reporting an empty table and letting the user conclude their network
 * is misconfigured.
 *
 * NETSTATUS_SYS_ROUTING in the SYSTEM query is how a caller tells the two
 * worlds apart without guessing.
 */
static VOID ns_fill_routes(NX_IP *ip, NsWriter *w)
{
    ULONG gateway = 0;
    UINT  i;
#ifdef NX_ENABLE_IP_STATIC_ROUTING
    UINT  r;
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

    if (nx_ip_gateway_address_get(ip, &gateway) == NX_SUCCESS && gateway != 0)
    {
        NetStatusRoute *out = (NetStatusRoute *)ns_writer_next(w);

        if (out != NULL)
        {
            out->nsr_Destination = 0;
            out->nsr_NetMask     = 0;
            out->nsr_Gateway     = gateway;
            out->nsr_Flags       = NETSTATUS_RT_UP | NETSTATUS_RT_GATEWAY;
            out->nsr_Interface   = 0;

            if (ip->nx_ip_gateway_interface != NX_NULL)
            {
                for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
                {
                    if (ip->nx_ip_gateway_interface == &ip->nx_ip_interface[i])
                    {
                        out->nsr_Interface = (UWORD)i;
                        break;
                    }
                }
            }
        }
    }

#ifdef NX_ENABLE_IP_STATIC_ROUTING
    for (r = 0; r < ip->nx_ip_routing_table_entry_count; r++)
    {
        NetStatusRoute *out = (NetStatusRoute *)ns_writer_next(w);

        if (out == NULL)
            continue;

        out->nsr_Destination = ip->nx_ip_routing_table[r].nx_ip_routing_dest_ip;
        out->nsr_NetMask     = ip->nx_ip_routing_table[r].nx_ip_routing_net_mask;
        out->nsr_Gateway     = ip->nx_ip_routing_table[r].nx_ip_routing_next_hop_address;
        out->nsr_Flags       = NETSTATUS_RT_UP | NETSTATUS_RT_STATIC |
                               NETSTATUS_RT_GATEWAY;
        out->nsr_Interface   = 0;
    }
#endif
}

/*
 * The created-socket lists are singly linked and CIRCULAR too, so the walk is
 * bounded by the count NetX Duo keeps rather than by a NULL that never comes.
 * These are the sockets of every task using the library, which is the point:
 * netstat run from one Shell must see the connection another Shell's `fetch`
 * has open.
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
 * The header check, done BEFORE anything is written. A caller that landed
 * here by accident presents the wrong magic and leaves with its buffer
 * untouched -- which is the whole reason the magic is in the buffer's first
 * four bytes rather than nowhere.
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
     * the size check has to happen before the walk rather than inside it.
     */
    switch (what)
    {
        case NETSTATUS_SYSTEM:      need = sizeof(NetStatusSystem);  break;
        case NETSTATUS_STATS:       need = sizeof(NetStatusStats);   break;
        case NETSTATUS_INTERFACES:  need = 0;                        break;
        case NETSTATUS_ARP:         need = 0;                        break;
        case NETSTATUS_ROUTES:      need = 0;                        break;
        case NETSTATUS_SOCKETS:     need = 0;                        break;
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

        default:    /* NETSTATUS_SOCKETS -- the switch above rejected the rest */
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
     * knows about both. It brackets itself, so it is called OUTSIDE ours.
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
             * to add to. ENOSYS rather than a lie; NETSTATUS_SYS_ROUTING lets
             * a command say so before it asks.
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
             * an address may be cached statically or dynamically and the
             * person typing `arp -d` neither knows nor should have to. This
             * one removes whichever it is; the static-only call needs the
             * hardware address as well and fails on a dynamic entry.
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
