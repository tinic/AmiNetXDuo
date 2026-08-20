/*
 * arp, the address resolution cache: what is at each address on this network.
 *
 *     arp ADDRESS,DELETE/S,SET/K,UNIT/K/N,STATS/S,QUIET/S
 *
 * Every packet leaving this machine for the local network needs the ethernet
 * address of its destination, and ARP is how that address is found and
 * remembered. The cache separates "the network does not work" from "that
 * machine is not answering". It needs nothing of the other end but ARP, which
 * a machine that drops pings still answers.
 *
 *   an entry with a hardware address   it answered. The wire is fine, and
 *                                      whatever is wrong is above this layer
 *   an entry with no reply             we asked and nothing came back:
 *                                      wrong address, wrong network, or it
 *                                      is switched off
 *   no entry at all                    nothing here has tried to reach it
 *
 *     arp                     the whole cache
 *     arp 192.168.1.1         one address
 *     arp STATS               how much asking it has taken
 *     arp 192.168.1.1 DELETE  forget it, so the next packet asks again
 *     arp 192.168.1.1 SET=02:11:22:33:44:55
 *                             a permanent entry, for something that does not
 *                             answer ARP or that keeps changing
 *
 * IPv6 is in the same command, in a section of its own. There is no ARP in
 * IPv6. RFC 4861 neighbour discovery does the same job over ICMPv6, and NetX
 * Duo keeps its answers in a separate table. Both families stay in one command
 * here, as they do in ping, nslookup and netstat, so there is one place to ask
 * what is at an address.
 *
 * What the section adds over the ARP one is the state. An ARP entry has
 * answered or it has not. A neighbour entry says what the stack currently
 * believes: INCOMPLETE is asked with nothing back, STALE is answered once and
 * not checked since, PROBE is being re-checked now. Each state that appears is
 * spelled out under the list.
 *
 * Nothing ages out on the IPv4 side. NX_ARP_EXPIRATION_RATE is 0 in this
 * build, so an entry stays until DELETE removes it or the stack stops, and a
 * machine that has since moved is still listed. Neighbour entries do age:
 * that is what STALE and PROBE are.
 *
 * It does not start the network, unlike fetch or nslookup. The cache describes
 * a running stack, so starting one to report an empty cache would make the
 * answer meaningless. ShowNetStatus follows the same rule.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "tools_nx.h"

const char *const tool_name = "arp";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("arp");

#define TEMPLATE    "ADDRESS,DELETE/S,SET/K,UNIT/K/N,STATS/S,QUIET/S"

enum
{
    ARG_ADDRESS = 0,
    ARG_DELETE,
    ARG_SET,
    ARG_UNIT,
    ARG_STATS,
    ARG_QUIET,
    ARG_COUNT
};

static ToolStats      arp_stats;
static ToolSnapshot   arp_snap;
static ToolNeighbours arp_nd;
static ToolRoutes6    arp_routes6;

static VOID zero_control(NetStatusControl *ctl)
{
    ULONG i;

    for (i = 0; i < (ULONG)(sizeof(*ctl) / sizeof(ULONG)); i++)
        ((ULONG *)ctl)[i] = 0;
}

/*
 * "02:11:22:33:44:55". The separator can be ':' or '-' or absent. All three
 * turn up on labels and in documentation.
 */
static BOOL parse_mac(const char *text, UBYTE *out)
{
    UWORD got = 0;

    if (text == NULL)
        return FALSE;

    while (*text != '\0' && got < (UWORD)AMI_ETH_ADDR_SIZE)
    {
        UWORD digits = 0;
        ULONG value  = 0;

        while (digits < 2)
        {
            char  c = *text;
            ULONG d;

            if (c >= '0' && c <= '9')       d = (ULONG)(c - '0');
            else if (c >= 'a' && c <= 'f')  d = (ULONG)(c - 'a') + 10UL;
            else if (c >= 'A' && c <= 'F')  d = (ULONG)(c - 'A') + 10UL;
            else                            break;

            value = (value << 4) | d;
            digits++;
            text++;
        }

        if (digits == 0)
            return FALSE;

        out[got++] = (UBYTE)value;

        if (*text == ':' || *text == '-')
            text++;
    }

    return (BOOL)(got == (UWORD)AMI_ETH_ADDR_SIZE && *text == '\0');
}

/*
 * The interface an entry was learnt on, by the name the rest of the tools use.
 * "interface 0" is a number out of the stack's internals. "a2065.0" is the
 * line from the configuration file, which is what identifies the card on a
 * machine with two of them.
 */
