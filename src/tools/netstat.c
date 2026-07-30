/*
 * netstat -- interfaces, routes and connections.
 *
 *     netstat INTERFACES=-i/S,ROUTES=-r/S,ALL=-a/S,STATS=-s/S
 *
 * Every switch carries its Unix spelling as a ReadArgs alias, so "netstat -r"
 * and "netstat ROUTES" do the same thing and "netstat ?" shows both. With no
 * switches it prints everything.
 *
 * -s is per-protocol statistics followed by the SANA-II per-interface counters;
 * no other switch shows the driver's own numbers.
 *
 * This command covers the same ground as ShowNetStatus: that one has named
 * categories and a diagnosis, this one has switches and columns. Neither reads
 * the stack directly -- both take the same two snapshots from tool_nx.c,
 * ToolSnapshot and ToolStats, so they cannot disagree.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"

const char *const tool_name = "netstat";

static const char version_tag[] __attribute__((used)) =
    "$VER: netstat 1.1 (26.7.2026)";

#define TEMPLATE    "INTERFACES=-i/S,ROUTES=-r/S,ALL=-a/S,STATS=-s/S"

enum
{
    ARG_INTERFACES = 0,
    ARG_ROUTES,
    ARG_ALL,
    ARG_STATS,
    ARG_COUNT
};

/* Static: an AmiConfig is far too big for a Shell command's 4 KB stack. */
static AmiConfig netstat_config;

/*
 * The IPv6 addresses of one interface, under its line.  Nothing is printed on
 * a machine that has none, so an IPv4-only stack looks exactly as it did.
 */
static VOID show_addresses6(const ToolSnapshot *snap, UWORD nx_index)
{
    UWORD i;

    for (i = 0; i < snap->addr6_count; i++)
    {
        const ToolAddr6Info *a6 = &snap->addr6[i];
        const char          *note;

        if (a6->nx_index != nx_index || a6->text[0] == '\0')
            continue;

        note = tool_addr6_state(a6->state);

        if (note != NULL)
            tool_printf("        inet6 %s/%lu (%s)\n", (LONG)a6->text,
                        a6->prefix, (LONG)note);
        else
            tool_printf("        inet6 %s/%lu\n", (LONG)a6->text, a6->prefix);
    }
}

static VOID show_interfaces(const AmiConfig *cfg, const ToolSnapshot *snap)
{
    char  addr[16];
    char  mac[24];
    UWORD i;

    tool_printf("Network interfaces\n");
    tool_printf("Name    Mtu   Address          Link   "
                "Ipkts      Ierrs  Opkts      Oerrs\n");

    for (i = 0; i < snap->iface_count; i++)
    {
        const ToolIfInfo *info = &snap->iface[i];

        if (!info->attached)
            continue;

        ami_config_format_ip(info->address, addr, sizeof(addr));

        tool_printf("%-7s %-5lu %-16s %-6s ",
                    (LONG)tool_iface_name(cfg, i), info->mtu, (LONG)addr,
                    (LONG)(info->link_up ? "up" : "down"));

        if (info->have_sana2)
        {
            tool_printf("%-10lu %-6lu %-10lu %-6lu\n",
                        info->stats.packets_received, info->stats.rx_errors,
                        info->stats.packets_sent, info->stats.tx_errors);
        }
        else
        {
            tool_printf("%-10s %-6s %-10s %-6s\n",
                        (LONG)"-", (LONG)"-", (LONG)"-", (LONG)"-");
        }

        tool_format_mac(info->mac, mac, sizeof(mac));
        tool_printf("        hardware %s\n", (LONG)mac);

        show_addresses6(snap, info->nx_index);
    }
}

/*
 * The per-protocol half of -s: the same ToolStats ShowNetStatus prints under
 * IP, ICMP, TCP and UDP, laid out the way netstat lays things out.
 */
