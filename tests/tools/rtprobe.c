/*
 * RtProbe -- exercises the Roadshow routing API on a running stack.
 *
 * tests/tools/routeprobe.c covers the private NETCTRL_ROUTE_ADD vector.  This
 * covers the published API on top of it: AddRouteTagList(),
 * DeleteRouteTagList(), GetRouteInfo() and FreeRouteInfo(), called at their
 * LVOs by a program that knows only the NDK headers.
 *
 * The shape under test is GetRouteInfo()'s, which a compiler cannot check.
 * The autodoc says the table is "a header followed by a small number of
 * sockadders, interpreted by position", with rtm_addrs as the map and a
 * terminator whose rtm_msglen is zero.  So this walks the table the way a
 * caller must -- advance by rtm_msglen, stop at zero, read the sockaddrs by
 * their bit order -- rather than indexing an array of rt_msghdr, which the
 * prototype alone would suggest and which walks off the end of the first
 * entry.
 *
 * The default gateway is read and then set to the value it already had.  This
 * run is riding on it; deleting it to exercise DeleteRouteTagList() on default
 * gateways would take the machine off the network for the rest of the test.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <dos/dos.h>
#include <utility/tagitem.h>

/* <libraries/bsdsocket.h> pulls in <sys/socket.h>, which uses size_t and
   ssize_t without declaring them. Same ordering note as ifprobe.c. */
#include <stddef.h>
#include <sys/types.h>
#include <libraries/bsdsocket.h>
#include <net/route.h>

#include <proto/exec.h>
#include <proto/dos.h>

/* ------------------------------------------------------------- vectors ---- */

static LONG p_add_route(struct Library *base, struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-414:W)"     /* AddRouteTagList    -0x19e */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static LONG p_delete_route(struct Library *base, struct TagItem *tags)
{
    register struct Library *a6  __asm("a6") = base;
    register APTR            a0  __asm("a0") = (APTR)tags;
    register LONG            res __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-420:W)"     /* DeleteRouteTagList -0x1a4 */
                      : "=r" (res), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
    return res;
}

static VOID p_free_route_info(struct Library *base, struct rt_msghdr *table)
{
    register struct Library  *a6 __asm("a6") = base;
    register struct rt_msghdr *a0 __asm("a0") = table;
    register LONG _clob_d0 __asm("d0");
    register LONG _clob_d1 __asm("d1");
    register LONG _clob_a0 __asm("a0");

    __asm __volatile ("jsr a6@(-432:W)"     /* FreeRouteInfo      -0x1b0 */
                      : "=r" (_clob_d0), "=r" (_clob_d1), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "cc", "memory");
}

static struct rt_msghdr *p_get_route_info(struct Library *base, LONG af,
                                          LONG flags)
{
    register struct Library  *a6  __asm("a6") = base;
    register LONG             d0  __asm("d0") = af;
    register LONG             d1  __asm("d1") = flags;
    register struct rt_msghdr *res __asm("d0");
    register LONG _clob_d1 __asm("d1");

    __asm __volatile ("jsr a6@(-438:W)"     /* GetRouteInfo       -0x1b6 */
                      : "=r" (res), "=r" (_clob_d1)
                      : "r" (a6), "r" (d0), "r" (d1)
                      : "a0", "a1", "cc", "memory");
    return res;
}

