/*
 * AddNetRoute / DeleteNetRoute -- routes for packets that are not for this
 * network.
 *
 *     AddNetRoute    QUIET/S,DST=DESTINATION/K,HOSTDST=HOSTDESTINATION/K,
 *                    NETDST=NETDESTINATION/K,VIA=GATEWAY/K,
 *                    DEFAULT=DEFAULTGATEWAY/K
 *     DeleteNetRoute QUIET/S,DST=DESTINATION/K,DEFAULT=DEFAULTGATEWAY/K
 *
 * One source, two executables: TOOL_DELETE picks which. They share the address
 * parsing, the name lookup, the netmask rules and the messages, so the pair
 * cannot disagree about what "192.168.10.0" means.
 *
 * These needed a stack change first. NetX Duo's nx_ip_static_route_add()
 * compiles to a stub returning NX_NOT_SUPPORTED without
 * NX_ENABLE_IP_STATIC_ROUTING, and NX_IP then carries no routing table.
 * port/netxduo-amiga/inc/nx_user.h set NX_IP_ROUTING_TABLE_SIZE but not the
 * enable, so there was nothing to add to and NETCTRL_ROUTE_ADD answered
 * ENOSYS. docs/RESEARCH.md 22.5.
 *
 * Two kinds of route, on different mechanisms:
 *
 *   DEFAULTGATEWAY is the default route: everything with nowhere better to go.
 *   NetX Duo keeps it in nx_ip_gateway_address, present in every build of this
 *   stack, so this half works even where the other does not.
 *
 *   DESTINATION / HOSTDESTINATION / NETDESTINATION with GATEWAY add an entry
 *   to the static routing table, which NetX Duo consults before the gateway
 *   and matches longest prefix first, so a route can override the default for
 *   part of the address space -- what a subnet behind a second router needs,
 *   and not expressible as a gateway. NETSTATUS_SYS_ROUTING says which build
 *   this is, and is asked before anything is attempted so the command refuses
 *   rather than quietly doing nothing.
 *
 * The GATEWAY must be an address on one of this machine's own subnets. NetX
 * Duo derives the outgoing interface from the next hop
 * (nx_ip_static_route_add.c), so a next hop it cannot reach directly is
 * rejected rather than stored; the command says so instead of printing
 * "invalid argument".
 *
 * Netmasks are inferred, because the template has nowhere to write one:
 *
 *   HOSTDESTINATION       one machine: /32, whatever the address looks like
 *   NETDESTINATION        the mask covering the octets that are not zero, so
 *                         10.0.0.0 is /8, 172.16.0.0 is /16, 192.168.1.0 is
 *                         /24. An address with a non-zero last octet is not a
 *                         network and is refused rather than guessed at.
 *   DESTINATION           whichever of the two the address looks like.
 *
 * A prefix length written into the address -- 10.1.2.0/24 -- overrides all of
 * that. It is not a new keyword, so the template is still Roadshow's.
 *
 * DeleteNetRoute infers nothing. It reads the live routing table and deletes
 * the entry whose destination matches, with the netmask that entry really has,
 * so a route added with any mask can be removed by naming where it goes.
 *
 * A destination may be a name as well as an address. DEVS:Internet/hosts is
 * consulted first because it needs no network and cannot time out; the running
 * stack's resolver is asked only if that fails.
 *
 * Nothing here is persistent: this is the live table, as Online and Offline
 * are the live interface state. A route that should survive a reboot belongs
 * in S:User-Startup next to the AddNetInterface line.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"

#ifdef TOOL_DELETE
const char *const tool_name = "DeleteNetRoute";

static const char version_tag[] __attribute__((used)) =
    "$VER: DeleteNetRoute 1.0 (26.7.2026)";

#define TEMPLATE    "QUIET/S,DST=DESTINATION/K,DEFAULT=DEFAULTGATEWAY/K"

enum
{
    ARG_QUIET = 0,
    ARG_DST,
    ARG_DEFAULT,
    ARG_COUNT
};
#else
const char *const tool_name = "AddNetRoute";

static const char version_tag[] __attribute__((used)) =
    "$VER: AddNetRoute 1.0 (26.7.2026)";

#define TEMPLATE    "QUIET/S,DST=DESTINATION/K,HOSTDST=HOSTDESTINATION/K," \
                    "NETDST=NETDESTINATION/K,VIA=GATEWAY/K," \
                    "DEFAULT=DEFAULTGATEWAY/K"

enum
{
    ARG_QUIET = 0,
    ARG_DST,
    ARG_HOSTDST,
    ARG_NETDST,
    ARG_VIA,
    ARG_DEFAULT,
    ARG_COUNT
};
#endif

/* The errno numbers this command names, as bsdsocket.library reports them
   (src/bsdsocket/bsdsocket_internal.h). */
