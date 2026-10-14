/*
 * arp, the address resolution cache: what is at each address on this network.
 * IPv6 is in the same command, in a section of its own -- RFC 4861 neighbour
 * discovery, kept in a separate table. Nothing ages out on the IPv4 side:
 * NX_ARP_EXPIRATION_RATE is 0 in this build. It does not start the network.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tools.h"
#include "tools_nx.h"

const char *const tool_name = "arp";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("arp");

/* The AmigaDOS Command Reference's template, with one correction: it writes
   -n=NONAMES/S=NUMBERS/S, which puts NUMBERS among the modifiers where
   ReadArgs never matches it.  test_refcompat holds TEMPLATE to this. */
#define REFERENCE_TEMPLATE "-a=ALL/S,-d=DELETE/S,-s=SET/S,HOSTNAME,ADDRESS," \
                           "TEMP/S,PUB=PUBLISH/S,PRO=PROXY/S,-f=FILE/K,"     \
                           "-n=NONAMES=NUMBERS/S"

#define TEMPLATE    "-a=ALL/S,-d=DELETE/S,-s=SET/S,HOSTNAME,ADDRESS,TEMP/S,"  \
                    "PUB=PUBLISH/S,PRO=PROXY/S,-f=FILE/K,"                    \
                    "-n=NONAMES=NUMBERS/S,UNIT/K/N,STATS/S,QUIET/S"

enum
{
    ARG_ALL = 0,
    ARG_DELETE,
    ARG_SET,
    ARG_HOSTNAME,
    ARG_ADDRESS,
    ARG_TEMP,
    ARG_PUB,
    ARG_PRO,
    ARG_FILE,
    ARG_NONAMES,
    ARG_UNIT,
    ARG_STATS,
    ARG_QUIET,
    ARG_COUNT
};

static ToolStats      arp_stats;
static ToolSnapshot   arp_snap;
static ToolNeighbours arp_nd;
static ToolRoutes6    arp_routes6;
static NetStatusControl arp_ctl;    /* static for the Shell's 4K stack */
static char           arp_line[128];  /* a FILE line, or a formatted MAC  */
static BOOL           arp_quiet;

static VOID zero_control(NetStatusControl *ctl)
{
    ULONG i;

    for (i = 0; i < (ULONG)(sizeof(*ctl) / sizeof(ULONG)); i++)
        ((ULONG *)ctl)[i] = 0;
}

/* "02:11:22:33:44:55". The separator can be ':' or '-' or absent. */
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
 * "a2065.0" identifies the card on a machine with two of them.
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
     * An unresolved entry has an all-zero hardware address; printing it would
     * read as a strange address rather than as silence.
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

/* A colon says the user meant an IPv6 address. Nothing else can have one. */
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
 * address can be used in every state but INCOMPLETE.
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

    /* 24 rather than 39: it lines up every address short enough to be typed by
       hand, and a longer one pushes the rest right instead of being cut. */
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

/* What each state that appeared means, once, under the list. The names are
   RFC 4861's and say nothing to a reader who has not read it. */
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
 * TRUE when nothing on this machine can ever put `addr` in the neighbour cache:
 * not link-local and no on-link prefix covers it, so packets for it go to a
 * router and the router's own entry is the one to check.
 */
