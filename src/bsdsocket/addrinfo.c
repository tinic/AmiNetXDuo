/*
 * bsdsocket.library, getaddrinfo / getnameinfo / freeaddrinfo / gai_strerror.
 *
 * These are the four vectors at the end of the Roadshow LVO table
 * (0x324..0x336), and the only family-agnostic name lookup the ABI has.  They
 * ship in both build configurations.  An IPv4-only build still answers them,
 * but never with an AF_INET6 result.
 *
 * What AF_UNSPEC returns, and in what order.
 *
 * The NDK's netdb.h:176 defines AI_MASK as only
 *     AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV
 * There is no AI_V4MAPPED and no AI_ADDRCONFIG, so a caller compiled against
 * this header cannot express the two hints modern POSIX uses to steer
 * dual-stack results.  The behaviour is therefore fixed here:
 *
 *   1. IPv6 results come first, then IPv4, so a caller that walks the list in
 *      order and connects to the first address that works prefers IPv6.
 *   2. AI_ADDRCONFIG is implied for AF_UNSPEC and cannot be disabled.  AAAA is
 *      only looked up when this machine holds a global unicast address that
 *      finished duplicate address detection (netstack_ipv6_have_global()).
 *      "IPv6 is running" is not that test, and was the one used here.  Every
 *      interface gets a link-local unasked, so a machine that never saw a
 *      router passed that test, preferred the AAAA, and connected nowhere.  A
 *      caller that names AF_INET6 said what it wants and is taken at its
 *      word.  It gets the AAAA lookup whenever IPv6 is running at all.
 *   3. AI_V4MAPPED is not implied and not available.  An AF_INET6 query
 *      returns AAAA records only.  It never synthesises ::ffff:a.b.c.d from an
 *      A record.  A caller that wants both asks for AF_UNSPEC.
 *   4. At most one address per family is returned.  The resolver underneath
 *      (netstack_resolve/netstack_resolve6 over NetX Duo's addons/dns) answers
 *      with a single address, not a set, so a one-element list misrepresents
 *      round-robin DNS.  A caller that walks the list still behaves correctly
 *      with fewer entries.
 *
 * Each result is one allocation.  The addrinfo, its sockaddr and (on the first
 * node only) its canonical name live in one block, so freeaddrinfo() is a walk
 * and a free per node.  ai_canonname on the second node points into the first
 * node's block, so freeaddrinfo() must not free it separately.  The list is
 * only ever freed as a whole, as POSIX requires.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"
#include "interfaces.h"

#include <proto/exec.h>

/* Resolver timeout, in ThreadX ticks. Matches resolver.c. */
#define BSD_GAI_TIMEOUT     (30UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * One result node. The sockaddr is embedded so the whole node is one
 * allocation, and ai_addr points at it.
 *
 * The union is sized by the larger of the two sockaddrs in the build. In the
 * floor build that is sockaddr_in (16 bytes) and the node is smaller.
 */
typedef struct BsdAddrInfoNode
{
    struct addrinfo     ai;
    union
    {
        struct sockaddr     sa;
        struct sockaddr_in  sin;
#ifdef AMINETXDUO_IPV6
        struct sockaddr_in6 sin6;
#endif
    } addr;
    char                name[1];      /* canonical name, when asked for */
} BsdAddrInfoNode;

/* ---------------------------------------------------------------- helpers */

/* Strict unsigned decimal. FALSE on anything else, including empty. */
static BOOL bsd_gai_number(const char *s, ULONG *out)
{
    ULONG value  = 0;
    ULONG digits = 0;

    if (s == NULL)
        return FALSE;

    while (*s >= '0' && *s <= '9')
    {
        value = value * 10UL + (ULONG)(*s - '0');
        if (value > 0xFFFFUL)
            return FALSE;
        digits++;
        s++;
    }

    if (digits == 0 || *s != '\0')
        return FALSE;

    *out = value;

    return TRUE;
}

