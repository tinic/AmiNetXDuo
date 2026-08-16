/*
 * AddNetRoute / DeleteNetRoute, routes for packets that are not for this
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
 *   and matches longest prefix first. A route can therefore override the
 *   default for part of the address space, which is what a subnet behind a
 *   second router needs and what a gateway cannot express.
 *   NETSTATUS_SYS_ROUTING says which build this is, and is asked before
 *   anything is attempted so the command refuses rather than quietly doing
 *   nothing.
 *
 * The GATEWAY must be an address on one of this machine's own subnets. NetX
 * Duo derives the outgoing interface from the next hop
 * (nx_ip_static_route_add.c), so a next hop it cannot reach directly is
 * rejected rather than stored. The command says so instead of passing on a
 * bare errno.
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
 * A prefix length written into the address, 10.1.2.0/24, overrides all of
 * that. It is not a new keyword, so the template is still Roadshow's.
 *
 * IPv6 uses the same keywords and a different pair of mechanisms, because NetX
 * Duo has no IPv6 equivalent of the static routing table. Nothing anywhere in
 * it maps a prefix to a next hop. _nx_ipv6_packet_send() asks two lists and
 * nothing else, so those two are what these commands write:
 *
 *   DEFAULTGATEWAY fe80::1%eth0    the default-router list. Everything with
 *                                  nowhere better to go is handed to a router
 *                                  on it. Unlike IPv4 there can be more than
 *                                  one, and each is per interface, which is
 *                                  why a link-local next hop needs the zone
 *                                  after the '%': fe80::/64 exists on every
 *                                  interface and the address alone does not
 *                                  say which one is meant.
 *
 *   DESTINATION fd00:9::/64        the on-link prefix list: this prefix is
 *   (with no GATEWAY)              reachable directly, so packets for it get a
 *                                  neighbour solicitation rather than being
 *                                  handed to a router.
 *
 *   DESTINATION ... GATEWAY ...    refused. There is nowhere in the stack to
 *                                  put a per-prefix next hop, and a command
 *                                  that accepted one would be storing nothing.
 *
 * An IPv6 destination needs its prefix length written in. There is no
 * equivalent of the non-zero-octet rule to infer one from, and a bare address
 * is a single machine (/128).
 *
 * Both halves flush the IPv6 destination cache, which remembers where each
 * destination went last time and would otherwise keep sending packets the old
 * way after the route deciding it has changed.
 *
 * DeleteNetRoute infers nothing. It reads the live routing table and deletes
 * the entry whose destination matches, with the netmask that entry really has,
 * so a route added with any mask can be removed by naming where it goes.
 *
 * A destination can be a name as well as an address. DEVS:Internet/hosts is
 * consulted first because it needs no network and cannot time out. The running
 * stack's resolver is asked only if that fails.
 *
 * Nothing here is persistent: this is the live table, as Online and Offline
 * are the live interface state. A route that must survive a reboot belongs in
 * S:User-Startup next to the AddNetInterface line.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"


#ifdef TOOL_DELETE
const char *const tool_name = "DeleteNetRoute";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("DeleteNetRoute");

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
    TOOL_VERSTAG("AddNetRoute");

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
#define ROUTE_EEXIST       17
#define ROUTE_EINVAL       22
#define ROUTE_ENOBUFS      55
#define ROUTE_ENOSYS       78

/* As many routes as the stack can report: one per interface, the static
   table, and the default gateway. */
#define NR_MAX_ROUTES   (NX_MAX_PHYSICAL_INTERFACES + NX_IP_ROUTING_TABLE_SIZE + 1)

/*
 * The IPv6 tables are sized by constants that exist only in an
 * AMINETXDUO_IPV6 build of nx_user.h, and this command is one binary for
 * either library, so these are its own. Comfortably above what the shipped
 * library can report (prefix list 4, routers 2, three addresses per
 * interface). A stack with more says so through nsh_Available.
 */
#define NR_MAX_ROUTES6      16
#define NR_MAX_ADDRS6       12

/* An interface index that was not given. */
#define NR_NO_ZONE          0xFFFFU

