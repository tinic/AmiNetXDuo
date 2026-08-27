/*
 * ShowNetStatus, report the stack's view of the network, and what is wrong
 * with it. One category switch per subject; with none it prints a general
 * summary and a closing diagnosis. It works with the stack down.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"
#include "tool_events.h"

#include "aminetxduo/events.h"

#include <exec/semaphores.h>

#include "aminetxduo/version.h"

const char *const tool_name = "ShowNetStatus";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("ShowNetStatus");

#define TEMPLATE    "INTERFACE/M,INTERFACES/S,ARPCACHE=ARP/S,ROUTES/S," \
                    "DNS=DOMAINNAMESERVERS/S,ICMP/S,IP/S,MB=MEMORY/S," \
                    "TCP/S,UDP/S,TCPSOCKETS/S,UDPSOCKETS/S,USERS/S," \
                    "EVENTS/S,NAMES/S,ALL/S,REPEAT/S"

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
    ARG_USERS,
    ARG_EVENTS,
    ARG_NAMES,
    ARG_ALL,
    ARG_REPEAT,
    ARG_COUNT
};

/* What the run was asked for. A struct rather than sixteen parameters, because
   REPEAT calls the whole report again. */
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
    BOOL    users;
    BOOL    events;
    BOOL    names;
    BOOL    all;
    BOOL    summary;                /* no category was asked for            */
} Wanted;

/* ------------------------------------------------------------- diagnosis, */

static UWORD problem_count;

static VOID problem_head(VOID)
{
    if (problem_count++ == 0)
        tool_printf("\nWhat to look at\n");
}

/* ------------------------------------------------------------------ names, */

/* NAMES prints symbolic names instead of numbers: the local DEVS:Internet
   files first, then the running stack's resolver. Loaded once on first use. */
static BOOL names_wanted;
static BOOL netdb_loaded;

