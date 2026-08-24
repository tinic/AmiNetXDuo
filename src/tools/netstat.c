/*
 * netstat, interfaces, routes and connections.
 *
 *     netstat INTERFACES=-i/S,ROUTES=-r/S,ALL=-a/S,STATS=-s/S,HEALTH=-h/S
 *
 * Every switch carries its Unix spelling as a ReadArgs alias, so "netstat -r"
 * and "netstat ROUTES" do the same thing and "netstat ?" shows both. With no
 * switches it prints everything except -h.
 *
 * -s is per-protocol statistics followed by the SANA-II per-interface counters.
 * No other switch shows the driver's own numbers.
 *
 * -h is the memory and scheduler blocks on their own, and takes a different
 * route to them: the published mark (aminetxduo/health.h) rather than a library
 * call, so it opens nothing, allocates nothing and cannot block. It is
 * therefore usable on a machine halfway into the fault it describes, and it is
 * the one switch that works without a stack answering for itself. It is the
 * command to ask for in a fault report, for a freeze and for a suspected leak
 * alike, docs/FREEZE-DIAGNOSTIC.md.
 *
 * This command covers the same ground as ShowNetStatus: that one has named
 * categories and a diagnosis, this one has switches and columns. Neither reads
 * the stack directly, both take the same two snapshots from tool_nx.c,
 * ToolSnapshot and ToolStats, so they cannot disagree.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"

#include "aminetxduo/version.h"

const char *const tool_name = "netstat";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("netstat");

#define TEMPLATE    "INTERFACES=-i/S,ROUTES=-r/S,ALL=-a/S,STATS=-s/S,HEALTH=-h/S"

enum
{
    ARG_INTERFACES = 0,
    ARG_ROUTES,
    ARG_ALL,
    ARG_STATS,
    ARG_HEALTH,
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
 * What the stack owns, and the most it has ever owned.
 *
 * This is the half of -h that answers a suspected leak. Each of the three
 * counts is a different fault with a different fix, so they are not one
 * number: allocations are the general heap, sockets are the structure
 * docs/RESEARCH.md 37.5 lost 776 of, and packets are a fixed pool that starves
 * rather than grows. The peak beside each is what makes a single reading
 * usable, because a count on its own cannot say whether it is climbing.
 *
 * AvailMem is read here rather than carried in ToolStats so both routes into
 * this function report the same machine at the same moment. It is the number a
 * user is already watching, and it is only interpretable next to ours.
 */
static VOID show_memory(const ToolStats *st)
{
    tool_printf("\nmemory:\n");
    tool_printf("\t%lu allocations outstanding, %lu at the peak, %lu refused\n",
                st->alloc_live, st->alloc_peak, st->alloc_refused);
    tool_printf("\t%lu sockets open, %lu at the peak, %lu %s the library "
                "open\n", st->sockets, st->sockets_peak, st->opens,
                (st->opens == 1UL) ? "program has" : "programs have");

    if (st->have_pool)
    {
        tool_printf("\t%lu of %lu packets free, %lu fewest ever, %lu bytes "
                    "each\n", st->pool_free, st->pool_total, st->pool_low,
                    st->pool_payload);
        tool_printf("\t%lu found the pool empty, %lu waited, %lu released "
                    "twice\n", st->pool_empty_requests,
                    st->pool_empty_suspensions, st->pool_invalid_releases);
    }
    else
    {
        tool_printf("\tno packet pool\n");
    }

    tool_printf("\t%lu bytes of system memory free, %lu in the largest block\n",
                AvailMem(MEMF_PUBLIC), AvailMem(MEMF_PUBLIC | MEMF_LARGEST));
}

/*
 * A worst stall in the hundreds of milliseconds beside a service cost in the
 * hundreds of microseconds says the tick task was not dispatched, rather than
 * that it was slow. Anything but zero on the last line is a defect.
 *
 * The stall is the longest gap between two wakeups, whether or not it clipped,
 * so it is non-zero on a healthy machine and is what the skew peak above it
 * has to be read against: a peak worth no more than that stall at 20 ms a tick
 * is one late wakeup, and a larger one accumulated across several.
 *
 * The skew line is the timers, not the clock: tx_time_get() comes from the
 * E-Clock, so a tick the wheel never got made timers late and did not lose any
 * time. The peak counts lateness that was subsequently made good, so it moves
 * on a machine where nothing was ever clipped. Its floor is one wakeup's worth
 * of ticks, 2 on the VBlank source, because a wakeup is sampled owing every
 * period since the last one. Deferred ticks reach the wheel late. Lost ones
 * never reach it.
 *
 * Memory first, then the scheduler. Both routes print through this one
 * function, -h off the published mark and -s -h through the library, so the
 * two cannot disagree about what they found.
 */
