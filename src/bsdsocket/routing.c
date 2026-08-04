/*
 * bsdsocket.library, the Roadshow routing API.
 *
 *   AddRouteTagList()       a gateway route, or the default one
 *   DeleteRouteTagList()    the same, undone
 *   GetRouteInfo()          a copy of the whole table, as rt_msghdr entries
 *
 * Same primary source as interfaces.c: NDK 3.2's
 * SANA+RoadshowTCP-IP/doc/bsdsocket.doc, plus libraries/bsdsocket.h and
 * net/route.h from the same NDK, used as an ABI reference only. No Roadshow,
 * AmiTCP, AROSTCP or Miami code was consulted or is present.
 *
 * Three things the autodoc specifies that the prototypes do not show:
 *
 *   1. GetRouteInfo() returns "a header followed by a small number of
 *      sockadders, interpreted by position ... The interpretation of which
 *      address are present is given by a bit mask within the header, and the
 *      sequence is least significant to most significant bit within the
 *      vector", the routing-socket layout, so rtm_addrs is the map and the
 *      sockaddrs follow in RTA_DST, RTA_GATEWAY, RTA_NETMASK order.
 *
 *   2. "The table is terminated by a dummy entry whose 'rtm_msglen' member is
 *      zero." Not a count, not a NULL. A caller loops on rtm_msglen and stops
 *      when it is zero, so a table without that entry never ends.
 *
 *   3. rtm_version is 3: "The 'struct rt_msghdr' layout described above
 *      corresponds to version 3", and RTM_VERSION in net/route.h agrees.
 *
 * The rt_msghdr the autodoc prints has the same eleven members as net/route.h's
 * in a different order: the document has rtm_pid before rtm_addrs and rtm_flags
 * after rtm_errno, the header has rtm_flags second and rtm_pid fourth. The
 * header is the ABI, since it is what a caller compiles against. Everything
 * below is written against the header and uses member names throughout.
 *
 * There is no netmask tag. AddRouteTagList() takes RTA_Destination,
 * RTA_Gateway, RTA_DefaultGateway, RTA_DestinationHost and RTA_DestinationNet
 * and nothing else, so the prefix length is implied by which of them was used
 *, see bsd_route_mask_for() below.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"

/* struct sockaddr_dl and IFT_ETHER, for the ARP entries below. */
#include <net/if_dl.h>
#include <net/if_types.h>

#include <proto/exec.h>

/* How long a name in RTA_Destination or RTA_Gateway is given to resolve: the
   same thirty seconds resolver.c gives gethostbyname(), same lookup. */
#define BSD_ROUTE_RESOLVE_TIMEOUT   (30UL * (ULONG)NX_IP_PERIODIC_RATE)


/* --------------------------------------------------------- the mask rule, */

/*
 * "if the destination has a local address part of INADDR_ANY or if the
 * destination is the symbolic name of a network, then the route is assumed to
 * be a to a network", otherwise it is a route to a host. RTA_DestinationHost
 * and RTA_DestinationNet override the guess in either direction.
 *
 * "Network" here means the classful network: the only prefix length an address
 * alone can imply, and the only one the 4.2BSD grammar this inherits from could
 * express. It is a poor netmask in 2026, but it is what the published API
 * defines, and a route added with one mask must be deletable under the same
 * rule or DeleteRouteTagList() can never find it.
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

    /* Guess: a zero host part under the classful mask means a network. */
    return ((addr & ~classful) == 0) ? classful : 0xFFFFFFFFUL;
}

