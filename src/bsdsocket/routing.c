/*
 * bsdsocket.library -- the Roadshow routing API.
 *
 *   AddRouteTagList()       a gateway route, or the default one
 *   DeleteRouteTagList()    the same, undone
 *   GetRouteInfo()          a copy of the whole table, as rt_msghdr entries
 *   FreeRouteInfo()
 *
 * WRITTEN FROM THE AUTODOC
 *
 * Same primary source as interfaces.c: NDK 3.2's
 * SANA+RoadshowTCP-IP/doc/bsdsocket.doc, plus libraries/bsdsocket.h and
 * net/route.h from the same NDK, used as an ABI reference only. No Roadshow,
 * AmiTCP, AROSTCP or Miami code was consulted or is present.
 *
 * THREE THINGS THE DOCUMENT SETTLED
 *
 *   1. GetRouteInfo() returns "a header followed by a small number of
 *      sockadders, interpreted by position ... The interpretation of which
 *      address are present is given by a bit mask within the header, and the
 *      sequence is least significant to most significant bit within the
 *      vector" -- the routing-socket layout, so rtm_addrs is the map and the
 *      sockaddrs follow in RTA_DST, RTA_GATEWAY, RTA_NETMASK order. Nothing
 *      in the prototype says any of that; a caller handed a bare array of
 *      rt_msghdr would walk straight off the end of the first entry.
 *
 *   2. "The table is terminated by a dummy entry whose 'rtm_msglen' member is
 *      zero." Not a count, not a NULL. A caller loops on rtm_msglen and stops
 *      when it is zero, so a table without that entry never ends.
 *
 *   3. rtm_version is 3: "The 'struct rt_msghdr' layout described above
 *      corresponds to version 3", and RTM_VERSION in net/route.h agrees.
 *
 * AND ONE THE DOCUMENT GETS WRONG, WHICH IS WORTH KNOWING
 *
 * The rt_msghdr the autodoc prints has the same eleven members as the one in
 * net/route.h and NOT the same order: the document has rtm_pid before
 * rtm_addrs and rtm_flags after rtm_errno, the header has rtm_flags second and
 * rtm_pid fourth. The HEADER is the ABI, because it is what a caller compiles
 * against. Everything below is written against the header and uses member
 * names throughout, so the disagreement cannot reach the wire.
 *
 * WHAT THE ROUTING TAG GRAMMAR DOES NOT HAVE
 *
 * There is no netmask tag. AddRouteTagList() takes RTA_Destination,
 * RTA_Gateway, RTA_DefaultGateway, RTA_DestinationHost and RTA_DestinationNet
 * and nothing else, so the prefix length is IMPLIED by which of them was used
 * -- see bsd_route_mask_for() below, where that rule is written out.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"

#include "tagwalk.h"

#include <proto/exec.h>

/*
 * How long a name in RTA_Destination or RTA_Gateway is given to resolve --
 * the same thirty seconds resolver.c gives gethostbyname(), because it is the
 * same lookup.
 */
#define BSD_ROUTE_RESOLVE_TIMEOUT   (30UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * 4.4BSD's ESRCH, which the -route- page of the autodoc names as the code for
 * "requested to delete a non-existent entry". bsdsocket_internal.h's AMI_E*
 * set does not carry it, because nothing else in this library can produce it.
 */
#define BSD_ESRCH   3

/* --------------------------------------------------------- the mask rule -- */

/*
 * "if the destination has a local address part of INADDR_ANY or if the
 * destination is the symbolic name of a network, then the route is assumed to
 * be a to a network" -- otherwise it is a route to a host. RTA_DestinationHost
 * and RTA_DestinationNet override the guess in either direction.
 *
 * "Network" here means the CLASSFUL network, because that is the only prefix
 * length an address alone can imply and the only one the 4.2BSD grammar this
 * inherits from could express. It is a poor netmask in 2026; it is also the
 * one the published API defines, and a route added with one mask must be
 * deletable with the same rule or DeleteRouteTagList() can never find it.
 */
#define BSD_ROUTE_DEST_GUESS    0       /* RTA_Destination     */
#define BSD_ROUTE_DEST_HOST     1       /* RTA_DestinationHost */
#define BSD_ROUTE_DEST_NET      2       /* RTA_DestinationNet  */