static VOID show_health(const ToolStats *st)
{
    if (!st->have_health)
        return;

    show_memory(st);

    tool_printf("\nscheduler:\n");
    tool_printf("\t%lu ticks in %lu ms, %lu clipped, %lu lost\n",
                st->tick_ticks, st->tick_uptime_ms,
                st->tick_clipped, st->tick_lost);
    tool_printf("\t%lu over budget, %lu ticks deferred\n",
                st->tick_over_budget, st->tick_deferred);
    tool_printf("\ttimer wheel %lu ticks late, worst %lu\n",
                st->tick_skew, st->tick_skew_peak);
    tool_printf("\tworst stall %lu ms, service %lu us at the time\n",
                st->tick_worst_stall_ms, st->tick_worst_service_us);
    tool_printf("\tbaton: %lu transitions, %lu at once at the peak\n",
                st->baton_transitions, st->baton_live_max);
    tool_printf("\tbaton: %lu table full, %lu moved, state max %lu\n",
                st->baton_full, st->baton_moved, st->baton_state_max);
    tool_printf("\tbaton: %lu shared interrupt states\n",
                st->baton_state_shared);

    if (st->health_mark != 0)
        tool_printf("\tmark at 0x%08lx\n", st->health_mark);
}

/*
 * One leg of the receive step budget: mean and max in microseconds, and the
 * histogram as the three fullest power-of-two buckets, which is the whole
 * story when a distribution is bimodal and noise when it is not.  Ticks
 * convert at ~709/ms, so `ticks * 1000 / (rate / 1000)` stays inside 32 bits
 * for every delta the library's ceiling admits.
 */
static VOID show_budget_leg(const char *name, const NetStatusBudgetLeg *leg,
                            ULONG rate)
{
    ULONG khz = (rate != 0UL) ? rate / 1000UL : 709UL;
    UWORD top[3] = { 0, 0, 0 };
    UWORD i;
    UWORD n = 0;

    if (leg->nbl_Count == 0)
    {
        tool_printf("\t%s: no samples\n", (LONG)name);
        return;
    }

    tool_printf("\t%s: %lu samples, mean %lu us, max %lu us\n", (LONG)name,
                leg->nbl_Count,
                (leg->nbl_Sum / leg->nbl_Count) * 1000UL / khz,
                leg->nbl_Max * 1000UL / khz);

    for (i = 0; i < NETSTATUS_BUDGET_BUCKETS; i++)
    {
        if (leg->nbl_Hist[i] == 0)
            continue;
        if (n < 3)
            top[n++] = i;
        else if (leg->nbl_Hist[i] > leg->nbl_Hist[top[0]] ||
                 leg->nbl_Hist[i] > leg->nbl_Hist[top[1]] ||
                 leg->nbl_Hist[i] > leg->nbl_Hist[top[2]])
        {
            UWORD least = (leg->nbl_Hist[top[0]] <= leg->nbl_Hist[top[1]])
                              ? ((leg->nbl_Hist[top[0]] <=
                                  leg->nbl_Hist[top[2]]) ? 0 : 2)
                              : ((leg->nbl_Hist[top[1]] <=
                                  leg->nbl_Hist[top[2]]) ? 1 : 2);
            top[least] = i;
        }
    }

    for (i = 0; i < n; i++)
        tool_printf("\t    %lu under %lu us\n",
                    leg->nbl_Hist[top[i]],
                    ((1UL << top[i]) * 1000UL) / khz + 1UL);
}

/*
 * The baton holder ring: every hold of the ThreadX baton longer than the
 * library's ~50 ms threshold, most recent NETSTATUS_HOLD_RING of them, with
 * the holder's thread name (copied by the library at record time), the hold
 * duration, and where the hold ended.  Printed longest first, because the
 * question the ring answers is "who is the machine waiting on".
 */
