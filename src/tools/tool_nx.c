/*
 * AmiNetXDuo tools, talking to the live NetX Duo instance.
 *
 * Every function here is a NetStackQuery() call and a copy out of the answer.
 * No ThreadX is involved: the ThreadX a command can adopt into is never the
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

/* tool_health_mark(): the published mark, and the tick counters it points at. */
#include "aminetxduo/health.h"
#include "tx_amiga.h"

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
    struct { NetStatusHeader hdr; NetStatusRoute6    e[TOOL_MAX_ROUTE6]; } route6;
    struct { NetStatusHeader hdr; NetStatusNeighbour e[TOOL_MAX_ND]; }     nd;
    struct { NetStatusHeader hdr; NetStatusHealth    e; }      health;
    struct { NetStatusHeader hdr; NetStatusTcpStall  e[TOOL_MAX_SOCK]; } stall;
    struct { NetStatusHeader hdr; NetStatusDest6     e[TOOL_MAX_DEST6]; } dest6;
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

    tool_error("the network is up, but it did not report on itself");
    tool_explain_no_netstatus(base);
}

static struct Library *nx_open(VOID)
{
    return tool_netstatus_open(nx_quiet);
}

/* ------------------------------------------------------------ snapshots, */

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
    out->host_source    = (UWORD)AMI_HOSTNAME_NONE;

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
        info->mdns     = (src->nsi_Flags & NETSTATUS_IF_MDNS) ? TRUE : FALSE;
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
            info->stats.rx_err_runt      = src->nsi_RxErrRunt;
            info->stats.rx_err_verify    = src->nsi_RxErrVerify;
            info->stats.rx_err_length    = src->nsi_RxErrLength;
            info->stats.rx_err_io        = src->nsi_RxErrIo;
            info->stats.rx_copy_hook     = src->nsi_RxCopyHook;
            info->stats.rx_copy_summed   = src->nsi_RxCopySummed;
            info->stats.rx_direct_fill   = src->nsi_RxDirectFill;
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

        out->host_source = (UWORD)nx_answer.system.e.nss_HostSource;
    }

    /*
     * A library without IPv6 answers with no entries and one too old to know
     * the selector answers -1.  Both mean this machine has no IPv6 address to
     * report, which is not a failure and prints nothing.
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
        a6->origin   = src->nsn_Origin;

        tool_format_ip6(src->nsn_Address, a6->text, sizeof(a6->text));

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

        /*
         * Second call, because the stall numbers are their own table. It reuses
         * nx_answer, so it has to come after the copy above. A library that
         * predates the selector answers an error and every row keeps its zeros.
         *
         * Joined on the tuple rather than by index: the two walks are the same
         * order at the same instant, but these are two calls and a connection
         * can arrive or leave between them.
         */
        n = tool_netstatus_query(base, NETSTATUS_TCPSTALL, &nx_answer,
                                 sizeof(nx_answer.stall),
                                 sizeof(NetStatusTcpStall));
        for (i = 0; i < n && i < (LONG)TOOL_MAX_SOCK; i++)
        {
            const NetStatusTcpStall *src = &nx_answer.stall.e[i];
            UWORD                    j;

            for (j = 0; j < out->sock_count; j++)
            {
                ToolSockInfo *si = &out->sock[j];

                if (si->is_tcp &&
                    si->local_port == src->nst_LocalPort &&
                    si->peer_port == src->nst_PeerPort &&
                    si->peer_address == src->nst_PeerAddress)
                {
                    si->stalled_ms      = src->nst_Stalled;
                    si->retransmits     = src->nst_Retransmits;
                    si->rto_ms          = src->nst_Rto;
                    si->user_timeout_ms = src->nst_UserTimeout;
                    break;
                }
            }
        }
    }

    tool_netstatus_close(base);

    return 0;
}

/* NULL for a manually configured address: a static address is what the file
   said and needs no note. */