static VOID names_prepare(BOOL wanted)
{
    names_wanted = wanted;

    if (wanted && !netdb_loaded)
    {
        /*
         * AmigaOS does not reclaim AllocVec() memory when a process exits, so
         * the blocks ami_netdb_load() builds outlive this command. main()
         * frees it on the way out.
         */
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

/* The service this port belongs to, or NULL, see the callers for the format. */
static const char *service_name(UWORD port, BOOL is_tcp)
{
    const AmiNetdbEntry *entry;

    if (!names_wanted || port == 0)
        return NULL;

    entry = ami_netdb_serv_by_port((LONG)port, is_tcp ? "tcp" : "udp");

    return (entry != NULL) ? entry->name : NULL;
}

/* ---------------------------------------------------------------- report, */

/* The running stack's own name for an NX slot, never the description at that
   array position: see tool_iface_name() in tool_nx.c. */
static const char *iface_name(const AmiConfig *cfg, UWORD index)
{
    return tool_iface_name(cfg, index);
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
    tool_printf("  vblank RX polls   %10lu    deaf RX resets    %10lu\n",
                st->tick_polls, st->rx_kicks);
}

/*
 * A lease time in the largest whole unit. Returns the unit and writes the
 * count, or NULL when the lease never expires.
 */
static const char *lease_duration(ULONG seconds, ULONG *value_out)
{
    if (seconds == NETSTATUS_DHCP_FOREVER)
        return NULL;

    if (seconds >= 86400UL && (seconds % 86400UL) == 0)
    {
        *value_out = seconds / 86400UL;
        return "day";
    }
    if (seconds >= 3600UL && (seconds % 3600UL) == 0)
    {
        *value_out = seconds / 3600UL;
        return "hour";
    }
    if (seconds >= 60UL && (seconds % 60UL) == 0)
    {
        *value_out = seconds / 60UL;
        return "minute";
    }

    *value_out = seconds;
    return "second";
}

/* One line of the "it offered" block. The label is printed once for the whole
   block and the remaining lines align under it. */
static VOID offered_line(BOOL *first, const char *what, const ULONG *addr,
                         UWORD count, const char *text)
{
    UWORD i;
    char  buf[16];

    if (count == 0 && text == NULL)
        return;

    tool_printf("  %-11s %s ", (LONG)(*first ? "it offered" : ""), (LONG)what);
    *first = FALSE;

    if (text != NULL)
    {
        tool_printf("%s\n", (LONG)text);
        return;
    }

    for (i = 0; i < count; i++)
    {
        ami_config_format_ip(addr[i], buf, sizeof(buf));
        tool_printf("%s%s", (LONG)buf, (LONG)((i + 1 < count) ? ", " : ""));
    }

    tool_printf("\n");
}

/*
 * What the DHCP server said. The offered lists are shown whether or not they
 * were taken up: a server offering a name server this machine is not using
 * usually explains the symptom being investigated.
 */
static VOID show_lease(const ToolDhcpInfo *d)
{
    char addr[16];
    BOOL first = TRUE;

    if (d == NULL)
        return;

    if (d->state == NETSTATUS_DHCP_WORKING)
    {
        tool_printf("  lease       asking, no server has answered yet\n");
        return;
    }

    if (d->state != NETSTATUS_DHCP_BOUND)
        return;

    if (d->server != 0)
    {
        ami_config_format_ip(d->server, addr, sizeof(addr));
        tool_printf("  lease       from %s", (LONG)addr);
    }
    else
    {
        /* Rare: the server answered without identifying itself. Named rather
           than left blank. */
        tool_printf("  lease       from a server that did not name itself");
    }

    if (d->lease_seconds != 0)
    {
        ULONG       value = 0;
        const char *unit  = lease_duration(d->lease_seconds, &value);

        if (unit == NULL)
            tool_printf(", which never expires");
        else
            tool_printf(", good for %lu %s%s", (LONG)value, (LONG)unit,
                        (LONG)((value == 1) ? "" : "s"));
    }

    tool_printf("\n");

    offered_line(&first, "router     ", d->router, d->router_count, NULL);
    offered_line(&first, "name server", d->dns, d->dns_count, NULL);
    offered_line(&first, "routes     ", d->static_route,
                 d->static_route_count, NULL);

    if (d->domain_name[0] != '\0')
        offered_line(&first, "domain     ", NULL, 0, d->domain_name);

    if (d->host_name[0] != '\0')
        offered_line(&first, "the name   ", NULL, 0, d->host_name);
}

/* The interface's IPv6 addresses, under its IPv4 line. A machine with none
   prints nothing, so an IPv4-only stack reads exactly as it did. */
static VOID show_addresses6(const ToolSnapshot *snap, const ToolIfInfo *live)
{
    UWORD i;

    if (snap == NULL || live == NULL)
        return;

    for (i = 0; i < snap->addr6_count; i++)
    {
        const ToolAddr6Info *a6 = &snap->addr6[i];
        const char          *note;

        if (a6->nx_index != live->nx_index || a6->text[0] == '\0')
            continue;

        note = tool_addr6_state(a6->state);
        if (note == NULL)
            note = tool_addr6_origin(a6->origin);

        if (note != NULL)
            tool_printf("  address6    %s/%lu (%s)\n", (LONG)a6->text,
                        a6->prefix, (LONG)note);
        else
            tool_printf("  address6    %s/%lu\n", (LONG)a6->text, a6->prefix);
    }
}

/* The lease belonging to a live interface, joined on its NX slot rather than
   on any array position: the lease table is per slot and the descriptions are
   not. */
static const ToolDhcpInfo *lease_for(const ToolDhcp *dhcp,
                                     const ToolIfInfo *live)
{
    UWORD i;

    if (dhcp == NULL || live == NULL)
        return NULL;

    for (i = 0; i < dhcp->count && i < (UWORD)TOOL_MAX_IF; i++)
    {
        if (dhcp->iface[i].nx_index == live->nx_index)
            return &dhcp->iface[i];
    }

    return NULL;
}

static VOID show_interface(const AmiIfConfig *cfg, const ToolIfInfo *live,
                           const ToolSnapshot *snap,
                           const ToolDhcpInfo *lease,
                           const ToolDhcp6 *lease6,
                           BOOL up, BOOL stats, BOOL stack_running,
                           BOOL readable)
{
    char addr[AMI_CFG_NAME_LEN];
    char mask[16];
    char bcast[16];
    char mac[24];

    /*
     * The driver the stack has open, when it can be read, and the
     * configuration file only as a fallback: when the two differ the file is
     * the stale one.
     */
    if (live != NULL && live->nx_device[0] != '\0')
    {
        tool_printf("\nInterface %s (%s unit %ld)\n",
                    (LONG)cfg->name, (LONG)live->nx_device,
                    (LONG)live->nx_unit);

        if (cfg->device[0] != '\0' &&
            (tool_stricmp(cfg->device, live->nx_device) != 0 ||
             cfg->unit != live->nx_unit))
        {
            tool_printf("  NOTE        running on this driver. The "
                        "configuration file says\n");
            tool_printf("              %s unit %ld. The file was changed "
                        "after the\n", (LONG)cfg->device, (LONG)cfg->unit);
            tool_printf("              network started, or this interface "
                        "was brought up by hand.\n");
        }
    }
    else
    {
        tool_printf("\nInterface %s (%s unit %ld)\n",
                    (LONG)cfg->name, (LONG)cfg->device, (LONG)cfg->unit);
    }

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
         * The stack is up but inside another program's library, where no
         * per-interface flag can be read from here.
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

        /*
         * An IPv6-only interface has no IPv4 address and never will; three
         * zero addresses read as a fault.
         */
        if (live->address == 0UL &&
            !ami_config_iface_wants_ipv4(cfg))
        {
            tool_printf("  address     none, this interface carries no "
                        "IPv4\n");
        }
        else
        {
            tool_printf("  address     %-15s netmask %s (/%ld)\n",
                        (LONG)addr, (LONG)mask,
                        (LONG)tool_prefix_len(live->netmask));
            tool_printf("  broadcast   %s\n", (LONG)bcast);
        }
        tool_printf("  hardware    %s\n", (LONG)mac);
        /* What is running, not what the file asked for: an interface whose
           responder refused to start reads "no" here. */
        tool_printf("  mDNS        %s\n",
                    (LONG)(live->mdns ? "yes, answering .local"
                                      : "no"));
        tool_printf("  mtu         %lu bytes", live->mtu);
        if (live->bps != 0)
            tool_printf("        %lu bits/s", live->bps);
        tool_printf("\n");

        show_addresses6(snap, live);
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
                       cfg->iptype == AMI_IPTYPE_LINKLOCAL ? "link-local" :
                       cfg->iptype == AMI_IPTYPE_NONE      ? "no IPv4"
                                                           : "static"));

    /*
     * Printed only when there is something to say: the floor build always
     * reports OFF and would gain a line that means nothing there.
     */
    if (cfg->ip6type != AMI_IP6TYPE_OFF)
        tool_printf("  configured6 %s\n",
                    (LONG)(cfg->ip6type == AMI_IP6TYPE_DHCP      ? "DHCPv6" :
                           cfg->ip6type == AMI_IP6TYPE_STATIC    ? "static" :
                           cfg->ip6type == AMI_IP6TYPE_LINKLOCAL ? "link-local"
                                                                 : "auto"));

    show_lease(lease);

    /* The DHCPv6 lease is a table of its own, joined on the NX slot: this
       stack runs one DHCPv6 client, so at most one interface has a row. */
    if (live != NULL)
        tool_print_lease6(lease6, live->nx_index, "  lease6      ");

    if (stats)
    {
        if (live != NULL && live->have_sana2)
            show_counters(cfg->name, &live->stats);
        else
            tool_printf("\nSANA-II counters for %s are not available "
                        "(no driver attached)\n", (LONG)cfg->name);
    }
}

