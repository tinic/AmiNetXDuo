/*
 * bsdsocket.library, getaddrinfo / getnameinfo / freeaddrinfo / gai_strerror.
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include "aminetxduo/config.h"
#include "aminetxduo/addrinfo.h"
#include "interfaces.h"

#include <proto/exec.h>
#include <stddef.h>

#ifdef AMINETXDUO_IPV6
#include "../ipv6/ipv6_srcsel.h"
#endif

#ifdef AMINETXDUO_ADDRINFO

/* Resolver timeout, in ThreadX ticks. Matches resolver.c. */
#define BSD_GAI_TIMEOUT     (30UL * (ULONG)NX_IP_PERIODIC_RATE)

/*
 * One result node. The sockaddr is embedded so the whole node is one
 * allocation, and ai_addr points at it.
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

/* Strict unsigned decimal. FALSE on anything else, including empty. */
static BOOL bsd_gai_number(const char *s, ULONG *out)
{
    ULONG value  = 0;
    ULONG digits = 0;

    if (s == NULL)
        return FALSE;

    while (*s >= '0' && *s <= '9')
    {
        ULONG digit = (ULONG)(*s - '0');

        if (value > (0xFFFFUL - digit) / 10UL)
            return FALSE;
        value = value * 10UL + digit;
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
static LONG bsd_gai_service(const char *servname, LONG flags, LONG *socktype,
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

    entry = ami_netdb_serv_by_name(servname,
                                   (*socktype == SOCK_DGRAM) ? "udp" : "tcp");
    if (entry == NULL && *socktype == 0)
    {
        entry = ami_netdb_serv_by_name(servname, "udp");
        if (entry != NULL)
            *socktype = SOCK_DGRAM;
    }

    if (entry == NULL)
        return EAI_SERVICE;

    *port = (UINT)entry->value;

    return 0;
}

#ifdef AMINETXDUO_IPV6
/*
 * RFC 4007 §11's <zone_id>, as an interface index. "SHOULD support at least
 * numerical indices ... MAY support other kinds of non-null strings", so both
 */
static ULONG bsd_gai_zone_index(struct AmiSocketBase *base, const char *zone)
{
    ULONG value  = 0;
    BOOL  digits = TRUE;
    ULONG i;

    if (zone == NULL || zone[0] == '\0')
        return 0;

    for (i = 0; zone[i] != '\0'; i++)
    {
        ULONG digit;

        if (zone[i] < '0' || zone[i] > '9')
        {
            digits = FALSE;
            break;
        }

        digit = (ULONG)(zone[i] - '0');
        if (digit > (ULONG)NX_MAX_PHYSICAL_INTERFACES ||
            value > ((ULONG)NX_MAX_PHYSICAL_INTERFACES - digit) / 10UL)
            return 0;
        value = value * 10UL + digit;
    }

    if (digits)
        return value;

    {
        LONG index = bsd_if_index_of(bsd_stack_ip(base), zone);

        return (index < 0) ? 0UL : (ULONG)(index + 1);
    }
}
#endif

#ifdef AMINETXDUO_IPV6
/*
 * RFC 6724 6 destination ordering, for the one AAAA and one A a lookup can
 * return. Rules 1, 2, 3, 5, 6 and 8 are packed into one key, most significant
 * first, and IPv4 moves ahead only on a strictly greater key, so a tie keeps
 * IPv6 first (rule 10). Not implemented: rule 4 (no Mobile IPv6) and rule 7
 * (no tunnels to tell apart); rule 9 only compares within one family. The
 * sources are the ones connect() would use (src/ipv6/ipv6_srcsel.c, IPv4
 * route lookup), and a destination with none (no address, or off-link with
 * no router) fails rule 1. Both failing is a tie.
 */
static ULONG bsd_gai_key(const ULONG d[4], const ULONG s[4], BOOL deprecated)
{
    UINT dscope = anx6_scope(d);
    UINT prec;
    UINT dlabel;
    UINT slabel;

    (VOID)anx6_policy_lookup(d, &prec, &dlabel);
    (VOID)anx6_policy_lookup(s, NULL, &slabel);

    return 0x8000UL |                                           /* rule 1 */
           ((anx6_scope(s) == dscope) ? 0x4000UL : 0UL) |       /* rule 2 */
           (deprecated ? 0UL : 0x2000UL) |                      /* rule 3 */
           ((slabel == dlabel) ? 0x1000UL : 0UL) |              /* rule 5 */
           ((ULONG)prec << 4) |                                 /* rule 6 */
           (ULONG)(15U - (dscope & 15U));                       /* rule 8 */
}

/* Swap an AAAA-then-A list when the A answer orders first. Each node owns
   its canonical name, so the swap moves nothing else. */
static VOID bsd_gai_order(struct AmiSocketBase *base, struct addrinfo **head)
{
    BsdAddrInfoNode *n6 = (BsdAddrInfoNode *)*head;
    BsdAddrInfoNode *n4 = (BsdAddrInfoNode *)n6->ai.ai_next;
    ULONG            d6[4] = { 0, 0, 0, 0 };
    ULONG            s6[4];
    ULONG            d4[4] = { 0, 0, 0xFFFFUL, 0 };
    ULONG            s4[4] = { 0, 0, 0xFFFFUL, 0 };
    ULONG            state = 0;
    ULONG            k6 = 0;
    ULONG            k4 = 0;
    UWORD            i;

    if (n4 == NULL || n6->ai.ai_family != AF_INET6 ||
        n4->ai.ai_family != AF_INET)
        return;

    for (i = 0; i < 16; i++)
        d6[i >> 2] = (d6[i >> 2] << 8) | n6->addr.sin6.sin6_addr.s6_addr[i];
    d4[3] = BSD_NTOHL(n4->addr.sin.sin_addr.s_addr);

    if (bsd_nx_enter(base) != 0)
        return;

    if (netstack_ipv6_source_state(d6, s6, &state))
        k6 = bsd_gai_key(d6, s6,
                         state == (ULONG)NX_IPV6_ADDR_STATE_DEPRECATED);
    if (netstack_ipv4_route(d4[3], -1L, NULL, NULL, &s4[3]) && s4[3] != 0UL)
        k4 = bsd_gai_key(d4, s4, FALSE);

    bsd_nx_leave(base);

    if (k4 > k6)
    {
        n4->ai.ai_next = &n6->ai;
        n6->ai.ai_next = NULL;
        *head          = &n4->ai;
    }
}
#endif

/*
 * Build one node. `canon` is copied into it when non-NULL. Every node owns
 * its copy, so a sublist the caller detaches and frees takes no other node's
 * name with it (RFC 3493 6.1).
 */
static BsdAddrInfoNode *bsd_gai_node(LONG family, LONG socktype, LONG protocol,
                                     const NXD_ADDRESS *addr, UINT port,
                                     const char *canon)
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
        node->addr.sin6.sin6_len = (UBYTE)sizeof(struct sockaddr_in6);
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

    return node;
}

/*
 * netstack error -> EAI_*, the getaddrinfo() half of bsd_herrno_of()
 * (resolver.c).
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
 */
static LONG bsd_gai_merge(LONG so_far, LONG now)
{
    if (so_far == EAI_AGAIN || now == EAI_AGAIN)
        return EAI_AGAIN;

    return (so_far != EAI_NONAME) ? so_far : now;
}

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

/*
 * Register assignment comes from the NDK pragma, not from the C prototype.
 *
 *   pragmas/bsdsocket_pragmas.h:139  freeaddrinfo(a0)
 *   pragmas/bsdsocket_pragmas.h:140  getaddrinfo(a0,a1,a2,a3)
 *   pragmas/bsdsocket_pragmas.h:141  gai_strerror(a0)      <-- not d0
 *   pragmas/bsdsocket_pragmas.h:142  getnameinfo(a0,d0,a1,d1,a2,d2,d3)
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
    BOOL             allow4 = TRUE;
#ifdef AMINETXDUO_IPV6
    BOOL             allow6 = TRUE;
#endif
    LONG             status;
    UINT             port = 0;
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
        if (hints->ai_addrlen != 0 || hints->ai_addr != NULL ||
            hints->ai_canonname != NULL || hints->ai_next != NULL)
            return EAI_BADHINTS;

        flags    = (LONG)hints->ai_flags;
        family   = (LONG)hints->ai_family;
        socktype = (LONG)hints->ai_socktype;
        protocol = (LONG)hints->ai_protocol;

        if ((flags & ~(LONG)(AI_MASK | AI_EXT | AI_ADDRCONFIG)) != 0)
            return EAI_BADFLAGS;

        if (family != AF_UNSPEC && family != AF_INET
#ifdef AMINETXDUO_IPV6
            && family != AF_INET6
#endif
            )
            return EAI_FAMILY;

        if (socktype != 0 && socktype != SOCK_STREAM && socktype != SOCK_DGRAM)
            return EAI_SOCKTYPE;

        if (protocol != 0 && protocol != IPPROTO_TCP &&
            protocol != IPPROTO_UDP)
            return EAI_PROTOCOL;

        if ((socktype == SOCK_STREAM && protocol == IPPROTO_UDP) ||
            (socktype == SOCK_DGRAM  && protocol == IPPROTO_TCP))
            return EAI_SOCKTYPE;
    }

    if (socktype == 0 && protocol != 0)
        socktype = (protocol == IPPROTO_UDP) ? SOCK_DGRAM : SOCK_STREAM;

    status = bsd_gai_service((const char *)servname, flags, &socktype, &port);
    if (status != 0)
        return status;

    if (socktype == 0)
        socktype = SOCK_STREAM;
    if (protocol == 0)
        protocol = (socktype == SOCK_DGRAM) ? IPPROTO_UDP : IPPROTO_TCP;

    /* Explicit AI_ADDRCONFIG filtering includes ULA and link-local addresses.
       The legacy AF_UNSPEC hostname policy is applied separately below. */
    if ((flags & AI_ADDRCONFIG) != 0)
    {
        allow4 = netstack_ipv4_configured();
#ifdef AMINETXDUO_IPV6
        allow6 = netstack_ipv6_configured();
#endif
    }

    if (nodename == NULL || *nodename == '\0')
    {
        /*
         * AI_PASSIVE: the address a server binds to (INADDR_ANY / in6addr_any).
         * Without it, the address a client connects to on this machine, which
         * is the loopback address.
         */
#ifdef AMINETXDUO_IPV6
        if ((family == AF_UNSPEC || family == AF_INET6) &&
            allow6 && netstack_ipv6_enabled())
        {
            addr.nxd_ip_version       = NX_IP_VERSION_V6;
            addr.nxd_ip_address.v6[0] = 0UL;
            addr.nxd_ip_address.v6[1] = 0UL;
            addr.nxd_ip_address.v6[2] = 0UL;
            addr.nxd_ip_address.v6[3] = ((flags & AI_PASSIVE) != 0) ? 0UL : 1UL;

            node = bsd_gai_node(AF_INET6, socktype, protocol, &addr, port,
                                NULL);
            if (node == NULL)
                return EAI_MEMORY;

            bsd_gai_append(&head, &tail, node);
        }
#endif
        if (allow4 && (family == AF_UNSPEC || family == AF_INET))
        {
            bsd_addr_from_v4(&addr, ((flags & AI_PASSIVE) != 0)
                                        ? 0UL : 0x7F000001UL);

            node = bsd_gai_node(AF_INET, socktype, protocol, &addr, port,
                                NULL);
            if (node == NULL)
            {
                bsd_freeaddrinfo(head, SocketBase);
                return EAI_MEMORY;
            }

            bsd_gai_append(&head, &tail, node);
        }

        if (head == NULL)
        {
            return EAI_ADDRFAMILY;
        }

        *res = head;

        return 0;
    }

#ifdef AMINETXDUO_IPV6
    {
        ULONG words[4];
        char  zone[AMI_CFG_IP6_ZONE_LEN];

        if (ami_config_parse_ip6_zone((const char *)nodename, words, NULL,
                                      zone, sizeof(zone)))
        {
            ULONG scope;

            if (family == AF_INET || !allow6)
                return EAI_ADDRFAMILY;

            scope = bsd_gai_zone_index(SocketBase, zone);
            if (zone[0] != '\0' && scope == 0)
                return EAI_NONAME;

            addr.nxd_ip_version       = NX_IP_VERSION_V6;
            addr.nxd_ip_address.v6[0] = words[0];
            addr.nxd_ip_address.v6[1] = words[1];
            addr.nxd_ip_address.v6[2] = words[2];
            addr.nxd_ip_address.v6[3] = words[3];

            node = bsd_gai_node(AF_INET6, socktype, protocol, &addr, port,
                                ((flags & AI_CANONNAME) != 0)
                                    ? (const char *)nodename : NULL);
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
            if (family == AF_INET6 || !allow4)
            {
                return EAI_ADDRFAMILY;
            }

            bsd_addr_from_v4(&addr, v4);

            node = bsd_gai_node(AF_INET, socktype, protocol, &addr, port,
                                ((flags & AI_CANONNAME) != 0)
                                    ? (const char *)nodename : NULL);
            if (node == NULL)
                return EAI_MEMORY;

            bsd_gai_append(&head, &tail, node);
            *res = head;

            return 0;
        }
    }

    if ((flags & AI_NUMERICHOST) != 0)
        return EAI_NONAME;

    verdict = EAI_NONAME;

#ifdef AMINETXDUO_IPV6
    /* Preserve unflagged AF_UNSPEC lookups on IPv4-only LANs: merely enabling
       IPv6 (and assigning a link-local address) must not add an AAAA query or
       put an unreachable IPv6 result ahead of IPv4. Explicit AF_INET6 and
       AI_ADDRCONFIG requests retain their own semantics, including ULA use. */
    if (allow6 && netstack_ipv6_enabled() &&
        (family == AF_INET6 ||
         (family == AF_UNSPEC &&
          ((flags & AI_ADDRCONFIG) != 0 || netstack_ipv6_have_global()))))
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
                                    ? (const char *)nodename : NULL);
            if (node == NULL)
            {
                bsd_freeaddrinfo(head, SocketBase);
                return EAI_MEMORY;
            }

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

    if (allow4 && (family == AF_UNSPEC || family == AF_INET) && !aborted)
    {
        ULONG v4 = 0;

        status = netstack_resolve_until((const char *)nodename, &v4,
                                        BSD_GAI_TIMEOUT, bsd_resolve_break,
                                        SocketBase);
        if (status == AMI_NET_OK)
        {
            bsd_addr_from_v4(&addr, v4);

            node = bsd_gai_node(AF_INET, socktype, protocol, &addr, port,
                                ((flags & AI_CANONNAME) != 0)
                                    ? (const char *)nodename : NULL);
            if (node == NULL)
            {
                bsd_freeaddrinfo(head, SocketBase);
                return EAI_MEMORY;
            }

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
#ifdef AMINETXDUO_IPV6
        bsd_gai_order(SocketBase, &head);
#endif
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
         * ai_canonname is never freed separately: it is inside this node's
         * own block.
         */
        ami_free(ai);

        ai = next;
    }
}