static VOID show_protocol_stats(const ToolStats *st)
{
    tool_printf("\nip:\n");
    if (st->have_ip)
    {
        tool_printf("\t%lu packets sent (%lu bytes)\n",
                    st->ip_packets_sent, st->ip_bytes_sent);
        tool_printf("\t%lu packets received (%lu bytes)\n",
                    st->ip_packets_received, st->ip_bytes_received);
        tool_printf("\t%lu bad packets, %lu checksum errors\n",
                    st->ip_invalid, st->ip_checksum_errors);
        tool_printf("\t%lu dropped on receipt, %lu dropped on send\n",
                    st->ip_receive_dropped, st->ip_send_dropped);
        tool_printf("\t%lu fragments sent, %lu received\n",
                    st->ip_fragments_sent, st->ip_fragments_received);
    }
    else
    {
        tool_printf("\tno counters\n");
    }

    tool_printf("\nicmp:\n");
    if (st->have_icmp)
    {
        tool_printf("\t%lu echo requests sent, %lu replies received\n",
                    st->icmp_pings_sent, st->icmp_responses);
        tool_printf("\t%lu timed out, %lu checksum errors, %lu not handled\n",
                    st->icmp_ping_timeouts, st->icmp_checksum_errors,
                    st->icmp_unhandled);
    }
    else
    {
        tool_printf("\tnot enabled\n");
    }

    tool_printf("\ntcp:\n");
    if (st->have_tcp)
    {
        tool_printf("\t%lu packets sent (%lu bytes)\n",
                    st->tcp_packets_sent, st->tcp_bytes_sent);
        tool_printf("\t%lu packets received (%lu bytes)\n",
                    st->tcp_packets_received, st->tcp_bytes_received);
        tool_printf("\t%lu retransmitted, %lu dropped on receipt\n",
                    st->tcp_retransmits, st->tcp_receive_dropped);
        tool_printf("\t%lu bad packets, %lu checksum errors\n",
                    st->tcp_invalid, st->tcp_checksum_errors);
        tool_printf("\t%lu connections made, %lu closed, %lu lost\n",
                    st->tcp_connections, st->tcp_disconnections,
                    st->tcp_connections_dropped);
    }
    else
    {
        tool_printf("\tnot enabled\n");
    }

    tool_printf("\nudp:\n");
    if (st->have_udp)
    {
        tool_printf("\t%lu datagrams sent (%lu bytes)\n",
                    st->udp_packets_sent, st->udp_bytes_sent);
        tool_printf("\t%lu datagrams received (%lu bytes)\n",
                    st->udp_packets_received, st->udp_bytes_received);
        tool_printf("\t%lu bad datagrams, %lu checksum errors, %lu dropped\n",
                    st->udp_invalid, st->udp_checksum_errors,
                    st->udp_receive_dropped);
    }
    else
    {
        tool_printf("\tnot enabled\n");
    }

    tool_printf("\npacket pool:\n");
    if (st->have_pool)
    {
        tool_printf("\t%lu packets of %lu bytes, %lu free\n",
                    st->pool_total, st->pool_payload, st->pool_free);
        tool_printf("\t%lu requests found it empty, %lu waited\n",
                    st->pool_empty_requests, st->pool_empty_suspensions);
    }
    else
    {
        tool_printf("\tno packet pool\n");
    }

    /* A worst stall in the hundreds of milliseconds beside a service cost in
       the hundreds of microseconds says the tick task was not dispatched. */
    if (st->have_health)
    {
        tool_printf("\nscheduler:\n");
        tool_printf("\t%lu ticks in %lu ms, %lu clipped, %lu lost\n",
                    st->tick_ticks, st->tick_uptime_ms,
                    st->tick_clipped, st->tick_lost);
        tool_printf("\tworst stall %lu ms, service %lu us at the time\n",
                    st->tick_worst_stall_ms, st->tick_worst_service_us);
        tool_printf("\tbaton: %lu transitions, %lu at once at the peak\n",
                    st->baton_transitions, st->baton_live_max);
        tool_printf("\tbaton: %lu table full, %lu moved, state max %lu\n",
                    st->baton_full, st->baton_moved, st->baton_state_max);
    }
}

/*
 * The driver half: the SANA-II counters, which say whether a card is seeing
 * traffic at all.
 */
static VOID show_stats(const AmiConfig *cfg, const ToolSnapshot *snap)
{
    UWORD i;
    UWORD shown = 0;

    tool_printf("\nInterface statistics\n");

    for (i = 0; i < snap->iface_count; i++)
    {
        const ToolIfInfo   *info = &snap->iface[i];
        const AmiSana2Stats *st  = &info->stats;

        if (!info->attached)
            continue;

        if (!info->have_sana2)
        {
            tool_printf("\n%s: no driver attached, so it has no counters\n",
                        (LONG)tool_iface_name(cfg, i));
            shown++;
            continue;
        }

        tool_printf("\n%s (%s)\n", (LONG)tool_iface_name(cfg, i),
                    (LONG)(info->sana2_online ? "online" : "offline"));
        tool_printf("  packets received  %10lu    packets sent      %10lu\n",
                    st->packets_received, st->packets_sent);
        tool_printf("  receive errors    %10lu    transmit errors   %10lu\n",
                    st->rx_errors, st->tx_errors);
        tool_printf("  bad data          %10lu    overruns          %10lu\n",
                    st->bad_data, st->overruns);
        tool_printf("  unknown types     %10lu    reconfigurations  %10lu\n",
                    st->unknown_types, st->reconfigurations);
        tool_printf("  buffer failures   %10lu\n", st->alloc_failures);

        if (st->packets_received == 0 && st->packets_sent == 0)
        {
            tool_printf("  Nothing has gone in or out of this interface at all.\n");
            tool_printf("  If it should have, check the cable and that the\n");
            tool_printf("  interface is online (ShowNetStatus says).\n");
        }

        shown++;
    }

    if (shown == 0)
        tool_printf("  no interfaces are attached\n");
}