/*
 * Resolve the service name. Returns an EAI_* code, or 0 with *port set in host
 * order. The sockaddr writers do the conversion.
 */
static LONG bsd_gai_service(const char *servname, LONG flags, LONG socktype,
                            UINT *port)
{
    const AmiNetdbEntry *entry;
    ULONG                value;

    *port = 0;

    if (servname == NULL || *servname == '\0')
        return 0;

    if (bsd_gai_number(servname, &value))
    {
        *port = (UINT)value;
        return 0;
    }

    if ((flags & AI_NUMERICSERV) != 0)
        return EAI_NONAME;

    /*
     * DEVS:Internet/services, through the same store getservbyname() uses.
     * With no socktype hint both protocols are acceptable and tcp is tried
     * first.
     */
    entry = ami_netdb_serv_by_name(servname,
                                   (socktype == SOCK_DGRAM) ? "udp" : "tcp");
    if (entry == NULL && socktype == 0)
        entry = ami_netdb_serv_by_name(servname, "udp");

    if (entry == NULL)
        return EAI_SERVICE;

    *port = (UINT)entry->value;

    return 0;
}

#ifdef AMINETXDUO_IPV6
/*
 * RFC 4007 §11's <zone_id>, as an interface index. "SHOULD support at least
 * numerical indices ... MAY support other kinds of non-null strings", so both
 * a number and an interface name are taken. 0 when it names nothing, which the
 * caller turns into EAI_NONAME: a zone that resolves to no interface makes the
 * whole address unusable, and a guessed interface is what the zone was given
 * to prevent.
 */
static ULONG bsd_gai_zone_index(const char *zone)
{
    ULONG value  = 0;
    BOOL  digits = TRUE;
    ULONG i;

    if (zone == NULL || zone[0] == '\0')
        return 0;

    for (i = 0; zone[i] != '\0'; i++)
    {
        if (zone[i] < '0' || zone[i] > '9')
        {
            digits = FALSE;
            break;
        }

        value = value * 10UL + (ULONG)(zone[i] - '0');
        if (value > (ULONG)NX_MAX_PHYSICAL_INTERFACES)
            return 0;
    }

    if (digits)
        return value;

    {
        LONG index = bsd_if_index_of(netstack_ip(), zone);

        return (index < 0) ? 0UL : (ULONG)(index + 1);
    }
}
#endif

/*
 * Build one node. `canon` is copied into it when non-NULL. Otherwise the node
 * carries no name and ai_canonname is set to `inherit`, which points into an
 * earlier node's block, see the memory note at the top.
 */
static BsdAddrInfoNode *bsd_gai_node(LONG family, LONG socktype, LONG protocol,
                                     const NXD_ADDRESS *addr, UINT port,
                                     const char *canon, char *inherit)
{
    BsdAddrInfoNode *node;
    ULONG            namelen = (canon != NULL) ? bsd_strlen(canon) + 1 : 0;
    ULONG            size    = (ULONG)sizeof(BsdAddrInfoNode) + namelen;

    node = (BsdAddrInfoNode *)ami_alloc(size);
    if (node == NULL)
        return NULL;

    bsd_bzero(node, size);

    node->ai.ai_flags    = 0;
    node->ai.ai_family   = (int)family;
    node->ai.ai_socktype = (int)socktype;
    node->ai.ai_protocol = (int)protocol;
    node->ai.ai_addr     = &node->addr.sa;
    node->ai.ai_next     = NULL;

#ifdef AMINETXDUO_IPV6
    if (family == AF_INET6)
    {
        /* No sin6_len on this NDK, see src/bsdsocket/in6.c. */
        node->addr.sin6.sin6_family = AF_INET6;
        node->addr.sin6.sin6_port   = (in_port_t)BSD_HTONS((UWORD)port);
        bsd_words_to_in6(addr->nxd_ip_address.v6,
                         node->addr.sin6.sin6_addr.s6_addr);
        node->ai.ai_addrlen = (socklen_t)sizeof(struct sockaddr_in6);
    }
    else
#endif
    {
        node->addr.sin.sin_len         = (UBYTE)sizeof(struct sockaddr_in);
        node->addr.sin.sin_family      = AF_INET;
        node->addr.sin.sin_port        = (in_port_t)BSD_HTONS((UWORD)port);
        node->addr.sin.sin_addr.s_addr =
            BSD_HTONL((addr->nxd_ip_version == NX_IP_VERSION_V4)
                          ? addr->nxd_ip_address.v4 : 0UL);
        node->ai.ai_addrlen = (socklen_t)sizeof(struct sockaddr_in);
    }

    if (canon != NULL)
    {
        bsd_strncpy(node->name, canon, namelen);
        node->ai.ai_canonname = node->name;
    }
    else
    {
        node->ai.ai_canonname = inherit;
    }

    return node;
}