/*
 * A destination or gateway string: "a host name to be resolved or an IP
 * address in dotted-decimal notation (see RFC1700)", plus, for a destination
 *, "the symbolic name of a network". That is a different database and a
 * different shape: DEVS:Internet/networks holds network numbers (127, not
 * 127.0.0.0), so a hit there is shifted back into an address the way
 * inet_makeaddr() does.
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

/* ---------------------------------------------------------- the tag lists, */

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
 * Same two-pass shape as ConfigureInterfaceTagList(): a route is applied
 * atomically, so a tag list that will be refused must not have changed the
 * table first.
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
     * again in the other direction. Both directions are enforced here rather
     * than silently preferring one.
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
         * A destination with no gateway. The autodoc allows the combination
         *, "The RTA_Destination tag CAN be used in conjunction with the
         * RTA_Gateway tag", but every entry NetX Duo's table holds has a
         * next hop. The routes that do not (the directly attached prefix of
         * each interface) come from configuring the interface and cannot be
         * added or removed on their own. EINVAL rather than a success that
         * stores nothing.
         */
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, AMI_EINVAL);
    }

    bsd_nx_leave(SocketBase);

    if (status == NX_SUCCESS)
        return 0;

    /*
     * "ENOBUFS if insufficient resources were available to install a new
     * route", the -route- page of the same autodoc; NX_IP_ROUTING_TABLE_SIZE
     * is small enough that a user can reach it.
     *
     * The same page names EEXIST for a duplicate. NetX Duo never produces that
     * condition: a second add for a destination already in the table updates
     * its next hop and reports success, matching what a route command does, so
     * it is reported as success.
     *
     * NX_IP_ADDRESS_ERROR means the next hop is on none of this machine's own
     * subnets, NetX Duo derives the outgoing interface from it and there is
     * none, so it maps to ENETUNREACH, not a full-table error.
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
     * else; RTA_Gateway is not one of its tags, since the destination alone
     * identifies the entry.
     */
    if (!req.brr_HaveDefault && !req.brr_HaveDest)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if (req.brr_HaveDefault)
    {
        /*
         * "The address of the default gateway all packets were forwarded to"
         *, past tense, on the delete page.
         * The tag names the entry to remove, so it has to be the one installed:
         * clearing on any address at all would let `route delete default
         * 10.0.0.1` succeed on a machine whose default gateway is 192.168.1.1
         * and take that one away. ESRCH is the -route- page's code "if
         * requested to delete a non-existent entry", and no gateway at all is
         * the same condition.
         */
        ULONG installed = 0;

        if (nx_ip_gateway_address_get(ip, &installed) != NX_SUCCESS ||
            installed == 0 || installed != req.brr_Default)
        {
            bsd_nx_leave(SocketBase);
            return bsd_fail(SocketBase, AMI_ESRCH);
        }

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
     * "ESRCH if requested to delete a non-existent entry", again the -route-
     * page. One hole is NetX Duo's and cannot be closed from here:
     * nx_ip_static_route_delete() returns NX_SUCCESS outright when the table is
     * empty, without searching. Deleting a route that was never added therefore
     * fails on a machine that has other routes and succeeds on one that has
     * none.
     */
    if (status == NX_NOT_SUCCESSFUL || status == NX_ENTRY_NOT_FOUND)
        return bsd_fail(SocketBase, AMI_ESRCH);

    if (status == NX_NOT_SUPPORTED)
        return bsd_fail(SocketBase, AMI_ENOSYS);

    return bsd_fail(SocketBase, AMI_EINVAL);
}

/* ------------------------------------------------------------ GetRouteInfo */

/*
 * One table entry: the header, then the three sockaddrs the routing-socket
 * convention puts in RTA_DST, RTA_GATEWAY, RTA_NETMASK order.
 *
 * A struct rather than hand-computed offsets: the convention's padding rule
 * (each sockaddr rounded up to a multiple of sizeof(long)) is a no-op for
 * AF_INET, since sockaddr_in is sixteen bytes.
 */
typedef struct BsdRouteEntry
{
    struct rt_msghdr    bre_Header;
    struct sockaddr_in  bre_Dest;
    struct sockaddr_in  bre_Gateway;
    struct sockaddr_in  bre_NetMask;
} BsdRouteEntry;

/* Room for every route the stack can hold at once: the directly attached
   prefix of each interface, the whole static table, the default gateway, and
   the terminator. */
#ifdef NX_ENABLE_IP_STATIC_ROUTING
#define BSD_ROUTE_STATIC_MAX    NX_IP_ROUTING_TABLE_SIZE
#else
#define BSD_ROUTE_STATIC_MAX    0
#endif

/*
 * ARP entries are routes on this interface: 4.4BSD keeps the ARP cache in the
 * routing table flagged RTF_LLINFO, and that is what `arp -a` reads, it asks
 * for routes carrying that flag and formats whatever comes back. With none
 * emitted, Roadshow's arp printed nothing at all (docs/RESEARCH.md 55).
 *
 * The cache is AMI_ARP_CACHE_SIZE bytes of NX_ARP, so this bound is generous
 * rather than exact; running out truncates the list, and the walk below stops
 * cleanly rather than overrunning.
 */
#define BSD_ROUTE_ARP_MAX       32

