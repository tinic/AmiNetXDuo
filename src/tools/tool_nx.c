/*
 * AmiNetXDuo tools -- talking to the live NetX Duo instance.
 *
 * Every function here is a NetStackQuery() call and a copy out of the answer.
 * No ThreadX is involved: the ThreadX a command could adopt into is never the
 * one the stack is running on. See tools_nx.h, and
 * include/aminetxduo/netstatus.h for the interface these go through.
 *
 * The library is opened and closed around each of the three calls rather than
 * held across them. Opening it is a child-base clone and costs almost nothing,
 * and a base held open across a printing loop would keep the network alive
 * after the command exits.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"

/*
 * Static rather than automatic: a Shell command starts with a 4 KB stack and
 * the socket table alone is 512 bytes of answer. Each command runs one of
 * these at a time.
 */
static union
{
    struct { NetStatusHeader hdr; NetStatusSystem    e; }      system;
    struct { NetStatusHeader hdr; NetStatusInterface e[TOOL_MAX_IF]; } iface;
    struct { NetStatusHeader hdr; NetStatusStats     e; }      stats;
    struct { NetStatusHeader hdr; NetStatusArp       e[TOOL_MAX_ARP]; } arp;
    struct { NetStatusHeader hdr; NetStatusRoute     e[TOOL_MAX_ROUTE]; } route;
    struct { NetStatusHeader hdr; NetStatusSocket    e[TOOL_MAX_SOCK]; } sock;
    struct { NetStatusHeader hdr; NetStatusDhcp      e[TOOL_MAX_IF]; }   dhcp;
    struct { NetStatusHeader hdr; NetStatusAddress6  e[TOOL_MAX_ADDR6]; } addr6;
} nx_answer;

/*
 * The failure message, once. tool_netstatus_open() already distinguishes
 * "nothing is running" from "something is running that is not us", so this only
 * covers a library that opened and then would not answer.
 */
static BOOL nx_quiet;

VOID tool_nx_quiet(BOOL quiet)
{
    nx_quiet = quiet;
}

static VOID nx_query_failed(struct Library *base)
{
    if (nx_quiet)
        return;

    tool_error("the network is up, but it would not report on itself");
    tool_explain_no_netstatus(base);
}

static struct Library *nx_open(VOID)
{
    return tool_netstatus_open(nx_quiet);
}

/* ------------------------------------------------------------ snapshots -- */