/*
 * netstack error -> EAI_*, the getaddrinfo() half of bsd_herrno_of()
 * (resolver.c).
 *
 * RFC 3493 6.1 requires EAI_AGAIN, "temporary failure in name resolution".
 * Every failure here used to be reported as EAI_NONAME. A caller that retries
 * on EAI_AGAIN therefore never retried, and a slow name server looked like a
 * name that does not exist. The backend always told the two apart, and nothing
 * read it.
 *
 * EAI_NODATA is not used. It means "the name exists but has no address of the
 * family asked for", which needs an empty answer section to be told from
 * NXDOMAIN, and the resolver underneath reports both as no answer.
 */
static LONG bsd_gai_error(LONG status)
{
    switch (status)
    {
        case AMI_NET_ERR_TIMEOUT:       /* nobody answered           */
        case AMI_NET_ERR_NOSERVER:      /* nobody to ask, yet        */
        case AMI_NET_ERR_STATE:         /* stack not up, yet         */
            return EAI_AGAIN;

        case AMI_NET_ERR_NOMEM:
            return EAI_MEMORY;

        case AMI_NET_ERR_CONFIG:
        case AMI_NET_ERR_KERNEL:
            return EAI_FAIL;

        default:
            return EAI_NONAME;
    }
}

/*
 * With AF_UNSPEC two lookups run and each has a verdict. "Try again" outranks
 * "no such name", because only the first is worth a retry. An A query dropped
 * by a name server does not prove that a machine with an AAAA record is absent
 * from IPv4.
 */
static LONG bsd_gai_merge(LONG so_far, LONG now)
{
    if (so_far == EAI_AGAIN || now == EAI_AGAIN)
        return EAI_AGAIN;

    return (so_far != EAI_NONAME) ? so_far : now;
}

/* Append to the list, remembering the tail. */
static VOID bsd_gai_append(struct addrinfo **head, struct addrinfo **tail,
                           BsdAddrInfoNode *node)
{
    if (node == NULL)
        return;

    if (*head == NULL)
        *head = &node->ai;
    else
        (*tail)->ai_next = &node->ai;

    *tail = &node->ai;
}

/* ---------------------------------------------------------------- vectors */

/*
 * Register assignment comes from the NDK pragma, not from the C prototype.
 *
 *   pragmas/bsdsocket_pragmas.h:139  freeaddrinfo(a0)
 *   pragmas/bsdsocket_pragmas.h:140  getaddrinfo(a0,a1,a2,a3)
 *   pragmas/bsdsocket_pragmas.h:141  gai_strerror(a0)      <-- not d0
 *   pragmas/bsdsocket_pragmas.h:142  getnameinfo(a0,d0,a1,d1,a2,d2,d3)
 *
 * gai_strerror() takes a LONG in an address register. The libcall form on line
 * 264 says "801" too, which is the same a0, so it is not a typo in the header.
 * This is the second register assignment in this ABI that does not follow the
 * pattern (bpf_set_notify_mask takes (d1,d0) where its neighbour takes
 * (d0,d1)).
 * Every prototype here is generated from that pragma table by
 * tools/gen_vectors.py, so the definitions below must match
 * bsdsocket_vectors.h exactly.
 */