/* Internal offsets avoid one pointer relocation per message. Returned
   strings still live in read-only library storage for the library lifetime. */
#define BSD_GAI_MESSAGES(X) \
    X(0, "no error") \
    X(EAI_BADFLAGS, "invalid value for ai_flags") \
    X(EAI_NONAME, "name or service is not known") \
    X(EAI_AGAIN, "temporary failure in name resolution") \
    X(EAI_FAIL, "non-recoverable failure in name resolution") \
    X(EAI_NODATA, "no address associated with name") \
    X(EAI_FAMILY, "ai_family not supported") \
    X(EAI_SOCKTYPE, "ai_socktype not supported") \
    X(EAI_SERVICE, "service not supported for ai_socktype") \
    X(EAI_ADDRFAMILY, "address family for name not supported") \
    X(EAI_MEMORY, "memory allocation failure") \
    X(EAI_SYSTEM, "system error") \
    X(EAI_BADHINTS, "invalid value for hints") \
    X(EAI_PROTOCOL, "resolved protocol is unknown")

typedef struct BsdGaiTextPool
{
#define BSD_GAI_FIELD(code, text) char message_##code[sizeof(text)];
    BSD_GAI_MESSAGES(BSD_GAI_FIELD)
#undef BSD_GAI_FIELD
} BsdGaiTextPool;