static VOID show_budget_holds(const NetStatusRxBudget *b)
{
    static const char *site_name[] = {
        "?", "yield", "suspend", "discard", "orphan", "bracket", "reap"
    };
    ULONG khz = (b->nrb_EClockRate != 0UL) ? b->nrb_EClockRate / 1000UL
                                           : 709UL;
    BOOL  done[NETSTATUS_HOLD_RING];
    UWORD i;
    UWORD n;

    tool_printf("\tholds:  %lu measured, %lu over %lu ms, max %lu ms\n",
                b->nrb_HoldTotal, b->nrb_HoldSlow,
                b->nrb_HoldThreshold / khz, b->nrb_HoldMax / khz);

    for (i = 0; i < NETSTATUS_HOLD_RING; i++)
        done[i] = (b->nrb_Hold[i].nsh_Seq == 0UL);

    /* Selection over sixteen slots; a sort library would be the bigger
       code. */
    for (;;)
    {
        const NetStatusHold *h;
        char                 name[NETSTATUS_HOLD_NAME];
        UWORD                best = NETSTATUS_HOLD_RING;

        for (i = 0; i < NETSTATUS_HOLD_RING; i++)
        {
            if (done[i])
                continue;
            if (best == NETSTATUS_HOLD_RING ||
                b->nrb_Hold[i].nsh_Ticks > b->nrb_Hold[best].nsh_Ticks)
                best = i;
        }
        if (best == NETSTATUS_HOLD_RING)
            break;

        done[best] = TRUE;
        h          = &b->nrb_Hold[best];

        /* The library NUL-terminates, but the copy travelled as bytes. */
        for (n = 0; n < NETSTATUS_HOLD_NAME; n++)
            name[n] = h->nsh_Name[n];
        name[NETSTATUS_HOLD_NAME - 1] = '\0';

        tool_printf("\t    seq %-4lu %-15s %5lu.%lu ms  site %-8s state %lu  "
                    "thr 0x%08lx\n",
                    h->nsh_Seq, (LONG)name,
                    h->nsh_Ticks / khz,
                    (h->nsh_Ticks % khz) * 10UL / khz,
                    (LONG)((h->nsh_Site < 7)
                               ? site_name[h->nsh_Site] : site_name[0]),
                    (ULONG)h->nsh_State, h->nsh_Thread);
    }
}

/*
 * The receive step budget, when the library was built to keep one
 * (AMINETXDUO_RXPROBE).  Any library answers the selector; only an
 * instrumented one has counts, and the difference is said out loud rather
 * than shown as a wall of zeros.
 */