/*
 * INTERFACES: one line per interface, and whether it is switched on.
 *
 * link_up, not sana2_online: link_up (nx_interface_link_up) is the field Online
 * and Offline set. The SANA-II shim's online flag is a different layer.
 */
static BOOL iface_online(const ToolIfInfo *live)
{
    if (live == NULL || !live->attached)
        return FALSE;

    return live->link_up;
}

/* Whether a responder is running somewhere. The system flag beside it only says
   the library was built with mDNS; MDNS= is per interface and defaults to off. */
static BOOL mdns_answering(const ToolSnapshot *snap)
{
    UWORD i;

    for (i = 0; i < snap->iface_count; i++)
    {
        if (snap->iface[i].mdns)
            return TRUE;
    }

    return FALSE;
}

static VOID list_addresses6(const ToolSnapshot *snap, const ToolIfInfo *live)
{
    UWORD i;

    if (snap == NULL || live == NULL)
        return;

    for (i = 0; i < snap->addr6_count; i++)
    {
        const ToolAddr6Info *a6 = &snap->addr6[i];

        if (a6->nx_index != live->nx_index || a6->text[0] == '\0')
            continue;

        tool_printf("%-33s %s/%lu\n", (LONG)"", (LONG)a6->text, a6->prefix);
    }
}

/*
 * The IPv6 default router, as text, or NULL: the ::/0 entry of the routing
 * table. A route with no next hop is on-link and is not a router.
 */
static const char *default_router6(void)
{
    static ToolRoutes6 snr_routes6;
    UWORD              i;

    if (tool_routes6(&snr_routes6) != 0)
        return NULL;

    for (i = 0; i < snr_routes6.count; i++)
    {
        const ToolRoute6 *r = &snr_routes6.route[i];

        if (r->prefix != 0 || r->next_hop[0] == '\0')
            continue;

        if ((r->dest_words[0] | r->dest_words[1] |
             r->dest_words[2] | r->dest_words[3]) != 0UL)
            continue;

        return r->next_hop;
    }

    return NULL;
}

static VOID show_interface_list(const AmiConfig *cfg, const ToolSnapshot *snap,
                                BOOL have_live, BOOL readable)
{
    char  addr[AMI_CFG_NAME_LEN];
    UWORD i;

    tool_printf("\nInterfaces\n");
    tool_printf("Name            State    Link     Address\n");
    /* State: online (attached, link up), offline (attached, link down),
       defined (described in DEVS:NetInterfaces, not attached), ? (the
       running stack could not be read). */

    if (cfg->interface_count == 0)
    {
        tool_printf("(none configured)\n");
        return;
    }

    for (i = 0; i < cfg->interface_count; i++)
    {
        const ToolIfInfo *live = NULL;

        /* BY IDENTITY.  The row a description gets is the interface really
           running it, not the NX slot that happens to share its subscript --
           see tool_iface_live(). */
        if (have_live)
            live = tool_iface_live(snap, &cfg->interfaces[i]);

        if (live != NULL && live->attached)
            address_text(live->address, addr, sizeof(addr));
        else
            tool_copy_string(addr, sizeof(addr), "-");

        /*
         * Every definition is listed, and State says which of three things it
         * is. "defined" is a description that was never attached, which is not
         * the same as an interface that was up and was taken down.
         */
        tool_printf("%-15s %-8s %-8s %s\n",
                    (LONG)cfg->interfaces[i].name,
                    (LONG)(!readable ? "?" :
                           (live == NULL || !live->attached) ? "defined" :
                           iface_online(live) ? "online" : "offline"),
                    (LONG)(live != NULL && live->attached
                               ? (live->link_up ? "up" : "down") : "?"),
                    (LONG)addr);

        /* Indented under the line they belong to. A machine with no IPv6
           prints none and the table is unchanged. */
        list_addresses6(snap, live);
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
    static ToolRoutes     routes;
    static ToolRoutes6    routes6;
    static ToolDest6Table dest6;
    char                  gw[AMI_CFG_NAME_LEN];

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
            tool_printf("(the stack is not readable from here: only the "
                        "configuration)\n");
        }
        return;
    }

    /*
     * The live table from NETSTATUS_ROUTES rather than derived from the
     * interface list: with NX_ENABLE_IP_STATIC_ROUTING there are routes the
     * interface list does not show.
     */
    if (tool_routes(&routes) != 0)
        return;

    tool_print_routes(&routes, cfg, address_text);

    /* Prints nothing on a machine with no IPv6 routes. No NAMES here, because
       there is no reverse lookup for an IPv6 address in this report. */
    if (tool_routes6(&routes6) == 0)
        tool_print_routes6(&routes6, cfg);

    /*
     * The destination cache under the two lists it is resolved from: it is the
     * first table _nx_ipv6_packet_send() reads, and it is fixed-size.
     */
    if (tool_dest6(&dest6) == 0)
        tool_print_dest6(&dest6, cfg);
}