static const BsdGaiTextPool bsd_gai_text_pool = {
#define BSD_GAI_TEXT(code, text) text,
    BSD_GAI_MESSAGES(BSD_GAI_TEXT)
#undef BSD_GAI_TEXT
};

static const struct
{
    WORD code;
    UWORD offset;
} bsd_gai_messages[] = {
#define BSD_GAI_ENTRY(code, text) \
    { code, (UWORD)offsetof(BsdGaiTextPool, message_##code) },
    BSD_GAI_MESSAGES(BSD_GAI_ENTRY)
#undef BSD_GAI_ENTRY
};

#define BSD_GAI_CODE_BOUND(code, text) \
    _Static_assert((code) >= -32768L && (code) <= 32767L, \
                   "gai error codes must fit in WORD");
BSD_GAI_MESSAGES(BSD_GAI_CODE_BOUND)
#undef BSD_GAI_CODE_BOUND
#undef BSD_GAI_MESSAGES
_Static_assert(sizeof(BsdGaiTextPool) <= 65535UL,
               "gai error string offsets must fit in UWORD");

STRPTR bsd_gai_strerror(register LONG errnum __asm("a0"),
                        register struct AmiSocketBase *SocketBase __asm("a6"))
{
    UWORD i;

    (VOID)SocketBase;
    for (i = 0; i < sizeof(bsd_gai_messages) / sizeof(bsd_gai_messages[0]); i++)
    {
        if (errnum == (LONG)bsd_gai_messages[i].code)
            return (STRPTR)((const char *)&bsd_gai_text_pool + bsd_gai_messages[i].offset);
    }
    return (STRPTR)"unknown error";
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

    if ((flags & ~(ULONG)(NI_NUMERICHOST | NI_NUMERICSERV | NI_NOFQDN |
                          NI_NAMEREQD | NI_DGRAM | NI_WITHSCOPEID)) != 0)
        return EAI_BADFLAGS;

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
         */
        if (!have_v4 && bsd_addr_is_v4mapped(&addr, &v4))
            have_v4 = TRUE;

        /*
         * RFC 3493 6.2: "If the address is the IPv6 unspecified address
         * ("::"), a lookup is not performed, and the EAI_NONAME error is
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
        }

        if (!resolved)
        {
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
                 */
                zone[0] = '\0';
                if ((flags & (ULONG)NI_WITHSCOPEID) != 0 && scope != 0 &&
                    (addr.nxd_ip_address.v6[0] & 0xFFC00000UL) == 0xFE800000UL)
                {
                    (VOID)bsd_if_name_by_index(bsd_stack_ip(SocketBase), scope,
                                               zone,
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

#else /* !AMINETXDUO_ADDRINFO */

/*
 * The four vectors with no lookup behind them.  EAI_NONAME rather than a new
 * code: it is what netstack_dns_off.c reports for a name it cannot answer, and
 * every caller of getaddrinfo() already has a path for it.  freeaddrinfo() and
 * gai_strerror() still behave, because a caller that got a failure will call
 * both anyway.
 */
LONG bsd_getaddrinfo(register STRPTR nodename         __asm("a0"),
                     register STRPTR servname         __asm("a1"),
                     register struct addrinfo *hints  __asm("a2"),
                     register struct addrinfo **res   __asm("a3"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)nodename; (VOID)servname; (VOID)hints; (VOID)SocketBase;

    if (res != NULL)
        *res = NULL;

    return EAI_NONAME;
}

VOID bsd_freeaddrinfo(register struct addrinfo *ai __asm("a0"),
                      register struct AmiSocketBase *SocketBase __asm("a6"))
{
    /* Nothing was ever handed out, so there is nothing to give back. */
    (VOID)ai;
    (VOID)SocketBase;
}

STRPTR bsd_gai_strerror(register LONG errnum __asm("a0"),
                        register struct AmiSocketBase *SocketBase __asm("a6"))
{
    (VOID)errnum;
    (VOID)SocketBase;
    return (STRPTR)"name resolution is not built into this library";
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
    (VOID)sa; (VOID)salen; (VOID)host; (VOID)hostlen;
    (VOID)serv; (VOID)servlen; (VOID)flags; (VOID)SocketBase;
    return EAI_NONAME;
}

#endif /* AMINETXDUO_ADDRINFO */