static BOOL off_link6(const ULONG addr[4], BOOL have_routes)
{
    UWORD i;

    if (!have_routes)
        return FALSE;               /* cannot tell, so do not assert otherwise */

    /* _nxd_ipv6_search_onlink() answers 1 for every fe80:: address before it
       looks at any list, so there is no prefix to find. */
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

/* Why an address is not in the neighbour cache: the IPv6 half of
   explain_absence() below.  On-link needs no words: neighbour entries age
   out, so an absence is "nothing has spoken to it lately", not a fault. */
static VOID explain_absence6(const ULONG addr[4], BOOL have_routes)
{
    const char *router;

    if (!off_link6(addr, have_routes))
        return;

    router = default_router6(have_routes);
    if (router != NULL)
        tool_printf("  off-link, router %s\n", (LONG)router);
    else
        tool_printf("  off-link\n");
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
 * Why an address is not in the cache. ARP is only spoken to machines on this
 * machine's own network; an address anywhere else never appears here however
 * much traffic goes to it.  On-net needs no words: an absence there means
 * nothing has sent to it since the stack came up.
 */
static VOID explain_absence(ULONG addr, BOOL have_snapshot)
{
    char gw[16];

    if (on_our_network(addr, have_snapshot))
        return;

    if (arp_snap.have_gateway && arp_snap.gateway != 0)
    {
        ami_config_format_ip(arp_snap.gateway, gw, sizeof(gw));
        tool_printf("  off-net, router %s\n", (LONG)gw);
    }
    else
    {
        tool_printf("  off-net\n");
    }
}

/*
 * The protocol counters. Requests sent with few responses received is a quiet
 * network or a wrong netmask; invalid messages are somebody else's frames.
 */
static VOID print_stats(const ToolStats *s)
{
    if (!s->have_arp)
    {
        tool_printf("no ARP counters\n");
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

/* ------------------------------------------------------ host and changes, */

/* A host as the reference takes it: a name or a number.  An IPv6 address is
   written with colons and is never looked up.  RETURN_OK, or the code to exit
   with after the message has been printed. */
static LONG resolve_host(const char *text, ULONG *want, ULONG want6[4],
                         BOOL *is6)
{
    *is6 = FALSE;

    if (is_written_as_ip6(text))
    {
        struct Library *pb;
        BOOL            has6;

        if (!tool_parse_ip6(text, want6))
        {
            tool_error("\"%s\" is not an address", (LONG)text);
            return RETURN_ERROR;
        }

        /*
         * Asked before anything is printed, because every later answer would be
         * "not in the cache" and read as a fact about the address.
         */
        pb = tool_netstatus_open(FALSE);
        if (pb == NULL)
            return RETURN_ERROR;

        has6 = stack_has_ipv6(pb);
        tool_netstatus_close(pb);

        if (!has6)
        {
            tool_error("%s: stack has no IPv6", (LONG)text);
            return RETURN_FAIL;
        }

        *is6 = TRUE;
        return RETURN_OK;
    }

    if (ami_config_parse_ip(text, want))
        return RETURN_OK;

    /* The running stack's resolver: DEVS:Internet/hosts, then DNS. */
    if (!tool_stack_library_running())
    {
        tool_error("%s: network not started, names cannot be looked up",
                   (LONG)text);
        return RETURN_ERROR;
    }

    if (!tool_stack_lookup(text, want))
    {
        tool_error("%s: unknown host", (LONG)text);
        return RETURN_ERROR;
    }

    return RETURN_OK;
}

/* One SET: the cache that matches how the host was written.  TEMP makes an
   IPv4 entry dynamic -- replaced by later traffic, gone with a flush --
   rather than permanent. */
static LONG set_entry(struct Library *base, const char *host,
                      const char *mac_text, BOOL temp, const LONG *unit)
{
    UBYTE mac[AMI_ETH_ADDR_SIZE];
    ULONG want = 0;
    ULONG want6[4];
    BOOL  is6;
    LONG  err = 0;
    LONG  rc;
    ULONG i;

    if (!parse_mac(mac_text, mac))
    {
        tool_error("\"%s\" is not a hardware address", (LONG)mac_text);
        return RETURN_ERROR;
    }

    rc = resolve_host(host, &want, want6, &is6);
    if (rc != RETURN_OK)
        return rc;

    /*
     * The IPv4 ARP cache is one table, not one per interface: the static-ARP
     * call takes no interface and derives it from the address's route, so a
     * UNIT on an IPv4 SET would be read nowhere.  Refuse it rather than drop
     * it.  The IPv6 path is per-interface and its add reads the index.
     */
    if (!is6 && unit != NULL)
    {
        tool_error("UNIT cannot be used with an IPv4 SET");
        return RETURN_ERROR;
    }

    zero_control(&arp_ctl);

    if (is6)
    {
        for (i = 0; i < 4UL; i++)
            arp_ctl.nsc_Destination6[i] = want6[i];
        if (unit != NULL)
            arp_ctl.nsc_Index = (UWORD)*unit;
    }
    else
    {
        arp_ctl.nsc_Destination = want;
        if (temp)
            arp_ctl.nsc_Flags = NETCTRL_F_TEMPORARY;
    }

    for (i = 0; i < (ULONG)AMI_ETH_ADDR_SIZE; i++)
        arp_ctl.nsc_HwAddress[i] = mac[i];

    if (tool_netstatus_control(base, is6 ? (ULONG)NETCTRL_ND_ADD
                                         : (ULONG)NETCTRL_ARP_ADD,
                               &arp_ctl, &err) != 0)
    {
        tool_error("%s was not added to the cache", (LONG)host);
        return RETURN_FAIL;
    }

    if (!arp_quiet)
    {
        tool_format_mac(mac, arp_line, sizeof(arp_line));
        tool_printf("%s: %s, %s\n", (LONG)host, (LONG)arp_line,
                    (LONG)((temp && !is6) ? "temporary" : "permanent"));
    }

    return RETURN_OK;
}

static LONG delete_entry(struct Library *base, const char *host)
{
    ULONG want = 0;
    ULONG want6[4];
    BOOL  is6;
    LONG  err = 0;
    LONG  rc;
    ULONG i;

    rc = resolve_host(host, &want, want6, &is6);
    if (rc != RETURN_OK)
        return rc;

    zero_control(&arp_ctl);

    if (is6)
    {
        for (i = 0; i < 4UL; i++)
            arp_ctl.nsc_Destination6[i] = want6[i];
    }
    else
    {
        arp_ctl.nsc_Destination = want;
    }

    if (tool_netstatus_control(base, is6 ? (ULONG)NETCTRL_ND_DELETE
                                         : (ULONG)NETCTRL_ARP_DELETE,
                               &arp_ctl, &err) != 0)
    {
        tool_error("%s was not removed from the cache", (LONG)host);
        return RETURN_FAIL;
    }

    if (!arp_quiet)
        tool_printf("%s: deleted\n", (LONG)host);

    return RETURN_OK;
}

/* The next word of a FILE line, terminated in place, or NULL. */
static char *next_word(char **cursor)
{
    char *p = *cursor;
    char *word;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0' || *p == '\n' || *p == '\r')
        return NULL;

    word = p;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    if (*p != '\0')
        *p++ = '\0';

    *cursor = p;
    return word;
}

/*
 * FILE: one `<host> <hardware address> [TEMP] [PUB]` per line, as SET takes
 * them.  Blank lines and lines starting with # or ; are skipped.  Every line
 * is tried; the return code is the worst.
 */
static LONG set_from_file(struct Library *base, const char *path)
{
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    LONG worst = RETURN_OK;
    LONG line = 0;

    if (fh == (BPTR)0)
    {
        tool_error("%s cannot be opened", (LONG)path);
        return RETURN_ERROR;
    }

    while (FGets(fh, (STRPTR)arp_line, sizeof(arp_line) - 1) != NULL)
    {
        char *cursor = arp_line;
        char *host;
        char *mac;
        char *word;
        BOOL  temp = FALSE;
        LONG  rc   = RETURN_OK;

        line++;

        host = next_word(&cursor);
        if (host == NULL || host[0] == '#' || host[0] == ';')
            continue;

        mac = next_word(&cursor);
        if (mac == NULL)
        {
            tool_error("%s line %ld: no hardware address", (LONG)path, line);
            rc = RETURN_ERROR;
        }

        while (rc == RETURN_OK && (word = next_word(&cursor)) != NULL)
        {
            if (tool_stricmp(word, "TEMP") == 0)
                temp = TRUE;
            else if (tool_stricmp(word, "PUB") == 0 ||
                     tool_stricmp(word, "PUBLISH") == 0)
            {
                tool_error("%s line %ld: published entries are not supported",
                           (LONG)path, line);
                rc = RETURN_ERROR;
            }
            else
            {
                tool_error("%s line %ld: \"%s\" is not TEMP or PUB",
                           (LONG)path, line, (LONG)word);
                rc = RETURN_ERROR;
            }
        }

        if (rc == RETURN_OK)
            rc = set_entry(base, host, mac, temp, NULL);

        if (rc > worst)
            worst = rc;

        if (tool_break())
        {
            tool_fault(ERROR_BREAK);
            if (worst < RETURN_WARN)
                worst = RETURN_WARN;
            break;
        }
    }

    Close(fh);
    return worst;
}

static VOID usage(VOID)
{
    tool_usage("[ALL] | <host> | DELETE <host> | SET <host> <hardware address> "
               "[TEMP] | FILE <file>",
               "List or change the address resolution cache.");
}

/* What is wrong with this combination, or NULL. */
static const char *misuse(const LONG *args)
{
    BOOL set  = (BOOL)(args[ARG_SET] != 0);
    BOOL del  = (BOOL)(args[ARG_DELETE] != 0);
    BOOL file = (BOOL)(args[ARG_FILE] != 0);
    BOOL host = (BOOL)(args[ARG_HOSTNAME] != 0);

    if (set && del)
        return "SET and DELETE cannot be combined";
    if (args[ARG_ALL] != 0 && (set || del || file))
        return "ALL lists the cache; it does not go with SET, DELETE or FILE";
    if (file && (set || del || host))
        return "FILE cannot be combined with SET, DELETE or a host";
    if ((args[ARG_TEMP] != 0 || args[ARG_PUB] != 0 || args[ARG_PRO] != 0) &&
        !set)
        return "TEMP, PUBLISH and PROXY go with SET";
    if (args[ARG_PUB] != 0 || args[ARG_PRO] != 0)
        return "published and proxy entries are not supported";
    if (args[ARG_ADDRESS] != 0 && !set)
        return "ADDRESS is the hardware address for SET";
    if (set && (!host || args[ARG_ADDRESS] == 0))
        return "SET needs a host and a hardware address";
    if (del && !host)
        return "DELETE needs a host";

    return NULL;
}

int main(int argc, char **argv)
{
    LONG              args[ARG_COUNT];
    struct RDArgs    *rda;
    struct Library   *base;
    const char       *address_text = NULL;
    const char       *wrong;
    ULONG             want         = 0;
    ULONG             want6[4]     = { 0, 0, 0, 0 };
    BOOL              want_one     = FALSE;
    BOOL              want_one6    = FALSE;
    BOOL              is6          = FALSE;
    BOOL              have_snapshot;
    BOOL              have_routes6 = FALSE;
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
        usage();
        return RETURN_ERROR;
    }

    arp_quiet = (BOOL)(args[ARG_QUIET] != 0);

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

    wrong = misuse(args);
    if (wrong != NULL)
    {
        tool_error("%s", (LONG)wrong);
        usage();
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    /* ALL is the listing with no host, as on Roadshow.  -n changes nothing:
       the listing is numeric already. */
    if (args[ARG_ALL] == 0)
        address_text = (const char *)args[ARG_HOSTNAME];

    /* ------------------------------------------- SET, DELETE and FILE --- */
    if (args[ARG_SET] != 0 || args[ARG_DELETE] != 0 || args[ARG_FILE] != 0)
    {
        /* Changing the cache needs the library, but still must not start it.
           FALSE: the reason there is none is printed. */
        base = tool_netstatus_open(FALSE);
        if (base == NULL)
        {
            FreeArgs(rda);
            return RETURN_ERROR;
        }

        if (args[ARG_FILE] != 0)
            rc = set_from_file(base, (const char *)args[ARG_FILE]);
        else if (args[ARG_SET] != 0)
            rc = set_entry(base, address_text,
                           (const char *)args[ARG_ADDRESS],
                           (BOOL)(args[ARG_TEMP] != 0),
                           (const LONG *)args[ARG_UNIT]);
        else
            rc = delete_entry(base, address_text);

        tool_netstatus_close(base);
        FreeArgs(rda);
        return rc;
    }

    if (address_text != NULL)
    {
        rc = resolve_host(address_text, &want, want6, &is6);
        if (rc != RETURN_OK)
        {
            FreeArgs(rda);
            return rc;
        }

        want_one6 = is6;
        want_one  = (BOOL)!is6;
    }

    /*
     * The interface table, for the name column and for deciding whether ARP can
     * ever have an answer about an address. A failure here only costs detail.
     */
    tool_nx_quiet(TRUE);
    have_snapshot = (BOOL)(tool_snapshot(&arp_snap, FALSE) == 0);
    /* The IPv6 on-link prefixes, which are what says whether an address can
       ever be a neighbour. Empty on a machine without IPv6. */
    have_routes6  = (BOOL)(tool_routes6(&arp_routes6) == 0);
    tool_nx_quiet(FALSE);

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

    /* An IPv6 address was asked for: the ARP cache cannot hold one, so the loop
       below would print a heading over nothing. */
    if (!want_one6)
    {
        for (i = 0; i < (ULONG)arp_stats.arp_count; i++)
        {
            if (want_one && arp_stats.arp[i].address != want)
                continue;

            if (shown == 0 && !arp_quiet)
                tool_printf("Address          Hardware address\n");

            print_entry(&arp_stats.arp[i], have_snapshot);
            shown++;
        }
    }

    /*
     * The neighbour cache, under the ARP one. Empty on a machine whose library
     * has no IPv6.
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

            if (shown6 == 0 && !arp_quiet)
            {
                if (shown != 0)
                    tool_printf("\n");
                tool_printf("Neighbour                 Hardware address\n");
            }

            print_neighbour(e, have_snapshot);
            shown6++;
        }

        if (shown6 != 0 && !arp_quiet)
            explain_states(&arp_nd, want6, want_one6);
    }

    if (shown == 0 && shown6 == 0)
    {
        if (want_one6)
        {
            tool_printf("%s is not in the neighbour cache.\n",
                        (LONG)address_text);
            explain_absence6(want6, have_routes6);
            rc = RETURN_WARN;
        }
        else if (want_one)
        {
            tool_printf("%s is not in the cache.\n", (LONG)address_text);
            explain_absence(want, have_snapshot);
            rc = RETURN_WARN;
        }
        else
        {
            /* The answer, not commentary: an empty cache is what the run found.
               QUIET drops the headings above it, not this. */
            tool_printf("The address cache is empty.\n");
        }
    }
    else if ((arp_stats.arp_truncated || arp_nd.truncated) && !arp_quiet)
    {
        /*
         * The snapshot holds TOOL_MAX_ARP entries and the stack can report more
         * in nsh_Available. A list that silently stops reads as complete.
         */
        tool_printf("\n");

        if (arp_stats.arp_truncated)
            tool_printf("address cache: first %ld entries shown\n",
                        (LONG)TOOL_MAX_ARP);

        if (arp_nd.truncated)
            tool_printf("neighbour cache: first %ld entries shown\n",
                        (LONG)TOOL_MAX_ND);
    }

    FreeArgs(rda);
    return rc;
}