/*
 * The routing table the stack has, not one derived from the interface list.
 * NETSTATUS_ROUTES answers with the connected prefixes, the static table and
 * the default gateway together, in match order, so a route added with
 * AddNetRoute appears here.
 */
static VOID show_routes(const AmiConfig *cfg)
{
    static ToolRoutes  routes;
    static ToolRoutes6 routes6;

    tool_printf("\nRouting table\n");

    if (tool_routes(&routes) != 0)
        return;

    tool_print_routes(&routes, cfg, NULL);

    /* Prints nothing on a machine with no IPv6 routes. */
    if (tool_routes6(&routes6) == 0)
        tool_print_routes6(&routes6, cfg);
}

static VOID show_connections(const ToolSnapshot *snap)
{
    char  peer[16];
    UWORD i;

    tool_printf("\nActive connections\n");
    tool_printf("Proto  Local  Foreign               State\n");

    if (snap->sock_count == 0)
    {
        tool_printf("(none)\n");
        return;
    }

    for (i = 0; i < snap->sock_count; i++)
    {
        const ToolSockInfo *s = &snap->sock[i];

        if (s->is_tcp)
        {
            if (s->peer_address != 0)
            {
                char joined[24];
                ULONG n;
                ULONG o = 0;

                ami_config_format_ip(s->peer_address, peer, sizeof(peer));
                for (n = 0; peer[n] != '\0' && o < sizeof(joined) - 8; n++)
                    joined[o++] = peer[n];
                joined[o++] = ':';
                joined[o]   = '\0';

                tool_printf("tcp    %-6lu %s%-6lu        %s\n",
                            (LONG)s->local_port, (LONG)joined,
                            (LONG)s->peer_port,
                            (LONG)tool_tcp_state_name(s->state));
            }
            else
            {
                tool_printf("tcp    %-6lu %-21s %s\n",
                            (LONG)s->local_port, (LONG)"*",
                            (LONG)tool_tcp_state_name(s->state));
            }
        }
        else
        {
            tool_printf("udp    %-6lu %-21s %lu queued\n",
                        (LONG)s->local_port, (LONG)"*", s->queued);
        }
    }

    if (snap->sock_truncated)
        tool_printf("(list truncated at %ld sockets)\n", (LONG)TOOL_MAX_SOCK);
}

int main(int argc, char **argv)
{
    LONG             args[ARG_COUNT];
    struct RDArgs   *rda;
    const AmiConfig *cfg;
    static ToolSnapshot snap;
    static ToolStats    stats;
    BOOL             want_if;
    BOOL             want_routes;
    BOOL             want_conn;
    BOOL             want_stats;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_INTERFACES] = 0;
    args[ARG_ROUTES]     = 0;
    args[ARG_ALL]        = 0;
    args[ARG_STATS]      = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    want_if     = (args[ARG_INTERFACES] != 0) ? TRUE : FALSE;
    want_routes = (args[ARG_ROUTES] != 0) ? TRUE : FALSE;
    want_conn   = (args[ARG_ALL] != 0) ? TRUE : FALSE;
    want_stats  = (args[ARG_STATS] != 0) ? TRUE : FALSE;

    if (!want_if && !want_routes && !want_conn && !want_stats)
        want_if = want_routes = want_conn = TRUE;

    /*
     * Straight to the running library. Do not add a tool_require_stack() call:
     * it asks netstack_get(), which in a command is src/tools/netstack_weak.c's
     * stub and is always NULL, which is what made this command inert in v0.2.0.
     * tool_snapshot() opens bsdsocket.library, where the stack really is, and
     * explains itself when it cannot.
     */
    if (tool_snapshot(&snap, want_conn) != 0)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (want_stats && tool_stats(&stats) != 0)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    /*
     * The interface names come off the disk rather than out of the stack:
     * netstack_config() is another weak stub, and DEVS:NetInterfaces is the
     * same file the running stack read. The live snapshot carries the name too
     * (ToolIfInfo.nx_name) and the two agree.
     */
    tool_config_watch();
    cfg = (ami_config_load(&netstat_config) == AMI_CFG_OK) ? &netstat_config
                                                           : NULL;
    tool_config_unwatch();

    if (want_if)
        show_interfaces(cfg, &snap);
    if (want_stats)
    {
        show_protocol_stats(&stats);
        show_stats(cfg, &snap);
    }
    if (want_routes)
        show_routes(cfg);
    if (want_conn)
        show_connections(&snap);

    FreeArgs(rda);
    return RETURN_OK;
}
