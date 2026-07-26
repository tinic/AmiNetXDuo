/*
 * ShowNetStatus -- what the stack thinks the network looks like, and what is
 * wrong with it.
 *
 *     ShowNetStatus INTERFACE/M,INTERFACES/S,ARPCACHE=ARP/S,ROUTES/S,
 *                   DNS=DOMAINNAMESERVERS/S,ICMP/S,IP/S,MB=MEMORY/S,TCP/S,
 *                   UDP/S,TCPSOCKETS/S,UDPSOCKETS/S,NAMES/S,ALL/S,REPEAT/S,
 *                   QUIET/S
 *
 * This is the one introspection command with a category per subject, and with
 * no category at all it prints a general summary -- which is what someone who
 * has just found out the network does not work will type.
 *
 * WHY THIS AND netstat BOTH EXIST. This command is the Amiga-shaped one: named
 * categories, a summary by default, and a diagnosis at the end aimed at
 * somebody who does not yet know what is wrong. netstat is the BSD-shaped one:
 * -i, -r, -a, -s, columns, and nothing but data. Neither is a subset of the
 * other in presentation, so both stay -- but they take their numbers from the
 * SAME two snapshots in tool_nx.c (ToolSnapshot and ToolStats), so there is no
 * way for them to report different values for one fact. Adding a counter in
 * one place makes it available to both.
 *
 * THE SUMMARY has two jobs. The first is the report: the parsed Roadshow
 * configuration (netstack_config(), or the files themselves when nothing is
 * running) for the intent, and the live NX_IP plus the SANA-II shim for the
 * reality. Where they disagree -- a DHCP lease that differs from the static
 * address in the file, say -- the live values are the ones shown.
 *
 * The second job is the diagnosis at the end. An interface that is down, an
 * interface with no address, a missing default route and a missing name
 * server are all states where every field printed above is individually
 * correct and the machine still cannot reach anything; each one is called out
 * as a problem with the command that fixes it. It runs in summary mode only:
 * somebody who asked for TCP counters asked for numbers, not advice.
 *
 * It deliberately works with the stack down. That is the state that most
 * needs explaining, and refusing to say anything until the network works is
 * exactly backwards.
 *
 * THREE CATEGORIES THIS COMMAND'S INTERFACE CARRIES ELSEWHERE ARE ABSENT,
 * because there is nothing behind them here rather than as a matter of taste:
 * IGMP (this stack does not call nx_igmp_enable(), so there is no group
 * membership and no counters), MULTICASTROUTING (there is no multicast router
 * to have statistics about) and ROUTING as a statistics category -- NetX Duo
 * keeps no routing counters, and NX_ENABLE_IP_STATIC_ROUTING is off, so the
 * routing table is the connected routes and the default gateway. Those are
 * what ROUTES prints.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"

const char *const tool_name = "ShowNetStatus";

static const char version_tag[] __attribute__((used)) =
    "$VER: ShowNetStatus 2.0 (26.7.2026)";

#define TEMPLATE    "INTERFACE/M,INTERFACES/S,ARPCACHE=ARP/S,ROUTES/S," \
                    "DNS=DOMAINNAMESERVERS/S,ICMP/S,IP/S,MB=MEMORY/S," \
                    "TCP/S,UDP/S,TCPSOCKETS/S,UDPSOCKETS/S,NAMES/S,ALL/S," \
                    "REPEAT/S,QUIET/S"

enum
{
    ARG_INTERFACE = 0,
    ARG_INTERFACES,
    ARG_ARP,
    ARG_ROUTES,
    ARG_DNS,
    ARG_ICMP,
    ARG_IP,
    ARG_MEMORY,
    ARG_TCP,
    ARG_UDP,
    ARG_TCPSOCKETS,
    ARG_UDPSOCKETS,
    ARG_NAMES,
    ARG_ALL,
    ARG_REPEAT,
    ARG_QUIET,
    ARG_COUNT
};

/*
 * What the run was asked for. A struct rather than sixteen parameters, because
 * REPEAT means the whole report is a function that has to be called again.
 */
typedef struct Wanted
{
    STRPTR *interface;              /* NULL-terminated, or NULL             */
    BOOL    interfaces;
    BOOL    arp;
    BOOL    routes;
    BOOL    dns;
    BOOL    icmp;
    BOOL    ip;
    BOOL    memory;
    BOOL    tcp;
    BOOL    udp;
    BOOL    tcpsockets;
    BOOL    udpsockets;
    BOOL    names;
    BOOL    all;
    BOOL    quiet;
    BOOL    summary;                /* no category was asked for            */
} Wanted;

/* ------------------------------------------------------------- diagnosis -- */

static UWORD problem_count;

static VOID problem_head(VOID)
{
    if (problem_count++ == 0)
        tool_printf("\nWhat to look at\n");
}

/* ------------------------------------------------------------------ names -- */

/*
 * NAMES asks for symbolic names instead of numbers, which is two lookups: the
 * local DEVS:Internet files, which work with no network at all, and then the
 * running stack's own resolver. Loaded once, on first use, so a report that
 * was not asked for names never touches the disk.
 */