/* ---------------------------------------------------------------- output, */

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
    struct { NetStatusHeader hdr; NetStatusRoute6    e[NR_MAX_ROUTES6]; } route6;
    struct { NetStatusHeader hdr; NetStatusAddress6  e[NR_MAX_ADDRS6]; } addr6;
} nr_answer;

/* ------------------------------------------------------------- addresses, */

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

    /*
     * AmigaOS does not reclaim AllocVec() memory when a process exits, and
     * ami_alloc() is AllocVec(), so the twelve blocks ami_netdb_load() builds
     * out of DEVS:Internet outlive this command, 12,616 bytes per run on a
     * stock netdb, gone until reboot.  main() frees it on the way out.
     */
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
}

/* ------------------------------------------------------------------ IPv6, */

/*
 * An IPv6 address as it was written: the address, the prefix length if one was
 * given, and the zone after a '%' if one was.
 */
typedef struct NrAddr6
{
    ULONG   addr[4];
    ULONG   prefix;
    BOOL    have_prefix;
    char    zone[NETSTATUS_NAME_LEN];
    BOOL    have_zone;
} NrAddr6;

/*
 * A colon is the whole test. No host name has one, and neither has a dotted
 * quad, so a text with one is meant as an IPv6 literal and nothing else. The
 * IPv4 half below is unchanged, including when and how it refuses.
 */
static BOOL is_written_as_ip6(const char *text)
{
    if (text == NULL)
        return FALSE;

    while (*text != '\0')
    {
        if (*text == ':')
            return TRUE;
        text++;
    }

    return FALSE;
}

/* Any of the addresses this run was given, written the IPv6 way. */
static BOOL wants_ip6(const LONG *args)
{
    if (is_written_as_ip6((const char *)args[ARG_DST]))
        return TRUE;
    if (is_written_as_ip6((const char *)args[ARG_DEFAULT]))
        return TRUE;
#ifndef TOOL_DELETE
    if (is_written_as_ip6((const char *)args[ARG_HOSTDST]))
        return TRUE;
    if (is_written_as_ip6((const char *)args[ARG_NETDST]))
        return TRUE;
    if (is_written_as_ip6((const char *)args[ARG_VIA]))
        return TRUE;
#endif

    return FALSE;
}

/*
 * "fd00:9::/64%eth0" taken apart. The zone and the prefix length are cut off
 * here because tool_parse_ip6() takes neither.
 */
static BOOL parse_address6(const char *text, NrAddr6 *out)
{
    char  copy[64];
    ULONG i     = 0;
    LONG  slash = -1;
    LONG  pct   = -1;

    out->prefix      = 128;
    out->have_prefix = FALSE;
    out->have_zone   = FALSE;
    out->zone[0]     = '\0';

    if (text == NULL || *text == '\0')
        return FALSE;

    while (text[i] != '\0' && i + 1 < (ULONG)sizeof(copy))
    {
        if (text[i] == '/')
            slash = (LONG)i;
        else if (text[i] == '%' && pct < 0)
            pct = (LONG)i;
        copy[i] = text[i];
        i++;
    }
    if (text[i] != '\0')
        return FALSE;
    copy[i] = '\0';

    /* If both are there, the zone comes after the prefix length, so the zone is
       cut off first and the slash it moved past is re-found below. */
    if (pct >= 0)
    {
        if (copy[pct + 1] == '\0')
            return FALSE;

        tool_copy_string(out->zone, sizeof(out->zone), &copy[pct + 1]);
        out->have_zone = TRUE;
        copy[pct]      = '\0';

        if (slash > pct)
            slash = -1;
    }

    if (slash >= 0)
    {
        ULONG bits = 0;
        ULONG j;

        copy[slash] = '\0';

        if (copy[slash + 1] == '\0')
            return FALSE;

        for (j = (ULONG)slash + 1; copy[j] != '\0'; j++)
        {
            if (copy[j] < '0' || copy[j] > '9')
                return FALSE;
            bits = bits * 10UL + (ULONG)(copy[j] - '0');
            if (bits > 128UL)
                return FALSE;
        }

        out->prefix      = bits;
        out->have_prefix = TRUE;
    }

    return tool_parse_ip6(copy, out->addr);
}

