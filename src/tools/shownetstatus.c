/*
 * ShowNetStatus -- what the stack thinks the network looks like.
 *
 *     ShowNetStatus INTERFACE/K,STATS/S,ALL/S
 *
 * Everything comes from two places: the parsed Roadshow configuration
 * (netstack_config()) for the intent, and the live NX_IP plus the SANA-II
 * shim for the reality. Where they disagree -- a DHCP lease that differs from
 * the static address in the file, say -- the live values are the ones shown.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"

const char *const tool_name = "ShowNetStatus";

static const char version_tag[] __attribute__((used)) =
    "$VER: ShowNetStatus 1.0 (24.7.2026)";

#define TEMPLATE    "INTERFACE/K,STATS/S,ALL/S"

enum
{
    ARG_INTERFACE = 0,
    ARG_STATS,
    ARG_ALL,
    ARG_COUNT
};

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
                           BOOL up, BOOL stats)
{
    char addr[16];
    char mask[16];
    char bcast[16];
    char mac[24];

    tool_printf("\nInterface %s (%s unit %ld)\n",
                (LONG)cfg->name, (LONG)cfg->device, (LONG)cfg->unit);

    tool_printf("  state       %-10s      link %s\n",
                (LONG)(up ? "online" : "offline"),
                (LONG)(live != NULL && live->attached
                           ? (live->link_up ? "up" : "down")
                           : "unknown"));

    if (live != NULL && live->attached)
    {
        ami_config_format_ip(live->address, addr, sizeof(addr));
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
    else
    {
        ami_config_format_ip(cfg->address, addr, sizeof(addr));
        ami_config_format_ip(cfg->netmask, mask, sizeof(mask));
        tool_printf("  address     %-15s netmask %s  (from the config file;"
                    " the interface is not attached)\n",
                    (LONG)addr, (LONG)mask);
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

static VOID show_resolver(const AmiResolverConfig *r)
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
}

int main(int argc, char **argv)
{
    LONG             args[ARG_COUNT];
    struct RDArgs   *rda;
    const AmiConfig *cfg;
    NX_IP           *ip;
    ToolSnapshot     snap;
    const char      *only;
    BOOL             stats;
    BOOL             have_live = FALSE;
    char             addr[16];
    UWORD            i;
    UWORD            shown = 0;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    args[ARG_INTERFACE] = 0;
    args[ARG_STATS]     = 0;
    args[ARG_ALL]       = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    only  = (const char *)args[ARG_INTERFACE];
    stats = (args[ARG_STATS] != 0 || args[ARG_ALL] != 0) ? TRUE : FALSE;

    if (tool_require_stack() == NULL)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    cfg = netstack_config();
    if (cfg == NULL)
    {
        tool_error("the stack is running but has no configuration");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    ip = netstack_ip();
    if (ip != NULL && tool_snapshot(ip, &snap, FALSE) == 0)
        have_live = TRUE;

    tool_printf("Network stack:  running\n");
    if (cfg->hostname[0] != '\0')
        tool_printf("Host name:      %s\n", (LONG)cfg->hostname);

    if (have_live && snap.have_gateway)
    {
        ami_config_format_ip(snap.gateway, addr, sizeof(addr));
        tool_printf("Default route:  %s\n", (LONG)addr);
    }
    else if (cfg->default_gateway != 0)
    {
        ami_config_format_ip(cfg->default_gateway, addr, sizeof(addr));
        tool_printf("Default route:  %s (configured)\n", (LONG)addr);
    }
    else
    {
        tool_printf("Default route:  none\n");
    }

    for (i = 0; i < cfg->interface_count; i++)
    {
        const ToolIfInfo *live = NULL;

        if (only != NULL)
        {
            const char *a = cfg->interfaces[i].name;
            const char *b = tool_basename(only);
            BOOL        match = TRUE;

            while (*a != '\0' || *b != '\0')
            {
                char ca = *a++;
                char cb = *b++;

                if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
                if (ca != cb) { match = FALSE; break; }
            }

            if (!match)
                continue;
        }

        /*
         * Interface handles are "index 0..count-1 in config order"
         * (aminetxduo/netstack.h) and the stack attaches them to NX_IP in the
         * same order, so the two indices line up.
         */
        if (have_live && i < snap.iface_count)
            live = &snap.iface[i];

        show_interface(&cfg->interfaces[i], live,
                       netstack_interface_is_up(i), stats);
        shown++;
    }

    if (shown == 0)
    {
        if (only != NULL)
        {
            tool_error("no interface called \"%s\"", (LONG)only);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        tool_printf("\nNo interfaces are configured.\n");
    }

    show_resolver(&cfg->resolver);

    FreeArgs(rda);
    return RETURN_OK;
}