#define ROUTE_ENOENT        2
#define ROUTE_EINVAL       22
#define ROUTE_ENOBUFS      55
#define ROUTE_ENOSYS       78

/* As many routes as the stack can report: one per interface, the static
   table, and the default gateway. */
#define NR_MAX_ROUTES   (NX_MAX_PHYSICAL_INTERFACES + NX_IP_ROUTING_TABLE_SIZE + 1)

/* ---------------------------------------------------------------- output -- */

static BOOL nr_quiet;

static VOID say(const char *fmt, ...)
{
    va_list args;

    if (nr_quiet)
        return;

    va_start(args, fmt);
    VPrintf((CONST_STRPTR)fmt, (APTR)args);         /* (APTR): see tool_util.c */
    va_end(args);
}

/* Static: three tables that between them are more than a 4 KB stack holds. */
static union
{
    struct { NetStatusHeader hdr; NetStatusSystem    e; } system;
    struct { NetStatusHeader hdr; NetStatusRoute     e[NR_MAX_ROUTES]; } route;
    struct { NetStatusHeader hdr; NetStatusInterface e[NX_MAX_PHYSICAL_INTERFACES]; } iface;
} nr_answer;

/* ------------------------------------------------------------- addresses -- */

/*
 * An address, an address with a prefix length, or a name. The local hosts file
 * is tried before the resolver: it needs no network, while a lookup costs
 * BSD_RESOLVE_TIMEOUT per name server against one that need not answer
 * (docs/RESEARCH.md 22.8), so a typo would cost thirty seconds.
 */
static BOOL resolve_address(const char *text, ULONG *addr, ULONG *mask,
                            BOOL *have_mask)
{
    const AmiNetdbEntry *entry;
    char                 copy[64];
    ULONG                i     = 0;
    LONG                 slash = -1;

    if (have_mask != NULL)
        *have_mask = FALSE;

    if (text == NULL || *text == '\0')
        return FALSE;

    while (text[i] != '\0' && i + 1 < (ULONG)sizeof(copy))
    {
        if (text[i] == '/')
            slash = (LONG)i;
        copy[i] = text[i];
        i++;
    }
    if (text[i] != '\0')
        return FALSE;                   /* far longer than any address or name */
    copy[i] = '\0';

    if (slash >= 0)
    {
        ULONG bits = 0;
        ULONG j;

        if (mask == NULL || have_mask == NULL)
            return FALSE;               /* a gateway has no prefix length */

        copy[slash] = '\0';

        if (copy[slash + 1] == '\0')
            return FALSE;

        for (j = (ULONG)slash + 1; copy[j] != '\0'; j++)
        {
            if (copy[j] < '0' || copy[j] > '9')
                return FALSE;
            bits = bits * 10UL + (ULONG)(copy[j] - '0');
            if (bits > 32UL)
                return FALSE;
        }

        *mask      = (bits == 0) ? 0UL : (0xFFFFFFFFUL << (32UL - bits));
        *have_mask = TRUE;
    }

    if (ami_config_parse_ip(copy, addr))
        return TRUE;

    (VOID)ami_netdb_load();

    entry = ami_netdb_host_by_name(copy);
    if (entry != NULL)
    {
        *addr = entry->value;
        return TRUE;
    }

    return tool_stack_lookup(copy, addr);
}

static VOID explain_bad_address(const char *what, const char *text)
{
    tool_error("%s: \"%s\" is not an address this command can use",
               (LONG)what, (LONG)text);
    tool_advise_blank();
    tool_advise("Write it as four numbers with dots between them, for");
    tool_advise("example 192.168.1.1, optionally with a prefix length after a");
    tool_advise("slash. A name works too, if it is in DEVS:Internet/hosts or");
    tool_advise("the name servers know it.");
}