static ULONG bsd_route_classful_mask(ULONG addr)
{
    if ((addr & 0x80000000UL) == 0)
        return 0xFF000000UL;                    /* class A */
    if ((addr & 0xC0000000UL) == 0x80000000UL)
        return 0xFFFF0000UL;                    /* class B */

    return 0xFFFFFF00UL;                        /* class C and everything else */
}

static ULONG bsd_route_mask_for(ULONG addr, UWORD kind)
{
    ULONG classful = bsd_route_classful_mask(addr);

    if (kind == BSD_ROUTE_DEST_HOST)
        return 0xFFFFFFFFUL;

    if (kind == BSD_ROUTE_DEST_NET)
        return classful;

    /* The guess: a zero host part under the classful mask means a network. */
    return ((addr & ~classful) == 0) ? classful : 0xFFFFFFFFUL;
}

/*
 * A destination or gateway string. "a host name to be resolved or an IP
 * address in dotted-decimal notation (see RFC1700)", plus -- for a
 * destination -- "the symbolic name of a network", which is a different
 * database and a different shape: DEVS:Internet/networks holds network
 * NUMBERS (127, not 127.0.0.0), so a hit there has to be shifted back into an
 * address the way inet_makeaddr() does.
 */
static BOOL bsd_route_parse(const char *text, BOOL allow_network, ULONG *out)
{
    const AmiNetdbEntry *net;

    if (text == NULL || text[0] == '\0')
        return FALSE;

    if (ami_config_parse_ip(text, out))
        return TRUE;

    if (allow_network)
    {
        net = ami_netdb_net_by_name(text);
        if (net != NULL)
        {
            ULONG number = net->value;

            if (number < 128)
                *out = number << 24;
            else if (number < 65536)
                *out = number << 16;
            else
                *out = number << 8;

            return TRUE;
        }
    }

    return (netstack_resolve(text, out, BSD_ROUTE_RESOLVE_TIMEOUT) == AMI_NET_OK)
               ? TRUE : FALSE;
}

/* ---------------------------------------------------------- the tag lists -- */

typedef struct BsdRouteReq
{
    BOOL    brr_HaveDest;
    ULONG   brr_Dest;
    UWORD   brr_DestKind;

    BOOL    brr_HaveGateway;
    ULONG   brr_Gateway;

    BOOL    brr_HaveDefault;
    ULONG   brr_Default;
} BsdRouteReq;

/*
 * Same two-pass shape as ConfigureInterfaceTagList(), and for the same
 * reason: a route is one atomic thing and a list that is going to be refused
 * must not have changed the table on its way to being refused.
 */
static LONG bsd_route_parse_tags(struct AmiSocketBase *SocketBase,
                                 struct TagItem *tags, BsdRouteReq *req)
{
    struct TagItem *cursor = tags;
    struct TagItem *item;

    bsd_bzero(req, sizeof(*req));

    while ((item = bsd_next_tag(&cursor)) != NULL)
    {
        const char *text = (const char *)item->ti_Data;

        switch (item->ti_Tag)
        {
            case RTA_Destination:
            case RTA_DestinationHost:
            case RTA_DestinationNet:
                if (req->brr_HaveDest)
                    return bsd_fail(SocketBase, AMI_EINVAL);

                if (!bsd_route_parse(text,
                                     item->ti_Tag != RTA_DestinationHost,
                                     &req->brr_Dest))
                    return bsd_fail(SocketBase, AMI_EINVAL);

                req->brr_DestKind =
                    (item->ti_Tag == RTA_DestinationHost) ? BSD_ROUTE_DEST_HOST
                  : (item->ti_Tag == RTA_DestinationNet)  ? BSD_ROUTE_DEST_NET
                                                          : BSD_ROUTE_DEST_GUESS;
                req->brr_HaveDest = TRUE;
                break;

            case RTA_Gateway:
                if (!bsd_route_parse(text, FALSE, &req->brr_Gateway))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                req->brr_HaveGateway = TRUE;
                break;

            case RTA_DefaultGateway:
                if (!bsd_route_parse(text, FALSE, &req->brr_Default))
                    return bsd_fail(SocketBase, AMI_EINVAL);
                req->brr_HaveDefault = TRUE;
                break;

            default:
                break;
        }
    }