static BOOL names_wanted;
static BOOL netdb_loaded;

static VOID names_prepare(BOOL wanted)
{
    names_wanted = wanted;

    if (wanted && !netdb_loaded)
    {
        (VOID)ami_netdb_load();
        netdb_loaded = TRUE;
    }
}

/* The address as text: a name when one is known and NAMES was asked for. */
static VOID address_text(ULONG addr, char *buf, ULONG buflen)
{
    ami_config_format_ip(addr, buf, buflen);

    if (!names_wanted || addr == 0)
        return;

    {
        const AmiNetdbEntry *local = ami_netdb_host_by_addr(addr);

        if (local != NULL && local->name != NULL)
        {
            tool_copy_string(buf, buflen, local->name);
            return;
        }
    }

    {
        char found[AMI_CFG_NAME_LEN];

        if (tool_stack_lookup_addr(addr, found, sizeof(found)) &&
            found[0] != '\0')
        {
            tool_copy_string(buf, buflen, found);
        }
    }
}

/* The service this port belongs to, or NULL -- see the callers for the format. */
static const char *service_name(UWORD port, BOOL is_tcp)
{
    const AmiNetdbEntry *entry;

    if (!names_wanted || port == 0)
        return NULL;

    entry = ami_netdb_serv_by_port((LONG)port, is_tcp ? "tcp" : "udp");

    return (entry != NULL) ? entry->name : NULL;
}

/* ---------------------------------------------------------------- report -- */

static const char *iface_name(const AmiConfig *cfg, UWORD index)
{
    if (cfg != NULL && index < cfg->interface_count &&
        cfg->interfaces[index].name[0] != '\0')
    {
        return cfg->interfaces[index].name;
    }

    return "?";
}

static VOID show_counters(const char *name, const AmiSana2Stats *st)
{
    tool_printf("\nSANA-II counters for %s\n", (LONG)name);
    tool_printf("  packets received  %10lu    packets sent      %10lu\n",
                st->packets_received, st->packets_sent);
    tool_printf("  bad data          %10lu    overruns          %10lu\n",
                st->bad_data, st->overruns);
    tool_printf("  unknown types     %10lu    reconfigurations  %10lu\n",
                st->unknown_types, st->reconfigurations);
    tool_printf("  transmit errors   %10lu    receive errors    %10lu\n",
                st->tx_errors, st->rx_errors);
    tool_printf("  buffer failures   %10lu\n", st->alloc_failures);
}

static VOID show_interface(const AmiIfConfig *cfg, const ToolIfInfo *live,
                           BOOL up, BOOL stats, BOOL stack_running,
                           BOOL readable)
{
    char addr[AMI_CFG_NAME_LEN];
    char mask[16];
    char bcast[16];
    char mac[24];

    tool_printf("\nInterface %s (%s unit %ld)\n",
                (LONG)cfg->name, (LONG)cfg->device, (LONG)cfg->unit);

    if (stack_running && readable)
    {
        tool_printf("  state       %-10s      link %s\n",
                    (LONG)(up ? "online" : "offline"),
                    (LONG)(live != NULL && live->attached
                               ? (live->link_up ? "up" : "down")
                               : "unknown"));
    }
    else if (stack_running)
    {
        /*
         * The stack is up but in another program's library, where nothing
         * here can read a per-interface flag. Saying "offline" would be a
         * guess -- and the wrong one, because a running stack has usually
         * brought its interfaces up.
         */
        tool_printf("  state       running, but this command cannot read it\n");
    }
    else
    {
        tool_printf("  state       not started\n");
    }

    if (live != NULL && live->attached)
    {
        address_text(live->address, addr, sizeof(addr));
        ami_config_format_ip(live->netmask, mask, sizeof(mask));
        ami_config_format_ip(tool_broadcast(live->address, live->netmask),
                             bcast, sizeof(bcast));
        tool_format_mac(live->mac, mac, sizeof(mac));

        tool_printf("  address     %-15s netmask %s (/%ld)\n",
                    (LONG)addr, (LONG)mask,
                    (LONG)tool_prefix_len(live->netmask));
        tool_printf("  broadcast   %s\n", (LONG)bcast);
        tool_printf("  hardware    %s\n", (LONG)mac);
        tool_printf("  mtu         %lu bytes", live->mtu);
        if (live->bps != 0)
            tool_printf("        %lu bits/s", live->bps);
        tool_printf("\n");
    }
    else if (cfg->iptype == AMI_IPTYPE_DHCP)
    {
        tool_printf("  address     handed out by DHCP when the interface "
                    "comes up\n");
    }
    else
    {
        address_text(cfg->address, addr, sizeof(addr));
        ami_config_format_ip(cfg->netmask, mask, sizeof(mask));
        tool_printf("  address     %-15s netmask %s   (from the interface "
                    "file)\n", (LONG)addr, (LONG)mask);
    }

    tool_printf("  configured  %s\n",
                (LONG)(cfg->iptype == AMI_IPTYPE_DHCP      ? "DHCP" :
                       cfg->iptype == AMI_IPTYPE_LINKLOCAL ? "link-local"
                                                           : "static"));

    if (stats)
    {
        if (live != NULL && live->have_sana2)
            show_counters(cfg->name, &live->stats);
        else
            tool_printf("\nSANA-II counters for %s are not available "
                        "(no driver attached)\n", (LONG)cfg->name);
    }
}