const char *tool_addr6_origin(ULONG origin)
{
    switch (origin)
    {
        case NETSTATUS_IP6_ORIGIN_SLAAC:  return "advertised";
        case NETSTATUS_IP6_ORIGIN_DHCPV6: return "dhcpv6";
        default:                          return NULL;
    }
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

BOOL tool_iface_has_address6(const ToolSnapshot *snap, UWORD nx_index)
{
    UWORD i;

    if (snap == NULL)
        return FALSE;

    for (i = 0; i < snap->addr6_count; i++)
    {
        const ToolAddr6Info *a6 = &snap->addr6[i];

        if (a6->nx_index != nx_index || a6->text[0] == '\0')
            continue;

        /* RFC 4862 5.4: TENTATIVE is still under duplicate address detection
           and is not yet an address anything may use. DEPRECATED is: it is an
           address on its way out that still carries existing traffic. */
        if (a6->state == NETSTATUS_IP6_TENTATIVE)
            continue;

        return TRUE;
    }

    return FALSE;
}

BOOL tool_iface_has_address(const ToolSnapshot *snap, const ToolIfInfo *live)
{
    if (live == NULL)
        return FALSE;

    if (live->address != 0UL)
        return TRUE;

    return tool_iface_has_address6(snap, live->nx_index);
}

const ToolIfInfo *tool_iface_live(const ToolSnapshot *snap,
                                  const AmiIfConfig *cfg)
{
    UWORD i;

    if (snap == NULL || cfg == NULL || cfg->name[0] == '\0')
        return NULL;

    /* The name first, which is the identity the user typed and the one the
       library reports for the slot. */
    for (i = 0; i < snap->iface_count && i < (UWORD)TOOL_MAX_IF; i++)
    {
        const ToolIfInfo *live = &snap->iface[i];

        if (!live->attached || live->nx_name[0] == '\0')
            continue;

        if (tool_stricmp(live->nx_name, cfg->name) == 0)
            return live;
    }

    /*
     * Then the card.  A library too old to answer with a name leaves nx_name
     * empty, and an interface is still identified by the device and unit it
     * has open -- which is the pair a description names.  A description with no
     * DEVICE line describes no card and matches nothing here.
     */
    if (cfg->device[0] == '\0')
        return NULL;

    for (i = 0; i < snap->iface_count && i < (UWORD)TOOL_MAX_IF; i++)
    {
        const ToolIfInfo *live = &snap->iface[i];

        if (!live->attached || live->nx_name[0] != '\0' ||
            live->nx_device[0] == '\0')
            continue;

        if (tool_stricmp(live->nx_device, cfg->device) == 0 &&
            live->nx_unit == cfg->unit)
            return live;
    }

    return NULL;
}

/* ---------------------------------------------------- protocol counters, */

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
    out->have_health   = FALSE;
    out->health_mark   = 0;
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

    /* A library older than this selector fails the call rather than answering
       zeroes, so have_health stays FALSE and nothing is printed. */
    if (tool_netstatus_query(base, NETSTATUS_HEALTH, &nx_answer,
                             sizeof(nx_answer.health),
                             sizeof(NetStatusHealth)) > 0)
    {
        const NetStatusHealth *h = &nx_answer.health.e;

        out->have_health             = TRUE;
        out->tick_ticks              = h->nsl_TickTicks;
        out->tick_clipped            = h->nsl_TickClipped;
        out->tick_lost               = h->nsl_TickLost;
        out->tick_service_us         = h->nsl_TickServiceUs;
        out->tick_uptime_ms          = h->nsl_TickUptimeMs;
        out->tick_worst_stall_ms     = h->nsl_TickWorstStallMs;
        out->tick_worst_service_us   = h->nsl_TickWorstServiceUs;
        out->tick_over_budget        = h->nsl_TickOverBudget;
        out->tick_deferred           = h->nsl_TickDeferred;
        out->tick_skew               = h->nsl_TickSkew;
        out->tick_skew_peak          = h->nsl_TickSkewPeak;
        out->baton_live              = h->nsl_BatonLive;
        out->baton_live_max          = h->nsl_BatonLiveMax;
        out->baton_full              = h->nsl_BatonFull;
        out->baton_transitions       = h->nsl_BatonTransitions;
        out->baton_state_max         = h->nsl_BatonStateMax;
        out->baton_moved             = h->nsl_BatonMoved;
        out->baton_state_shared      = h->nsl_BatonStateShared;

        out->alloc_live              = h->nsl_AllocLive;
        out->alloc_peak              = h->nsl_AllocPeak;
        out->alloc_refused           = h->nsl_AllocRefused;
        out->sockets                 = h->nsl_Sockets;
        out->sockets_peak            = h->nsl_SocketsPeak;
        out->opens                   = h->nsl_Opens;

        /* The health half is read after NETSTATUS_SYSTEM and wins: it is the
           record -h reads too, and it carries the low-water mark. */
        if (h->nsl_PoolTotal != 0)
        {
            out->have_pool              = TRUE;
            out->pool_total             = h->nsl_PoolTotal;
            out->pool_free              = h->nsl_PoolFree;
            out->pool_low               = h->nsl_PoolLow;
            out->pool_payload           = h->nsl_PoolPayload;
            out->pool_empty_requests    = h->nsl_PoolEmpty;
            out->pool_empty_suspensions = h->nsl_PoolWaited;
            out->pool_invalid_releases  = h->nsl_PoolBadRelease;
        }
    }

    tool_netstatus_close(base);

    return 0;
}