LONG tool_snapshot(ToolSnapshot *out, BOOL want_sockets)
{
    struct Library *base;
    LONG            n;
    LONG            i;
    LONG            j;

    if (out == NULL)
        return -1;

    for (i = 0; i < (LONG)TOOL_MAX_IF; i++)
    {
        ToolIfInfo *info = &out->iface[i];

        info->nx_index     = (UWORD)i;
        info->attached     = FALSE;
        info->link_up      = FALSE;
        info->address      = 0;
        info->netmask      = 0;
        info->mtu          = 0;
        info->nx_name[0]   = '\0';
        info->nx_device[0] = '\0';
        info->nx_unit      = 0;
        info->have_sana2   = FALSE;
        info->sana2_online = FALSE;
        info->bps          = 0;
        for (j = 0; j < (LONG)AMI_ETH_ADDR_SIZE; j++)
            info->mac[j] = 0;
    }
    out->iface_count    = 0;
    out->addr6_count    = 0;
    out->sock_count     = 0;
    out->sock_truncated = FALSE;
    out->gateway        = 0;
    out->have_gateway   = FALSE;
    out->have_mdns      = FALSE;
    out->mdns_name[0]   = '\0';

    base = nx_open();
    if (base == NULL)
        return -1;

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &nx_answer,
                             sizeof(nx_answer.iface),
                             sizeof(NetStatusInterface));
    if (n < 0)
    {
        nx_query_failed(base);
        tool_netstatus_close(base);
        return -1;
    }

    for (i = 0; i < n && i < (LONG)TOOL_MAX_IF; i++)
    {
        const NetStatusInterface *src  = &nx_answer.iface.e[i];
        ToolIfInfo               *info = &out->iface[i];

        info->nx_index = src->nsi_Index;
        info->attached = (src->nsi_Flags & NETSTATUS_IF_ATTACHED) ? TRUE : FALSE;
        info->link_up  = (src->nsi_Flags & NETSTATUS_IF_LINKUP) ? TRUE : FALSE;
        info->address  = src->nsi_Address;
        info->netmask  = src->nsi_NetMask;
        info->mtu      = src->nsi_MTU;

        for (j = 0; j < (LONG)AMI_ETH_ADDR_SIZE; j++)
            info->mac[j] = src->nsi_HwAddress[j];

        tool_copy_string(info->nx_name, sizeof(info->nx_name), src->nsi_Name);
        tool_copy_string(info->nx_device, sizeof(info->nx_device),
                         src->nsi_Device);
        info->nx_unit = src->nsi_Unit;

        if (src->nsi_Flags & NETSTATUS_IF_SANA2)
        {
            info->have_sana2   = TRUE;
            info->sana2_online = (src->nsi_Flags & NETSTATUS_IF_ONLINE)
                                     ? TRUE : FALSE;
            info->bps          = src->nsi_Speed;

            info->stats.packets_received = src->nsi_PacketsIn;
            info->stats.packets_sent     = src->nsi_PacketsOut;
            info->stats.bad_data         = src->nsi_BadData;
            info->stats.overruns         = src->nsi_Overruns;
            info->stats.unknown_types    = src->nsi_UnknownTypes;
            info->stats.reconfigurations = src->nsi_Reconfigurations;
            info->stats.tx_errors        = src->nsi_TxErrors;
            info->stats.rx_errors        = src->nsi_RxErrors;
            info->stats.alloc_failures   = src->nsi_AllocFailures;
        }
    }
    out->iface_count = (UWORD)n;

    if (tool_netstatus_query(base, NETSTATUS_SYSTEM, &nx_answer,
                             sizeof(nx_answer.system),
                             sizeof(NetStatusSystem)) > 0)
    {
        if (nx_answer.system.e.nss_Flags & NETSTATUS_SYS_GATEWAY)
        {
            out->gateway      = nx_answer.system.e.nss_Gateway;
            out->have_gateway = TRUE;
        }

        if (nx_answer.system.e.nss_Flags & NETSTATUS_SYS_MDNS)
        {
            out->have_mdns = TRUE;
            tool_copy_string(out->mdns_name, sizeof(out->mdns_name),
                             nx_answer.system.e.nss_MdnsName);
        }
    }

    /*
     * A library without IPv6 answers with no entries and one too old to know
     * the selector answers -1.  Both mean "this machine has no IPv6 address to
     * report", which is not a failure and prints nothing.
     */
    n = tool_netstatus_query(base, NETSTATUS_ADDRESSES6, &nx_answer,
                             sizeof(nx_answer.addr6),
                             sizeof(NetStatusAddress6));
    for (i = 0; i < n && i < (LONG)TOOL_MAX_ADDR6; i++)
    {
        const NetStatusAddress6 *src = &nx_answer.addr6.e[i];
        ToolAddr6Info           *a6  = &out->addr6[i];

        a6->nx_index = src->nsn_Interface;
        a6->state    = src->nsn_State;
        a6->prefix   = src->nsn_PrefixLength;

        tool_format_ip6(base, src->nsn_Address, a6->text, sizeof(a6->text));

        out->addr6_count = (UWORD)(i + 1);
    }

    if (want_sockets)
    {
        n = tool_netstatus_query(base, NETSTATUS_SOCKETS, &nx_answer,
                                 sizeof(nx_answer.sock),
                                 sizeof(NetStatusSocket));
        if (n > 0)
        {
            if (nx_answer.sock.hdr.nsh_Available > nx_answer.sock.hdr.nsh_Count)
                out->sock_truncated = TRUE;

            for (i = 0; i < n && i < (LONG)TOOL_MAX_SOCK; i++)
            {
                const NetStatusSocket *src = &nx_answer.sock.e[i];
                ToolSockInfo          *si  = &out->sock[i];

                si->is_tcp = (src->nso_Flags & NETSTATUS_SOCK_TCP)
                                 ? TRUE : FALSE;
                si->local_port   = src->nso_LocalPort;
                si->peer_port    = src->nso_PeerPort;
                si->peer_address = src->nso_PeerAddress;
                si->state        = (UINT)src->nso_State;
                si->queued       = src->nso_Queued;
            }

            out->sock_count = (UWORD)n;
        }
    }

    tool_netstatus_close(base);

    return 0;
}