/* INTERFACES: one line each, which is what "the list" means. */
/*
 * Whether the interface is switched on, read out of the running stack.
 *
 * It used to be netstack_interface_is_up(), which is one of
 * src/tools/netstack_weak.c's weak stubs and answered FALSE for every
 * interface in every build that shipped -- so this column read "offline"
 * beside an interface that was carrying traffic. docs/RESEARCH.md 22.
 *
 * link_up and not sana2_online, deliberately: this is the state Online and
 * Offline set, and netstack_interface_is_up() read the same field
 * (nx_interface_link_up). The SANA-II shim's own online flag is a different
 * fact about a different layer, and showing it here would make "Offline eth0"
 * followed by ShowNetStatus disagree with itself.
 */
static BOOL iface_online(const ToolIfInfo *live)
{
    if (live == NULL || !live->attached)
        return FALSE;

    return live->link_up;
}

static VOID show_interface_list(const AmiConfig *cfg, const ToolSnapshot *snap,
                                BOOL have_live, BOOL readable)
{
    char  addr[AMI_CFG_NAME_LEN];
    UWORD i;

    tool_printf("\nInterfaces\n");
    tool_printf("Name            State    Link     Address\n");

    if (cfg->interface_count == 0)
    {
        tool_printf("(none configured)\n");
        return;
    }

    for (i = 0; i < cfg->interface_count; i++)
    {
        const ToolIfInfo *live = NULL;

        if (have_live && i < snap->iface_count)
            live = &snap->iface[i];

        if (live != NULL && live->attached)
            address_text(live->address, addr, sizeof(addr));
        else
            tool_copy_string(addr, sizeof(addr), "-");

        tool_printf("%-15s %-8s %-8s %s\n",
                    (LONG)cfg->interfaces[i].name,
                    (LONG)(!readable ? "?" :
                           iface_online(live) ? "online" : "offline"),
                    (LONG)(live != NULL && live->attached
                               ? (live->link_up ? "up" : "down") : "?"),
                    (LONG)addr);
    }
}

static VOID show_arp(const AmiConfig *cfg, const ToolStats *st)
{
    char  addr[AMI_CFG_NAME_LEN];
    char  mac[24];
    UWORD i;

    tool_printf("\nARP cache\n");

    if (!st->have_arp)
    {
        tool_printf("Address resolution is not enabled on this stack.\n");
        return;
    }

    tool_printf("Address                         Hardware           "
                "Type     Interface\n");

    if (st->arp_count == 0)
    {
        tool_printf("(empty)\n");
    }

    for (i = 0; i < st->arp_count; i++)
    {
        const ToolArpEntry *e = &st->arp[i];

        address_text(e->address, addr, sizeof(addr));

        if (e->resolved)
            tool_format_mac(e->mac, mac, sizeof(mac));
        else
            tool_copy_string(mac, sizeof(mac), "(incomplete)");

        tool_printf("%-31s %-18s %-8s %s\n",
                    (LONG)addr, (LONG)mac,
                    (LONG)(e->is_static ? "static" : "dynamic"),
                    (LONG)iface_name(cfg, e->nx_index));
    }

    if (st->arp_truncated)
        tool_printf("(list truncated at %ld entries)\n", (LONG)TOOL_MAX_ARP);

    tool_printf("\n  requests sent     %10lu    requests received %10lu\n",
                st->arp_requests_sent, st->arp_requests_received);
    tool_printf("  responses sent    %10lu    responses received%10lu\n",
                st->arp_responses_sent, st->arp_responses_received);
    tool_printf("  dynamic entries   %10lu    static entries    %10lu\n",
                st->arp_dynamic_entries, st->arp_static_entries);
    tool_printf("  aged out          %10lu    bad messages      %10lu\n",
                st->arp_aged_entries, st->arp_invalid_messages);
}

static VOID show_routes(const AmiConfig *cfg, BOOL have_live)
{
    static ToolRoutes routes;
    char              gw[AMI_CFG_NAME_LEN];

    tool_printf("\nRoutes\n");

    if (!have_live)
    {
        tool_printf("Destination      Gateway          "
                    "Netmask          Flags  Interface\n");

        if (cfg->default_gateway != 0)
        {
            address_text(cfg->default_gateway, gw, sizeof(gw));
            tool_printf("%-16s %-16s %-16s %-6s %s\n",
                        (LONG)"default", (LONG)gw, (LONG)"0.0.0.0",
                        (LONG)"UG", (LONG)"(configured)");
        }
        else
        {
            tool_printf("(the stack is not readable from here; nothing but the "
                        "configuration)\n");
        }
        return;
    }

    /*
     * The live table, from NETSTATUS_ROUTES rather than derived from the
     * interface list: with NX_ENABLE_IP_STATIC_ROUTING in the stack there are
     * routes that no amount of looking at interfaces would show.  netstat
     * prints the same rows through the same function.
     */
    if (tool_routes(&routes) != 0)
        return;

    tool_print_routes(&routes, cfg, address_text);
}