static VOID show_resolver(const AmiResolverConfig *r, BOOL from_files)
{
    char  addr[AMI_CFG_IP6_STRLEN];
    UWORD shown = 0;
    UWORD i;

    if (r->nameserver_count == 0 && r->nameserver6_count == 0)
    {
        tool_printf("\nName servers:   none configured\n");
    }
    else
    {
        for (i = 0; i < r->nameserver_count; i++)
        {
            ami_config_format_ip(r->nameserver[i], addr, sizeof(addr));
            tool_printf("%s%s\n",
                        (LONG)(shown++ == 0 ? "\nName servers:   " :
                                              "                "),
                        (LONG)addr);
        }

        /* A router advertisement's, RFC 8106.  Nothing on disk can name one:
           name_resolution's NAMESERVER line takes a dotted quad. */
        for (i = 0; i < r->nameserver6_count; i++)
        {
            ami_config_format_ip6(r->nameserver6[i], addr, sizeof(addr));
            tool_printf("%s%s\n",
                        (LONG)(shown++ == 0 ? "\nName servers:   " :
                                              "                "),
                        (LONG)addr);
        }
    }

    /*
     * Say where the list came from: a DHCP lease's name servers replace the
     * file's, so a list read off disk can name a server nothing is using.
     */
    if (from_files && r->nameserver_count > 0)
        tool_printf("                (from DEVS:Internet. A DHCP interface is\n"
                    "                given its own, which replace these)\n");
}

/*
 * What a name with no dot in it is looked up under. The domain comes from the
 * running stack when there is one, through GetDefaultDomainName(); the search
 * list below it is still the file's, and says so.
 */
static VOID show_search(const AmiResolverConfig *r)
{
    char  live[AMI_CFG_DOMAIN_LEN];
    UWORD i;

    if (tool_stack_domain(live, sizeof(live)) && live[0] != '\0')
        tool_printf("Domain:         %s\n", (LONG)live);
    else if (r->domain[0] != '\0')
        tool_printf("Domain:         %s (DEVS:Internet)\n", (LONG)r->domain);

    for (i = 0; i < r->search_count; i++)
    {
        tool_printf("%s%s\n",
                    (LONG)(i == 0 ? "Search:         " :
                                     "                "),
                    (LONG)r->search[i]);
    }
}

/*
 * Name servers, from the running stack when it can be asked: a DHCP lease
 * replaces whatever the file says. TRUE when at least one is in use.
 */
static BOOL show_dns(const AmiConfig *cfg, BOOL elsewhere, BOOL from_disk)
{
    (VOID)elsewhere;

    /*
     * Always ask the running stack first, whether or not the snapshot worked:
     * the snapshot does not carry name servers at all.
     * tool_stack_name_servers() returns 0 when nothing is running, so the file
     * remains the fallback.
     */
    {
        char  live_ns[TOOL_NAME_SERVERS_MAX][AMI_CFG_IP6_STRLEN];
        ULONG live_count =
            tool_stack_name_servers(live_ns, (ULONG)TOOL_NAME_SERVERS_MAX);

        if (live_count > 0)
        {
            ULONG n;

            for (n = 0; n < live_count; n++)
                tool_printf("%s%s\n",
                            (LONG)(n == 0 ? "\nName servers:   "
                                          : "                "),
                            (LONG)live_ns[n]);

            show_search(&cfg->resolver);

            return TRUE;
        }
    }

    show_resolver(&cfg->resolver, from_disk);
    show_search(&cfg->resolver);

    return (BOOL)(cfg->resolver.nameserver_count > 0);
}

/* ----------------------------------------------------- protocol counters, */

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

/* MEMORY is the packet pool, which is all the memory the network has here. A
   free count of zero is what a stall looks like from outside. */
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
    tool_printf("  payload each      %10lu    fewest ever free  %10lu\n",
                st->pool_payload, st->pool_low);
    tool_printf("  found empty       %10lu    waited for one    %10lu\n",
                st->pool_empty_requests, st->pool_empty_suspensions);
    tool_printf("  bad releases      %10lu\n", st->pool_invalid_releases);
}

