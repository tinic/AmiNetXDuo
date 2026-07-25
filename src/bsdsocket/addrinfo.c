/*
 * bsdsocket.library -- getaddrinfo / getnameinfo / freeaddrinfo / gai_strerror.
 *
 * These are the four vectors at the very end of the Roadshow LVO table
 * (0x324..0x336) and the only family-agnostic name lookup the ABI has.  They
 * ship in both build configurations -- an IPv4-only build still answers them,
 * just never with an AF_INET6 result.
 *
 * WHAT AF_UNSPEC RETURNS, AND IN WHAT ORDER
 *
 * The NDK's netdb.h:176 defines AI_MASK as only
 *     AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV
 * There is no AI_V4MAPPED and no AI_ADDRCONFIG, so the two hints modern POSIX
 * uses to steer dual-stack results cannot be expressed by a caller compiled
 * against this header.  The behaviour therefore has to be chosen once, here,
 * and documented -- a caller has no way to ask for anything else:
 *
 *   1. IPv6 results come FIRST, then IPv4.  A dual-stack host should prefer
 *      IPv6, and a caller that walks the list in order and connects to the
 *      first address that works gets that for free.
 *   2. AI_ADDRCONFIG is implied and cannot be turned off: AAAA is only looked
 *      up when the stack actually has IPv6 running (netstack_ipv6_enabled()),
 *      and A is only looked up when it has an IPv4 address.  Returning an
 *      address family the machine cannot use has no honest purpose, and the
 *      caller cannot ask for it.
 *   3. AI_V4MAPPED is NOT implied and is not available.  An AF_INET6 query
 *      returns AAAA records and nothing else -- it never synthesises
 *      ::ffff:a.b.c.d from an A record.  A caller that wants both asks for
 *      AF_UNSPEC, which is what the flag exists to avoid needing and what is
 *      left when the flag does not exist.
 *   4. At most ONE address per family is returned.  The resolver underneath
 *      (netstack_resolve/netstack_resolve6 over NetX Duo's addons/dns) answers
 *      with a single address, not a set; reporting a one-element list as if it
 *      were the whole RRset would be a lie about round-robin DNS.  A caller
 *      that walks the list still behaves correctly, it just has fewer things
 *      to walk.
 *
 * MEMORY
 *
 * Each result is ONE allocation: the addrinfo, its sockaddr and (on the first
 * node only) its canonical name live in one block, so freeaddrinfo() is a walk
 * and a free per node with nothing to get wrong.  ai_canonname on the second
 * node points into the first node's block, which is why freeaddrinfo() must
 * not free it separately and why the list is only ever freed as a whole -- as
 * POSIX requires.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"

#include <proto/exec.h>

/* Resolver patience, in ThreadX ticks. Matches resolver.c. */
#define BSD_GAI_TIMEOUT     (30UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * One result node. The sockaddr is embedded rather than pointed at somewhere
 * else so that the whole node is one allocation; ai_addr points at it.
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

/* Strict unsigned decimal; FALSE on anything else, including empty. */
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
 * Resolve the service name. Returns an EAI_* code, or 0 with *port set (in
 * HOST order -- the sockaddr writers do the conversion).
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
     * first, which is what every other resolver does.
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