static VOID show_resolver(const AmiResolverConfig *r, BOOL from_files)
{
    char  addr[16];
    UWORD i;

    if (r->nameserver_count == 0)
    {
        tool_printf("\nName servers:   none configured\n");
    }
    else
    {
        for (i = 0; i < r->nameserver_count; i++)
        {
            ami_config_format_ip(r->nameserver[i], addr, sizeof(addr));
            tool_printf("%s%s\n",
                        (LONG)(i == 0 ? "\nName servers:   " :
                                          "                "),
                        (LONG)addr);
        }
    }

    if (r->domain[0] != '\0')
        tool_printf("Domain:         %s\n", (LONG)r->domain);

    for (i = 0; i < r->search_count; i++)
    {
        tool_printf("%s%s\n",
                    (LONG)(i == 0 ? "Search:         " :
                                     "                "),
                    (LONG)r->search[i]);
    }

    /*
     * Where those came from matters. A DHCP lease supplies name servers and
     * they replace what the file says -- so a list read off the disk can name
     * a server the machine is not using. Saying which it is stops the reader
     * "fixing" a file that is not being consulted.
     */
    if (from_files && r->nameserver_count > 0)
        tool_printf("                (from DEVS:Internet; a DHCP interface is\n"
                    "                given its own, which replace these)\n");
}

/*
 * Name servers, from the running stack when it can be asked. A DHCP lease
 * replaces whatever the file says, so reading the file alone reports a server
 * the machine is not using -- or "none configured" on a machine whose lookups
 * work perfectly. Returns TRUE when at least one is in use.
 */
static BOOL show_dns(const AmiConfig *cfg, BOOL elsewhere, BOOL from_disk)
{
    if (elsewhere)
    {
        char  live_ns[AMI_CFG_MAX_NAMESERVERS][16];
        ULONG live_count =
            tool_stack_name_servers(live_ns, (ULONG)AMI_CFG_MAX_NAMESERVERS);

        if (live_count > 0)
        {
            ULONG n;

            for (n = 0; n < live_count; n++)
                tool_printf("%s%s\n",
                            (LONG)(n == 0 ? "\nName servers:   "
                                          : "                "),
                            (LONG)live_ns[n]);
            return TRUE;
        }
    }

    show_resolver(&cfg->resolver, from_disk);

    return (BOOL)(cfg->resolver.nameserver_count > 0);
}

/* ----------------------------------------------------- protocol counters -- */

static VOID show_ip_stats(const ToolStats *st)
{
    tool_printf("\nIP\n");

    if (!st->have_ip)
    {
        tool_printf("  no counters: the stack is not readable from here\n");
        return;
    }

    tool_printf("  packets sent      %10lu    bytes sent        %10lu\n",
                st->ip_packets_sent, st->ip_bytes_sent);
    tool_printf("  packets received  %10lu    bytes received    %10lu\n",
                st->ip_packets_received, st->ip_bytes_received);
    tool_printf("  bad packets       %10lu    checksum errors   %10lu\n",
                st->ip_invalid, st->ip_checksum_errors);
    tool_printf("  dropped on receipt%10lu    dropped on send   %10lu\n",
                st->ip_receive_dropped, st->ip_send_dropped);
    tool_printf("  fragments sent    %10lu    fragments received%10lu\n",
                st->ip_fragments_sent, st->ip_fragments_received);
}

static VOID show_icmp_stats(const ToolStats *st)
{
    tool_printf("\nICMP\n");

    if (!st->have_icmp)
    {
        tool_printf("  ICMP is not enabled on this stack\n");
        return;
    }

    tool_printf("  echo requests sent%10lu    replies received  %10lu\n",
                st->icmp_pings_sent, st->icmp_responses);
    tool_printf("  timed out         %10lu    waiting now       %10lu\n",
                st->icmp_ping_timeouts, st->icmp_threads_suspended);
    tool_printf("  checksum errors   %10lu    not handled       %10lu\n",
                st->icmp_checksum_errors, st->icmp_unhandled);
}