/*
 * TCPSOCKETS / UDPSOCKETS. Without ALL only sockets with a peer are listed;
 * ALL adds every listener and every idle datagram socket.
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

        tool_printf("%s", (LONG)tool_tcp_state_name(s->state));

        /* Only when a retransmission has already fired with nothing coming
           back. netstat.c's show_stall() says why that is the gate. */
        if (s->retransmits != 0)
            tool_printf("  stalled %lus, %lu retx",
                        (LONG)(s->stalled_ms / 1000UL), (LONG)s->retransmits);

        tool_printf("\n");
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

/* ------------------------------------------------------------- diagnosis, */

/* Field values that are individually correct and collectively a machine that
   cannot reach anything. */
static VOID diagnose_interface(const AmiIfConfig *cfg, const ToolIfInfo *live,
                               const ToolSnapshot *snap,
                               BOOL up, BOOL stack_running, BOOL readable)
{
    /*
     * Only diagnose what can be seen: every check below reads live interface
     * state, and guessing it for an unreadable stack gives wrong advice.
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
        tool_printf("  * %s is described in DEVS:NetInterfaces but was never "
                    "attached, so it is not part of the running network.\n",
                    (LONG)cfg->name);
        tool_printf("    ShowNetStatus EVENTS names the call that refused; "
                    "if every interface slot is taken, RemoveNetInterface "
                    "frees one.\n");
        return;
    }

    if (!live->link_up)
    {
        problem_head();
        tool_printf("  * %s has no link: the card sees no network.\n",
                    (LONG)cfg->name);
        tool_printf("    Check the cable at both ends. Check that the device\n");
        tool_printf("    at the far end is switched on.\n");
    }

    /*
     * "No address" is a question about the machine, not about IPv4: an
     * IPv6-only interface has no IPv4 address by design.
     */
    if (tool_iface_has_address(snap, live))
        return;

    if (!ami_config_iface_wants_ipv4(cfg) &&
        ami_config_iface_wants_ipv6(cfg))
    {
        problem_head();
        tool_printf("  * %s carries IPv6 only and has no IPv6 address yet.\n",
                    (LONG)cfg->name);
        tool_printf("    Check the cable, and that a router on this network\n");
        tool_printf("    advertises IPv6. Even without one the link-local\n");
        tool_printf("    address should appear within a few seconds.\n");
        return;
    }

    {
        problem_head();
        tool_printf("  * %s has no address, so it cannot be used yet.\n",
                    (LONG)cfg->name);

        if (cfg->iptype == AMI_IPTYPE_DHCP)
        {
            tool_printf("    It is set to ask for one (DHCP) and nothing has\n");
            tool_printf("    answered. Check the cable. Check that something\n");
            tool_printf("    on this network hands out addresses. To use a\n");
            tool_printf("    fixed address instead, run NetSetup.\n");
        }
        else
        {
            tool_printf("    It is set to use a fixed address but the interface\n");
            tool_printf("    file has no ADDRESS line. Run NetSetup to set one.\n");
        }
    }
}

/* --------------------------------------------------------- the host name,
 *
 * ".local" is the mDNS suffix, and RFC 6762 3 makes a .local name link-local in
 * scope and explicitly not globally unique, so it is never this machine's name.
 * It is reported on its own line, and where the name came from is named too.
 */

/* The domain to show after the host name, or FALSE for none. */
static BOOL host_domain(const AmiConfig *cfg, const ToolDhcp *dhcp,
                        BOOL have_lease, char *out, ULONG outlen)
{
    UWORD i;

    /* The running stack first: a DHCP option 15 lands there and not on disk. */
    if (tool_stack_domain(out, outlen))
        return TRUE;

    if (cfg->resolver.domain[0] != '\0')
    {
        tool_copy_string(out, outlen, cfg->resolver.domain);
        return TRUE;
    }

    /* A lease whose domain the stack has not adopted, an older library. */
    for (i = 0; have_lease && i < (UWORD)dhcp->count; i++)
    {
        if (dhcp->iface[i].domain_name[0] != '\0')
        {
            tool_copy_string(out, outlen, dhcp->iface[i].domain_name);
            return TRUE;
        }
    }

    return FALSE;
}

static BOOL has_dot(const char *s)
{
    ULONG i;

    for (i = 0; s[i] != '\0'; i++)
    {
        if (s[i] == '.')
            return TRUE;
    }

    return FALSE;
}

static VOID show_host_name(const char *host, UWORD source,
                           const AmiConfig *cfg, const ToolDhcp *dhcp,
                           BOOL have_lease)
{
    const char *from = ami_config_hostname_source_text(source);
    char        domain[AMI_CFG_NAME_LEN];

    tool_printf("Host name:      %s", (LONG)host);

    /* Not when the name is already qualified, and never ".local". */
    if (!has_dot(host) &&
        host_domain(cfg, dhcp, have_lease, domain, sizeof(domain)) &&
        tool_stricmp(domain, "local") != 0)
        tool_printf(".%s", (LONG)domain);

    /* No source means nothing configured one, so the stack named the machine
       after its card's hardware address. `hostname` tells the two apart. */
    if (from != NULL)
        tool_printf(" (from %s)\n", (LONG)from);
    else
        tool_printf(" (derived)\n");
}

/* -------------------------------------------------------------- the run --- */

/* TRUE when `name` was given to INTERFACE. With no list, every interface
   matches, as the summary needs. */
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
 * The programs using the network, which nothing else on the Amiga reports.
 * Static: 16 rows is most of a Shell command's stack on its own.
 */
