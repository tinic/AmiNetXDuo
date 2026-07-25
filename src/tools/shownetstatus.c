/*
 * ShowNetStatus -- what the stack thinks the network looks like, and what is
 * wrong with it.
 *
 *     ShowNetStatus INTERFACE/K,STATS/S,ALL/S
 *
 * This is the command someone types when the network does not work, so it has
 * two jobs. The first is the report: the parsed Roadshow configuration
 * (netstack_config(), or the files themselves when nothing is running) for
 * the intent, and the live NX_IP plus the SANA-II shim for the reality. Where
 * they disagree -- a DHCP lease that differs from the static address in the
 * file, say -- the live values are the ones shown.
 *
 * The second job is the diagnosis at the end. An interface that is down, an
 * interface with no address, a missing default route and a missing name
 * server are all states where every field printed above is individually
 * correct and the machine still cannot reach anything; each one is called out
 * as a problem with the command that fixes it.
 *
 * It deliberately works with the stack down. That is the state that most
 * needs explaining, and refusing to say anything until the network works is
 * exactly backwards.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools_nx.h"

const char *const tool_name = "ShowNetStatus";

static const char version_tag[] __attribute__((used)) =
    "$VER: ShowNetStatus 1.1 (25.7.2026)";

#define TEMPLATE    "INTERFACE/K,STATS/S,ALL/S"

enum
{
    ARG_INTERFACE = 0,
    ARG_STATS,
    ARG_ALL,
    ARG_COUNT
};

/* ------------------------------------------------------------- diagnosis -- */

static UWORD problem_count;

static VOID problem_head(VOID)
{
    if (problem_count++ == 0)
        tool_printf("\nWhat to look at\n");
}

/* ---------------------------------------------------------------- report -- */

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
    char addr[16];
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
    else if (cfg->iptype == AMI_IPTYPE_DHCP)
    {
        tool_printf("  address     handed out by DHCP when the interface "
                    "comes up\n");
    }
    else
    {
        ami_config_format_ip(cfg->address, addr, sizeof(addr));
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

int main(int argc, char **argv)
{
    LONG             args[ARG_COUNT];
    struct RDArgs   *rda;
    const AmiConfig *cfg;
    AmiConfig       *from_disk = NULL;
    NX_IP           *ip;
    ToolSnapshot     snap;
    const char      *only;
    BOOL             stats;
    BOOL             have_live     = FALSE;
    BOOL             stack_running = FALSE;
    BOOL             elsewhere     = FALSE;
    BOOL             have_ns       = FALSE;
    ULONG            ext_addr      = 0;
    char             ext_host[AMI_CFG_NAME_LEN];
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

    ext_host[0] = '\0';

    /*
     * Three states, not two. The stack may be linked into this command (then
     * everything is readable), running inside bsdsocket.library (then only
     * what its vectors will tell an outsider is readable), or not running at
     * all (then the files on disk are all there is -- and that is exactly the
     * case a beginner is in).
     */
    if (netstack_get() != NULL)
    {
        stack_running = TRUE;
    }
    else if (tool_stack_library_running())
    {
        stack_running = TRUE;
        elsewhere     = TRUE;
        (VOID)tool_stack_query(&ext_addr, ext_host, sizeof(ext_host));
    }

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
            tool_error("out of memory");
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        tool_config_watch();
        (VOID)ami_config_load(from_disk);
        tool_config_unwatch();

        cfg = from_disk;
    }

    ip = netstack_ip();
    if (ip != NULL && tool_snapshot(ip, &snap, FALSE) == 0)
        have_live = TRUE;

    /* ---- the report ---------------------------------------------------- */

    tool_printf("\nNetwork stack:  %s\n",
                (LONG)(stack_running ? "running" : "not started"));

    if (ext_host[0] != '\0')
        tool_printf("Host name:      %s\n", (LONG)ext_host);
    else if (cfg->hostname[0] != '\0')
        tool_printf("Host name:      %s\n", (LONG)cfg->hostname);

    if (elsewhere && ext_addr != 0)
    {
        ami_config_format_ip(ext_addr, addr, sizeof(addr));
        tool_printf("This machine:   %s\n", (LONG)addr);
    }

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
        BOOL              up;

        if (only != NULL)
        {
            if (tool_stricmp(cfg->interfaces[i].name, tool_basename(only)) != 0)
                continue;
        }

        /*
         * Interface handles are "index 0..count-1 in config order"
         * (aminetxduo/netstack.h) and the stack attaches them to NX_IP in the
         * same order, so the two indices line up.
         */
        if (have_live && i < snap.iface_count)
            live = &snap.iface[i];

        up = netstack_interface_is_up(i);

        show_interface(&cfg->interfaces[i], live, up, stats, stack_running,
                       (BOOL)!elsewhere);
        shown++;
    }

    if (shown == 0 && only != NULL)
    {
        tool_error("there is no interface called \"%s\"", (LONG)only);

        if (cfg->interface_count > 0)
        {
            tool_advise_blank();
            tool_advise("The interfaces this machine has are:");
            for (i = 0; i < cfg->interface_count; i++)
                tool_printf("      %s\n", (LONG)cfg->interfaces[i].name);
        }

        if (from_disk != NULL)
            ami_free(from_disk);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * Name servers, from the running stack when it can be asked. A DHCP lease
     * replaces whatever the file says, so reading the file alone reports a
     * server the machine is not using -- or "none configured" on a machine
     * whose lookups work perfectly.
     */
    if (elsewhere)
    {
        char  live_ns[AMI_CFG_MAX_NAMESERVERS][16];
        ULONG live_count = tool_stack_name_servers(live_ns,
                                                   (ULONG)AMI_CFG_MAX_NAMESERVERS);

        if (live_count > 0)
        {
            ULONG n;

            for (n = 0; n < live_count; n++)
                tool_printf("%s%s\n",
                            (LONG)(n == 0 ? "\nName servers:   "
                                          : "                "),
                            (LONG)live_ns[n]);
            have_ns = TRUE;
        }
        else
        {
            show_resolver(&cfg->resolver, TRUE);
            have_ns = (BOOL)(cfg->resolver.nameserver_count > 0);
        }
    }
    else
    {
        show_resolver(&cfg->resolver, (BOOL)(from_disk != NULL));
        have_ns = (BOOL)(cfg->resolver.nameserver_count > 0);
    }

    /* ---- the diagnosis --------------------------------------------------
     *
     * All of it after the report, never interleaved with it: "What to look
     * at" has to be one list in one place, or the reader has to reassemble it
     * from between the interface blocks.
     */

    for (i = 0; i < cfg->interface_count; i++)
    {
        const ToolIfInfo *live = NULL;

        if (only != NULL &&
            tool_stricmp(cfg->interfaces[i].name, tool_basename(only)) != 0)
            continue;

        if (have_live && i < snap.iface_count)
            live = &snap.iface[i];

        diagnose_interface(&cfg->interfaces[i], live,
                           netstack_interface_is_up(i), stack_running,
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

        if (from_disk != NULL)
            ami_free(from_disk);
        FreeArgs(rda);
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

    if (from_disk != NULL)
        ami_free(from_disk);

    FreeArgs(rda);
    return RETURN_OK;
}