/* ------------------------------------------------------------- the mark, */

/*
 * The same numbers as NETSTATUS_HEALTH, off the published mark instead
 * (aminetxduo/health.h).  Nothing is opened and nothing is obtained, so this
 * answers on a machine where the library would not, which is the machine
 * this whole block of counters exists for.
 *
 * Forbid() rather than ObtainSemaphore(): the stack removes the mark under
 * Forbid() before the memory holding the counters can go, so a reader that
 * holds it across the find and the copy cannot be reading a freed one.
 * Obtaining it would mean blocking on the machine being diagnosed.
 */
BOOL tool_health_mark(ToolStats *out)
{
    const AmiHealthMark *mark;
    TX_AMIGA_TICK_STATS  tick;
    AmiBatonStats        baton;
    AmiMemStats          mem;
    BOOL                 ok = FALSE;

    if (out == NULL)
        return FALSE;

    out->have_health = FALSE;
    out->have_pool   = FALSE;
    out->health_mark = 0;

    Forbid();

    /* (STRPTR): NDK 3.9 declares FindSemaphore(STRPTR), 3.2 CONST_STRPTR. */
    mark = (const AmiHealthMark *)FindSemaphore((STRPTR)AMI_HEALTH_NAME);

    if (mark != NULL &&
        mark->hm_Magic   == AMI_HEALTH_MAGIC &&
        mark->hm_Version == (UWORD)AMI_HEALTH_VERSION &&
        mark->hm_Size    == (UWORD)sizeof(AmiHealthMark) &&
        mark->hm_Tick    != NULL &&
        mark->hm_Baton   != NULL &&
        mark->hm_Mem     != NULL)
    {
        tick  = *(const TX_AMIGA_TICK_STATS *)mark->hm_Tick;
        baton = *(const AmiBatonStats *)mark->hm_Baton;
        mem   = *(const AmiMemStats *)mark->hm_Mem;
        out->health_mark = (ULONG)mark;
        ok = TRUE;
    }

    Permit();

    if (!ok)
        return FALSE;

    out->have_health           = TRUE;
    out->tick_ticks            = tick.tx_amiga_tick_delivered;
    out->tick_clipped          = tick.tx_amiga_tick_clipped;
    out->tick_lost             = tick.tx_amiga_tick_lost;
    out->tick_service_us       = tick.tx_amiga_tick_service_us;
    out->tick_uptime_ms        = tx_amiga_uptime_ms(&tick);
    out->tick_worst_stall_ms   = tick.tx_amiga_tick_worst_stall_ms;
    out->tick_worst_service_us = tick.tx_amiga_tick_worst_service_us;
    out->tick_over_budget      = tick.tx_amiga_tick_over_budget;
    out->tick_deferred         = tick.tx_amiga_tick_deferred;
    out->tick_skew             = tick.tx_amiga_tick_skew;
    out->tick_skew_peak        = tick.tx_amiga_tick_skew_peak;
    out->baton_live            = baton.bs_Live;
    out->baton_live_max        = baton.bs_LiveMax;
    out->baton_full            = baton.bs_Full;
    out->baton_transitions     = baton.bs_Transitions;
    out->baton_state_max       = baton.bs_StateMax;
    out->baton_moved           = baton.bs_BatonMoved;
    out->baton_state_shared    = baton.bs_StateShared;

    out->alloc_live            = mem.ms_Live;
    out->alloc_peak            = mem.ms_LiveMax;
    out->alloc_refused         = mem.ms_Refused;
    out->sockets               = mem.ms_Sockets;
    out->sockets_peak          = mem.ms_SocketsMax;
    out->opens                 = mem.ms_Opens;

    if (mem.ms_PoolTotal != 0)
    {
        out->have_pool              = TRUE;
        out->pool_total             = mem.ms_PoolTotal;
        out->pool_free              = mem.ms_PoolFree;
        out->pool_low               = mem.ms_PoolLow;
        out->pool_payload           = mem.ms_PoolPayload;
        out->pool_empty_requests    = mem.ms_PoolEmpty;
        out->pool_empty_suspensions = mem.ms_PoolWaited;
        out->pool_invalid_releases  = mem.ms_PoolBadRelease;
    }

    return TRUE;
}