static VOID explain_bad_address6(const char *what, const char *text)
{
    tool_error("%s: \"%s\" is not an address this command can use",
               (LONG)what, (LONG)text);
}

#ifndef TOOL_DELETE
/* Add half only: nothing on the delete path takes a next hop. */
static BOOL is_link_local6(const ULONG addr[4])
{
    return (BOOL)((addr[0] & 0xFFC00000UL) == 0xFE800000UL);
}
#endif

static BOOL same_address6(const ULONG a[4], const ULONG b[4])
{
    return (BOOL)(a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]);
}

/* The first `bits` of both, compared. */
static BOOL same_prefix6(const ULONG a[4], const ULONG b[4], ULONG bits)
{
    ULONG i;

    for (i = 0; i < 4UL; i++)
    {
        ULONG left = (bits > i * 32UL) ? (bits - i * 32UL) : 0UL;
        ULONG mask;

        if (left == 0UL)
            break;

        mask = (left >= 32UL) ? 0xFFFFFFFFUL : (0xFFFFFFFFUL << (32UL - left));

        if ((a[i] & mask) != (b[i] & mask))
            return FALSE;
    }

    return TRUE;
}

/* "fd00:9::/64", or a bare address when the length is 128. */
static VOID format_route6(const ULONG addr[4], ULONG bits,
                          char *buf, ULONG buflen)
{
    ULONG pos = 0;

    tool_format_ip6(addr, buf, buflen);

    while (buf[pos] != '\0')
        pos++;

    if (bits == 128UL || pos + 5 >= buflen)
        return;

    buf[pos++] = '/';
    if (bits >= 100UL)
        buf[pos++] = (char)('0' + (bits / 100UL));
    if (bits >= 10UL)
        buf[pos++] = (char)('0' + ((bits / 10UL) % 10UL));
    buf[pos++] = (char)('0' + (bits % 10UL));
    buf[pos]   = '\0';
}

#ifndef TOOL_DELETE
/*
 * The mask covering the octets of `addr` that are not zero. 0.0.0.0 has none
 * and answers /0, the default route, which is reached through the DEFAULT
 * keyword instead.
 *
 * Add half only: DeleteNetRoute takes the mask out of the live table, and a
 * guess here can disagree with it.
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

/* ----------------------------------------------------------- the library, */

static VOID zero_control(NetStatusControl *ctl)
{
    ULONG i;

    for (i = 0; i < (ULONG)(sizeof(*ctl) / sizeof(ULONG)); i++)
        ((ULONG *)ctl)[i] = 0;
}

/* The running stack's default gateway, or 0. *routing_out says whether the
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
         * the stack instead of a message saying the route was not added by
         * hand.
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
 * next hop has to be. Checked before the call, so the answer names the
 * unreachable router rather than being EINVAL. Add half only, because nothing
 * on the delete path takes a next hop.
 */
static BOOL gateway_is_reachable(struct Library *base, ULONG addr)
{
    LONG n;
    LONG i;

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &nr_answer,
                             sizeof(nr_answer.iface),
                             sizeof(NetStatusInterface));
    if (n <= 0)
        return TRUE;                /* cannot tell, so assume reachable */

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
            break;

        case ROUTE_ENOBUFS:
            break;

        case ROUTE_ENOENT:
            break;

        case ROUTE_EINVAL:
            if (gateway != 0)
            {
                ami_config_format_ip(gateway, addr, sizeof(addr));
                tool_printf("  %s is not on any of this machine's own "
                            "networks.\n", (LONG)addr);
            }
            break;

        default:
            break;
    }
}

/* --------------------------------------------------- IPv6, the live stack, */

/* TRUE when the running library has IPv6 in it at all. */
static BOOL stack_has_ipv6(struct Library *base)
{
    if (tool_netstatus_query(base, NETSTATUS_SYSTEM, &nr_answer,
                             sizeof(nr_answer.system),
                             sizeof(NetStatusSystem)) <= 0)
    {
        return FALSE;
    }

    return (nr_answer.system.e.nss_Flags & NETSTATUS_SYS_IPV6) ? TRUE : FALSE;
}