static VOID show_tcp_stats(const ToolStats *st)
{
    tool_printf("\nTCP\n");

    if (!st->have_tcp)
    {
        tool_printf("  TCP is not enabled on this stack\n");
        return;
    }

    tool_printf("  packets sent      %10lu    bytes sent        %10lu\n",
                st->tcp_packets_sent, st->tcp_bytes_sent);
    tool_printf("  packets received  %10lu    bytes received    %10lu\n",
                st->tcp_packets_received, st->tcp_bytes_received);
    tool_printf("  bad packets       %10lu    checksum errors   %10lu\n",
                st->tcp_invalid, st->tcp_checksum_errors);
    tool_printf("  dropped on receipt%10lu    retransmitted     %10lu\n",
                st->tcp_receive_dropped, st->tcp_retransmits);
    tool_printf("  connections made  %10lu    closed            %10lu\n",
                st->tcp_connections, st->tcp_disconnections);
    tool_printf("  connections lost  %10lu\n", st->tcp_connections_dropped);
}

static VOID show_udp_stats(const ToolStats *st)
{
    tool_printf("\nUDP\n");

    if (!st->have_udp)
    {
        tool_printf("  UDP is not enabled on this stack\n");
        return;
    }

    tool_printf("  datagrams sent    %10lu    bytes sent        %10lu\n",
                st->udp_packets_sent, st->udp_bytes_sent);
    tool_printf("  datagrams received%10lu    bytes received    %10lu\n",
                st->udp_packets_received, st->udp_bytes_received);
    tool_printf("  bad datagrams     %10lu    checksum errors   %10lu\n",
                st->udp_invalid, st->udp_checksum_errors);
    tool_printf("  dropped on receipt%10lu\n", st->udp_receive_dropped);
}

/*
 * MEMORY is the packet pool: on this machine that IS the network's memory, and
 * "free" running at zero is what a stall looks like from the outside.
 */
static VOID show_memory(const ToolStats *st)
{
    tool_printf("\nMemory buffers\n");

    if (!st->have_pool)
    {
        tool_printf("  no packet pool: the stack is not readable from here\n");
        return;
    }

    tool_printf("  packets           %10lu    free now          %10lu\n",
                st->pool_total, st->pool_free);
    tool_printf("  payload each      %10lu bytes\n", st->pool_payload);
    tool_printf("  found empty       %10lu    waited for one    %10lu\n",
                st->pool_empty_requests, st->pool_empty_suspensions);
    tool_printf("  bad releases      %10lu\n", st->pool_invalid_releases);
}

/*
 * TCPSOCKETS / UDPSOCKETS. Without ALL only sockets talking to somewhere else
 * are listed; ALL adds the ones that are merely bound here, which is every
 * listener and every idle datagram socket.
 */
static VOID show_tcp_sockets(const ToolSnapshot *snap, BOOL all)
{
    char  peer[AMI_CFG_NAME_LEN];
    UWORD i;
    UWORD shown = 0;

    tool_printf("\nTCP sockets\n");
    tool_printf("Local     Foreign                         State\n");

    for (i = 0; i < snap->sock_count; i++)
    {
        const ToolSockInfo *s = &snap->sock[i];
        const char         *service;

        if (!s->is_tcp)
            continue;
        if (!all && s->peer_address == 0)
            continue;

        service = service_name(s->local_port, TRUE);
        if (service != NULL)
            tool_printf("%-9s ", (LONG)service);
        else
            tool_printf("%-9lu ", (LONG)s->local_port);

        if (s->peer_address != 0)
        {
            address_text(s->peer_address, peer, sizeof(peer));
            tool_printf("%-24s %-6lu ", (LONG)peer, (LONG)s->peer_port);
        }
        else
        {
            tool_printf("%-31s ", (LONG)"*");
        }

        tool_printf("%s\n", (LONG)tool_tcp_state_name(s->state));
        shown++;
    }

    if (shown == 0)
        tool_printf("(none)\n");
    if (snap->sock_truncated)
        tool_printf("(list truncated at %ld sockets)\n", (LONG)TOOL_MAX_SOCK);
}

static VOID show_udp_sockets(const ToolSnapshot *snap, BOOL all)
{
    UWORD i;
    UWORD shown = 0;

    tool_printf("\nUDP sockets\n");
    tool_printf("Local     Queued\n");

    for (i = 0; i < snap->sock_count; i++)
    {
        const ToolSockInfo *s = &snap->sock[i];
        const char         *service;

        if (s->is_tcp)
            continue;
        if (!all && s->queued == 0)
            continue;

        service = service_name(s->local_port, FALSE);
        if (service != NULL)
            tool_printf("%-9s ", (LONG)service);
        else
            tool_printf("%-9lu ", (LONG)s->local_port);

        tool_printf("%lu\n", s->queued);
        shown++;
    }

    if (shown == 0)
        tool_printf("(none)\n");
}

/* ------------------------------------------------------------- diagnosis -- */

/*
 * Everything that is individually a correct field value and collectively a
 * machine that cannot reach anything.
 */