const char *tool_addr6_state(UWORD state)
{
    switch (state)
    {
        case NETSTATUS_IP6_TENTATIVE:   return "tentative";
        case NETSTATUS_IP6_DEPRECATED:  return "deprecated";
        default:                        return NULL;
    }
}

/* ---------------------------------------------------- protocol counters -- */

LONG tool_stats(ToolStats *out)
{
    struct Library        *base;
    const NetStatusStats  *s;
    LONG                   n;
    LONG                   i;
    LONG                   j;

    if (out == NULL)
        return -1;

    /* Zeroed field by field rather than with memset: these tools link no libc. */
    out->have_ip = out->have_icmp = out->have_tcp = FALSE;
    out->have_udp = out->have_arp = out->have_pool = FALSE;
    out->arp_count     = 0;
    out->arp_truncated = FALSE;

    for (i = 0; i < (LONG)TOOL_MAX_ARP; i++)
    {
        out->arp[i].address   = 0;
        out->arp[i].is_static = FALSE;
        out->arp[i].resolved  = FALSE;
        out->arp[i].retries   = 0;
        out->arp[i].nx_index  = 0;
        for (j = 0; j < (LONG)AMI_ETH_ADDR_SIZE; j++)
            out->arp[i].mac[j] = 0;
    }

    base = nx_open();
    if (base == NULL)
        return -1;

    if (tool_netstatus_query(base, NETSTATUS_STATS, &nx_answer,
                             sizeof(nx_answer.stats),
                             sizeof(NetStatusStats)) <= 0)
    {
        nx_query_failed(base);
        tool_netstatus_close(base);
        return -1;
    }

    s = &nx_answer.stats.e;

    out->have_ip = (s->nsx_Have & NETSTATUS_HAVE_IP) ? TRUE : FALSE;
    out->ip_packets_sent       = s->nsx_IpPacketsSent;
    out->ip_bytes_sent         = s->nsx_IpBytesSent;
    out->ip_packets_received   = s->nsx_IpPacketsReceived;
    out->ip_bytes_received     = s->nsx_IpBytesReceived;
    out->ip_invalid            = s->nsx_IpInvalid;
    out->ip_receive_dropped    = s->nsx_IpReceiveDropped;
    out->ip_checksum_errors    = s->nsx_IpChecksumErrors;
    out->ip_send_dropped       = s->nsx_IpSendDropped;
    out->ip_fragments_sent     = s->nsx_IpFragmentsSent;
    out->ip_fragments_received = s->nsx_IpFragmentsReceived;

    out->have_icmp = (s->nsx_Have & NETSTATUS_HAVE_ICMP) ? TRUE : FALSE;
    out->icmp_pings_sent        = s->nsx_IcmpPingsSent;
    out->icmp_ping_timeouts     = s->nsx_IcmpPingTimeouts;
    out->icmp_threads_suspended = s->nsx_IcmpThreadsSuspended;
    out->icmp_responses         = s->nsx_IcmpResponses;
    out->icmp_checksum_errors   = s->nsx_IcmpChecksumErrors;
    out->icmp_unhandled         = s->nsx_IcmpUnhandled;

    out->have_tcp = (s->nsx_Have & NETSTATUS_HAVE_TCP) ? TRUE : FALSE;
    out->tcp_packets_sent        = s->nsx_TcpPacketsSent;
    out->tcp_bytes_sent          = s->nsx_TcpBytesSent;
    out->tcp_packets_received    = s->nsx_TcpPacketsReceived;
    out->tcp_bytes_received      = s->nsx_TcpBytesReceived;
    out->tcp_invalid             = s->nsx_TcpInvalid;
    out->tcp_receive_dropped     = s->nsx_TcpReceiveDropped;
    out->tcp_checksum_errors     = s->nsx_TcpChecksumErrors;
    out->tcp_connections         = s->nsx_TcpConnections;
    out->tcp_disconnections      = s->nsx_TcpDisconnections;
    out->tcp_connections_dropped = s->nsx_TcpConnectionsDropped;
    out->tcp_retransmits         = s->nsx_TcpRetransmits;

    out->have_udp = (s->nsx_Have & NETSTATUS_HAVE_UDP) ? TRUE : FALSE;
    out->udp_packets_sent     = s->nsx_UdpPacketsSent;
    out->udp_bytes_sent       = s->nsx_UdpBytesSent;
    out->udp_packets_received = s->nsx_UdpPacketsReceived;
    out->udp_bytes_received   = s->nsx_UdpBytesReceived;
    out->udp_invalid          = s->nsx_UdpInvalid;
    out->udp_receive_dropped  = s->nsx_UdpReceiveDropped;
    out->udp_checksum_errors  = s->nsx_UdpChecksumErrors;

    out->have_arp = (s->nsx_Have & NETSTATUS_HAVE_ARP) ? TRUE : FALSE;
    out->arp_requests_sent      = s->nsx_ArpRequestsSent;
    out->arp_requests_received  = s->nsx_ArpRequestsReceived;
    out->arp_responses_sent     = s->nsx_ArpResponsesSent;
    out->arp_responses_received = s->nsx_ArpResponsesReceived;
    out->arp_dynamic_entries    = s->nsx_ArpDynamicEntries;
    out->arp_static_entries     = s->nsx_ArpStaticEntries;
    out->arp_aged_entries       = s->nsx_ArpAgedEntries;
    out->arp_invalid_messages   = s->nsx_ArpInvalidMessages;

    if (out->have_arp)
    {
        n = tool_netstatus_query(base, NETSTATUS_ARP, &nx_answer,
                                 sizeof(nx_answer.arp), sizeof(NetStatusArp));
        if (n > 0)
        {
            if (nx_answer.arp.hdr.nsh_Available > nx_answer.arp.hdr.nsh_Count)
                out->arp_truncated = TRUE;

            for (i = 0; i < n && i < (LONG)TOOL_MAX_ARP; i++)
            {
                const NetStatusArp *src  = &nx_answer.arp.e[i];
                ToolArpEntry       *slot = &out->arp[i];

                slot->address   = src->nsa_Address;
                slot->retries   = src->nsa_Retries;
                slot->nx_index  = src->nsa_Interface;
                slot->is_static = (src->nsa_Flags & NETSTATUS_ARP_STATIC)
                                      ? TRUE : FALSE;
                slot->resolved  = (src->nsa_Flags & NETSTATUS_ARP_RESOLVED)
                                      ? TRUE : FALSE;

                for (j = 0; j < (LONG)AMI_ETH_ADDR_SIZE; j++)
                    slot->mac[j] = src->nsa_HwAddress[j];
            }

            out->arp_count = (UWORD)n;
        }
    }

    if (tool_netstatus_query(base, NETSTATUS_SYSTEM, &nx_answer,
                             sizeof(nx_answer.system),
                             sizeof(NetStatusSystem)) > 0)
    {
        const NetStatusSystem *sys = &nx_answer.system.e;

        if (sys->nss_PoolTotal != 0)
        {
            out->have_pool               = TRUE;
            out->pool_total              = sys->nss_PoolTotal;
            out->pool_free               = sys->nss_PoolFree;
            out->pool_payload            = sys->nss_PoolPayload;
            out->pool_empty_requests     = sys->nss_PoolEmptyRequests;
            out->pool_empty_suspensions  = sys->nss_PoolEmptySuspensions;
            out->pool_invalid_releases   = sys->nss_PoolInvalidReleases;
        }
    }

    tool_netstatus_close(base);

    return 0;
}