static struct
{
    NetStatusHeader hdr;
    NetStatusOpener e[16];
} sns_users;

static VOID show_users(BOOL stack_running)
{
    struct Library *base;
    LONG            n;
    LONG            i;

    tool_printf("\nPrograms using the network\n");

    if (!stack_running)
    {
        tool_printf("(the network is not running, so no program is using "
                    "it)\n");
        return;
    }

    base = tool_netstatus_open(TRUE);
    if (base == NULL)
        return;

    n = tool_netstatus_query(base, NETSTATUS_OPENERS, &sns_users,
                             sizeof(sns_users), sizeof(NetStatusOpener));
    if (n < 0)
    {
        tool_printf("(this bsdsocket.library does not report them)\n");
        tool_netstatus_close(base);
        return;
    }

    tool_printf("Name                             Sockets\n");

    for (i = 0; i < n && i < 16; i++)
    {
        const NetStatusOpener *o = &sns_users.e[i];
        const char            *name;

        /* This command opened the library to ask, so it is on its own list. */
        if (o->nso_Flags & NETSTATUS_OPENER_SELF)
            name = "ShowNetStatus (this command)";
        else if (o->nso_Name[0] != '\0')
            name = o->nso_Name;
        else
            name = "(unnamed)";

        tool_printf("%-32s %ld", (LONG)name, (LONG)o->nso_Sockets);

        if (o->nso_Flags & NETSTATUS_OPENER_GONE)
            tool_printf("   exited without closing the library");

        tool_printf("\n");
    }

    if (n == 0)
        tool_printf("(none)\n");
    if ((LONG)sns_users.hdr.nsh_Available > n)
        tool_printf("(list truncated at %ld of %ld)\n", (LONG)n,
                    (LONG)sns_users.hdr.nsh_Available);

    tool_netstatus_close(base);
}

/* ------------------------------------------------------------------ events,
 *
 * Read through the published mark, NOT NetStackQuery(): OpenLibrary() would
 * call netstack_startup() and restart the network. Copied whole under Forbid();
 * the semaphore is never obtained. Static: 32 rows exceed a Shell stack.
 */
static struct
{
    AmiEventMark   mark;
    NetStatusEvent e[32];
} sns_events;

#define SNS_EVENT_ROWS  (sizeof(sns_events.e) / sizeof(sns_events.e[0]))

#define SNS_EV_OK           0
#define SNS_EV_ABSENT       1
#define SNS_EV_BAD_VERSION  2

/* Oldest first, into sns_events.e. How many were copied, or -1 and *status. */
static LONG sns_events_read(UWORD *status)
{
    const AmiEventMark   *mark;
    const NetStatusEvent *ring;
    LONG                  copied = 0;

    *status = SNS_EV_ABSENT;

    Forbid();

    /* (STRPTR): NDK 3.9 declares FindSemaphore(STRPTR), 3.2 CONST_STRPTR. */
    mark = (const AmiEventMark *)FindSemaphore((STRPTR)AMI_EVENTS_NAME);

    if (mark != NULL && mark->em_Magic == AMI_EVENTS_MAGIC)
    {
        /*
         * The header's shape and one entry's, and not the whole object's size:
         * the ring's length is a build option.
         */
        if (mark->em_Version   == (UWORD)AMI_EVENTS_VERSION &&
            mark->em_Size      == (UWORD)sizeof(AmiEventMark) &&
            mark->em_EntrySize == (UWORD)sizeof(NetStatusEvent))
        {
            ULONG entries = (ULONG)mark->em_Entries;
            ULONG have;
            ULONG first;
            ULONG i;

            sns_events.mark = *mark;
            ring            = AMI_EVENTS_RING(mark);

            have  = (mark->em_Seq < entries) ? mark->em_Seq : entries;
            first = (mark->em_Seq <= entries) ? 0UL : mark->em_Next;

            for (i = 0; i < have && copied < (LONG)SNS_EVENT_ROWS; i++)
            {
                sns_events.e[copied] = ring[(first + i) % entries];
                copied++;
            }

            *status = SNS_EV_OK;
        }
        else
        {
            *status = SNS_EV_BAD_VERSION;
        }
    }

    Permit();

    return (*status == SNS_EV_OK) ? copied : -1;
}