static VOID show_budget(VOID)
{
    static UBYTE buf[sizeof(NetStatusHeader) + sizeof(NetStatusRxBudget)];
    NetStatusRxBudget *b;
    struct Library    *base = tool_netstatus_open(TRUE);

    if (base == NULL)
        return;

    if (tool_netstatus_query(base, NETSTATUS_RXBUDGET, buf, sizeof(buf),
                             sizeof(NetStatusRxBudget)) <= 0)
        return;                     /* an older library: no selector, no line */

    b = (NetStatusRxBudget *)(buf + sizeof(NetStatusHeader));

    tool_printf("\nreceive budget:\n");

    if (b->nrb_Drain.nbl_Count == 0 && b->nrb_Settle.nbl_Count == 0 &&
        b->nrb_Fetch.nbl_Count == 0)
    {
        tool_printf("\tnot instrumented (a probe build keeps one)\n");
        return;
    }

    show_budget_leg("drain,  reader to IP thread ", &b->nrb_Drain,
                    b->nrb_EClockRate);
    show_budget_leg("baton,  asking to holding   ", &b->nrb_Baton,
                    b->nrb_EClockRate);
    show_budget_leg("settle, IP thread to notify ", &b->nrb_Settle,
                    b->nrb_EClockRate);
    show_budget_leg("fetch,  notify to recv()    ", &b->nrb_Fetch,
                    b->nrb_EClockRate);
    show_budget_leg("ack,    write to reply      ", &b->nrb_Ack,
                    b->nrb_EClockRate);
    show_budget_leg("reap,   tx completion walk  ", &b->nrb_Reap,
                    b->nrb_EClockRate);
    show_budget_leg("stuff,  claim and framing   ", &b->nrb_Stuff,
                    b->nrb_EClockRate);
    show_budget_leg("post,   BeginIO to return   ", &b->nrb_Post,
                    b->nrb_EClockRate);

    /* Coverage of the direct-completion fork, not a duration: how many
       recv() requests the IP thread completed in place against how many
       packets the classic blocking dequeue fetched. */
    tool_printf("\tdirect: %lu completed on the IP thread, %lu classic dequeues\n",
                b->nrb_RxDirect, b->nrb_RxFallback);

    show_budget_holds(b);
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

    /* The packet pool is reported by show_memory(), below, where the other two
       things that can run out are. */
    show_health(st);
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

        /* A card whose receive fill produces no sum has its frames walked a
           second time for a checksum that move could have produced. */
        if (st->packets_received != 0)
            tool_printf("  copy/direct fill  %10lu    summed while filling %7lu\n",
                        st->rx_copy_hook, st->rx_copy_summed);

        /* Only when there are any: the four causes behind receive errors are
           nothing alike, and the total on its own does not say which one
           fired. */
        if (st->rx_errors != 0)
            tool_printf("  of the receive errors: %lu checksum, %lu runt, "
                        "%lu length, %lu device\n",
                        st->rx_err_verify, st->rx_err_runt,
                        st->rx_err_length, st->rx_err_io);

        if (st->packets_received == 0 && st->packets_sent == 0)
        {
            tool_printf("  Nothing has gone in or out of this interface at all.\n");
            tool_printf("  If traffic was expected, check the cable and check\n");
            tool_printf("  that the interface is online (ShowNetStatus says).\n");
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
    static ToolRoutes      routes;
    static ToolRoutes6     routes6;
    static ToolDest6Table  dest6;

    tool_printf("\nRouting table\n");

    if (tool_routes(&routes) != 0)
        return;

    tool_print_routes(&routes, cfg, NULL);

    /* Prints nothing on a machine with no IPv6 routes. */
    if (tool_routes6(&routes6) == 0)
        tool_print_routes6(&routes6, cfg);

    /*
     * The destination cache under the lists it is resolved from, because it
     * is the routing decision this machine actually made and it is the first
     * table _nx_ipv6_packet_send() reads. Here as well as in ShowNetStatus,
     * for the reason routes6 is: both commands print the live routing surface,
     * and through the same function, so they cannot disagree.
     */
    if (tool_dest6(&dest6) == 0)
        tool_print_dest6(&dest6, cfg);
}

/*
 * Ends the connection line, and says so if the connection is going nowhere.
 *
 * Gated on a retransmission having already fired rather than on the stall
 * clock, because that clock is running on every healthy connection with a
 * segment in flight. One retransmit with no acknowledgement in between is the
 * first moment there is anything to report. A machine with nothing wrong
 * prints exactly what it printed before.
 */
static VOID show_stall(const ToolSockInfo *s)
{
    if (s->retransmits == 0)
    {
        tool_printf("\n");
        return;
    }

    tool_printf("  stalled %lus, %lu retx\n",
                (LONG)(s->stalled_ms / 1000UL), (LONG)s->retransmits);
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

                tool_printf("tcp    %-6lu %s%-6lu        %s",
                            (LONG)s->local_port, (LONG)joined,
                            (LONG)s->peer_port,
                            (LONG)tool_tcp_state_name(s->state));
                show_stall(s);
            }
            else
            {
                tool_printf("tcp    %-6lu %-21s %s",
                            (LONG)s->local_port, (LONG)"*",
                            (LONG)tool_tcp_state_name(s->state));
                show_stall(s);
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
    BOOL             want_health;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_INTERFACES] = 0;
    args[ARG_ROUTES]     = 0;
    args[ARG_ALL]        = 0;
    args[ARG_STATS]      = 0;
    args[ARG_HEALTH]     = 0;

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
    want_health = (args[ARG_HEALTH] != 0) ? TRUE : FALSE;

    if (!want_if && !want_routes && !want_conn && !want_stats && !want_health)
        want_if = want_routes = want_conn = TRUE;

    /*
     * -h on its own reads the mark and stops. Nothing below this runs: the
     * snapshot would open bsdsocket.library and take the stack's own locks,
     * which is exactly what a command asking whether the stack is stuck must
     * not do.
     */
    if (want_health && !want_if && !want_routes && !want_conn && !want_stats)
    {
        BOOL got = tool_health_mark(&stats);

        FreeArgs(rda);

        if (!got)
        {
            tool_error("no AmiNetXDuo stack is running, so there is nothing "
                       "to report on");
            return RETURN_WARN;
        }

        show_health(&stats);
        return RETURN_OK;
    }

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
        show_budget();
    }
    if (want_routes)
        show_routes(cfg);
    if (want_conn)
        show_connections(&snap);

    /* -h alongside something else: -s has already printed it. */
    if (want_health && !want_stats && tool_health_mark(&stats))
        show_health(&stats);

    FreeArgs(rda);
    return RETURN_OK;
}