/* ------------------------------------------------------------------ DHCP -- */

LONG tool_dhcp(ToolDhcp *out)
{
    struct Library *base;
    LONG            n;
    LONG            i;
    LONG            j;

    if (out == NULL)
        return -1;

    out->count = 0;

    base = nx_open();
    if (base == NULL)
        return -1;

    /*
     * No nx_query_failed() here: a stack that does not know the selector just
     * means one part of one report is missing, and the caller words that
     * itself.
     */
    n = tool_netstatus_query(base, NETSTATUS_DHCP, &nx_answer,
                             sizeof(nx_answer.dhcp), sizeof(NetStatusDhcp));
    if (n < 0)
    {
        tool_netstatus_close(base);
        return -1;
    }

    for (i = 0; i < n && i < (LONG)TOOL_MAX_IF; i++)
    {
        const NetStatusDhcp *src  = &nx_answer.dhcp.e[i];
        ToolDhcpInfo        *info = &out->iface[i];

        info->nx_index      = src->nsd_Index;
        info->state         = src->nsd_State;
        info->address       = src->nsd_Address;
        info->netmask       = src->nsd_NetMask;
        info->server        = src->nsd_Server;
        info->lease_seconds = src->nsd_LeaseSeconds;

        info->router_count = src->nsd_RouterCount;
        if (info->router_count > (UWORD)NETSTATUS_DHCP_ADDRS)
            info->router_count = (UWORD)NETSTATUS_DHCP_ADDRS;
        for (j = 0; j < (LONG)info->router_count; j++)
            info->router[j] = src->nsd_Router[j];

        info->dns_count = src->nsd_DnsCount;
        if (info->dns_count > (UWORD)NETSTATUS_DHCP_ADDRS)
            info->dns_count = (UWORD)NETSTATUS_DHCP_ADDRS;
        for (j = 0; j < (LONG)info->dns_count; j++)
            info->dns[j] = src->nsd_Dns[j];

        info->static_route_count = src->nsd_StaticRouteCount;
        if (info->static_route_count > (UWORD)NETSTATUS_DHCP_ADDRS)
            info->static_route_count = (UWORD)NETSTATUS_DHCP_ADDRS;
        for (j = 0; j < (LONG)info->static_route_count; j++)
            info->static_route[j] = src->nsd_StaticRoute[j];

        tool_copy_string(info->host_name, sizeof(info->host_name),
                         src->nsd_HostName);
        tool_copy_string(info->domain_name, sizeof(info->domain_name),
                         src->nsd_DomainName);

        out->count++;
    }

    tool_netstatus_close(base);

    return 0;
}