#ifndef TOOL_DELETE
/*
 * The mask covering the octets of `addr` that are not zero. 0.0.0.0 has none
 * and answers /0, the default route, which is reached through the DEFAULT
 * keyword instead.
 *
 * Add half only: DeleteNetRoute takes the mask out of the live table, and a
 * guess here could disagree with it.
 */
static ULONG mask_for_network(ULONG addr)
{
    if (addr == 0)
        return 0;
    if ((addr & 0x00ffffffUL) == 0)
        return 0xff000000UL;
    if ((addr & 0x0000ffffUL) == 0)
        return 0xffff0000UL;
    if ((addr & 0x000000ffUL) == 0)
        return 0xffffff00UL;

    return 0xffffffffUL;
}
#endif /* !TOOL_DELETE */

/* "192.168.10.0/24", or a bare address when the mask is /32. */
static VOID format_route(ULONG dest, ULONG mask, char *buf, ULONG buflen)
{
    char  text[16];
    ULONG pos = 0;
    UWORD bits;

    ami_config_format_ip(dest, text, sizeof(text));
    tool_copy_string(buf, buflen, text);

    while (buf[pos] != '\0')
        pos++;

    bits = tool_prefix_len(mask);

    if (bits == 32 || pos + 4 >= buflen)
        return;

    buf[pos++] = '/';
    if (bits >= 10)
        buf[pos++] = (char)('0' + (bits / 10));
    buf[pos++] = (char)('0' + (bits % 10));
    buf[pos]   = '\0';
}

/* ----------------------------------------------------------- the library -- */

static VOID zero_control(NetStatusControl *ctl)
{
    ULONG i;

    for (i = 0; i < (ULONG)(sizeof(*ctl) / sizeof(ULONG)); i++)
        ((ULONG *)ctl)[i] = 0;
}

/* The running stack's default gateway, or 0; *routing_out says whether the
   static routing table is in this build at all. */
static ULONG current_gateway(struct Library *base, BOOL *routing_out)
{
    if (routing_out != NULL)
        *routing_out = FALSE;

    if (tool_netstatus_query(base, NETSTATUS_SYSTEM, &nr_answer,
                             sizeof(nr_answer.system),
                             sizeof(NetStatusSystem)) <= 0)
    {
        return 0;
    }

    if (routing_out != NULL)
    {
        *routing_out = (nr_answer.system.e.nss_Flags & NETSTATUS_SYS_ROUTING)
                           ? TRUE : FALSE;
    }

    if (!(nr_answer.system.e.nss_Flags & NETSTATUS_SYS_GATEWAY))
        return 0;

    return nr_answer.system.e.nss_Gateway;
}

/*
 * The live route `dest` falls in, or -1. Matched with each route's own mask
 * rather than by equality, so naming either the network (192.168.77.0) or a
 * machine on it (192.168.77.5) finds the route that carries it.
 */
static LONG find_route(struct Library *base, ULONG dest, ULONG *mask_out)
{
    LONG n;
    LONG i;

    n = tool_netstatus_query(base, NETSTATUS_ROUTES, &nr_answer,
                             sizeof(nr_answer.route), sizeof(NetStatusRoute));
    if (n <= 0)
        return -1;

    for (i = 0; i < n; i++)
    {
        const NetStatusRoute *r = &nr_answer.route.e[i];

        if (r->nsr_NetMask == 0)
            continue;               /* the default route: DEFAULT deletes it */

        /*
         * Static entries only. An interface's directly-attached prefix is a
         * real route and netstat -r prints it, but it belongs to the
         * interface's address and nx_ip_static_route_delete() cannot remove
         * it, so matching one here would produce an unexplained failure from
         * the stack instead of "that is not yours to delete".
         */
        if (!(r->nsr_Flags & NETSTATUS_RT_STATIC))
            continue;

        if ((dest & r->nsr_NetMask) != r->nsr_Destination)
            continue;

        if (mask_out != NULL)
            *mask_out = r->nsr_NetMask;

        return i;
    }

    return -1;
}

#ifndef TOOL_DELETE
/*
 * TRUE when `addr` is on the network of an interface the stack has up, which a
 * next hop has to be. Checked before the call so the answer is "your router is
 * not reachable" rather than EINVAL. Add half only; nothing on the delete path
 * takes a next hop.
 */