/* ------------------------------------------------------------------ DHCP, */

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

/* ---------------------------------------------------------------- routes, */

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

/* ----------------------------------------------------------- IPv6 routes, */

LONG tool_routes6(ToolRoutes6 *out)
{
    struct Library *base;
    LONG            n;
    LONG            i;

    if (out == NULL)
        return -1;

    out->count     = 0;
    out->truncated = FALSE;

    base = nx_open();
    if (base == NULL)
        return -1;

    /*
     * A library without IPv6 answers with no entries and one too old to know
     * the selector answers -1. Both mean this machine has no IPv6 route to
     * report, which prints nothing and is not a failure.
     */
    n = tool_netstatus_query(base, NETSTATUS_ROUTES6, &nx_answer,
                             sizeof(nx_answer.route6), sizeof(NetStatusRoute6));
    if (n > 0)
    {
        if (nx_answer.route6.hdr.nsh_Available > nx_answer.route6.hdr.nsh_Count)
            out->truncated = TRUE;

        for (i = 0; i < n && i < (LONG)TOOL_MAX_ROUTE6; i++)
        {
            const NetStatusRoute6 *src = &nx_answer.route6.e[i];
            ToolRoute6            *r   = &out->route[i];

            tool_format_ip6(src->nsr6_Destination, r->dest,
                            sizeof(r->dest));

            if (src->nsr6_Flags & NETSTATUS_RT6_GATEWAY)
                tool_format_ip6(src->nsr6_NextHop, r->next_hop,
                                sizeof(r->next_hop));
            else
                r->next_hop[0] = '\0';

            r->dest_words[0] = src->nsr6_Destination[0];
            r->dest_words[1] = src->nsr6_Destination[1];
            r->dest_words[2] = src->nsr6_Destination[2];
            r->dest_words[3] = src->nsr6_Destination[3];

            r->prefix   = src->nsr6_PrefixLength;
            r->lifetime = src->nsr6_Lifetime;
            r->flags    = src->nsr6_Flags;
            r->nx_index = src->nsr6_Interface;

            out->count = (UWORD)(i + 1);
        }
    }

    tool_netstatus_close(base);

    return 0;
}

/*
 * The IPv6 table, next to the IPv4 one and not merged with it: there is no
 * netmask column to fill, a default router has a lifetime where a gateway has
 * none, and "::/0" in a Netmask column would misstate how the stack decides.
 */