static VOID show_events(VOID)
{
    UWORD status;
    LONG  n;
    LONG  i;

    tool_printf("\nWhat the network did\n");

    n = sns_events_read(&status);

    if (n < 0)
    {
        if (status == SNS_EV_BAD_VERSION)
            tool_printf("(bsdsocket.library keeps a record this command "
                        "cannot read; install the two together)\n");
        else
            tool_printf("(bsdsocket.library is not in memory, so its record "
                        "of what happened has gone with it)\n");
        return;
    }

    if (n == 0)
    {
        tool_printf("(nothing has happened worth recording)\n");
        return;
    }

    /* nse_Seq counts every event ever recorded, so the oldest one surviving in
       the ring having a sequence above 1 says how many went past. */
    if (sns_events.e[0].nse_Seq > 1UL)
        tool_printf("(the %ld events before these were overwritten)\n",
                    (LONG)(sns_events.e[0].nse_Seq - 1UL));

    for (i = 0; i < n; i++)
    {
        const NetStatusEvent *e    = &sns_events.e[i];
        const char           *text = tool_event_text(e->nse_Code);
        const char           *name;

        tool_printf("%6lu.%03lu  ", (unsigned long)(e->nse_Tick / 1000UL),
                    (unsigned long)(e->nse_Tick % 1000UL));

        if (e->nse_Index != (UWORD)NETEVENT_NOINDEX)
            tool_printf("interface %ld: ", (LONG)e->nse_Index);

        if (text == NULL)
        {
            /* A code from a newer library.  The number is still true. */
            tool_printf("event %ld, value %ld\n", (LONG)e->nse_Code,
                        (LONG)e->nse_Value);
            continue;
        }

        tool_printf("%s", (LONG)text);

        name = tool_event_value_name(e->nse_Code, e->nse_Value);
        if (name != NULL)
        {
            tool_printf(", %s", (LONG)name);
        }
        else
        {
            const char *detail = tool_event_detail(e->nse_Code);

            /* Signed: an OpenDevice error is negative and a count is small and
               positive, so one form reads correctly for both. */
            if (detail != NULL)
                tool_printf(", %s %ld", (LONG)detail, (LONG)e->nse_Value);
        }

        tool_printf("\n");
    }

    if ((LONG)sns_events.mark.em_Entries > n &&
        sns_events.mark.em_Seq > (ULONG)n)
        tool_printf("(list truncated at %ld)\n", (LONG)n);
}

/* One pass of the whole report. REPEAT calls it again every second, so nothing
   here allocates and everything it needs is passed in. */