static VOID diagnose_interface(const AmiIfConfig *cfg, const ToolIfInfo *live,
                               BOOL up, BOOL stack_running, BOOL readable)
{
    /*
     * Only diagnose what can actually be seen. Every check below reads live
     * interface state, and inventing it for a stack we cannot read produced
     * the worst possible output: "eth0 is offline, bring it up with Online
     * eth0" about an interface that was up and had a DHCP lease.
     */
    if (!stack_running || !readable)
        return;

    if (!up)
    {
        problem_head();
        tool_printf("  * %s is offline, so nothing can go in or out of it.\n",
                    (LONG)cfg->name);
        tool_printf("    Bring it up with:   Online %s\n", (LONG)cfg->name);
        return;
    }

    if (live == NULL || !live->attached)
    {
        problem_head();
        tool_printf("  * %s is configured but the stack never attached it.\n",
                    (LONG)cfg->name);
        tool_printf("    Its driver (%s) may not have opened. Start the\n",
                    (LONG)cfg->device);
        tool_printf("    network again and watch what AddNetInterface says.\n");
        return;
    }

    if (!live->link_up)
    {
        problem_head();
        tool_printf("  * %s has no link: the card sees no network.\n",
                    (LONG)cfg->name);
        tool_printf("    Check the cable at both ends, and that whatever it\n");
        tool_printf("    plugs into is switched on.\n");
    }

    if (live->address == 0)
    {
        problem_head();
        tool_printf("  * %s has no address, so it cannot be used yet.\n",
                    (LONG)cfg->name);

        if (cfg->iptype == AMI_IPTYPE_DHCP)
        {
            tool_printf("    It is set to ask for one (DHCP) and nothing has\n");
            tool_printf("    answered. Check the cable, and that something on\n");
            tool_printf("    this network hands out addresses -- or run\n");
            tool_printf("    NetSetup and choose a fixed address instead.\n");
        }
        else
        {
            tool_printf("    It is set to use a fixed address but the interface\n");
            tool_printf("    file has no ADDRESS line. Run NetSetup to set one.\n");
        }
    }
}

/* -------------------------------------------------------------- the run --- */

/*
 * TRUE when `name` is one of the names given to INTERFACE. With no list at all
 * every interface matches, which is what the summary wants.
 */