VOID tool_print_routes6(const ToolRoutes6 *routes, const AmiConfig *cfg)
{
    UWORD i;

    if (routes == NULL || routes->count == 0)
        return;

    tool_printf("\nDestination                              "
                "Next hop                       Flags  Interface\n");

    for (i = 0; i < routes->count; i++)
    {
        const ToolRoute6 *r = &routes->route[i];
        char              dest[56];
        char              flags[6];
        UWORD             f = 0;
        ULONG             pos;

        /* A default router is ::/0 and is written that way, so the table says
           where the packets with nowhere better to go are sent. */
        if (r->flags & NETSTATUS_RT6_GATEWAY)
        {
            tool_copy_string(dest, sizeof(dest), "::/0");
        }
        else
        {
            tool_copy_string(dest, sizeof(dest), r->dest);

            pos = 0;
            while (dest[pos] != '\0')
                pos++;

            if (pos + 5 < sizeof(dest))
            {
                dest[pos++] = '/';
                if (r->prefix >= 100UL)
                    dest[pos++] = (char)('0' + (r->prefix / 100UL));
                if (r->prefix >= 10UL)
                    dest[pos++] = (char)('0' + ((r->prefix / 10UL) % 10UL));
                dest[pos++] = (char)('0' + (r->prefix % 10UL));
                dest[pos]   = '\0';
            }
        }

        if (r->flags & NETSTATUS_RT6_UP)
            flags[f++] = 'U';
        if (r->flags & NETSTATUS_RT6_GATEWAY)
            flags[f++] = 'G';
        if (r->flags & NETSTATUS_RT6_HOST)
            flags[f++] = 'H';
        if (r->flags & NETSTATUS_RT6_STATIC)
            flags[f++] = 'S';
        flags[f] = '\0';

        tool_printf("%-40s %-30s %-6s %s\n",
                    (LONG)dest,
                    (LONG)((r->next_hop[0] != '\0') ? r->next_hop : "*"),
                    (LONG)flags, (LONG)tool_iface_name(cfg, r->nx_index));
    }

    if (routes->truncated)
        tool_printf("(more IPv6 routes than this command can hold)\n");
}

/* ----------------------------------------------------- destination cache, */

LONG tool_dest6(ToolDest6Table *out)
{
    struct Library *base;
    LONG            n;
    LONG            i;
    LONG            j;

    if (out == NULL)
        return -1;

    out->count     = 0;
    out->capacity  = 0;
    out->truncated = FALSE;

    base = nx_open();
    if (base == NULL)
        return -1;

    /*
     * A library without IPv6 answers with no entries. One that predates the
     * selector answers -1. Both print nothing and neither is a failure, the
     * way tool_routes6() treats the same two cases.
     */
    n = tool_netstatus_query(base, NETSTATUS_DEST6, &nx_answer,
                             sizeof(nx_answer.dest6), sizeof(NetStatusDest6));
    if (n > 0)
    {
        if (nx_answer.dest6.hdr.nsh_Available > nx_answer.dest6.hdr.nsh_Count)
            out->truncated = TRUE;

        for (i = 0; i < n && i < (LONG)TOOL_MAX_DEST6; i++)
        {
            const NetStatusDest6 *src = &nx_answer.dest6.e[i];
            ToolDest6            *e   = &out->entry[out->count];

            tool_format_ip6(src->nsd6_Destination, e->dest, sizeof(e->dest));
            tool_format_ip6(src->nsd6_NextHop, e->next_hop,
                            sizeof(e->next_hop));

            e->age      = src->nsd6_Age;
            e->path_mtu = src->nsd6_PathMtu;
            e->nd_state = src->nsd6_NdState;
            e->flags    = src->nsd6_Flags;
            e->nx_index = src->nsd6_Interface;

            out->capacity = (UWORD)src->nsd6_Capacity;
            out->count    = (UWORD)(out->count + 1);
        }

        /*
         * Sorted by age here rather than in the library: the table is a flat
         * array in slot order, and slot order says nothing. Ascending age is
         * most recently used first, which puts the entry about to be evicted
         * last, where a reader looking at a full table wants it.
         */
        for (i = 1; i < (LONG)out->count; i++)
        {
            ToolDest6 held = out->entry[i];

            for (j = i - 1; j >= 0 && out->entry[j].age > held.age; j--)
                out->entry[j + 1] = out->entry[j];

            out->entry[j + 1] = held;
        }
    }

    tool_netstatus_close(base);

    return 0;
}