static BOOL gateway_is_reachable(struct Library *base, ULONG addr)
{
    LONG n;
    LONG i;

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &nr_answer,
                             sizeof(nr_answer.iface),
                             sizeof(NetStatusInterface));
    if (n <= 0)
        return TRUE;                /* cannot tell; assume reachable */

    for (i = 0; i < n; i++)
    {
        const NetStatusInterface *nsi = &nr_answer.iface.e[i];

        if (!(nsi->nsi_Flags & NETSTATUS_IF_ATTACHED))
            continue;
        if (nsi->nsi_Address == 0 || nsi->nsi_NetMask == 0)
            continue;

        if ((nsi->nsi_Address & nsi->nsi_NetMask) == (addr & nsi->nsi_NetMask))
            return TRUE;
    }

    return FALSE;
}
#endif /* !TOOL_DELETE */

/*
 * The stack said no. Each of these is actionable, so it is spelled out rather
 * than mapped to a DOS error code.
 */
static VOID explain(LONG err, ULONG gateway)
{
    char addr[16];

    switch (err)
    {
        case ROUTE_ENOSYS:
            tool_advise_blank();
            tool_advise("The running stack was built without its routing");
            tool_advise("table, so there is nothing to add to. That is a build");
            tool_advise("option (NX_ENABLE_IP_STATIC_ROUTING) and not anything");
            tool_advise("that can be switched on from here.");
            tool_advise_blank();
            tool_advise("A default route still works:");
            tool_advise("   AddNetRoute DEFAULTGATEWAY=<your router>");
            break;

        case ROUTE_ENOBUFS:
            tool_advise_blank();
            tool_advise("The routing table is full. Delete a route before");
            tool_advise("adding another -- run  netstat -r  to see them.");
            break;

        case ROUTE_ENOENT:
            tool_advise_blank();
            tool_advise("Run  netstat -r  to see the routes there are. The");
            tool_advise("ones marked S were added by hand and are the ones");
            tool_advise("this command can remove; a directly-attached network");
            tool_advise("goes away when its interface does, and the default");
            tool_advise("route is removed with DEFAULTGATEWAY.");
            break;

        case ROUTE_EINVAL:
            tool_advise_blank();
            if (gateway != 0)
            {
                ami_config_format_ip(gateway, addr, sizeof(addr));
                tool_printf("  %s is not on any of this machine's own\n",
                            (LONG)addr);
                tool_advise("networks, so nothing here can reach it to use it");
                tool_advise("as a next hop. A gateway has to be an address you");
                tool_advise("could talk to directly -- usually a router on the");
                tool_advise("same Ethernet. Run  netstat -i  for the addresses");
                tool_advise("this machine has.");
            }
            else
            {
                tool_advise("The stack would not accept that route.");
            }
            break;

        default:
            break;
    }
}

static VOID usage(VOID)
{
#ifdef TOOL_DELETE
    tool_usage("[DESTINATION <address>] [DEFAULTGATEWAY <address>] [QUIET]",
               "Remove a route this machine is using.");
#else
    tool_usage("[DESTINATION <address> GATEWAY <address>] "
               "[DEFAULTGATEWAY <address>] [QUIET]",
               "Add a route, or set the address everything else goes to.");
#endif
}

/* --------------------------------------------------------------- the run -- */

int main(int argc, char **argv)
{
    LONG             args[ARG_COUNT];
    struct RDArgs   *rda;
    struct Library  *base;
    NetStatusControl ctl;
    char             text[24];
    ULONG            dest    = 0;
    ULONG            mask    = 0;
    ULONG            gateway = 0;
    ULONG            live_gw;
    BOOL             have_mask = FALSE;
    BOOL             routing   = FALSE;
    BOOL             have_default;
    BOOL             have_dest;
    LONG             err = 0;
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
        usage();
        return RETURN_ERROR;
    }

    nr_quiet = (args[ARG_QUIET] != 0) ? TRUE : FALSE;

    have_default = (args[ARG_DEFAULT] != 0) ? TRUE : FALSE;
    have_dest    = (args[ARG_DST] != 0) ? TRUE : FALSE;
#ifndef TOOL_DELETE
    if (args[ARG_HOSTDST] != 0 || args[ARG_NETDST] != 0)
        have_dest = TRUE;