#ifndef TOOL_DELETE
/* "eth0", or the bare slot number the tables use. */
static BOOL zone_matches(const NetStatusInterface *nsi, const char *zone)
{
    ULONG value = 0;
    ULONG i;

    if (tool_stricmp(nsi->nsi_Name, zone) == 0)
        return TRUE;

    for (i = 0; zone[i] != '\0'; i++)
    {
        if (zone[i] < '0' || zone[i] > '9')
            return FALSE;
        value = value * 10UL + (ULONG)(zone[i] - '0');
    }

    return (BOOL)(i != 0 && value == (ULONG)nsi->nsi_Index);
}

/*
 * Which interface a next hop is reached through, which the default-router
 * list needs and the address alone does not always give.
 *
 * A zone was written: that. A link-local next hop with none: only a machine
 * with one interface up can say, since fe80::/64 is on every interface at
 * once. Anything else: the interface holding an address whose prefix covers
 * it, which is also the test nxd_ipv6_default_router_add() applies before it
 * will store the entry, so failing here gives the reason instead of an
 * unexplained refusal from the stack.
 */
static BOOL interface_for_next_hop(struct Library *base, const NrAddr6 *gw,
                                   UWORD *out, UWORD *attached_out)
{
    LONG  n;
    LONG  i;
    UWORD attached = 0;
    UWORD only     = 0;

    n = tool_netstatus_query(base, NETSTATUS_INTERFACES, &nr_answer,
                             sizeof(nr_answer.iface),
                             sizeof(NetStatusInterface));

    for (i = 0; i < n; i++)
    {
        const NetStatusInterface *nsi = &nr_answer.iface.e[i];

        if (!(nsi->nsi_Flags & NETSTATUS_IF_ATTACHED))
            continue;

        if (gw->have_zone)
        {
            if (zone_matches(nsi, gw->zone))
            {
                *out = nsi->nsi_Index;
                if (attached_out != NULL)
                    *attached_out = 1;
                return TRUE;
            }
            continue;
        }

        attached++;
        only = nsi->nsi_Index;
    }

    if (attached_out != NULL)
        *attached_out = attached;

    if (gw->have_zone)
        return FALSE;

    if (is_link_local6(gw->addr))
    {
        if (attached != 1)
            return FALSE;

        *out = only;
        return TRUE;
    }

    n = tool_netstatus_query(base, NETSTATUS_ADDRESSES6, &nr_answer,
                             sizeof(nr_answer.addr6),
                             sizeof(NetStatusAddress6));

    for (i = 0; i < n; i++)
    {
        const NetStatusAddress6 *a = &nr_answer.addr6.e[i];

        if (a->nsn_State == NETSTATUS_IP6_TENTATIVE)
            continue;
        if (is_link_local6(a->nsn_Address))
            continue;

        if (same_prefix6(a->nsn_Address, gw->addr, a->nsn_PrefixLength))
        {
            *out = a->nsn_Interface;
            return TRUE;
        }
    }

    return FALSE;
}
#endif /* !TOOL_DELETE */

/* The live default router with this address, or -1. */
static LONG find_router6(struct Library *base, const ULONG addr[4])
{
    LONG n;
    LONG i;

    n = tool_netstatus_query(base, NETSTATUS_ROUTES6, &nr_answer,
                             sizeof(nr_answer.route6), sizeof(NetStatusRoute6));

    for (i = 0; i < n; i++)
    {
        const NetStatusRoute6 *r = &nr_answer.route6.e[i];

        if (!(r->nsr6_Flags & NETSTATUS_RT6_GATEWAY))
            continue;

        if (same_address6(r->nsr6_NextHop, addr))
            return i;
    }

    return -1;
}

#ifdef TOOL_DELETE
/*
 * The live on-link prefix `addr` falls in. If `bits` is given, only that exact
 * length matches. Without it the first covering entry matches, and the table
 * is kept longest prefix first, so that is the one the stack would use.
 *
 * Delete half only: the add path lets the stack answer EEXIST rather than
 * asking first.
 */