/*
 * Build one node. `canon` is copied into it when non-NULL; otherwise the node
 * carries no name and ai_canonname is left as `inherit` (which points into an
 * earlier node's block -- see the memory note at the top).
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
        /* No sin6_len on this NDK -- see src/bsdsocket/in6.c. */
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
 * REGISTER ASSIGNMENT IS FROM THE NDK PRAGMA, NOT FROM THE C PROTOTYPE.
 *
 *   pragmas/bsdsocket_pragmas.h:139  freeaddrinfo(a0)
 *   pragmas/bsdsocket_pragmas.h:140  getaddrinfo(a0,a1,a2,a3)
 *   pragmas/bsdsocket_pragmas.h:141  gai_strerror(a0)      <-- NOT d0
 *   pragmas/bsdsocket_pragmas.h:142  getnameinfo(a0,d0,a1,d1,a2,d2,d3)
 *
 * gai_strerror() takes a LONG in an ADDRESS register. That is not a typo in
 * the header -- the libcall form on line 264 says "801" too, which is the
 * same a0 -- and it is the second time this project has found an argument in
 * the register you would not have guessed (bpf_set_notify_mask takes (d1,d0)
 * where its neighbour takes (d0,d1)). Every prototype here is generated from
 * that pragma table by tools/gen_vectors.py rather than written by hand,
 * which is why the definitions below have to match bsdsocket_vectors.h
 * exactly and not merely plausibly.
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
    LONG             status;
    UINT             port = 0;
    char            *canon = NULL;
    BsdAddrInfoNode *node;
    NXD_ADDRESS      addr;

    (VOID)SocketBase;

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
         * against a different header and is asking for something this library
         * cannot promise, so say so rather than ignore it.
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
    if (family == AF_UNSPEC || family == AF_INET6)
    {
        ULONG words[4];

        if (ami_config_parse_ip6((const char *)nodename, words, NULL))
        {
            if (family == AF_INET)
                return EAI_ADDRFAMILY;

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
                 * AI_V4MAPPED this would return ::ffff:a.b.c.d; this NDK has
                 * no such flag, so there is no way for the caller to have
                 * asked for it and EAI_ADDRFAMILY is the truthful answer.
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

#ifdef AMINETXDUO_IPV6
    if ((family == AF_UNSPEC || family == AF_INET6) && netstack_ipv6_enabled())
    {
        ULONG words[4];

        if (netstack_resolve6((const char *)nodename, words,
                              BSD_GAI_TIMEOUT) == AMI_NET_OK)
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
    }
#endif

    if (family == AF_UNSPEC || family == AF_INET)
    {
        ULONG v4 = 0;

        if (netstack_resolve((const char *)nodename, &v4,
                             BSD_GAI_TIMEOUT) == AMI_NET_OK)
        {
            bsd_addr_from_v4(&addr, v4);

            node = bsd_gai_node(AF_INET, socktype, protocol, &addr, port,
                                ((flags & AI_CANONNAME) != 0 && canon == NULL)
                                    ? (const char *)nodename : NULL,
                                canon);

            bsd_gai_append(&head, &tail, node);
        }
    }

    if (head == NULL)
        return EAI_NONAME;

    *res = head;

    return 0;
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
         * ami_free() below (the first node is freed last only by accident of
         * order -- it is freed first, which is fine, because nothing reads
         * the name after freeaddrinfo() starts).
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
    UINT        port = 0;
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
                         NULL) != 0)
        return EAI_FAMILY;

    /* ---- the host half --------------------------------------------------- */

    if (host != NULL && hostlen > 0)
    {
        char name[256];
        BOOL resolved = FALSE;

        if ((flags & (ULONG)NI_NUMERICHOST) == 0)
        {
            if (addr.nxd_ip_version == NX_IP_VERSION_V4)
            {
                resolved = (netstack_resolve_reverse(addr.nxd_ip_address.v4,
                                                     name, sizeof(name),
                                                     BSD_GAI_TIMEOUT)
                            == AMI_NET_OK);
            }
            /*
             * There is no reverse lookup for IPv6.
             *
             * It would be an ip6.arpa PTR query, which NetX Duo's DNS client
             * only offers through nxd_dns_host_by_address_get() on an
             * NXD_ADDRESS -- available -- but the nibble-reversed name it
             * builds has never been exercised here, and a name that is wrong
             * is worse than no name. NI_NAMEREQD callers get EAI_NONAME and
             * everyone else gets the numeric form, which is what a host with
             * no PTR record produces anyway.
             */
        }

        if (!resolved)
        {
            if ((flags & (ULONG)NI_NAMEREQD) != 0)
                return EAI_NONAME;

#ifdef AMINETXDUO_IPV6
            if (addr.nxd_ip_version == NX_IP_VERSION_V6)
            {
                ami_config_format_ip6(addr.nxd_ip_address.v6, name,
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
            /* Strip everything from the first dot: the caller asked for the
               short form, which is only meaningful in the local domain. */
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
            return EAI_MEMORY;      /* POSIX says EAI_OVERFLOW; the NDK has none */

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