#define BSD_ROUTE_MAX   (NX_MAX_PHYSICAL_INTERFACES + BSD_ROUTE_STATIC_MAX + \
                         BSD_ROUTE_ARP_MAX + 1)

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
 * The interface a next hop leaves by: the one whose subnet contains it.
 *
 * BSD numbers interfaces from 1 and keeps 0 for "no interface", which is the
 * right answer for a gateway that matches none. NetX numbers its array from 0,
 * so every index handed to rtm_index is +1, a caller comparing rtm_index
 * against an if_nametoindex() would otherwise be off by one on every route.
 */
static UWORD bsd_route_iface_for(NX_IP *ip, ULONG next_hop)
{
    UINT i;

    if (next_hop == 0)
        return 0;

    for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        const NX_INTERFACE *nxif = &ip->nx_ip_interface[i];
        ULONG               mask = nxif->nx_interface_ip_network_mask;

        if (nxif->nx_interface_valid == 0 || nxif->nx_interface_ip_address == 0)
            continue;

        if ((next_hop & mask) == (nxif->nx_interface_ip_address & mask))
            return (UWORD)(i + 1);
    }

    return 0;
}

/*
 * Fill one entry. `flags` is the caller's filter: "Flags which have to be set
 * in each routing table entry to be returned", so an entry is kept only when
 * it has all of them, and a filter of 0 keeps everything.
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

    /* rtm_inits says which metrics were filled in rather than left at zero, so
       the MTU is only claimed when the interface reported one. */
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
 * An ARP entry, in the space a route entry occupies.
 *
 * The stride stays sizeof(BsdRouteEntry) so the fixed array still works: a
 * destination sockaddr_in plus a gateway sockaddr_dl is 36 bytes against the
 * 48 that three sockaddr_in take, and rtm_addrs says only two are present so
 * nothing reads the remainder. A caller walking by rtm_msglen sees entries of
 * one size throughout.
 *
 * sockaddr_dl is net/if_dl.h's, from the NDK. sdl_data holds the interface
 * name first and the link address after it, which is what LLADDR() encodes and
 * why sdl_nlen has to be right.
 */
typedef struct BsdLinkEntry
{
    struct rt_msghdr    ble_Header;
    struct sockaddr_in  ble_Dest;
    struct sockaddr_dl  ble_Gateway;
} BsdLinkEntry;

/* One ARP entry, filled in place. Returns FALSE when the caller's filter
   excludes it, as bsd_route_emit() does. */
static BOOL bsd_route_emit_arp(BsdRouteEntry *slot, LONG want_flags,
                               LONG flags, UWORD index, ULONG dest,
                               ULONG mac_msw, ULONG mac_lsw)
{
    BsdLinkEntry *out = (BsdLinkEntry *)slot;
    UBYTE        *lla;

    if ((flags & want_flags) != want_flags)
        return FALSE;

    bsd_bzero(out, sizeof(*slot));

    out->ble_Header.rtm_msglen  = (UWORD)sizeof(BsdRouteEntry);
    out->ble_Header.rtm_version = RTM_VERSION;
    out->ble_Header.rtm_type    = RTM_GET;
    out->ble_Header.rtm_index   = index;
    out->ble_Header.rtm_flags   = flags;
    out->ble_Header.rtm_addrs   = RTA_DST | RTA_GATEWAY;

    /*
     * rtm_rmx.rmx_expire stays 0 and Roadshow's arp prints "permanent" for it.
     * That is correct: NX_ARP_EXPIRATION_RATE is 0 in this build, NetX Duo's
     * "entries do not expire". An invented expiry would make arp print a
     * countdown that never happens.
     */

    bsd_route_sockaddr(&out->ble_Dest, dest);

    out->ble_Gateway.sdl_len    = (UBYTE)sizeof(struct sockaddr_dl);
    out->ble_Gateway.sdl_family = AF_LINK;
    out->ble_Gateway.sdl_index  = index;
    out->ble_Gateway.sdl_type   = IFT_ETHER;
    out->ble_Gateway.sdl_nlen   = 0;    /* no name, so LLADDR is sdl_data */
    out->ble_Gateway.sdl_alen   = 6;
    out->ble_Gateway.sdl_slen   = 0;

    /* NetX Duo keeps a MAC as a 16-bit msw and a 32-bit lsw. */
    lla = (UBYTE *)out->ble_Gateway.sdl_data;
    lla[0] = (UBYTE)((mac_msw >> 8) & 0xFF);
    lla[1] = (UBYTE)( mac_msw       & 0xFF);
    lla[2] = (UBYTE)((mac_lsw >> 24) & 0xFF);
    lla[3] = (UBYTE)((mac_lsw >> 16) & 0xFF);
    lla[4] = (UBYTE)((mac_lsw >>  8) & 0xFF);
    lla[5] = (UBYTE)( mac_lsw        & 0xFF);

    return TRUE;
}