    /*
     * "The RTA_DefaultGateway tag excludes the use of the RTA_Destination and
     * RTA_Gateway tags", and on the delete side the exclusion is spelled out
     * again in the other direction. Both are enforced here rather than
     * silently preferring one.
     */
    if (req->brr_HaveDefault && (req->brr_HaveDest || req->brr_HaveGateway))
        return bsd_fail(SocketBase, AMI_EINVAL);

    return 0;
}

LONG bsd_AddRouteTagList(register struct TagItem *tags __asm("a0"),
                         register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP      *ip = netstack_ip();
    BsdRouteReq req;
    UINT        status;

    if (tags == NULL)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if (bsd_route_parse_tags(SocketBase, tags, &req) != 0)
        return -1;

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if (req.brr_HaveDefault)
    {
        status = nx_ip_gateway_address_set(ip, req.brr_Default);
    }
    else if (req.brr_HaveDest && req.brr_HaveGateway)
    {
#ifdef NX_ENABLE_IP_STATIC_ROUTING
        status = nx_ip_static_route_add(ip, req.brr_Dest,
                                        bsd_route_mask_for(req.brr_Dest,
                                                           req.brr_DestKind),
                                        req.brr_Gateway);
#else
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, AMI_ENOSYS);
#endif
    }
    else
    {
        /*
         * A destination with no gateway. The autodoc allows the tags in that
         * combination -- "The RTA_Destination tag CAN be used in conjunction
         * with the RTA_Gateway tag" -- but every entry NetX Duo's table holds
         * has a next hop, and the routes that do not (the directly attached
         * prefix of each interface) are created by configuring the interface
         * and cannot be added or removed on their own. EINVAL rather than a
         * success that stores nothing.
         */
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, AMI_EINVAL);
    }

    bsd_nx_leave(SocketBase);

    if (status == NX_SUCCESS)
        return 0;

    /*
     * "ENOBUFS if insufficient resources were available to install a new
     * route" -- the -route- page of the same autodoc, and NX_IP_ROUTING_TABLE_
     * SIZE is small enough that a user reaches it rather than only a bug.
     *
     * The same page names EEXIST for a duplicate, and it is not used here
     * because NetX Duo does not produce the condition: a second add for a
     * destination already in the table UPDATES its next hop and reports
     * success. That is the same "the last one wins" behaviour a route command
     * gives, so it is reported as the success it is rather than turned into
     * an error this stack would have to invent.
     *
     * NX_IP_ADDRESS_ERROR is what the next hop being on none of this
     * machine's own subnets comes back as -- NetX Duo derives the outgoing
     * interface from it and there is none -- which is a bad argument, not a
     * full table.
     */
    switch (status)
    {
        case NX_OVERFLOW:           return bsd_fail(SocketBase, AMI_ENOBUFS);
        case NX_IP_ADDRESS_ERROR:   return bsd_fail(SocketBase, AMI_ENETUNREACH);
        case NX_NOT_SUPPORTED:      return bsd_fail(SocketBase, AMI_ENOSYS);
        default:                    return bsd_fail(SocketBase, AMI_EINVAL);
    }
}

LONG bsd_DeleteRouteTagList(register struct TagItem *tags __asm("a0"),
                            register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP      *ip = netstack_ip();
    BsdRouteReq req;
    UINT        status;

    if (tags == NULL)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if (bsd_route_parse_tags(SocketBase, tags, &req) != 0)
        return -1;

    /*
     * The delete grammar is RTA_Destination or RTA_DefaultGateway and nothing
     * else; RTA_Gateway is not one of its tags, because the destination alone
     * identifies the entry.
     */
    if (!req.brr_HaveDefault && !req.brr_HaveDest)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if (req.brr_HaveDefault)
    {
        status = nx_ip_gateway_address_clear(ip);
    }
    else
    {
#ifdef NX_ENABLE_IP_STATIC_ROUTING
        status = nx_ip_static_route_delete(ip, req.brr_Dest,
                                           bsd_route_mask_for(req.brr_Dest,
                                                              req.brr_DestKind));
#else
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, AMI_ENOSYS);
#endif
    }

    bsd_nx_leave(SocketBase);

    if (status == NX_SUCCESS)
        return 0;

    /*
     * "ESRCH if requested to delete a non-existent entry" -- again the
     * -route- page.
     *
     * With one hole that is NetX Duo's and cannot be closed from here:
     * nx_ip_static_route_delete() returns NX_SUCCESS outright when the table
     * is EMPTY, without searching. So deleting a route that was never added
     * fails as it should on a machine that has other routes and succeeds on
     * one that has none.
     */
    if (status == NX_NOT_SUCCESSFUL || status == NX_ENTRY_NOT_FOUND)
        return bsd_fail(SocketBase, BSD_ESRCH);

    if (status == NX_NOT_SUPPORTED)
        return bsd_fail(SocketBase, AMI_ENOSYS);

    return bsd_fail(SocketBase, AMI_EINVAL);
}