static BOOL interface_selected(STRPTR *list, const char *name)
{
    ULONG i;

    if (list == NULL || list[0] == NULL)
        return TRUE;

    for (i = 0; list[i] != NULL; i++)
    {
        if (tool_stricmp(name, tool_basename((const char *)list[i])) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * One pass of the whole report. REPEAT calls this again every second, so
 * nothing here may allocate, and everything it needs is passed in.
 */
static LONG report(const Wanted *w, const AmiConfig *cfg, BOOL from_disk)
{
    static ToolSnapshot snap;
    static ToolStats    stats;
    BOOL                have_live     = FALSE;
    BOOL                have_stats    = FALSE;
    BOOL                stack_running = FALSE;
    BOOL                elsewhere     = FALSE;
    BOOL                have_ns       = FALSE;
    BOOL                want_sockets;
    ULONG               ext_addr = 0;
    char                ext_host[AMI_CFG_NAME_LEN];
    char                addr[AMI_CFG_NAME_LEN];
    UWORD               i;
    UWORD               shown = 0;

    problem_count = 0;
    ext_host[0]   = '\0';

    want_sockets = (BOOL)(w->tcpsockets || w->udpsockets);

    /*
     * Two states, and they used to be three.
     *
     * The third was "the stack is linked into this command", which was never
     * true in anything shipped -- no tool links aminetxduo_netstack, so
     * netstack_get() is src/tools/netstack_weak.c's stub and answers NULL.
     * What was left was "running somewhere I cannot see into", which is how
     * this report came to print the configuration and nothing else while the
     * network was working perfectly. docs/RESEARCH.md 21.
     *
     * Now: the stack is running inside bsdsocket.library, and NetStackQuery()
     * reads it; or it is not running, and the files on disk are all there is
     * -- which is exactly the case a beginner is in.
     */
    if (tool_stack_library_running())
    {
        stack_running = TRUE;
        (VOID)tool_stack_query(&ext_addr, ext_host, sizeof(ext_host));

        /*
         * TRUE quietly: a report is allowed to say "the network is up but I
         * could not read it" in its own words further down, and must not
         * interrupt itself with an error block halfway through a table.
         */
        if (tool_snapshot(&snap, want_sockets) == 0)
            have_live = TRUE;

        if (tool_stats(&stats) == 0)
            have_stats = TRUE;

        /* Running, and we could not see in. Say which. */
        elsewhere = (BOOL)(!have_live);
    }

    if (!have_stats)
    {
        stats.have_ip = stats.have_icmp = stats.have_tcp = FALSE;
        stats.have_udp = stats.have_arp = stats.have_pool = FALSE;
        stats.arp_count     = 0;
        stats.arp_truncated = FALSE;
    }

    /* ---- the summary --------------------------------------------------- */

    if (w->summary)
    {
        tool_printf("\nNetwork stack:  %s\n",
                    (LONG)(stack_running ? "running" : "not started"));

        if (ext_host[0] != '\0')
            tool_printf("Host name:      %s\n", (LONG)ext_host);
        else if (cfg->hostname[0] != '\0')
            tool_printf("Host name:      %s\n", (LONG)cfg->hostname);

        if (elsewhere && ext_addr != 0)
        {
            address_text(ext_addr, addr, sizeof(addr));
            tool_printf("This machine:   %s\n", (LONG)addr);
        }

        if (have_live && snap.have_gateway)
        {
            address_text(snap.gateway, addr, sizeof(addr));
            tool_printf("Default route:  %s\n", (LONG)addr);
        }
        else if (cfg->default_gateway != 0)
        {
            address_text(cfg->default_gateway, addr, sizeof(addr));
            tool_printf("Default route:  %s (configured)\n", (LONG)addr);
        }
        else
        {
            tool_printf("Default route:  none\n");
        }
    }

    /* ---- the categories ------------------------------------------------ */

    if (w->interfaces)
        show_interface_list(cfg, &snap, have_live, (BOOL)!elsewhere);

    /*
     * The per-interface blocks: every interface in summary mode, or the ones
     * INTERFACE named. The named form is the detailed one, so it carries the
     * SANA-II counters with it.
     */
    if (w->summary || (w->interface != NULL && w->interface[0] != NULL))
    {
        BOOL detailed = (BOOL)(w->interface != NULL && w->interface[0] != NULL);

        for (i = 0; i < cfg->interface_count; i++)
        {
            const ToolIfInfo *live = NULL;

            if (!interface_selected(w->interface, cfg->interfaces[i].name))
                continue;

            /*
             * Interface handles are "index 0..count-1 in config order"
             * (aminetxduo/netstack.h) and the stack attaches them to NX_IP in
             * the same order, so the two indices line up.
             */
            if (have_live && i < snap.iface_count)
                live = &snap.iface[i];

            show_interface(&cfg->interfaces[i], live,
                           iface_online(live), detailed,
                           stack_running, (BOOL)!elsewhere);
            shown++;
        }

        if (shown == 0 && detailed)
        {
            if (!w->quiet)
            {
                tool_error("there is no interface called \"%s\"",
                           (LONG)tool_basename((const char *)w->interface[0]));

                if (cfg->interface_count > 0)
                {
                    tool_advise_blank();
                    tool_advise("The interfaces this machine has are:");
                    for (i = 0; i < cfg->interface_count; i++)
                        tool_printf("      %s\n",
                                    (LONG)cfg->interfaces[i].name);
                }
            }

            return RETURN_ERROR;
        }
    }

    if (w->arp)
        show_arp(cfg, &stats);
    if (w->routes)
        show_routes(cfg, have_live);

    if (w->summary || w->dns)
        have_ns = show_dns(cfg, elsewhere, from_disk);

    if (w->ip)
        show_ip_stats(&stats);
    if (w->icmp)
        show_icmp_stats(&stats);
    if (w->tcp)
        show_tcp_stats(&stats);
    if (w->udp)
        show_udp_stats(&stats);
    if (w->memory)
        show_memory(&stats);
    if (w->tcpsockets)
        show_tcp_sockets(&snap, w->all);
    if (w->udpsockets)
        show_udp_sockets(&snap, w->all);

    /* ---- the diagnosis --------------------------------------------------
     *
     * Summary mode only, and all of it after the report, never interleaved
     * with it: "What to look at" has to be one list in one place, or the
     * reader has to reassemble it from between the interface blocks.
     */

    if (!w->summary)
        return RETURN_OK;

    for (i = 0; i < cfg->interface_count; i++)
    {
        const ToolIfInfo *live = NULL;

        if (have_live && i < snap.iface_count)
            live = &snap.iface[i];

        diagnose_interface(&cfg->interfaces[i], live,
                           iface_online(live), stack_running,
                           (BOOL)!elsewhere);
    }

    if (cfg->interface_count == 0)
    {
        /*
         * Nothing is set up. Saying "and there is no default route, and no
         * name server" on top of that would be three complaints about one
         * fact, so this is the only one; the explainer covers what to do.
         */
        problem_head();
        tool_explain_no_interfaces();
        return RETURN_WARN;
    }

    if (!stack_running)
    {
        problem_head();
        tool_printf("  * The network has not been started.\n");
        tool_printf("    Start it with:   AddNetInterface %s\n",
                    (LONG)cfg->interfaces[0].name);
        tool_printf("    Put that line in S:User-Startup to have it happen at\n");
        tool_printf("    every boot.\n");
    }

    /*
     * A DHCP interface is given a router and a name server along with its
     * address, and a stack we can only see from outside will not tell us
     * either. Complaining in those two cases would be a false alarm on the
     * commonest setup of all, so the route and name-server checks only run
     * when nothing else is going to supply them.
     */
    {
        BOOL any_dhcp = FALSE;

        for (i = 0; i < cfg->interface_count; i++)
        {
            if (cfg->interfaces[i].iptype == AMI_IPTYPE_DHCP)
                any_dhcp = TRUE;
        }

        if (!any_dhcp && !elsewhere)
        {
            if (cfg->default_gateway == 0 && (!have_live || !snap.have_gateway))
            {
                problem_head();
                tool_printf("  * There is no default route, so only machines on "
                            "your own\n");
                tool_printf("    network can be reached -- nothing beyond it.\n");
                tool_printf("    Run NetSetup and give it your router's address, "
                            "or put\n");
                tool_printf("    DEFAULT=<router address> in "
                            "DEVS:Internet/routes.\n");
            }

            if (!have_ns)
            {
                problem_head();
                tool_printf("  * No name server is configured, so names like\n");
                tool_printf("    www.example.com cannot be looked up. Numeric "
                            "addresses\n");
                tool_printf("    still work.\n");
                tool_printf("    Run NetSetup, or put  NAMESERVER <address>  in\n");
                tool_printf("    DEVS:Internet/name_resolution. On a home network "
                            "the\n");
                tool_printf("    router is usually the name server too.\n");
            }
        }
    }

    if (elsewhere)
    {
        problem_head();
        tool_printf("  * The counters and per-interface detail above come from\n");
        tool_printf("    the configuration, not from the running stack: the\n");
        tool_printf("    stack is inside bsdsocket.library and has no call yet\n");
        tool_printf("    that lets another command read it.\n");
    }

    if (problem_count == 0)
        tool_printf("\nNo problems found.\n");

    return RETURN_OK;
}

int main(int argc, char **argv)
{
    LONG             args[ARG_COUNT];
    struct RDArgs   *rda;
    const AmiConfig *cfg;
    AmiConfig       *from_disk = NULL;
    Wanted           w;
    BOOL             repeat;
    LONG             rc;
    UWORD            i;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < (UWORD)ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    w.interface  = (STRPTR *)args[ARG_INTERFACE];
    w.interfaces = (args[ARG_INTERFACES] != 0) ? TRUE : FALSE;
    w.arp        = (args[ARG_ARP]        != 0) ? TRUE : FALSE;
    w.routes     = (args[ARG_ROUTES]     != 0) ? TRUE : FALSE;
    w.dns        = (args[ARG_DNS]        != 0) ? TRUE : FALSE;
    w.icmp       = (args[ARG_ICMP]       != 0) ? TRUE : FALSE;
    w.ip         = (args[ARG_IP]         != 0) ? TRUE : FALSE;
    w.memory     = (args[ARG_MEMORY]     != 0) ? TRUE : FALSE;
    w.tcp        = (args[ARG_TCP]        != 0) ? TRUE : FALSE;
    w.udp        = (args[ARG_UDP]        != 0) ? TRUE : FALSE;
    w.tcpsockets = (args[ARG_TCPSOCKETS] != 0) ? TRUE : FALSE;
    w.udpsockets = (args[ARG_UDPSOCKETS] != 0) ? TRUE : FALSE;
    w.names      = (args[ARG_NAMES]      != 0) ? TRUE : FALSE;
    w.all        = (args[ARG_ALL]        != 0) ? TRUE : FALSE;
    w.quiet      = (args[ARG_QUIET]      != 0) ? TRUE : FALSE;
    repeat       = (args[ARG_REPEAT]     != 0) ? TRUE : FALSE;

    /*
     * ALL and NAMES modify the other categories rather than selecting one, so
     * neither of them counts here: "ShowNetStatus ALL" is still the summary.
     */
    w.summary = (BOOL)!(w.interfaces || w.arp || w.routes || w.dns ||
                        w.icmp || w.ip || w.memory || w.tcp || w.udp ||
                        w.tcpsockets || w.udpsockets ||
                        (w.interface != NULL && w.interface[0] != NULL));

    names_prepare(w.names);

    /*
     * This command words its own "up but unreadable" line, in its own place
     * in the report. tool_snapshot()'s error block belongs to netstat, which
     * has nothing else to print.
     */
    tool_nx_quiet(TRUE);

    cfg = netstack_config();
    if (cfg == NULL)
    {
        /*
         * Read the files ourselves. ami_config_load() is what the stack does
         * at startup, so this is the same view it would get -- including,
         * through tool_config_watch(), every complaint it would make.
         */
        from_disk = (AmiConfig *)ami_alloc((ULONG)sizeof(AmiConfig));
        if (from_disk == NULL)
        {
            if (!w.quiet)
                tool_error("out of memory");
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        tool_config_watch();
        (VOID)ami_config_load(from_disk);
        tool_config_unwatch();

        cfg = from_disk;
    }

    for (;;)
    {
        rc = report(&w, cfg, (BOOL)(from_disk != NULL));

        if (!repeat || rc != RETURN_OK)
            break;

        /*
         * REPEAT: once a second until Ctrl-C, with the screen cleared first so
         * the numbers stay in the same place and a change is visible. Form
         * feed is what an Amiga console clears on.
         */
        if (tool_delay_ticks((ULONG)TICKS_PER_SECOND))
            break;

        tool_printf("\014");
    }

    if (from_disk != NULL)
        ami_free(from_disk);

    FreeArgs(rda);
    return (int)rc;
}