/*
 * Walk the ARP cache. NetX Duo publishes no enumerator, only
 * nx_arp_hardware_address_find() for one address at a time, so this reads
 * the table the same way bsd_route_fill() reads nx_ip_interface[].
 *
 * Each bucket is a circular list threaded on nx_arp_active_next, so the walk
 * stops when it comes back to the head; looping until NULL would spin for
 * ever. Entries with no hardware address yet are requests in flight and are
 * skipped, rather than reported as 00:00:00:00:00:00.
 */
static UWORD bsd_route_fill_arp(NX_IP *ip, BsdRouteTable *table,
                                LONG want_flags, UWORD used)
{
    UINT bucket;

    for (bucket = 0; bucket < NX_ARP_TABLE_SIZE; bucket++)
    {
        NX_ARP *head = ip->nx_ip_arp_table[bucket];
        NX_ARP *entry = head;

        if (head == NX_NULL)
            continue;

        do
        {
            LONG  flags;
            UWORD index = 0;

            if (used >= (UWORD)BSD_ROUTE_MAX)
                return used;

            if (entry->nx_arp_physical_address_msw == 0 &&
                entry->nx_arp_physical_address_lsw == 0)
            {
                entry = entry->nx_arp_active_next;
                continue;
            }

            /* Which interface it was learned on, by identity. */
            if (entry->nx_arp_ip_interface != NX_NULL)
            {
                UINT i;

                for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
                {
                    if (&ip->nx_ip_interface[i] == entry->nx_arp_ip_interface)
                    {
                        index = (UWORD)(i + 1);
                        break;
                    }
                }
            }

            flags = RTF_UP | RTF_HOST | RTF_LLINFO;
            if (entry->nx_arp_route_static)
                flags |= RTF_STATIC;

            if (bsd_route_emit_arp(&table->brt_Entry[used], want_flags, flags,
                                   index, entry->nx_arp_ip_address,
                                   entry->nx_arp_physical_address_msw,
                                   entry->nx_arp_physical_address_lsw))
                used++;

            entry = entry->nx_arp_active_next;
        }
        while (entry != head && entry != NX_NULL);
    }

    return used;
}

/* The three kinds of route, in the order the stack consults them, the same
   order netstatus.c's NETSTATUS_ROUTES walk uses. */
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
                           (UWORD)(i + 1), nxif->nx_interface_ip_mtu_size,
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

        if (bsd_route_emit(&table->brt_Entry[used], want_flags, flags,
                           bsd_route_iface_for(ip,
                               e->nx_ip_routing_next_hop_address), 0,
                           e->nx_ip_routing_dest_ip,
                           e->nx_ip_routing_net_mask,
                           e->nx_ip_routing_next_hop_address))
            used++;
    }
#endif

    if (nx_ip_gateway_address_get(ip, &gateway) == NX_SUCCESS && gateway != 0)
    {
        if (bsd_route_emit(&table->brt_Entry[used], want_flags,
                           RTF_UP | RTF_GATEWAY,
                           bsd_route_iface_for(ip, gateway), 0, 0, 0, gateway))
            used++;
    }

    used = bsd_route_fill_arp(ip, table, want_flags, used);

    return used;
}

struct rt_msghdr *bsd_GetRouteInfo(register LONG address_family __asm("d0"),
                                   register LONG flags __asm("d1"),
                                   register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NX_IP         *ip = netstack_ip();
    BsdRouteTable *table;
    UWORD          used;

    /* "this can be AF_UNSPEC or AF_INET"; the routing table this stack keeps
       is IPv4's, so there is nothing else to report. */
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

    /* Allocated at the maximum before the bracket: nothing may allocate inside
       one, and the table cannot grow while it is held. */
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
     * read the unused entries as routes. A whole zeroed rt_msghdr rather than
     * a zeroed UWORD, so a caller that reads rtm_version before checking
     * rtm_msglen, which the autodoc invites, sees zero.
     */
    if (used < (UWORD)BSD_ROUTE_MAX)
        bsd_bzero(&table->brt_Entry[used].bre_Header,
                  sizeof(struct rt_msghdr));
    else
        bsd_bzero(&table->brt_End, sizeof(table->brt_End));

    /* An empty table is a success with one terminator in it. NULL is reserved
       for the failures above; a filter that matched nothing is not one. */
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