/* ------------------------------------------------------------ GetRouteInfo */

/*
 * One table entry: the header, then the three sockaddrs the routing-socket
 * convention puts in RTA_DST, RTA_GATEWAY, RTA_NETMASK order.
 *
 * They are a struct here rather than a hand-computed offset because the
 * padding rule the convention specifies -- each sockaddr rounded up to a
 * multiple of sizeof(long) -- is a no-op for AF_INET: sockaddr_in is sixteen
 * bytes and sixteen is already a multiple of four. Writing the round-up out
 * would suggest it does something.
 */
typedef struct BsdRouteEntry
{
    struct rt_msghdr    bre_Header;
    struct sockaddr_in  bre_Dest;
    struct sockaddr_in  bre_Gateway;
    struct sockaddr_in  bre_NetMask;
} BsdRouteEntry;

/*
 * Room for every route the stack can hold at once: the directly attached
 * prefix of each interface, the whole static table, the default gateway, and
 * the terminator.
 */
#ifdef NX_ENABLE_IP_STATIC_ROUTING
#define BSD_ROUTE_STATIC_MAX    NX_IP_ROUTING_TABLE_SIZE
#else
#define BSD_ROUTE_STATIC_MAX    0
#endif

#define BSD_ROUTE_MAX   (NX_MAX_PHYSICAL_INTERFACES + BSD_ROUTE_STATIC_MAX + 1)

typedef struct BsdRouteTable
{
    BsdRouteEntry       brt_Entry[BSD_ROUTE_MAX];
    struct rt_msghdr    brt_End;          /* rtm_msglen == 0: the terminator */
} BsdRouteTable;

static VOID bsd_route_sockaddr(struct sockaddr_in *sa, ULONG host_addr)
{
    bsd_bzero(sa, sizeof(*sa));
    sa->sin_len         = (UBYTE)sizeof(struct sockaddr_in);
    sa->sin_family      = AF_INET;
    sa->sin_addr.s_addr = (in_addr_t)BSD_HTONL(host_addr);
}

/*
 * Fill one entry. `flags` is the caller's filter: "Flags which have to be set
 * in each routing table entry to be returned", so an entry is kept only when
 * it has ALL of them, and a filter of 0 keeps everything.
 */
static BOOL bsd_route_emit(BsdRouteEntry *out, LONG want_flags, LONG flags,
                           UWORD index, ULONG mtu, ULONG dest, ULONG mask,
                           ULONG gateway)
{
    if ((flags & want_flags) != want_flags)
        return FALSE;

    bsd_bzero(out, sizeof(*out));

    out->bre_Header.rtm_msglen  = (UWORD)sizeof(BsdRouteEntry);
    out->bre_Header.rtm_version = RTM_VERSION;
    out->bre_Header.rtm_type    = RTM_GET;
    out->bre_Header.rtm_index   = index;
    out->bre_Header.rtm_flags   = flags;
    out->bre_Header.rtm_addrs   = RTA_DST | RTA_GATEWAY | RTA_NETMASK;

    /* rtm_inits says which metrics were filled in rather than left at zero,
       so the MTU is only claimed when the interface actually reported one. */
    if (mtu != 0)
    {
        out->bre_Header.rtm_rmx.rmx_mtu = mtu;
        out->bre_Header.rtm_inits       = RTV_MTU;
    }

    bsd_route_sockaddr(&out->bre_Dest, dest);
    bsd_route_sockaddr(&out->bre_Gateway, gateway);
    bsd_route_sockaddr(&out->bre_NetMask, mask);

    return TRUE;
}

/*
 * The three kinds of route, in the order the stack consults them, which is
 * the order netstatus.c's NETSTATUS_ROUTES walk uses and for the same reason:
 * that order is the answer to "why did my packet go there".
 */