static const char *interface_name(UWORD nx_index, BOOL have_snapshot)
{
    UWORD i;

    if (!have_snapshot)
        return NULL;

    for (i = 0; i < arp_snap.iface_count; i++)
    {
        if (arp_snap.iface[i].nx_index == nx_index &&
            arp_snap.iface[i].nx_name[0] != '\0')
        {
            return arp_snap.iface[i].nx_name;
        }
    }

    return NULL;
}

static VOID print_entry(const ToolArpEntry *e, BOOL have_snapshot)
{
    char        addr[16];
    char        mac[24];
    const char *ifname = interface_name(e->nx_index, have_snapshot);

    ami_config_format_ip(e->address, addr, sizeof(addr));

    /*
     * An unresolved entry has an all-zero hardware address. Printing it as
     * 00:00:00:00:00:00 would read as a machine with a strange address rather
     * than as silence. The retry count is how many times we asked.
     */
    if (!e->resolved)
    {
        tool_printf("  %-15s  no reply", (LONG)addr);
        if (e->retries != 0)
            tool_printf(" after %lu request%s",
                        (LONG)e->retries,
                        (LONG)((e->retries == 1) ? "" : "s"));
        tool_printf("\n");
        return;
    }

    tool_format_mac(e->mac, mac, sizeof(mac));

    tool_printf("  %-15s  %s", (LONG)addr, (LONG)mac);

    if (ifname != NULL)
        tool_printf("  %s", (LONG)ifname);
    else if (e->nx_index != 0)
        tool_printf("  interface %lu", (LONG)e->nx_index);

    if (e->is_static)
        tool_printf("  permanent");

    tool_printf("\n");
}

/* ------------------------------------------------------------- neighbours, */

/*
 * A colon says the user meant an IPv6 address. Nothing else can have one: not
 * a dotted quad, and not a host name, which arp does not take anyway.
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

/* Whether the running library has IPv6 in it at all, the same question
   AddNetRoute asks before it touches the IPv6 lists. */
static BOOL stack_has_ipv6(struct Library *base)
{
    struct
    {
        NetStatusHeader hdr;
        NetStatusSystem sys;
    } answer;

    if (tool_netstatus_query(base, NETSTATUS_SYSTEM, &answer, sizeof(answer),
                             sizeof(NetStatusSystem)) <= 0)
    {
        return FALSE;
    }

    return (answer.sys.nss_Flags & NETSTATUS_SYS_IPV6) ? TRUE : FALSE;
}

static BOOL same_address6(const ULONG a[4], const ULONG b[4])
{
    return (BOOL)(a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]);
}

/*
 * Resolved for display purposes: RFC 4861 7.3.3 says the cached link-layer
 * address can be used in every state but INCOMPLETE, so those are the states
 * with a hardware address to print.
 */
static BOOL neighbour_resolved(UWORD state)
{
    return (BOOL)(state >= NETSTATUS_ND_REACHABLE &&
                  state <= NETSTATUS_ND_PROBE);
}

static VOID print_neighbour(const ToolNeighbour *e, BOOL have_snapshot)
{
    char        mac[24];
    const char *ifname = interface_name(e->nx_index, have_snapshot);

    /* 24 rather than 39: it lines up every address short enough to be typed
       by hand, and a longer one pushes the rest right instead of being cut. */
    tool_printf("  %-24s  ", (LONG)e->text);

    if (neighbour_resolved(e->state))
    {
        tool_format_mac(e->mac, mac, sizeof(mac));
        tool_printf("%s", (LONG)mac);
    }
    else
    {
        tool_printf("no reply");
        if (e->solicitations != 0)
            tool_printf(" after %lu request%s",
                        (LONG)e->solicitations,
                        (LONG)((e->solicitations == 1) ? "" : "s"));
    }

    tool_printf("  %s", (LONG)tool_nd_state_name(e->state));

    if (ifname != NULL)
        tool_printf("  %s", (LONG)ifname);
    else if (e->nx_index != 0)
        tool_printf("  interface %lu", (LONG)e->nx_index);

    if (e->flags & NETSTATUS_ND_ROUTER)
        tool_printf("  router");
    if (e->flags & NETSTATUS_ND_STATIC)
        tool_printf("  permanent");
    if (e->queued != 0)
        tool_printf("  %lu packet%s waiting", (LONG)e->queued,
                    (LONG)((e->queued == 1) ? "" : "s"));

    tool_printf("\n");
}