LONG bsd_getaddrinfo(register STRPTR nodename         __asm("a0"),
                     register STRPTR servname         __asm("a1"),
                     register struct addrinfo *hints  __asm("a2"),
                     register struct addrinfo **res   __asm("a3"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    struct addrinfo *head = NULL;
    struct addrinfo *tail = NULL;
    LONG             family   = AF_UNSPEC;
    LONG             socktype = 0;
    LONG             protocol = 0;
    LONG             flags    = 0;
    LONG             verdict  = EAI_NONAME;
    BOOL             aborted  = FALSE;
    LONG             status;
    UINT             port = 0;
    char            *canon = NULL;
    BsdAddrInfoNode *node;
    NXD_ADDRESS      addr;

    if (res == NULL)
        return EAI_FAIL;

    *res = NULL;

    if ((nodename == NULL || *nodename == '\0') &&
        (servname == NULL || *servname == '\0'))
        return EAI_NONAME;

    if (hints != NULL)
    {
        flags    = (LONG)hints->ai_flags;
        family   = (LONG)hints->ai_family;
        socktype = (LONG)hints->ai_socktype;
        protocol = (LONG)hints->ai_protocol;

        /*
         * AI_MASK is the NDK's own list of the flags that exist here
         * (netdb.h:176). A caller that sets a bit outside it was compiled
         * against a different header and asks for something this library
         * cannot do. It is refused rather than ignored.
         */
        if ((flags & ~(LONG)(AI_MASK | AI_EXT)) != 0)
            return EAI_BADFLAGS;

        if (family != AF_UNSPEC && family != AF_INET
#ifdef AMINETXDUO_IPV6
            && family != AF_INET6
#endif
            )
            return EAI_FAMILY;

        if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM)
            return EAI_SOCKTYPE;
    }

    status = bsd_gai_service((const char *)servname, flags, socktype, &port);
    if (status != 0)
        return status;

    if (socktype == 0)
        socktype = SOCK_STREAM;
    if (protocol == 0)
        protocol = (socktype == SOCK_DGRAM) ? IPPROTO_UDP : IPPROTO_TCP;

    /* ---- no node name: the wildcard/loopback address ------------------- */

    if (nodename == NULL || *nodename == '\0')
    {
        /*
         * AI_PASSIVE: the address a server binds to (INADDR_ANY / in6addr_any).
         * Without it, the address a client connects to on this machine, which
         * is the loopback address.
         */
#ifdef AMINETXDUO_IPV6
        if ((family == AF_UNSPEC || family == AF_INET6) &&
            netstack_ipv6_enabled())
        {
            addr.nxd_ip_version       = NX_IP_VERSION_V6;
            addr.nxd_ip_address.v6[0] = 0UL;
            addr.nxd_ip_address.v6[1] = 0UL;
            addr.nxd_ip_address.v6[2] = 0UL;
            addr.nxd_ip_address.v6[3] = ((flags & AI_PASSIVE) != 0) ? 0UL : 1UL;

            node = bsd_gai_node(AF_INET6, socktype, protocol, &addr, port,
                                NULL, NULL);
            bsd_gai_append(&head, &tail, node);
        }
#endif
        if (family == AF_UNSPEC || family == AF_INET)
        {
            bsd_addr_from_v4(&addr, ((flags & AI_PASSIVE) != 0)
                                        ? 0UL : 0x7F000001UL);

            node = bsd_gai_node(AF_INET, socktype, protocol, &addr, port,
                                NULL, NULL);
            bsd_gai_append(&head, &tail, node);
        }

        if (head == NULL)
            return EAI_MEMORY;

        *res = head;

        return 0;
    }

    /* ---- numeric literals ---------------------------------------------- */

#ifdef AMINETXDUO_IPV6
    /* No family guard out here: an IPv6 literal asked for with an AF_INET hint
     * has to reach the check below and be told the family is wrong. A guard on
     * the block made that check unreachable, so the literal fell through to
     * the resolver and was looked up as a name. Same shape as the IPv4 block. */
    {
        ULONG words[4];
        char  zone[AMI_CFG_IP6_ZONE_LEN];

        if (ami_config_parse_ip6_zone((const char *)nodename, words, NULL,
                                      zone, sizeof(zone)))
        {
            ULONG scope;

            if (family == AF_INET)
                return EAI_ADDRFAMILY;

            /* Safe to accept only because the send path honours it: see
               bsd_source_select() in socket.c. A zone parsed and then never
               read makes `ping fe80::1%eth0` look supported, while the packet
               leaves by whatever interface was routed to. */
            scope = bsd_gai_zone_index(zone);
            if (zone[0] != '\0' && scope == 0)
                return EAI_NONAME;

            addr.nxd_ip_version       = NX_IP_VERSION_V6;
            addr.nxd_ip_address.v6[0] = words[0];
            addr.nxd_ip_address.v6[1] = words[1];
            addr.nxd_ip_address.v6[2] = words[2];
            addr.nxd_ip_address.v6[3] = words[3];

            node = bsd_gai_node(AF_INET6, socktype, protocol, &addr, port,
                                ((flags & AI_CANONNAME) != 0)
                                    ? (const char *)nodename : NULL,
                                NULL);
            if (node == NULL)
                return EAI_MEMORY;

            node->addr.sin6.sin6_scope_id = scope;

            bsd_gai_append(&head, &tail, node);
            *res = head;

            return 0;
        }
    }
#endif

    {
        ULONG v4;

        if (ami_config_parse_ip((const char *)nodename, &v4))
        {
            if (family == AF_INET6)
            {
                /*
                 * A dotted quad with an AF_INET6 hint. On a stack with
                 * AI_V4MAPPED this returns ::ffff:a.b.c.d. This NDK has no
                 * such flag, so the caller cannot ask for it, and
                 * EAI_ADDRFAMILY is the right answer.
                 */
                return EAI_ADDRFAMILY;
            }

            bsd_addr_from_v4(&addr, v4);

            node = bsd_gai_node(AF_INET, socktype, protocol, &addr, port,
                                ((flags & AI_CANONNAME) != 0)
                                    ? (const char *)nodename : NULL,
                                NULL);
            if (node == NULL)
                return EAI_MEMORY;

            bsd_gai_append(&head, &tail, node);
            *res = head;

            return 0;
        }
    }

    if ((flags & AI_NUMERICHOST) != 0)
        return EAI_NONAME;

    /* ---- the resolver --------------------------------------------------- */

    /*
     * Both lookups are given the caller's break signal, so Ctrl-C reaches a
     * getaddrinfo() the way it reaches a recv(). See bsd_resolve_break() in
     * resolver.c. A break during the second lookup keeps what the first one
     * found.
     */
    verdict = EAI_NONAME;

#ifdef AMINETXDUO_IPV6
    /*
     * AF_INET6 was asked for by name, so it is asked for whatever this machine
     * can reach. AF_UNSPEC is a preference, and a preferred address the
     * machine has no source for is worse than no preference at all. Every
     * interface carries a link-local whether or not a router ever answered it,
     * so netstack_ipv6_enabled() cannot tell those apart.
     * netstack_ipv6_have_global() is the test.
     */
    if ((family == AF_INET6 && netstack_ipv6_enabled()) ||
        (family == AF_UNSPEC && netstack_ipv6_have_global()))
    {
        ULONG words[4];

        status = netstack_resolve6_until((const char *)nodename, words,
                                         BSD_GAI_TIMEOUT, bsd_resolve_break,
                                         SocketBase);
        if (status == AMI_NET_OK)
        {
            addr.nxd_ip_version       = NX_IP_VERSION_V6;
            addr.nxd_ip_address.v6[0] = words[0];
            addr.nxd_ip_address.v6[1] = words[1];
            addr.nxd_ip_address.v6[2] = words[2];
            addr.nxd_ip_address.v6[3] = words[3];

            node = bsd_gai_node(AF_INET6, socktype, protocol, &addr, port,
                                ((flags & AI_CANONNAME) != 0)
                                    ? (const char *)nodename : NULL,
                                NULL);
            if (node != NULL && (flags & AI_CANONNAME) != 0)
                canon = node->name;

            bsd_gai_append(&head, &tail, node);
        }
        else
        {
            verdict = bsd_gai_merge(verdict, bsd_gai_error(status));

            if (status == AMI_NET_ERR_ABORTED)
                aborted = TRUE;
        }
    }
#endif

    if ((family == AF_UNSPEC || family == AF_INET) && !aborted)
    {
        ULONG v4 = 0;

        status = netstack_resolve_until((const char *)nodename, &v4,
                                        BSD_GAI_TIMEOUT, bsd_resolve_break,
                                        SocketBase);
        if (status == AMI_NET_OK)
        {
            bsd_addr_from_v4(&addr, v4);

            node = bsd_gai_node(AF_INET, socktype, protocol, &addr, port,
                                ((flags & AI_CANONNAME) != 0 && canon == NULL)
                                    ? (const char *)nodename : NULL,
                                canon);

            bsd_gai_append(&head, &tail, node);
        }
        else
        {
            verdict = bsd_gai_merge(verdict, bsd_gai_error(status));

            if (status == AMI_NET_ERR_ABORTED)
                aborted = TRUE;
        }
    }

    if (head != NULL)
    {
        *res = head;
        return 0;
    }

    /*
     * POSIX has no EAI code for an interrupted lookup. EAI_SYSTEM with errno
     * set is the documented way to report one, and gai_strerror() already
     * names it.
     */
    if (aborted)
    {
        bsd_set_errno(SocketBase, AMI_EINTR);
        return EAI_SYSTEM;
    }

    return verdict;
}