static UWORD bsd_route_fill(NX_IP *ip, BsdRouteTable *table, LONG want_flags)
{
    UWORD used = 0;
    UINT  i;
    ULONG gateway = 0;
#ifdef NX_ENABLE_IP_STATIC_ROUTING
    ULONG r;
#endif

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        NX_INTERFACE *nxif = &ip->nx_ip_interface[i];
        LONG          flags;

        if (nxif->nx_interface_valid == 0 || nxif->nx_interface_ip_address == 0)
            continue;

        flags = RTF_UP;
        if (nxif->nx_interface_ip_network_mask == 0xFFFFFFFFUL)
            flags |= RTF_HOST;

        if (bsd_route_emit(&table->brt_Entry[used], want_flags, flags,
                           (UWORD)i, nxif->nx_interface_ip_mtu_size,
                           nxif->nx_interface_ip_address &
                               nxif->nx_interface_ip_network_mask,
                           nxif->nx_interface_ip_network_mask, 0))
            used++;
    }

#ifdef NX_ENABLE_IP_STATIC_ROUTING
    for (r = 0; r < ip->nx_ip_routing_table_entry_count; r++)
    {
        const NX_IP_ROUTING_ENTRY *e = &ip->nx_ip_routing_table[r];
        LONG                       flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;

        if (e->nx_ip_routing_net_mask == 0xFFFFFFFFUL)
            flags |= RTF_HOST;

        if (bsd_route_emit(&table->brt_Entry[used], want_flags, flags, 0, 0,
                           e->nx_ip_routing_dest_ip,
                           e->nx_ip_routing_net_mask,
                           e->nx_ip_routing_next_hop_address))
            used++;
    }
#endif

    if (nx_ip_gateway_address_get(ip, &gateway) == NX_SUCCESS && gateway != 0)
    {
        if (bsd_route_emit(&table->brt_Entry[used], want_flags,
                           RTF_UP | RTF_GATEWAY, 0, 0, 0, 0, gateway))
            used++;
    }

    return used;
}

struct rt_msghdr *bsd_GetRouteInfo(register LONG address_family __asm("d0"),
                                   register LONG flags __asm("d1"),
                                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP         *ip = netstack_ip();
    BsdRouteTable *table;
    UWORD          used;

    /* "this can be AF_UNSPEC or AF_INET" -- and nothing else exists here to
       report, because the routing table this stack keeps is IPv4's. */
    if (address_family != AF_UNSPEC && address_family != AF_INET)
    {
        (VOID)bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
        return NULL;
    }

    if (ip == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENETDOWN);
        return NULL;
    }

    /* Allocated before the bracket, at the maximum: nothing may allocate
       inside one, and the table cannot grow while it is held anyway. */
    table = (BsdRouteTable *)ami_alloc(sizeof(BsdRouteTable));
    if (table == NULL)
    {
        (VOID)bsd_fail(SocketBase, AMI_ENOBUFS);
        return NULL;
    }

    if (bsd_nx_enter(SocketBase) != 0)
    {
        ami_free(table);
        (VOID)bsd_fail(SocketBase, AMI_ENETDOWN);
        return NULL;
    }

    used = bsd_route_fill(ip, table, flags);

    bsd_nx_leave(SocketBase);

    /*
     * The terminator goes immediately after the last entry used, not at the
     * end of the allocation: a caller walks by rtm_msglen and would otherwise
     * read the unused entries as routes. It is a whole zeroed rt_msghdr
     * rather than a zeroed UWORD, so a caller that reads rtm_version before
     * checking rtm_msglen -- which the autodoc invites, since it tells you to
     * check rtm_version -- sees zero rather than whatever was there.
     */
    if (used < (UWORD)BSD_ROUTE_MAX)
        bsd_bzero(&table->brt_Entry[used].bre_Header,
                  sizeof(struct rt_msghdr));
    else
        bsd_bzero(&table->brt_End, sizeof(table->brt_End));

    /*
     * An empty table is a SUCCESS with one terminator in it. NULL is reserved
     * for the failures above, and a filter that matched nothing is not one of
     * them.
     */
    return &table->brt_Entry[0].bre_Header;
}

VOID bsd_FreeRouteInfo(register struct rt_msghdr *table __asm("a0"),
                       register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)SocketBase;

    /* "This parameter can be NULL in which case this routine does nothing."
       The header is the first member of the block, so this is the block. */
    if (table != NULL)
        ami_free(table);
}