/*
 * What each state that appeared means, once, under the list. The names are
 * RFC 4861's and say nothing to a reader who has not read it.
 */
static VOID explain_states(const ToolNeighbours *nd, const ULONG want[4],
                           BOOL one)
{
    UWORD said[8];
    UWORD count = 0;
    UWORD i;
    UWORD j;

    for (i = 0; i < nd->count; i++)
    {
        const ToolNeighbour *e    = &nd->entry[i];
        const char          *note;
        BOOL                 seen = FALSE;

        if (one && !same_address6(e->addr, want))
            continue;

        note = tool_nd_state_note(e->state);
        if (note == NULL)
            continue;

        for (j = 0; j < count; j++)
        {
            if (said[j] == e->state)
                seen = TRUE;
        }
        if (seen || count >= (UWORD)(sizeof(said) / sizeof(said[0])))
            continue;

        said[count++] = e->state;

        if (count == 1)
            tool_printf("\n");

        tool_printf("  %-12s%s\n", (LONG)tool_nd_state_name(e->state),
                    (LONG)note);
    }
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

/*
 * TRUE when nothing on this machine can ever put `addr` in the neighbour
 * cache: it is not link-local and no on-link prefix covers it, so packets for
 * it go to a router and the router's own entry is the one to check. The IPv4
 * half says the same thing about an address off this machine's subnet.
 */
static BOOL off_link6(const ULONG addr[4], BOOL have_routes)
{
    UWORD i;

    if (!have_routes)
        return FALSE;               /* cannot tell, so do not assert otherwise */

    /* _nxd_ipv6_search_onlink() answers 1 for every fe80:: address before it
       looks at any list, so there is no prefix to find and none is needed. */
    if ((addr[0] & 0xFFC00000UL) == 0xFE800000UL)
        return FALSE;

    for (i = 0; i < arp_routes6.count; i++)
    {
        const ToolRoute6 *r = &arp_routes6.route[i];

        if (r->flags & NETSTATUS_RT6_GATEWAY)
            continue;

        if (same_prefix6(r->dest_words, addr, r->prefix))
            return FALSE;
    }

    return TRUE;
}

/* The first default router this machine has, as text, or NULL. */
static const char *default_router6(BOOL have_routes)
{
    UWORD i;

    if (!have_routes)
        return NULL;

    for (i = 0; i < arp_routes6.count; i++)
    {
        if ((arp_routes6.route[i].flags & NETSTATUS_RT6_GATEWAY) &&
            arp_routes6.route[i].next_hop[0] != '\0')
        {
            return arp_routes6.route[i].next_hop;
        }
    }

    return NULL;
}

/*
 * Why an address is not in the neighbour cache. The IPv6 half of
 * explain_absence() below, and the same two reasons.
 */
static VOID explain_absence6(const ULONG addr[4], const char *text,
                             BOOL have_routes)
{
    const char *router;

    if (off_link6(addr, have_routes))
    {
        tool_printf("  It is not on this machine's link, so nothing ever asks "
                    "for its hardware address. Packets to it go to the "
                    "router.\n");

        router = default_router6(have_routes);
        if (router != NULL)
        {
            tool_printf("  The router is %s, and its entry is the one to "
                        "check:\n", (LONG)router);
            tool_printf("      arp %s\n", (LONG)router);
        }
        return;
    }

    /*
     * On this machine's link, or a machine that could not tell. An entry is
     * made when something sends to the address, and unlike the ARP side it is
     * aged out again when nothing does, so an absence here is "nothing has
     * spoken to it lately" rather than a fault.
     */
    if (have_routes)
        tool_printf("  It is on this machine's link, so an entry appears as "
                    "soon as something sends to it:\n");
    else
        tool_printf("  An entry appears as soon as something sends to it:\n");

    tool_printf("      ping %s\n", (LONG)text);
}

/* TRUE when `addr` is on the network of an interface this machine has up. */
static BOOL on_our_network(ULONG addr, BOOL have_snapshot)
{
    UWORD i;

    if (!have_snapshot)
        return TRUE;                /* cannot tell, so do not assert otherwise */

    for (i = 0; i < arp_snap.iface_count; i++)
    {
        const ToolIfInfo *nif = &arp_snap.iface[i];

        if (!nif->attached || nif->address == 0 || nif->netmask == 0)
            continue;

        if ((nif->address & nif->netmask) == (addr & nif->netmask))
            return TRUE;
    }

    return FALSE;
}

/*
 * Why an address is not in the cache. There are two distinct reasons.
 *
 * ARP is only spoken to machines on this machine's own network. An address
 * anywhere else is reached by handing the packet to the router, so it never
 * appears here however much traffic goes to it. That needs saying rather than
 * "nothing has tried to reach it", which would be wrong after a successful
 * ping.
 */
static VOID explain_absence(ULONG addr, const char *text, BOOL have_snapshot)
{
    char gw[16];

    if (!on_our_network(addr, have_snapshot))
    {
        tool_printf("  It is not on this machine's network, so nothing ever "
                    "sends it an ARP request. Packets to it go to the "
                    "router.\n");

        if (arp_snap.have_gateway && arp_snap.gateway != 0)
        {
            ami_config_format_ip(arp_snap.gateway, gw, sizeof(gw));
            tool_printf("  The router is %s, and its entry is the one to "
                        "check:\n", (LONG)gw);
            tool_printf("      arp %s\n", (LONG)gw);
        }
        return;
    }

    /*
     * On this machine's network, or a machine that could not tell. An entry
     * is made when something sends to the address, and on this side nothing
     * ages out (NX_ARP_EXPIRATION_RATE is 0, see the top of this file), so an
     * absence here means nothing has ever sent to it since the stack came up.
     * That is not a fault, which is why it needs saying.
     */
    if (have_snapshot)
        tool_printf("  It is on this machine's network, so an entry appears "
                    "as soon as something sends to it:\n");
    else
        tool_printf("  An entry appears as soon as something sends to it:\n");

    tool_printf("      ping %s\n", (LONG)text);
}

/*
 * The protocol counters, which say how the cache came to look the way it
 * does. Requests sent with few responses received is a quiet network or a
 * wrong netmask. Invalid messages are somebody else's malformed frames.
 */
static VOID print_stats(const ToolStats *s)
{
    if (!s->have_arp)
    {
        tool_printf("This stack does not keep ARP counters.\n");
        return;
    }

    tool_printf("Asking       %lu request%s sent, %lu response%s received\n",
                (LONG)s->arp_requests_sent,
                (LONG)((s->arp_requests_sent == 1) ? "" : "s"),
                (LONG)s->arp_responses_received,
                (LONG)((s->arp_responses_received == 1) ? "" : "s"));

    tool_printf("Answering    %lu request%s received, %lu response%s sent\n",
                (LONG)s->arp_requests_received,
                (LONG)((s->arp_requests_received == 1) ? "" : "s"),
                (LONG)s->arp_responses_sent,
                (LONG)((s->arp_responses_sent == 1) ? "" : "s"));

    tool_printf("Cache        %lu learnt, %lu permanent",
                (LONG)s->arp_dynamic_entries,
                (LONG)s->arp_static_entries);

    if (s->arp_aged_entries != 0)
        tool_printf(", %lu aged out", (LONG)s->arp_aged_entries);

    tool_printf("\n");

    if (s->arp_invalid_messages != 0)
    {
        tool_printf("Discarded    %lu malformed message%s\n",
                    (LONG)s->arp_invalid_messages,
                    (LONG)((s->arp_invalid_messages == 1) ? "" : "s"));
    }
}

int main(int argc, char **argv)
{
    LONG              args[ARG_COUNT];
    struct RDArgs    *rda;
    struct Library   *base;
    NetStatusControl  ctl;
    const char       *address_text = NULL;
    ULONG             want         = 0;
    ULONG             want6[4]     = { 0, 0, 0, 0 };
    BOOL              want_one     = FALSE;
    BOOL              want_one6    = FALSE;
    BOOL              quiet;
    BOOL              have_snapshot;
    BOOL              have_routes6 = FALSE;
    LONG              err          = 0;
    LONG              rc           = RETURN_OK;
    ULONG             i;
    UWORD             shown        = 0;
    UWORD             shown6       = 0;

    (VOID)argv;

    if (tool_from_workbench(argc))
        return RETURN_FAIL;

    tool_break_arm();

    for (i = 0; i < (ULONG)ARG_COUNT; i++)
        args[i] = 0;

    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        tool_fault(IoErr());
        return RETURN_ERROR;
    }

    quiet        = (BOOL)(args[ARG_QUIET] != 0);
    address_text = (const char *)args[ARG_ADDRESS];

    if (args[ARG_UNIT] != 0)
    {
        LONG unit = *(LONG *)args[ARG_UNIT];

        if (unit < 0 || unit > 65535)
        {
            tool_error("UNIT must be between 0 and 65535");
            FreeArgs(rda);
            return RETURN_ERROR;
        }
    }

    if (address_text != NULL && is_written_as_ip6(address_text))
    {
        struct Library *pb;
        BOOL            has6;

        if (!tool_parse_ip6(address_text, want6))
        {
            tool_error("\"%s\" is not an address", (LONG)address_text);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        /*
         * A well-formed address the running stack still has no cache for.
         * Asked before anything is printed, because every later answer would
         * be "not in the cache" and read as a fact about the address.
         * Opening never starts the stack, and arp has nothing to say about
         * one that is not running.
         */
        /* FALSE: QUIET drops the listing, never the reason there is none. */
        pb = tool_netstatus_open(FALSE);
        if (pb == NULL)
        {
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        has6 = stack_has_ipv6(pb);
        tool_netstatus_close(pb);

        if (!has6)
        {
            tool_error("the running stack has no IPv6");
            FreeArgs(rda);
            return RETURN_FAIL;
        }

        want_one6 = TRUE;
    }
    else if (address_text != NULL)
    {
        if (!ami_config_parse_ip(address_text, &want))
        {
            tool_error("\"%s\" is not an address", (LONG)address_text);
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        want_one = TRUE;
    }
    else if (args[ARG_DELETE] != 0 || args[ARG_SET] != 0)
    {
        tool_error("DELETE and SET each need an address");
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /*
     * The interface table, for the name column and for deciding whether ARP
     * can ever have an answer about an address. Wanted on every path. A
     * failure here only costs detail.
     */
    tool_nx_quiet(TRUE);
    have_snapshot = (BOOL)(tool_snapshot(&arp_snap, FALSE) == 0);
    /* The IPv6 on-link prefixes, which are what says whether an address can
       ever be a neighbour. Empty on a machine without IPv6. */
    have_routes6  = (BOOL)(tool_routes6(&arp_routes6) == 0);
    tool_nx_quiet(FALSE);

    /* ------------------------------------------------- SET and DELETE --- */
    if (args[ARG_SET] != 0 || args[ARG_DELETE] != 0)
    {
        UBYTE mac[AMI_ETH_ADDR_SIZE];
        char  text[24];

        if (args[ARG_SET] != 0 && !parse_mac((const char *)args[ARG_SET], mac))
        {
            tool_error("\"%s\" is not a hardware address",
                       (LONG)args[ARG_SET]);
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        /* Changing the cache needs the library, but still must not start it:
           there is no cache to change until something is running. */
        /* FALSE: QUIET drops the message, never the reason there is
           nothing to report. */
        base = tool_netstatus_open(FALSE);
        if (base == NULL)
        {
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        zero_control(&ctl);

        /* One command, two caches: which one is changed follows from how the
           address was written, exactly as the list below does. */
        if (want_one6)
        {
            ctl.nsc_Destination6[0] = want6[0];
            ctl.nsc_Destination6[1] = want6[1];
            ctl.nsc_Destination6[2] = want6[2];
            ctl.nsc_Destination6[3] = want6[3];
        }
        else
        {
            ctl.nsc_Destination = want;
        }

        if (args[ARG_SET] != 0)
        {
            ULONG op = want_one6 ? (ULONG)NETCTRL_ND_ADD
                                 : (ULONG)NETCTRL_ARP_ADD;

            if (args[ARG_UNIT] != 0)
                ctl.nsc_Index = (UWORD)(*(LONG *)args[ARG_UNIT]);

            for (i = 0; i < (ULONG)AMI_ETH_ADDR_SIZE; i++)
                ctl.nsc_HwAddress[i] = mac[i];

            if (tool_netstatus_control(base, op, &ctl, &err) != 0)
            {
                tool_error("%s was not added to the cache",
                           (LONG)address_text);

                tool_netstatus_close(base);
                FreeArgs(rda);
                return RETURN_FAIL;
            }

            tool_format_mac(mac, text, sizeof(text));
            if (!quiet)
                tool_printf("%s is now permanently %s.\n",
                            (LONG)address_text, (LONG)text);
        }
        else
        {
            ULONG op = want_one6 ? (ULONG)NETCTRL_ND_DELETE
                                 : (ULONG)NETCTRL_ARP_DELETE;

            if (tool_netstatus_control(base, op, &ctl, &err) != 0)
            {
                tool_error("%s was not removed from the cache",
                           (LONG)address_text);
                tool_netstatus_close(base);
                FreeArgs(rda);
                return RETURN_FAIL;
            }

            if (!quiet)
                tool_printf("%s forgotten. The next packet to it will ask "
                            "again.\n", (LONG)address_text);
        }

        tool_netstatus_close(base);
        FreeArgs(rda);
        return RETURN_OK;
    }

    /* ------------------------------------------------------------ list --- */
    if (tool_stats(&arp_stats) != 0)
    {
        FreeArgs(rda);
        return RETURN_ERROR;        /* tool_stats has already explained */
    }

    if (args[ARG_STATS] != 0)
    {
        print_stats(&arp_stats);
        if (!want_one && !want_one6)
        {
            FreeArgs(rda);
            return RETURN_OK;
        }
        tool_printf("\n");
    }

    /* An IPv6 address was asked for: the ARP cache cannot hold one, so the
       loop below would print a heading over nothing. */
    if (!want_one6)
    {
        for (i = 0; i < (ULONG)arp_stats.arp_count; i++)
        {
            if (want_one && arp_stats.arp[i].address != want)
                continue;

            if (shown == 0 && !quiet)
                tool_printf("Address          Hardware address\n");

            print_entry(&arp_stats.arp[i], have_snapshot);
            shown++;
        }
    }

    /*
     * The neighbour cache, under the ARP one. Empty on a machine whose
     * library has no IPv6, so an IPv4-only report is unchanged down to the
     * blank line.
     */
    if (!want_one)
    {
        tool_nx_quiet(TRUE);
        (VOID)tool_neighbours(&arp_nd);
        tool_nx_quiet(FALSE);

        for (i = 0; i < (ULONG)arp_nd.count; i++)
        {
            const ToolNeighbour *e = &arp_nd.entry[i];

            if (want_one6 && !same_address6(e->addr, want6))
                continue;

            if (shown6 == 0 && !quiet)
            {
                if (shown != 0)
                    tool_printf("\n");
                tool_printf("Neighbour                 Hardware address\n");
            }

            print_neighbour(e, have_snapshot);
            shown6++;
        }

        if (shown6 != 0 && !quiet)
            explain_states(&arp_nd, want6, want_one6);
    }

    if (shown == 0 && shown6 == 0)
    {
        if (want_one6)
        {
            tool_printf("%s is not in the neighbour cache.\n",
                        (LONG)address_text);
            explain_absence6(want6, address_text, have_routes6);
            rc = RETURN_WARN;
        }
        else if (want_one)
        {
            tool_printf("%s is not in the cache.\n", (LONG)address_text);
            explain_absence(want, address_text, have_snapshot);
            rc = RETURN_WARN;
        }
        else
        {
            /* The answer, not commentary: an empty cache is what the run
               found. QUIET drops the headings above it, not this. */
            tool_printf("The address cache is empty.\n");
        }
    }
    else if ((arp_stats.arp_truncated || arp_nd.truncated) && !quiet)
    {
        /*
         * The snapshot holds TOOL_MAX_ARP entries and the stack can report
         * more in nsh_Available. A list that silently stops reads as complete,
         * which is worse than a short one: it is the same output a machine
         * with nothing else in its cache produces.
         *
         * Neither cache can currently reach this. AMI_ARP_CACHE_SIZE is 1024
         * bytes of NX_ARP, which is 19 entries against TOOL_MAX_ARP's 32, and
         * NX_IPV6_NEIGHBOR_CACHE_SIZE is 8 against TOOL_MAX_ND's 16 --
         * measured, tests/tools/run-toolsay.sh fills the first with `arp SET=`
         * and the 20th is refused. So this is what happens when either
         * ceiling is raised past the other, not something to go looking for
         * today.
         */
        tool_printf("\n");

        if (arp_stats.arp_truncated)
            tool_printf("The address cache has more than %ld entries and only "
                        "the first %ld are shown.\n",
                        (LONG)TOOL_MAX_ARP, (LONG)TOOL_MAX_ARP);

        if (arp_nd.truncated)
            tool_printf("The neighbour cache has more than %ld entries and "
                        "only the first %ld are shown.\n",
                        (LONG)TOOL_MAX_ND, (LONG)TOOL_MAX_ND);

        tool_printf("Name one address to see just that entry:\n");
        tool_printf("      arp 192.168.1.1\n");
    }

    FreeArgs(rda);
    return rc;
}