static LONG find_prefix6(struct Library *base, const ULONG addr[4],
                         ULONG bits, BOOL exact, ULONG *bits_out)
{
    LONG n;
    LONG i;

    n = tool_netstatus_query(base, NETSTATUS_ROUTES6, &nr_answer,
                             sizeof(nr_answer.route6), sizeof(NetStatusRoute6));

    for (i = 0; i < n; i++)
    {
        const NetStatusRoute6 *r = &nr_answer.route6.e[i];

        if (r->nsr6_Flags & NETSTATUS_RT6_GATEWAY)
            continue;

        if (exact && r->nsr6_PrefixLength != bits)
            continue;

        if (!same_prefix6(r->nsr6_Destination, addr, r->nsr6_PrefixLength))
            continue;

        if (bits_out != NULL)
            *bits_out = r->nsr6_PrefixLength;

        return i;
    }

    return -1;
}
#endif /* TOOL_DELETE */

/* The stack said no to an IPv6 route. */
static VOID explain6(LONG err, const char *gateway_text)
{
    switch (err)
    {
        case ROUTE_ENOSYS:
            break;

        case ROUTE_ENOBUFS:
            break;

        case ROUTE_EEXIST:
            break;

        case ROUTE_ENOENT:
            break;

        case ROUTE_EINVAL:
            if (gateway_text != NULL)
                tool_printf("  %s is not on any network this machine has an "
                            "interface on.\n", (LONG)gateway_text);
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

static VOID say_no_stack(VOID)
{
    tool_error("the network is not running, so it has no routes");
}

/* ------------------------------------------------------------- the IPv6 run, */

/*
 * Reached when any address on the command line was written with a colon in
 * it. Everything below this point talks to the two IPv6 lists and nothing to
 * the IPv4 routing table, so the IPv4 path above is untouched, including
 * which of its refusals come before the library is opened.
 */
static LONG run_ipv6(const LONG *args, BOOL have_default)
{
    struct Library  *base;
    NetStatusControl ctl;
    NrAddr6          via;
    char             text[64];
    char             gw_text[64];
    const char      *given;
    ULONG            bits     = 128;
    LONG             err      = 0;
#ifndef TOOL_DELETE
    UWORD            index    = 0;
    UWORD            attached = 0;
#endif

    if (!tool_stack_library_running())
    {
        say_no_stack();
        return RETURN_ERROR;
    }

    /* FALSE: QUIET drops the route that was set, never why it was not. */
    base = tool_netstatus_open(FALSE);
    if (base == NULL)
        return RETURN_FAIL;

    if (!stack_has_ipv6(base))
    {
        tool_error("the running stack has no IPv6");
        tool_netstatus_close(base);
        return RETURN_FAIL;
    }

    zero_control(&ctl);

    /* ---- the default route ---------------------------------------------- */

    if (have_default)
    {
        given = (const char *)args[ARG_DEFAULT];

        if (!parse_address6(given, &via))
        {
            explain_bad_address6("DEFAULTGATEWAY", given);
            tool_netstatus_close(base);
            return RETURN_ERROR;
        }

        if (via.have_prefix)
        {
            tool_error("DEFAULTGATEWAY is one router, not a prefix");
            tool_netstatus_close(base);
            return RETURN_ERROR;
        }

        tool_format_ip6(via.addr, gw_text, sizeof(gw_text));

#ifdef TOOL_DELETE
        /* No interface is needed to remove one: the address identifies it,
           and requiring the zone would make a router unremovable once the
           interface it was on had gone. */
        if (find_router6(base, via.addr) < 0)
        {
            tool_error("there is no default route through %s", (LONG)gw_text);
            explain6(ROUTE_ENOENT, NULL);
            tool_netstatus_close(base);
            return RETURN_ERROR;
        }

        ctl.nsc_Gateway6[0] = via.addr[0];
        ctl.nsc_Gateway6[1] = via.addr[1];
        ctl.nsc_Gateway6[2] = via.addr[2];
        ctl.nsc_Gateway6[3] = via.addr[3];

        if (tool_netstatus_control(base, NETCTRL_ROUTE6_DELETE, &ctl,
                                   &err) != 0)
        {
            tool_error("the default route through %s did not go away",
                       (LONG)gw_text);
            explain6(err, NULL);
            tool_netstatus_close(base);
            return RETURN_FAIL;
        }

        say("The default route through %s is gone.\n", (LONG)gw_text);
#else
        if (find_router6(base, via.addr) >= 0)
        {
            say("The default route through %s is already there.\n",
                (LONG)gw_text);
            tool_netstatus_close(base);
            return RETURN_OK;
        }

        if (!interface_for_next_hop(base, &via, &index, &attached))
        {
            tool_error("the default route was not set to %s", (LONG)gw_text);

            if (via.have_zone)
            {
                tool_printf("  This machine has no interface called \"%s\".\n",
                            (LONG)via.zone);
            }
            else if (is_link_local6(via.addr))
            {
                tool_printf("     AddNetRoute DEFAULTGATEWAY %s%ceth0\n",
                            (LONG)gw_text, (LONG)'%');
            }
            else
            {
                explain6(ROUTE_EINVAL, gw_text);
            }

            tool_netstatus_close(base);
            return RETURN_ERROR;
        }

        ctl.nsc_Index       = index;
        ctl.nsc_Gateway6[0] = via.addr[0];
        ctl.nsc_Gateway6[1] = via.addr[1];
        ctl.nsc_Gateway6[2] = via.addr[2];
        ctl.nsc_Gateway6[3] = via.addr[3];

        if (tool_netstatus_control(base, NETCTRL_ROUTE6_ADD, &ctl, &err) != 0)
        {
            tool_error("the default route was not set to %s", (LONG)gw_text);
            explain6(err, gw_text);
            tool_netstatus_close(base);
            return RETURN_FAIL;
        }

        say("IPv6 packets with nowhere better to go now leave through\n");
        say("%s.\n", (LONG)gw_text);
#endif

        tool_netstatus_close(base);
        return RETURN_OK;
    }

    /* ---- a prefix on this link ------------------------------------------ */

#ifdef TOOL_DELETE
    given = (const char *)args[ARG_DST];

    if (!parse_address6(given, &via))
    {
        explain_bad_address6("DESTINATION", given);
        tool_netstatus_close(base);
        return RETURN_ERROR;
    }

    /*
     * The length comes out of the live table when none was written, so a
     * prefix added with any length can be removed by naming somewhere inside
     * it. An explicit one still wins.
     */
    bits = via.prefix;

    if (find_prefix6(base, via.addr, bits, via.have_prefix, &bits) < 0)
    {
        format_route6(via.addr, via.prefix, text, sizeof(text));
        tool_error("no IPv6 route to %s was added by hand, so there is none "
                   "to delete", (LONG)text);
        explain6(ROUTE_ENOENT, NULL);
        tool_netstatus_close(base);
        return RETURN_ERROR;
    }

    ctl.nsc_Destination6[0] = via.addr[0];
    ctl.nsc_Destination6[1] = via.addr[1];
    ctl.nsc_Destination6[2] = via.addr[2];
    ctl.nsc_Destination6[3] = via.addr[3];
    ctl.nsc_PrefixLength    = bits;

    format_route6(via.addr, bits, text, sizeof(text));

    if (tool_netstatus_control(base, NETCTRL_ROUTE6_DELETE, &ctl, &err) != 0)
    {
        tool_error("the route to %s was not deleted", (LONG)text);
        explain6(err, NULL);
        tool_netstatus_close(base);
        return RETURN_FAIL;
    }

    say("The route to %s is gone. Packets for it go to a router again.\n",
        (LONG)text);
#else
    if (args[ARG_HOSTDST] != 0)
        given = (const char *)args[ARG_HOSTDST];
    else if (args[ARG_NETDST] != 0)
        given = (const char *)args[ARG_NETDST];
    else
        given = (const char *)args[ARG_DST];

    if (!parse_address6(given, &via))
    {
        explain_bad_address6("DESTINATION", given);
        tool_netstatus_close(base);
        return RETURN_ERROR;
    }

    bits = via.prefix;                  /* 128 unless a length was written */

    if (args[ARG_NETDST] != 0 && !via.have_prefix)
    {
        tool_error("%s is not a network address", (LONG)given);
        tool_netstatus_close(base);
        return RETURN_ERROR;
    }

    if (args[ARG_VIA] != 0)
    {
        format_route6(via.addr, bits, text, sizeof(text));
        tool_error("the route to %s was not added", (LONG)text);
        explain6(ROUTE_ENOSYS, NULL);
        tool_netstatus_close(base);
        return RETURN_FAIL;
    }

    if (bits == 0)
    {
        tool_error("::/0 is not a prefix on this link. It is the default "
                   "route");
        tool_netstatus_close(base);
        return RETURN_ERROR;
    }

    ctl.nsc_Destination6[0] = via.addr[0];
    ctl.nsc_Destination6[1] = via.addr[1];
    ctl.nsc_Destination6[2] = via.addr[2];
    ctl.nsc_Destination6[3] = via.addr[3];
    ctl.nsc_PrefixLength    = bits;

    format_route6(via.addr, bits, text, sizeof(text));

    if (tool_netstatus_control(base, NETCTRL_ROUTE6_ADD, &ctl, &err) != 0)
    {
        tool_error("the route to %s was not added", (LONG)text);
        explain6(err, NULL);
        tool_netstatus_close(base);
        return RETURN_FAIL;
    }

    say("Packets for %s now go straight there rather than to a router.\n",
        (LONG)text);
#endif

    tool_netstatus_close(base);
    return RETURN_OK;
}

/* --------------------------------------------------------------- the run, */

/*
 * The body, so that ami_netdb_free() has one place to run.  atexit() is not
 * free here: libnix satisfies it out of an object that also references malloc
 * and __errno, which pulls in the C++ AVL allocator and the stdio FILE
 * machinery, about 7.7 KB nothing in this command calls.
 */
static int addnetroute_main(int argc, char **argv);

int main(int argc, char **argv)
{
    int rc = addnetroute_main(argc, argv);

    ami_netdb_free();
    return rc;
}

static int addnetroute_main(int argc, char **argv)
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

    /* DEFAULTGATEWAY wins outright, so say so rather than silently ignoring
       the destination. */
    if (have_default && have_dest)
        say("%s: DEFAULTGATEWAY was given, so the destination is ignored.\n",
            (LONG)tool_name);

    /* One binary, both families: how the address was written decides which
       half runs, and a text with no colon in it reaches the IPv4 half exactly
       as it did before there was another one. */
    if (wants_ip6(args))
    {
        LONG rc = run_ipv6(args, have_default);

        FreeArgs(rda);
        return rc;
    }

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

            /* A non-zero last octet is a machine, anything else a network. */
            if (!have_mask)
                mask = mask_for_network(dest);
        }

        if (args[ARG_VIA] == 0)
        {
            format_route(dest, mask, text, sizeof(text));
            tool_error("no GATEWAY was given, so there is nowhere to send "
                       "packets for %s", (LONG)text);
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
        /* The destination is the network, so the host bits are dropped. */
        dest &= mask;
#endif
        /*
         * Not done on the delete path: there the mask is not known yet, it
         * comes out of the live table below, and masking with the zero it
         * still holds turned every DESTINATION into 0.0.0.0. The command then
         * reported no route to 0.0.0.0 for a route that was plainly there.
         * Caught by tests/tools/run-livetools.sh.
         */
    }

    /* ---- the running stack ---------------------------------------------- */

    if (!tool_stack_library_running())
    {
        say_no_stack();
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /* FALSE: QUIET drops the route that was set, never why it was not. */
    base = tool_netstatus_open(FALSE);
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
            tool_netstatus_close(base);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        if (tool_netstatus_control(base, NETCTRL_GATEWAY_CLEAR, &ctl,
                                   &err) != 0)
        {
            tool_error("the default route did not go away");
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
     * Ask before doing rather than reading ENOSYS afterwards. A wrong gateway
     * and a build without static routing are otherwise reported the same way.
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

        /* A refusal with no error number, and the route already there, is the
           table saying so. Anything else needs explain(). */
        if (err != 0 || find_route(base, dest, NULL) < 0)
            explain(err, gateway);

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