VOID bsd_freeaddrinfo(register struct addrinfo *ai __asm("a0"),
                      register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)SocketBase;

    while (ai != NULL)
    {
        struct addrinfo *next = ai->ai_next;

        /*
         * ai_canonname is never freed separately: it is either inside this
         * node's own block or inside the first node's, and both go with the
         * ami_free() below. The first node is freed first. Nothing reads the
         * name once freeaddrinfo() starts.
         */
        ami_free(ai);

        ai = next;
    }
}

STRPTR bsd_gai_strerror(register LONG errnum __asm("a0"),
                        register struct AmiSocketBase *SocketBase __asm("a6"))
{
    LONG code = errnum;

    (VOID)SocketBase;

    switch (code)
    {
        case 0:                return (STRPTR)"no error";
        case EAI_BADFLAGS:     return (STRPTR)"invalid value for ai_flags";
        case EAI_NONAME:       return (STRPTR)"name or service is not known";
        case EAI_AGAIN:        return (STRPTR)"temporary failure in name resolution";
        case EAI_FAIL:         return (STRPTR)"non-recoverable failure in name resolution";
        case EAI_NODATA:       return (STRPTR)"no address associated with name";
        case EAI_FAMILY:       return (STRPTR)"ai_family not supported";
        case EAI_SOCKTYPE:     return (STRPTR)"ai_socktype not supported";
        case EAI_SERVICE:      return (STRPTR)"service not supported for ai_socktype";
        case EAI_ADDRFAMILY:   return (STRPTR)"address family for name not supported";
        case EAI_MEMORY:       return (STRPTR)"memory allocation failure";
        case EAI_SYSTEM:       return (STRPTR)"system error";
        case EAI_BADHINTS:     return (STRPTR)"invalid value for hints";
        case EAI_PROTOCOL:     return (STRPTR)"resolved protocol is unknown";
        default:               return (STRPTR)"unknown error";
    }
}

