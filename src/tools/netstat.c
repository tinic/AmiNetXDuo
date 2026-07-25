/*
 * netstat -- interfaces, routes and connections.
 *
 *     netstat INTERFACES=-i/S,ROUTES=-r/S,ALL=-a/S,STATS=-s/S
 *
 * Every switch carries its Unix spelling as a ReadArgs alias, so both
 * "netstat -r" (what a decade of Amiga documentation and install scripts
 * type) and "netstat ROUTES" (what the Shell's own conventions suggest) do
 * the same thing, and "netstat ?" prints a template that shows both.
 *
 * With no switches at all it prints everything, which is what someone typing
 * "netstat" on an Amiga is nearly always after.
 *
 * -s means the SANA-II per-interface counters rather than Unix's per-protocol
 * statistics: NetX Duo's protocol counters are a build option we do not turn
 * on, and the driver counters are the ones that answer "is the cable
 * plugged in".
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"

const char *const tool_name = "netstat";

static const char version_tag[] __attribute__((used)) =
    "$VER: netstat 1.0 (24.7.2026)";

#define TEMPLATE    "INTERFACES=-i/S,ROUTES=-r/S,ALL=-a/S,STATS=-s/S"

enum
{
    ARG_INTERFACES = 0,
    ARG_ROUTES,
    ARG_ALL,
    ARG_STATS,
    ARG_COUNT
};

static const char *iface_name(const AmiConfig *cfg, UWORD index)
{
    if (cfg != NULL && index < cfg->interface_count &&
        cfg->interfaces[index].name[0] != '\0')
    {
        return cfg->interfaces[index].name;
    }

    return "?";
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
                    (LONG)iface_name(cfg, i), info->mtu, (LONG)addr,
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
    }
}

static VOID show_routes(const AmiConfig *cfg, const ToolSnapshot *snap)
{
    char  dest[16];
    char  gw[16];
    char  mask[16];
    UWORD i;

    tool_printf("\nRouting table\n");
    tool_printf("Destination      Gateway          "
                "Netmask          Flags  Interface\n");

    if (snap->have_gateway)
    {
        ami_config_format_ip(snap->gateway, gw, sizeof(gw));
        tool_printf("%-16s %-16s %-16s %-6s %s\n",
                    (LONG)"default", (LONG)gw, (LONG)"0.0.0.0",
                    (LONG)"UG", (LONG)iface_name(cfg, 0));
    }

    for (i = 0; i < snap->iface_count; i++)
    {
        const ToolIfInfo *info = &snap->iface[i];

        if (!info->attached || info->address == 0)
            continue;

        ami_config_format_ip(info->address & info->netmask,
                             dest, sizeof(dest));
        ami_config_format_ip(info->netmask, mask, sizeof(mask));

        tool_printf("%-16s %-16s %-16s %-6s %s\n",
                    (LONG)dest, (LONG)"*", (LONG)mask,
                    (LONG)"U", (LONG)iface_name(cfg, i));
    }

    tool_printf("%-16s %-16s %-16s %-6s %s\n",
                (LONG)"127.0.0.0", (LONG)"*", (LONG)"255.0.0.0",
                (LONG)"U", (LONG)"lo0");
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
    NX_IP           *ip;
    ToolSnapshot     snap;
    BOOL             want_if;
    BOOL             want_routes;
    BOOL             want_conn;

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

    want_if     = (args[ARG_INTERFACES] != 0 || args[ARG_STATS] != 0) ? TRUE : FALSE;
    want_routes = (args[ARG_ROUTES] != 0) ? TRUE : FALSE;
    want_conn   = (args[ARG_ALL] != 0) ? TRUE : FALSE;

    if (!want_if && !want_routes && !want_conn)
        want_if = want_routes = want_conn = TRUE;

    if (tool_require_stack() == NULL)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    ip = netstack_ip();
    if (ip == NULL)
    {
        tool_error("the stack is running but has no IP instance");
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    if (tool_snapshot(ip, &snap, want_conn) != 0)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    cfg = netstack_config();

    if (want_if)
        show_interfaces(cfg, &snap);
    if (want_routes)
        show_routes(cfg, &snap);
    if (want_conn)
        show_connections(&snap);

    FreeArgs(rda);
    return RETURN_OK;
}