/*
 * The cache, printed under the routes it resolves to. Two things a reader
 * cannot get anywhere else: how many of the slots are taken, and how stale
 * each entry is, because a full cache evicts on every new destination and
 * used to do it without saying so.
 */
VOID tool_print_dest6(const ToolDest6Table *table, const AmiConfig *cfg)
{
    UWORD i;

    if (table == NULL || table->count == 0)
        return;

    tool_printf("\nIPv6 destination cache, %lu of %lu slots%s\n",
                (LONG)table->count, (LONG)table->capacity,
                (LONG)((table->capacity != 0 &&
                        table->count >= table->capacity)
                           ? " -- FULL, a new destination evicts the last row"
                           : ""));

    tool_printf("Destination                              "
                "Next hop                       Age    MTU    "
                "Neighbour   Interface\n");

    for (i = 0; i < table->count; i++)
    {
        const ToolDest6 *e = &table->entry[i];

        tool_printf("%-40s %-30s %-6lu %-6lu %-11s %s\n",
                    (LONG)e->dest,
                    (LONG)((e->flags & NETSTATUS_DEST6_ONLINK)
                               ? "(on link)" : e->next_hop),
                    (LONG)e->age, (LONG)e->path_mtu,
                    (LONG)((e->nd_state != 0)
                               ? tool_nd_state_name(e->nd_state)
                               : "(none)"),
                    (LONG)tool_iface_name(cfg, e->nx_index));
    }

    if (table->truncated)
        tool_printf("(more destinations than this command can hold)\n");
}

/* ------------------------------------------------------------ neighbours, */

LONG tool_neighbours(ToolNeighbours *out)
{
    struct Library *base;
    LONG            n;
    LONG            i;
    LONG            j;

    if (out == NULL)
        return -1;

    out->count     = 0;
    out->truncated = FALSE;

    base = nx_open();
    if (base == NULL)
        return -1;

    n = tool_netstatus_query(base, NETSTATUS_NEIGHBOURS, &nx_answer,
                             sizeof(nx_answer.nd), sizeof(NetStatusNeighbour));
    if (n > 0)
    {
        if (nx_answer.nd.hdr.nsh_Available > nx_answer.nd.hdr.nsh_Count)
            out->truncated = TRUE;

        for (i = 0; i < n && i < (LONG)TOOL_MAX_ND; i++)
        {
            const NetStatusNeighbour *src = &nx_answer.nd.e[i];
            ToolNeighbour            *e   = &out->entry[i];

            tool_format_ip6(src->nsn6_Address, e->text, sizeof(e->text));

            e->addr[0] = src->nsn6_Address[0];
            e->addr[1] = src->nsn6_Address[1];
            e->addr[2] = src->nsn6_Address[2];
            e->addr[3] = src->nsn6_Address[3];

            for (j = 0; j < (LONG)AMI_ETH_ADDR_SIZE; j++)
                e->mac[j] = src->nsn6_HwAddress[j];

            e->state         = src->nsn6_State;
            e->flags         = src->nsn6_Flags;
            e->nx_index      = src->nsn6_Interface;
            e->solicitations = src->nsn6_Solicitations;
            e->queued        = src->nsn6_Queued;

            out->count = (UWORD)(i + 1);
        }
    }

    tool_netstatus_close(base);

    return 0;
}

const char *tool_nd_state_name(UWORD state)
{
    switch (state)
    {
        case NETSTATUS_ND_INCOMPLETE:   return "INCOMPLETE";
        case NETSTATUS_ND_REACHABLE:    return "REACHABLE";
        case NETSTATUS_ND_STALE:        return "STALE";
        case NETSTATUS_ND_DELAY:        return "DELAY";
        case NETSTATUS_ND_PROBE:        return "PROBE";
        case NETSTATUS_ND_CREATED:      return "CREATED";
        default:                        return "UNKNOWN";
    }
}