/* ---------------------------------------------------------------- routes -- */

LONG tool_routes(ToolRoutes *out)
{
    struct Library *base;
    LONG            n;
    LONG            i;

    if (out == NULL)
        return -1;

    out->count          = 0;
    out->truncated      = FALSE;
    out->static_routing = FALSE;

    base = nx_open();
    if (base == NULL)
        return -1;

    if (tool_netstatus_query(base, NETSTATUS_SYSTEM, &nx_answer,
                             sizeof(nx_answer.system),
                             sizeof(NetStatusSystem)) > 0)
    {
        out->static_routing =
            (nx_answer.system.e.nss_Flags & NETSTATUS_SYS_ROUTING)
                ? TRUE : FALSE;
    }

    n = tool_netstatus_query(base, NETSTATUS_ROUTES, &nx_answer,
                             sizeof(nx_answer.route), sizeof(NetStatusRoute));
    if (n < 0)
    {
        nx_query_failed(base);
        tool_netstatus_close(base);
        return -1;
    }

    if (nx_answer.route.hdr.nsh_Available > nx_answer.route.hdr.nsh_Count)
        out->truncated = TRUE;

    for (i = 0; i < n && i < (LONG)TOOL_MAX_ROUTE; i++)
    {
        const NetStatusRoute *src = &nx_answer.route.e[i];

        out->route[i].destination = src->nsr_Destination;
        out->route[i].netmask     = src->nsr_NetMask;
        out->route[i].gateway     = src->nsr_Gateway;
        out->route[i].flags       = src->nsr_Flags;
        out->route[i].nx_index    = src->nsr_Interface;
    }
    out->count = (UWORD)n;

    tool_netstatus_close(base);

    return 0;
}