static LONG report(const Wanted *w, const AmiConfig *cfg, BOOL from_disk)
{
    static ToolSnapshot snap;
    static ToolDhcp     dhcp;
    BOOL                have_lease = FALSE;
    static ToolDhcp6    dhcp6;
    BOOL                have_lease6 = FALSE;
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
    const char         *router6 = NULL;
    UWORD               i;
    UWORD               shown = 0;

    problem_count = 0;
    ext_host[0]   = '\0';

    want_sockets = (BOOL)(w->tcpsockets || w->udpsockets);

    /*
     * Two states: the stack runs inside bsdsocket.library and NetStackQuery()
     * reads it, or it is not running and the files on disk are all there is.
     */
    if (tool_stack_library_running())
    {
        stack_running = TRUE;
        (VOID)tool_stack_query(&ext_addr, ext_host, sizeof(ext_host));

        /*
         * Failure is silent here: the line saying the stack is up and cannot be
         * read is printed further down rather than in the middle of a table.
         */
        if (tool_snapshot(&snap, want_sockets) == 0)
            have_live = TRUE;

        if (tool_stats(&stats) == 0)
            have_stats = TRUE;

        /* Never fatal: a stack too old to know the selector has no lease
           detail to add. */
        if (tool_dhcp(&dhcp) == 0)
            have_lease = TRUE;

        /* The same, for the DHCPv6 client: an IPv4-only library answers the
           selector with no rows, which prints nothing. */
        if (tool_dhcp6(&dhcp6) == 0)
            have_lease6 = TRUE;

        /* Running, and this command could not see in. Say which. */
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

        /*
         * Which stack, and which build of it: C: and LIBS: are updated
         * separately, so the library in memory is the one worth reporting.
         */
        {
            char id[64];

            if (tool_stack_version(id, sizeof(id)))
                tool_printf("Stack version:  %s\n", (LONG)id);
        }

        /*
         * The running stack's name, and its account of where it came from: DHCP
         * can rename the machine after the files on disk were read.
         */
        if (ext_host[0] != '\0')
            show_host_name(ext_host,
                           (have_live &&
                            snap.host_source != (UWORD)AMI_HOSTNAME_NONE)
                               ? snap.host_source : cfg->hostname_source,
                           cfg, &dhcp, have_lease);
        else if (cfg->hostname[0] != '\0')
            show_host_name(cfg->hostname, cfg->hostname_source, cfg, &dhcp,
                           have_lease);

        /* The name other machines can use, which differs from the host name. */
        if (have_live && snap.have_mdns && mdns_answering(&snap))
        {
            if (snap.mdns_name[0] != '\0')
                tool_printf("Known here as:  %s (to other machines on this "
                            "network)\n", (LONG)snap.mdns_name);
            else
                tool_printf("Known here as:  nothing yet, still claiming a "
                            "name on this network\n");
        }

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
        else if (!have_live && cfg->default_gateway != 0)
        {
            /* The file is evidence only when the running route table cannot
               be read. Once a live snapshot says there is no IPv4 default,
               printing this stale value would claim a route the machine does
               not have. */
            address_text(cfg->default_gateway, addr, sizeof(addr));
            tool_printf("Default route:  %s (configured)\n", (LONG)addr);
        }
        else
        {
            /*
             * The IPv6 default router, before giving up: an IPv6-only
             * machine's only route off its own network is one a router
             * advertisement gave it.
             */
            router6 = default_router6();

            if (router6 != NULL)
                tool_printf("Default route:  %s (IPv6)\n", (LONG)router6);
            else
                tool_printf("Default route:  none\n");
        }
    }

    /* ---- the categories ------------------------------------------------ */

    if (w->interfaces)
        show_interface_list(cfg, &snap, have_live, (BOOL)!elsewhere);

    /*
     * Every interface in summary mode, or the ones INTERFACE named. The named
     * form is the detailed one and carries the SANA-II counters.
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
             * By identity, never by subscript: the drawer may hold more
             * descriptions than there are slots, and a card that did not open
             * moves everything behind it down one. The lease table is per slot.
             */
            if (have_live)
                live = tool_iface_live(&snap, &cfg->interfaces[i]);

            show_interface(&cfg->interfaces[i], live, &snap,
                           have_lease ? lease_for(&dhcp, live) : NULL,
                           have_lease6 ? &dhcp6 : NULL,
                           iface_online(live), detailed,
                           stack_running, (BOOL)!elsewhere);
            shown++;
        }

        if (shown == 0 && detailed)
        {
            tool_error("there is no interface called \"%s\"",
                       (LONG)tool_basename((const char *)w->interface[0]));

            if (cfg->interface_count > 0)
            {
                for (i = 0; i < cfg->interface_count; i++)
                    tool_printf("      %s\n", (LONG)cfg->interfaces[i].name);
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
    if (w->users)
        show_users(stack_running);

    /*
     * No stack_running argument: this one reads the published mark rather than
     * the library, so it answers after a shutdown.
     */
    if (w->events)
        show_events();

    /* ---- the diagnosis --------------------------------------------------
     *
     * Summary mode only, and all of it after the report, so "What to look at"
     * is one list in one place.
     */

    if (!w->summary)
        return RETURN_OK;

    for (i = 0; i < cfg->interface_count; i++)
    {
        const ToolIfInfo *live = NULL;

        if (have_live)
            live = tool_iface_live(&snap, &cfg->interfaces[i]);

        diagnose_interface(&cfg->interfaces[i], live, &snap,
                           iface_online(live), stack_running,
                           (BOOL)!elsewhere);
    }

    if (cfg->interface_count == 0)
    {
        /*
         * Nothing is set up. Adding the missing default route and the missing
         * name server would be three complaints about one fact.
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
        tool_printf("    Put that line in S:User-Startup to run it at every\n");
        tool_printf("    boot.\n");
    }

    /*
     * A DHCP interface gets a router and a name server with its address, so the
     * route and name-server checks run only when nothing else supplies them.
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
            if ((have_live && !snap.have_gateway && router6 == NULL) ||
                (!have_live && cfg->default_gateway == 0))
            {
                problem_head();
                tool_printf("  * There is no default route, so only machines "
                            "on this\n");
                tool_printf("    network can be reached, nothing beyond it.\n");
                tool_printf("    Run NetSetup and give it the router address, "
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
        tool_printf("    the configuration, not from the running stack. The\n");
        tool_printf("    stack is inside bsdsocket.library, which has no call\n");
        tool_printf("    yet that lets another command read it.\n");
    }

    if (problem_count == 0)
        tool_printf("\nNo problems found.\n");

    return RETURN_OK;
}

/*
 * The body, so that ami_netdb_free() has one place to run. atexit() is not free
 * here: libnix satisfies it out of an object that drags in about 7.7 KB of
 * malloc and stdio machinery nothing in this command calls.
 */
static int shownetstatus_main(int argc, char **argv);

int main(int argc, char **argv)
{
    int rc = shownetstatus_main(argc, argv);

    ami_netdb_free();
    return rc;
}

static int shownetstatus_main(int argc, char **argv)
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
    w.users      = (args[ARG_USERS]      != 0) ? TRUE : FALSE;
    w.events     = (args[ARG_EVENTS]     != 0) ? TRUE : FALSE;
    w.names      = (args[ARG_NAMES]      != 0) ? TRUE : FALSE;
    w.all        = (args[ARG_ALL]        != 0) ? TRUE : FALSE;
    repeat       = (args[ARG_REPEAT]     != 0) ? TRUE : FALSE;

    /*
     * ALL and NAMES modify the other categories rather than selecting one, so
     * neither counts here.
     */
    w.summary = (BOOL)!(w.interfaces || w.arp || w.routes || w.dns ||
                        w.icmp || w.ip || w.memory || w.tcp || w.udp ||
                        w.tcpsockets || w.udpsockets || w.users ||
                        (w.interface != NULL && w.interface[0] != NULL));

    names_prepare(w.names);

    /*
     * This command prints its own line for a stack that is up and cannot be
     * read. tool_snapshot()'s error block is for netstat.
     */
    tool_nx_quiet(TRUE);

    cfg = netstack_config();
    if (cfg == NULL)
    {
        /*
         * ami_config_load() is what the stack calls at startup, so this is the
         * same view it would get, including every complaint it would make.
         */
        from_disk = (AmiConfig *)ami_alloc((ULONG)sizeof(AmiConfig));
        if (from_disk == NULL)
        {
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
         * REPEAT: once a second until Ctrl-C, screen cleared first. Form feed is
         * what an Amiga console clears on.
         */
        if (tool_delay_ticks((ULONG)TICKS_PER_SECOND))
            break;

        tool_printf("\014");
    }

    if (from_disk != NULL)
    {
        /* The list first, then the struct that held it: ami_free() on the
           AmiConfig alone would drop the container and orphan interfaces[]. */
        ami_config_free(from_disk);
        ami_free(from_disk);
    }

    FreeArgs(rda);
    return (int)rc;
}