/*
 * The state names are RFC 4861's and mean nothing to somebody who has not
 * read it, so each carries the one thing a reader wants: whether the address
 * is usable, and whether the stack is doing anything about it.
 */
const char *tool_nd_state_note(UWORD state)
{
    switch (state)
    {
        case NETSTATUS_ND_INCOMPLETE:
            return "asked, nothing back yet";
        case NETSTATUS_ND_STALE:
            return "answered once, not checked since";
        case NETSTATUS_ND_DELAY:
            return "sent something, about to check again";
        case NETSTATUS_ND_PROBE:
            return "being checked now";
        case NETSTATUS_ND_CREATED:
            return "known of, never asked about";
        default:
            return NULL;            /* REACHABLE needs no comment */
    }
}

/*
 * The live names, by NX slot, cached for the whole run of a command.
 *
 * The library knows what each slot is called, because it takes the name from
 * the description the slot really holds.  This is the only honest source:
 * subscripting the DESCRIPTIONS by an NX index assumes the two numberings
 * agree, and they do not -- a drawer may describe more interfaces than there
 * are slots, and a description whose device will not open takes no slot at all
 * and moves everything behind it down one.
 *
 * Filled once, on first use, so a command that never asks costs nothing and
 * one that asks per route costs one query.  A slot with no live name leaves
 * its entry empty and tool_iface_name() falls back to the descriptions, which
 * is the answer for a library too old to report names.
 */
static char nx_slot_name[TOOL_MAX_IF][NETSTATUS_NAME_LEN];
static BOOL nx_slot_named;

static VOID nx_learn_slot_names(VOID)
{
    /* Its own buffer, not the shared one: this runs from inside printing loops
       that are walking an answer already in nx_answer. */
    static struct { NetStatusHeader hdr; NetStatusInterface e[TOOL_MAX_IF]; }
                    names;
    struct Library *base;
    LONG            n;
    LONG            i;

    nx_slot_named = TRUE;

    for (i = 0; i < (LONG)TOOL_MAX_IF; i++)
        nx_slot_name[i][0] = '\0';

    /* Quiet whatever the command asked for: this is a lookup behind a name in
       a table, and a stack that is not running is not news here.  The command
       has said so already, in its own words. */
    base = tool_netstatus_open(TRUE);
    if (base == NULL)
        return;

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &names,
                             sizeof(names), sizeof(NetStatusInterface));

    for (i = 0; i < n && i < (LONG)TOOL_MAX_IF; i++)
    {
        const NetStatusInterface *src = &names.e[i];

        if (!(src->nsi_Flags & NETSTATUS_IF_NAMED) ||
            src->nsi_Index >= (UWORD)TOOL_MAX_IF)
            continue;

        tool_copy_string(nx_slot_name[src->nsi_Index],
                         sizeof(nx_slot_name[0]), src->nsi_Name);
    }

    tool_netstatus_close(base);
}

const char *tool_iface_name(const AmiConfig *cfg, UWORD index)
{
    /*
     * THE RUNNING STACK'S OWN NAME FOR THE SLOT, because `index` is a NetX Duo
     * interface index and the description list is not indexed by one.  On a
     * machine with three interface files, or with one whose card did not open,
     * the two numberings stop agreeing and the name printed beside a route was
     * some other interface's.
     */
    if (index < (UWORD)TOOL_MAX_IF)
    {
        if (!nx_slot_named)
            nx_learn_slot_names();

        if (nx_slot_name[index][0] != '\0')
            return nx_slot_name[index];
    }

    /* No live answer: a stack that is not running, or a library too old to
       name its slots.  The descriptions are then all there is, and on such a
       machine nothing has moved. */
    if (cfg != NULL && index < cfg->interface_count &&
        index < (UWORD)AMI_CFG_MAX_ATTACHED &&
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