/*
 * The routing table, printed the same way for netstat and ShowNetStatus. Both
 * used to derive it from the interface list and the default gateway, which
 * stopped being correct once NX_ENABLE_IP_STATIC_ROUTING landed: a hand-added
 * route was in the stack and in neither report. One function keeps them from
 * drifting again.
 *
 * Order is the order NETSTATUS_ROUTES hands them over, which is the order
 * _nx_ip_route_find() matches in: connected prefixes, then the static table
 * longest prefix first, then the default gateway.  Flags are the BSD letters,
 * with S for a route somebody added by hand.
 */
VOID tool_print_routes(const ToolRoutes *routes, const AmiConfig *cfg,
                       ToolAddrText fmt)
{
    UWORD i;

    if (fmt == NULL)
        fmt = ami_config_format_ip;

    tool_printf("Destination      Gateway          "
                "Netmask          Flags  Interface\n");

    for (i = 0; i < routes->count; i++)
    {
        const ToolRoute *r = &routes->route[i];
        char             dest[AMI_CFG_NAME_LEN];
        char             gw[AMI_CFG_NAME_LEN];
        char             mask[16];
        char             flags[6];
        UWORD            f = 0;

        if (r->destination == 0 && r->netmask == 0)
            tool_copy_string(dest, sizeof(dest), "default");
        else
            (*fmt)(r->destination, dest, sizeof(dest));

        if (r->gateway != 0)
            (*fmt)(r->gateway, gw, sizeof(gw));
        else
            tool_copy_string(gw, sizeof(gw), "*");

        ami_config_format_ip(r->netmask, mask, sizeof(mask));

        if (r->flags & NETSTATUS_RT_UP)
            flags[f++] = 'U';
        if (r->flags & NETSTATUS_RT_GATEWAY)
            flags[f++] = 'G';
        if (r->flags & NETSTATUS_RT_HOST)
            flags[f++] = 'H';
        if (r->flags & NETSTATUS_RT_STATIC)
            flags[f++] = 'S';
        flags[f] = '\0';

        tool_printf("%-16s %-16s %-16s %-6s %s\n",
                    (LONG)dest, (LONG)gw, (LONG)mask, (LONG)flags,
                    (LONG)tool_iface_name(cfg, r->nx_index));
    }

    /*
     * Loopback is real and is not in the table: NetX Duo's loopback interface
     * is not one of the nx_ip_interface[] slots NETSTATUS_ROUTES walks, and
     * _nx_ip_driver_packet_send() shortcuts 127/8 without consulting a route.
     * Printed anyway, so the report does not read as though there were no
     * loopback.
     */
    tool_printf("%-16s %-16s %-16s %-6s %s\n",
                (LONG)"127.0.0.0", (LONG)"*", (LONG)"255.0.0.0",
                (LONG)"U", (LONG)"lo0");

    if (routes->truncated)
        tool_printf("(more routes than this command can hold)\n");
}

const char *tool_iface_name(const AmiConfig *cfg, UWORD index)
{
    if (cfg != NULL && index < cfg->interface_count &&
        cfg->interfaces[index].name[0] != '\0')
    {
        return cfg->interfaces[index].name;
    }

    return "?";
}

const char *tool_tcp_state_name(UINT state)
{
    switch (state)
    {
        case NETSTATUS_TCP_CLOSED:          return "CLOSED";
        case NETSTATUS_TCP_LISTEN:          return "LISTEN";
        case NETSTATUS_TCP_SYN_SENT:        return "SYN_SENT";
        case NETSTATUS_TCP_SYN_RECEIVED:    return "SYN_RCVD";
        case NETSTATUS_TCP_ESTABLISHED:     return "ESTABLISHED";
        case NETSTATUS_TCP_CLOSE_WAIT:      return "CLOSE_WAIT";
        case NETSTATUS_TCP_FIN_WAIT_1:      return "FIN_WAIT_1";
        case NETSTATUS_TCP_FIN_WAIT_2:      return "FIN_WAIT_2";
        case NETSTATUS_TCP_CLOSING:         return "CLOSING";
        case NETSTATUS_TCP_TIMED_WAIT:      return "TIME_WAIT";
        case NETSTATUS_TCP_LAST_ACK:        return "LAST_ACK";
        default:                            return "UNKNOWN";
    }
}