LONG bsd_getnameinfo(register struct sockaddr *sa __asm("a0"),
                     register ULONG salen         __asm("d0"),
                     register STRPTR host         __asm("a1"),
                     register ULONG hostlen       __asm("d1"),
                     register STRPTR serv         __asm("a2"),
                     register ULONG servlen       __asm("d2"),
                     register ULONG flags         __asm("d3"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    NXD_ADDRESS addr;
    UINT        port  = 0;
    ULONG       scope = 0;
    LONG        family;

    if (sa == NULL)
        return EAI_FAIL;

    if ((host == NULL || hostlen == 0) && (serv == NULL || servlen == 0))
        return EAI_NONAME;

    family = bsd_sa_family(sa, (socklen_t)salen);
    if (family != AF_INET
#ifdef AMINETXDUO_IPV6
        && family != AF_INET6
#endif
        )
        return EAI_FAMILY;

    if (bsd_sockaddr_get(SocketBase, sa, (socklen_t)salen, &addr, &port,
                         &scope) != 0)
        return EAI_FAMILY;

    /* ---- the host half --------------------------------------------------- */

    if (host != NULL && hostlen > 0)
    {
        char  name[256];
        BOOL  resolved = FALSE;
        LONG  status   = AMI_NET_OK;
        ULONG v4       = 0;
        BOOL  have_v4  = (BOOL)(addr.nxd_ip_version == NX_IP_VERSION_V4);

        if (have_v4)
            v4 = addr.nxd_ip_address.v4;

#ifdef AMINETXDUO_IPV6
        /*
         * RFC 3493 6.2: "If the socket address structure contains an
         * IPv4-mapped IPv6 address ... the implementation shall extract the
         * embedded IPv4 address and lookup the node name for that IPv4
         * address." A ::ffff:a.b.c.d out of accept() on a dual-stack socket
         * arrives here still tagged V6, so without this the version test
         * below misses it and no query is ever made.
         *
         * bsd_sockaddr_get() is deliberately not the place for it: that one
         * feeds connect(), bind() and sendto() as well, where the mapped form
         * has to survive as far as bsd_addr_normalise() and its V6ONLY check.
         *
         * The IPv4-compatible form (::a.b.c.d) that sentence also names is
         * not handled. RFC 4291 2.5.5.1 deprecated it, and a plain prefix
         * test for it also matches :: and ::1.
         */
        if (!have_v4 && bsd_addr_is_v4mapped(&addr, &v4))
            have_v4 = TRUE;

        /*
         * RFC 3493 6.2: "If the address is the IPv6 unspecified address
         * ("::"), a lookup is not performed, and the EAI_NONAME error is
         * returned." It names no node, so the numeric string this used to
         * return named nothing.
         */
        if (addr.nxd_ip_version == NX_IP_VERSION_V6 &&
            addr.nxd_ip_address.v6[0] == 0UL &&
            addr.nxd_ip_address.v6[1] == 0UL &&
            addr.nxd_ip_address.v6[2] == 0UL &&
            addr.nxd_ip_address.v6[3] == 0UL)
        {
            return EAI_NONAME;
        }
#endif

        if ((flags & (ULONG)NI_NUMERICHOST) == 0)
        {
            if (have_v4)
            {
                status   = netstack_resolve_reverse_until(v4, name,
                                                          sizeof(name),
                                                          BSD_GAI_TIMEOUT,
                                                          bsd_resolve_break,
                                                          SocketBase);
                resolved = (BOOL)(status == AMI_NET_OK);
            }
            /*
             * No reverse lookup for a real IPv6 address. That is an ip6.arpa
             * PTR query, available through NetX Duo's
             * nxd_dns_host_by_address_get() on an NXD_ADDRESS, but the
             * nibble-reversed name it builds is untested here. An
             * NI_NAMEREQD caller gets EAI_NONAME and every other caller gets
             * the numeric form, which is also what a host with no PTR record
             * produces.
             */
        }

        if (!resolved)
        {
            /*
             * NI_NAMEREQD is the only path where the reason matters: without
             * it the numeric form is a correct answer whatever went wrong, and
             * with it the caller is told nothing else. A name server that did
             * not answer is not evidence that the address has no name.
             */
            if ((flags & (ULONG)NI_NAMEREQD) != 0)
            {
                if (status == AMI_NET_ERR_ABORTED)
                {
                    bsd_set_errno(SocketBase, AMI_EINTR);
                    return EAI_SYSTEM;
                }

                return bsd_gai_error(status);
            }

#ifdef AMINETXDUO_IPV6
            if (addr.nxd_ip_version == NX_IP_VERSION_V6)
            {
                char zone[AMI_CFG_IP6_ZONE_LEN];

                /*
                 * RFC 4007 11.1: the "%zone" notation is for addresses of
                 * non-global scope. A scope_id on a global address names
                 * nothing, so it is not printed even when a caller set one. A
                 * printed one produces a string that parses back to a
                 * destination the caller never gave.
                 *
                 * bsd_if_name_by_index() rather than the if_indextoname()
                 * vector: that one reports ENXIO, and getnameinfo() returning
                 * 0 must not leave errno moved.
                 */
                zone[0] = '\0';
                if (scope != 0 &&
                    (addr.nxd_ip_address.v6[0] & 0xFFC00000UL) == 0xFE800000UL)
                {
                    (VOID)bsd_if_name_by_index(netstack_ip(), scope, zone,
                                               sizeof(zone));
                }

                ami_config_format_ip6_zone(addr.nxd_ip_address.v6, zone, name,
                                           sizeof(name));
            }
            else
#endif
            {
                ami_config_format_ip(addr.nxd_ip_address.v4, name,
                                     sizeof(name));
            }
        }
        else if ((flags & (ULONG)NI_NOFQDN) != 0)
        {
            /* Strip everything from the first dot. The short form only means
               anything in the local domain. */
            ULONG i;

            for (i = 0; name[i] != '\0'; i++)
            {
                if (name[i] == '.')
                {
                    name[i] = '\0';
                    break;
                }
            }
        }

        if (bsd_strlen(name) >= hostlen)
            return EAI_MEMORY;      /* POSIX says EAI_OVERFLOW, the NDK has none */

        bsd_strncpy((char *)host, name, hostlen);
    }

    /* ---- the service half ------------------------------------------------ */

    if (serv != NULL && servlen > 0)
    {
        const AmiNetdbEntry *entry = NULL;
        char                 text[16];

        if ((flags & (ULONG)NI_NUMERICSERV) == 0)
            entry = ami_netdb_serv_by_port((LONG)port,
                                           ((flags & (ULONG)NI_DGRAM) != 0)
                                               ? "udp" : "tcp");

        if (entry != NULL && entry->name != NULL)
        {
            if (bsd_strlen(entry->name) >= servlen)
                return EAI_MEMORY;

            bsd_strncpy((char *)serv, entry->name, servlen);
        }
        else
        {
            ULONG value = (ULONG)port;
            ULONG n     = 0;
            char  digits[8];

            do
            {
                digits[n++] = (char)('0' + (value % 10UL));
                value /= 10UL;
            }
            while (value != 0UL && n < sizeof(digits));

            {
                ULONG i;

                for (i = 0; i < n; i++)
                    text[i] = digits[n - 1 - i];
                text[n] = '\0';
            }

            if (n >= servlen)
                return EAI_MEMORY;

            bsd_strncpy((char *)serv, text, servlen);
        }
    }

    return 0;
}