static LONG p_errno(struct Library *base)
{
    register struct Library *a6  __asm("a6") = base;
    register LONG            res __asm("d0");

    __asm __volatile ("jsr a6@(-162:W)"     /* Errno */
                      : "=r" (res)
                      : "r" (a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return res;
}

/* ----------------------------------------------------------- little helpers */

struct sockaddr_in_probe
{
    UBYTE   sin_len;
    UBYTE   sin_family;
    UWORD   sin_port;
    ULONG   sin_addr;               /* network byte order == host order here */
    UBYTE   sin_zero[8];
};

static VOID p_dotted(ULONG addr, char *out)
{
    static const char digits[] = "0123456789";
    const UBYTE      *b        = (const UBYTE *)&addr;
    ULONG             pos      = 0;
    ULONG             i;

    for (i = 0; i < 4; i++)
    {
        ULONG v = b[i];

        if (v >= 100)
            out[pos++] = digits[v / 100];
        if (v >= 10)
            out[pos++] = digits[(v / 10) % 10];
        out[pos++] = digits[v % 10];

        if (i != 3)
            out[pos++] = '.';
    }
    out[pos] = '\0';
}

/*
 * The sockaddr for one RTA_ bit, found the way the routing-socket convention
 * says: walk the bits from least significant, skipping the ones that are not
 * set, and step over each present address by its own sa_len.
 */
static const struct sockaddr_in_probe *p_route_addr(const struct rt_msghdr *hdr,
                                                    LONG want)
{
    const UBYTE *p = (const UBYTE *)hdr + sizeof(struct rt_msghdr);
    LONG         bit;

    for (bit = 1; bit != 0 && bit <= RTA_AUTHOR; bit <<= 1)
    {
        const struct sockaddr_in_probe *sa;

        if ((hdr->rtm_addrs & bit) == 0)
            continue;

        sa = (const struct sockaddr_in_probe *)p;

        if (bit == want)
            return sa;

        p += (sa->sin_len != 0) ? sa->sin_len
                                : sizeof(struct sockaddr_in_probe);
    }

    return NULL;
}

static ULONG p_route_addr_value(const struct rt_msghdr *hdr, LONG want)
{
    const struct sockaddr_in_probe *sa = p_route_addr(hdr, want);

    return (sa != NULL) ? sa->sin_addr : 0;
}

/*
 * Print the table and return how many entries it had. `label` goes on every
 * line so the shell can count occurrences of one listing without matching
 * another.
 */
static LONG p_show_table(struct Library *base, const char *label, LONG af,
                         LONG flags)
{
    struct rt_msghdr *table;
    struct rt_msghdr *hdr;
    LONG              count = 0;

    table = p_get_route_info(base, af, flags);
    if (table == NULL)
    {
        Printf((CONST_STRPTR)"routes %s: NULL (errno %ld)\n",
               (LONG)label, p_errno(base));
        return -1;
    }

    hdr = table;
    while (hdr->rtm_msglen != 0)
    {
        char  dest[16];
        char  gw[16];
        char  mask[16];
        char  text[8];
        UWORD n = 0;

        p_dotted(p_route_addr_value(hdr, RTA_DST), dest);
        p_dotted(p_route_addr_value(hdr, RTA_GATEWAY), gw);
        p_dotted(p_route_addr_value(hdr, RTA_NETMASK), mask);

        if (hdr->rtm_flags & RTF_UP)      text[n++] = 'U';
        if (hdr->rtm_flags & RTF_GATEWAY) text[n++] = 'G';
        if (hdr->rtm_flags & RTF_HOST)    text[n++] = 'H';
        if (hdr->rtm_flags & RTF_STATIC)  text[n++] = 'S';
        text[n] = '\0';

        Printf((CONST_STRPTR)"  %s %-16s %-16s %-16s %-5s v%ld if %ld mtu %ld\n",
               (LONG)label, (LONG)dest, (LONG)gw, (LONG)mask, (LONG)text,
               (LONG)hdr->rtm_version, (LONG)hdr->rtm_index,
               (LONG)hdr->rtm_rmx.rmx_mtu);

        count++;
        hdr = (struct rt_msghdr *)((UBYTE *)hdr + hdr->rtm_msglen);
    }

    Printf((CONST_STRPTR)"routes %s: %ld entries\n", (LONG)label, count);

    p_free_route_info(base, table);

    return count;
}

/* The default gateway's address, or 0 -- the entry whose destination and mask
   are both zero. */
static ULONG p_default_gateway(struct Library *base)
{
    struct rt_msghdr *table = p_get_route_info(base, AF_UNSPEC, 0);
    struct rt_msghdr *hdr;
    ULONG             gateway = 0;

    if (table == NULL)
        return 0;

    for (hdr = table; hdr->rtm_msglen != 0;
         hdr = (struct rt_msghdr *)((UBYTE *)hdr + hdr->rtm_msglen))
    {
        if (p_route_addr_value(hdr, RTA_DST) == 0 &&
            p_route_addr_value(hdr, RTA_NETMASK) == 0)
        {
            gateway = p_route_addr_value(hdr, RTA_GATEWAY);
            break;
        }
    }

    p_free_route_info(base, table);

    return gateway;
}

/* ------------------------------------------------------------------ main -- */

/* On the guest's own subnet, so NetX Duo will accept it as a next hop, and
   answered by nothing: SLIRP is 10.0.2.2 and 10.0.2.3. */
#define PROBE_NEXTHOP   "10.0.2.98"
#define PROBE_NET       "192.168.66.0"      /* class C, zero host part */
#define PROBE_HOST      "192.168.67.7"      /* class C, non-zero host part */

int main(void)
{
    struct Library   *base;
    struct rt_msghdr *table;
    struct TagItem    tags[4];
    char              gw_text[16];
    ULONG             gateway;
    LONG              rc;
    LONG              before;
    LONG              with;
    LONG              after;

    base = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (base == NULL)
    {
        Printf((CONST_STRPTR)"RtProbe: no bsdsocket.library\n");
        return RETURN_FAIL;
    }

    before = p_show_table(base, "before", AF_UNSPEC, 0);

    /* ---- a network route, whose mask the grammar has to imply -------------
     *
     * There is no netmask tag. 192.168.66.0 has a zero host part under its
     * classful mask, so "the route is assumed to be a to a network" and the
     * mask must come back as 255.255.255.0 -- from nothing but the address.
     */
    tags[0].ti_Tag  = RTA_Destination;
    tags[0].ti_Data = (ULONG)PROBE_NET;
    tags[1].ti_Tag  = RTA_Gateway;
    tags[1].ti_Data = (ULONG)PROBE_NEXTHOP;
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    rc = p_add_route(base, tags);
    Printf((CONST_STRPTR)"add %s via %s: rc %ld (errno %ld)\n",
           (LONG)PROBE_NET, (LONG)PROBE_NEXTHOP, rc, p_errno(base));

    /* ---- and a host route, from an address with a non-zero host part ----- */
    tags[0].ti_Tag  = RTA_Destination;
    tags[0].ti_Data = (ULONG)PROBE_HOST;
    tags[1].ti_Tag  = RTA_Gateway;
    tags[1].ti_Data = (ULONG)PROBE_NEXTHOP;
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    rc = p_add_route(base, tags);
    Printf((CONST_STRPTR)"add %s via %s: rc %ld (errno %ld)\n",
           (LONG)PROBE_HOST, (LONG)PROBE_NEXTHOP, rc, p_errno(base));

    with = p_show_table(base, "with", AF_UNSPEC, 0);

    /* ---- the flags filter ------------------------------------------------
     *
     * "Flags which have to be set in each routing table entry to be returned"
     * -- so RTF_STATIC returns the two just added and nothing else; the count
     * is the assertion.
     */
    (VOID)p_show_table(base, "static-only", AF_INET, RTF_STATIC);

    /* ---- the exclusions the autodoc states ------------------------------- */
    tags[0].ti_Tag  = RTA_Destination;
    tags[0].ti_Data = (ULONG)PROBE_NET;
    tags[1].ti_Tag  = RTA_DefaultGateway;
    tags[1].ti_Data = (ULONG)PROBE_NEXTHOP;
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    rc = p_add_route(base, tags);
    Printf((CONST_STRPTR)"add dest+default together: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    tags[0].ti_Tag  = RTA_Destination;
    tags[0].ti_Data = (ULONG)"192.168.68.0";
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_add_route(base, tags);
    Printf((CONST_STRPTR)"add dest with no gateway: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* A next hop on no local subnet: NetX Duo derives the outgoing interface
       from it, and there is none. */
    tags[0].ti_Tag  = RTA_Destination;
    tags[0].ti_Data = (ULONG)"172.16.0.0";
    tags[1].ti_Tag  = RTA_Gateway;
    tags[1].ti_Data = (ULONG)"8.8.8.8";
    tags[2].ti_Tag  = TAG_DONE;
    tags[2].ti_Data = 0;

    rc = p_add_route(base, tags);
    Printf((CONST_STRPTR)"add via an unreachable next hop: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* ---- the default gateway, set to what it already is ------------------ */
    gateway = p_default_gateway(base);
    if (gateway != 0)
    {
        p_dotted(gateway, gw_text);

        tags[0].ti_Tag  = RTA_DefaultGateway;
        tags[0].ti_Data = (ULONG)gw_text;
        tags[1].ti_Tag  = TAG_DONE;
        tags[1].ti_Data = 0;

        rc = p_add_route(base, tags);
        Printf((CONST_STRPTR)"set the default gateway to %s again: rc %ld\n",
               (LONG)gw_text, rc);
    }
    else
    {
        Printf((CONST_STRPTR)"set the default gateway: there is none to read\n");
    }

    /* ---- an address family that is not IPv4 ------------------------------- */
    table = p_get_route_info(base, 23, 0);      /* AF_INET6 in this NDK */
    Printf((CONST_STRPTR)"GetRouteInfo(AF_INET6): %s (errno %ld)%s\n",
           (LONG)((table == NULL) ? "NULL" : "a table"), p_errno(base),
           (LONG)((table == NULL) ? " -- refused, correctly"
                                  : " -- RETURNED ONE, WRONG"));
    p_free_route_info(base, table);

    /*
     * Deleting one that was never there.  This has to happen here, while the
     * two above are still in the static table:
     * nx_ip_static_route_delete() returns NX_SUCCESS outright when
     * nx_ip_routing_table_entry_count is zero, without searching, and the
     * default gateway is not in that table -- it lives in
     * nx_ip_gateway_address.  A machine whose only route is its default
     * gateway has an empty static table, so run after the two deletes below
     * this call reports success regardless.
     */
    tags[0].ti_Tag  = RTA_Destination;
    tags[0].ti_Data = (ULONG)"192.168.69.0";
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_delete_route(base, tags);
    Printf((CONST_STRPTR)"delete a route never added: rc %ld (errno %ld)%s\n",
           rc, p_errno(base),
           (LONG)((rc != 0) ? " -- refused, correctly" : " -- ACCEPTED, WRONG"));

    /* ---- and undo ---------------------------------------------------------
     *
     * The same destination strings, with no mask given either time: delete
     * has to derive the same prefix length add did, or the entry can never
     * be found again.
     */
    tags[0].ti_Tag  = RTA_Destination;
    tags[0].ti_Data = (ULONG)PROBE_NET;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_delete_route(base, tags);
    Printf((CONST_STRPTR)"delete %s: rc %ld (errno %ld)\n",
           (LONG)PROBE_NET, rc, p_errno(base));

    tags[0].ti_Tag  = RTA_Destination;
    tags[0].ti_Data = (ULONG)PROBE_HOST;
    tags[1].ti_Tag  = TAG_DONE;
    tags[1].ti_Data = 0;

    rc = p_delete_route(base, tags);
    Printf((CONST_STRPTR)"delete %s: rc %ld (errno %ld)\n",
           (LONG)PROBE_HOST, rc, p_errno(base));

    after = p_show_table(base, "after", AF_UNSPEC, 0);

    Printf((CONST_STRPTR)"counts: before %ld, with %ld, after %ld%s\n",
           before, with, after,
           (LONG)((before >= 0 && with == before + 2 && after == before)
                      ? " -- two added and two removed, correctly"
                      : " -- WRONG"));

    /* Documented to do nothing rather than to fault. */
    p_free_route_info(base, NULL);
    Printf((CONST_STRPTR)"FreeRouteInfo(NULL): returned\n");

    CloseLibrary(base);

    return RETURN_OK;
}