#endif

    if (!have_default && !have_dest)
    {
        tool_error("nothing was asked for");
        usage();
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /* DEFAULTGATEWAY wins outright; say so rather than silently ignoring the
       destination. */
    if (have_default && have_dest)
        say("%s: DEFAULTGATEWAY was given, so the destination is ignored.\n",
            (LONG)tool_name);

    /* ---- what was asked for --------------------------------------------- */

    if (have_default)
    {
        if (!resolve_address((const char *)args[ARG_DEFAULT], &gateway,
                             NULL, NULL))
        {
            explain_bad_address("DEFAULTGATEWAY",
                                (const char *)args[ARG_DEFAULT]);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }
    else
    {
        const char *given;
#ifdef TOOL_DELETE
        given = (const char *)args[ARG_DST];

        if (!resolve_address(given, &dest, &mask, &have_mask))
        {
            explain_bad_address("DESTINATION", given);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
#else
        if (args[ARG_HOSTDST] != 0)
        {
            given = (const char *)args[ARG_HOSTDST];

            if (!resolve_address(given, &dest, &mask, &have_mask))
            {
                explain_bad_address("HOSTDESTINATION", given);
                FreeArgs(rda);
                return RETURN_ERROR;
            }

            if (!have_mask)
                mask = 0xFFFFFFFFUL;
        }
        else if (args[ARG_NETDST] != 0)
        {
            given = (const char *)args[ARG_NETDST];

            if (!resolve_address(given, &dest, &mask, &have_mask))
            {
                explain_bad_address("NETDESTINATION", given);
                FreeArgs(rda);
                return RETURN_ERROR;
            }

            if (!have_mask)
                mask = mask_for_network(dest);

            if (mask == 0xFFFFFFFFUL || mask == 0)
            {
                tool_error("%s is not a network address", (LONG)given);
                tool_advise_blank();
                tool_advise("A network address ends in zero -- 192.168.1.0 for");
                tool_advise("a home network, 10.0.0.0 for a large one -- or");
                tool_advise("carries a prefix length, as 192.168.1.0/24 does.");
                tool_advise("Use HOSTDESTINATION for a single machine.");
                FreeArgs(rda);
                return RETURN_ERROR;
            }
        }
        else
        {
            given = (const char *)args[ARG_DST];

            if (!resolve_address(given, &dest, &mask, &have_mask))
            {
                explain_bad_address("DESTINATION", given);
                FreeArgs(rda);
                return RETURN_ERROR;
            }

            /* A non-zero last octet is a machine; anything else a network. */
            if (!have_mask)
                mask = mask_for_network(dest);
        }

        if (args[ARG_VIA] == 0)
        {
            format_route(dest, mask, text, sizeof(text));
            tool_error("no GATEWAY was given, so there is nowhere to send "
                       "packets for %s", (LONG)text);
            tool_advise_blank();
            tool_advise("A route needs the address of the machine that passes");
            tool_advise("packets on, and that machine has to be on this");
            tool_advise("network. For example:");
            tool_advise("   AddNetRoute NETDESTINATION 192.168.10.0 "
                        "GATEWAY 192.168.1.1");
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        if (!resolve_address((const char *)args[ARG_VIA], &gateway, NULL, NULL)
            || gateway == 0)
        {
            explain_bad_address("GATEWAY", (const char *)args[ARG_VIA]);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        /* The destination is the network; the host bits are not ours to keep. */
        dest &= mask;
#endif
        /*
         * Not done on the delete path: there the mask is not known yet -- it
         * comes out of the live table below -- and masking with the zero it
         * still holds turned every DESTINATION into 0.0.0.0, answering "there
         * is no route to 0.0.0.0" for a route that was plainly there. Caught
         * by tests/tools/run-livetools.sh.
         */
    }

    /* ---- the running stack ---------------------------------------------- */

    if (!tool_stack_library_running())
    {
        tool_error("the network is not running, so it has no routes");
        tool_advise_blank();
        tool_advise("Routes belong to a running stack and are not remembered");
        tool_advise("across a reboot. Start the network first with");
        tool_advise("AddNetInterface, and put this command after it in");
        tool_advise("S:User-Startup if it should happen at every boot.");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    base = tool_netstatus_open(nr_quiet);
    if (base == NULL)
    {
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    live_gw = current_gateway(base, &routing);

    zero_control(&ctl);

    /* ---- the default route ---------------------------------------------- */

    if (have_default)
    {
        ami_config_format_ip(gateway, text, sizeof(text));

#ifdef TOOL_DELETE
        if (live_gw == 0)
        {
            tool_error("there is no default route to remove");
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        if (live_gw != gateway)
        {
            char have[16];

            ami_config_format_ip(live_gw, have, sizeof(have));
            tool_error("the default route is %s, not %s", (LONG)have,
                       (LONG)text);
            tool_advise_blank();
            tool_advise("There is only ever one; give the address it has.");
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        if (tool_netstatus_control(base, NETCTRL_GATEWAY_CLEAR, &ctl,
                                   &err) != 0)
        {
            tool_error("the default route would not go away");
            explain(err, 0);
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        say("The default route through %s is gone. Only machines on this\n",
            (LONG)text);
        say("machine's own network can be reached now.\n");
#else
        if (live_gw != 0 && live_gw != gateway)
        {
            char have[16];

            ami_config_format_ip(live_gw, have, sizeof(have));
            tool_error("there is already a default route, through %s",
                       (LONG)have);
            tool_advise_blank();
            tool_advise("There is only one, so the old one has to go first:");
            tool_printf("     DeleteNetRoute DEFAULTGATEWAY=%s\n", (LONG)have);
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        if (live_gw == gateway)
        {
            say("The default route is already %s.\n", (LONG)text);
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_OK;
        }

        if (!gateway_is_reachable(base, gateway))
        {
            tool_error("the default route was not set to %s", (LONG)text);
            explain(ROUTE_EINVAL, gateway);
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        ctl.nsc_Gateway = gateway;

        if (tool_netstatus_control(base, NETCTRL_GATEWAY_SET, &ctl, &err) != 0)
        {
            tool_error("the default route was not set to %s", (LONG)text);
            explain(err, gateway);
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        say("Everything not on this machine's own network now goes through\n");
        say("%s.\n", (LONG)text);
#endif

        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_OK;
    }

    /* ---- a route to somewhere in particular ------------------------------ */

    /*
     * Ask before doing rather than reading ENOSYS afterwards: "your gateway is
     * wrong" and "this build cannot do it" read identically to a user.
     */
    if (!routing)
    {
        tool_error("this stack has no routing table");
        explain(ROUTE_ENOSYS, 0);
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

#ifdef TOOL_DELETE
    /*
     * The mask comes out of the live table rather than being inferred, so a
     * route added with any mask can be removed by naming its destination. An
     * explicit prefix length still wins, for two routes to the same address
     * with different masks.
     */
    if (!have_mask && find_route(base, dest, &mask) < 0)
    {
        ami_config_format_ip(dest, text, sizeof(text));
        tool_error("no route to %s was added by hand, so there is none to "
                   "delete", (LONG)text);
        explain(ROUTE_ENOENT, 0);
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    ctl.nsc_Destination = dest & mask;
    ctl.nsc_NetMask     = mask;

    format_route(dest & mask, mask, text, sizeof(text));

    if (tool_netstatus_control(base, NETCTRL_ROUTE_DELETE, &ctl, &err) != 0)
    {
        tool_error("the route to %s was not deleted", (LONG)text);
        explain(err, 0);
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    say("The route to %s is gone.\n", (LONG)text);
#else
    if (!gateway_is_reachable(base, gateway))
    {
        format_route(dest, mask, text, sizeof(text));
        tool_error("the route to %s was not added", (LONG)text);
        explain(ROUTE_EINVAL, gateway);
        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    ctl.nsc_Destination = dest;
    ctl.nsc_NetMask     = mask;
    ctl.nsc_Gateway     = gateway;

    format_route(dest, mask, text, sizeof(text));

    if (tool_netstatus_control(base, NETCTRL_ROUTE_ADD, &ctl, &err) != 0)
    {
        tool_error("the route to %s was not added", (LONG)text);

        if (err == 0 && find_route(base, dest, NULL) >= 0)
        {
            tool_advise_blank();
            tool_advise("There is already a route to it. Remove that one");
            tool_advise("first with DeleteNetRoute.");
        }
        else
        {
            explain(err, gateway);
        }

        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_FAIL;
    }

    {
        char via[16];

        ami_config_format_ip(gateway, via, sizeof(via));
        say("Packets for %s now go through %s.\n", (LONG)text, (LONG)via);
    }
#endif

    tool_netstatus_close(base);
    FreeArgs(rda);
    return RETURN_OK;
}
