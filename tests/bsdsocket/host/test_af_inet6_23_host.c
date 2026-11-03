/*
 * AF_INET6 == 23 beside AF_INET6 == 28, at each point a family enters or
 * leaves the library.
 *
 * The AmigaPorts/bebbo ndk-include spells AF_INET6 23 and its sockaddr_in6
 * has no sin6_len: the family is at byte 0 and byte 1 is padding. Programs
 * built with it pass that layout in and read it back through both their
 * sin6_family (byte 0) and their struct sockaddr's sa_family (byte 1).
 *
 * In: socket(), the sockaddr reader behind bind(), connect(), sendto(),
 * sendmsg() and getnameinfo(), getaddrinfo() hints, inet_pton()/inet_ntop()
 * and ObtainSocket(). Out: the sockaddr writer behind accept(),
 * getsockname(), getpeername(), recvfrom() and recvmsg(), and getaddrinfo()
 * results.
 *
 * Here AF_INET6 is the host's value, which stands in for the target's 28;
 * 23 is written as BSD_AF_INET6_23. socket.c, cmsg.c, addrinfo.c, inet.c
 * and handoff.c are #included; --gc-sections keeps only what the cases
 * reach, so the stubs below are the whole closure.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "aminetxduo/config.h"
#include "interfaces.h"
#include "../ipv6/ipv6_srcsel.h"

/* As test_addrinfo_dns_host.c: the target's flag values, not libc's. */
#undef AI_PASSIVE
#undef AI_CANONNAME
#undef AI_NUMERICHOST
#undef AI_NUMERICSERV
#undef AI_ADDRCONFIG
#undef AI_MASK
#undef AI_EXT
#define AI_PASSIVE 1
#define AI_CANONNAME 2
#define AI_NUMERICHOST 4
#define AI_EXT 8
#define AI_NUMERICSERV 16
#include "aminetxduo/addrinfo.h"
#define AI_MASK (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV)
#ifndef EAI_BADHINTS
#define EAI_BADHINTS (-1001)
#endif
#ifndef EAI_PROTOCOL
#define EAI_PROTOCOL (-1002)
#endif
#ifndef EAI_ADDRFAMILY
#define EAI_ADDRFAMILY (-1003)
#endif
#ifndef EAI_NODATA
#define EAI_NODATA (-1004)
#endif
#ifndef NI_WITHSCOPEID
#define NI_WITHSCOPEID 32
#endif

/* As test_addrinfo_dns_host.c: only the target writes this structure. */
#if !defined(__APPLE__)
#undef sin_len
#define sockaddr_in ami_sockaddr_in
struct ami_sockaddr_in {
    UBYTE sin_len;
    UBYTE sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    UBYTE sin_zero[8];
};
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        h_checks++;                                                           \
        if (!(cond)) {                                                        \
            h_failures++;                                                     \
            printf("  FAIL line %d: %s\n", __LINE__, (what));                 \
        }                                                                     \
    } while (0)

#define H_FDS  4
#define F23    BSD_AF_INET6_23

static struct AmiSocketBase h_base;
static struct AmiSocketBase h_other;
static AmiSocket           *h_table[H_FDS];
static AmiSocket           *h_table2[H_FDS];
static NX_IP                h_ip;

/* socket.c's, which comes in at the end. */
static AmiSocket *bsd_socket_alloc(struct AmiSocketBase *base, UWORD domain,
                                   UWORD type, LONG protocol);

/* fd00::5 is ours, fd00::1 the peer. */
static const ULONG h_local[4] = { 0xFD000000UL, 0, 0, 5 };
static const ULONG h_peer[4]  = { 0xFD000000UL, 0, 0, 1 };

/* ---- the externs the cases reach ----------------------------------------- */

APTR ami_alloc(ULONG size) { return calloc(1, size); }
VOID ami_free(APTR p) { free(p); }

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;
    return -1;
}

VOID bsd_set_errno(struct AmiSocketBase *base, LONG err) { base->sb_Errno = err; }

BOOL bsd_netmon_have(LONG type) { (VOID)type; return FALSE; }
STRPTR bsd_netmon_caller(struct AmiSocketBase *base) { (VOID)base; return NULL; }
LONG bsd_netmon_dispatch(LONG type, APTR message)
{
    (VOID)type; (VOID)message;
    return 0;
}

VOID bsd_bzero(APTR p, ULONG size) { memset(p, 0, size); }
VOID bsd_bcopy(CONST_APTR src, APTR dst, ULONG size) { memmove(dst, src, size); }
ULONG bsd_strlen(const char *s) { return (ULONG)strlen(s); }
VOID bsd_strncpy(char *dst, const char *src, ULONG size)
{ if (size) { strncpy(dst, src, size); dst[size - 1] = 0; } }
LONG bsd_nx_enter(struct AmiSocketBase *base) { (VOID)base; return 0; }
VOID bsd_nx_leave(struct AmiSocketBase *base) { (VOID)base; }
VOID bsd_raw_revalidate_endpoint(AmiSocket *sock) { (VOID)sock; }
LONG bsd_errno_from_nx(UINT status) { (VOID)status; return AMI_EIO; }
BOOL bsd_addr_normalise(const AmiSocket *sock, NXD_ADDRESS *addr)
{
    (VOID)sock; (VOID)addr;
    return TRUE;
}

BOOL bsd_udp_accepts_received_packet(const AmiSocket *sock,
                                     const NX_PACKET *packet)
{
    (VOID)sock; (VOID)packet;
    abort();
}

VOID bsd_in6_to_words(const UBYTE bytes[16], ULONG words[4])
{
    UINT i;

    for (i = 0; i < 4; i++)
        words[i] = ((ULONG)bytes[4 * i] << 24) | ((ULONG)bytes[4 * i + 1] << 16) |
                   ((ULONG)bytes[4 * i + 2] << 8) | (ULONG)bytes[4 * i + 3];
}

VOID bsd_words_to_in6(const ULONG words[4], UBYTE bytes[16])
{
    UINT i;

    for (i = 0; i < 16; i++)
        bytes[i] = (UBYTE)(words[i / 4] >> (24 - 8 * (i % 4)));
}

ULONG netstack_interface_epoch(UWORD index) { (VOID)index; return 0; }

BOOL netstack_ipv6_source_find(const ULONG dest[4], LONG required,
                               ULONG chosen[4], UINT *index)
{
    (VOID)dest; (VOID)required;
    memcpy(chosen, h_local, sizeof(h_local));
    *index = 0;
    return TRUE;
}

BOOL netstack_ipv6_enabled(VOID) { return TRUE; }
BOOL netstack_ipv6_have_global(VOID) { return TRUE; }
BOOL netstack_ipv4_configured(VOID) { return TRUE; }
BOOL netstack_ipv6_configured(VOID) { return TRUE; }
BOOL bsd_resolve_break(VOID *arg, ULONG pause)
{ (VOID)arg; (VOID)pause; return FALSE; }
LONG bsd_if_index_of(NX_IP *ip, const char *name)
{ (VOID)ip; (VOID)name; return -1; }
const AmiNetdbEntry *ami_netdb_serv_by_name(const char *name, const char *proto)
{ (VOID)name; (VOID)proto; return NULL; }

/* Only numeric names are looked up here. */
LONG netstack_resolve_until(const char *name, ULONG *out, ULONG timeout,
                            AmiNetGiveUpFn stop, VOID *arg)
{ (VOID)name; (VOID)out; (VOID)timeout; (VOID)stop; (VOID)arg; abort(); }
LONG netstack_resolve6_until(const char *name, ULONG out[4], ULONG timeout,
                             AmiNetGiveUpFn stop, VOID *arg)
{ (VOID)name; (VOID)out; (VOID)timeout; (VOID)stop; (VOID)arg; abort(); }
BOOL netstack_ipv6_source_state(const ULONG dest[4], ULONG addr_out[4],
                                ULONG *state_out)
{ (VOID)dest; (VOID)addr_out; (VOID)state_out; return FALSE; }
BOOL netstack_ipv4_route(ULONG destination, LONG preferred_index,
                         UWORD *index_out, ULONG *next_hop_out,
                         ULONG *source_address_out)
{
    (VOID)destination; (VOID)preferred_index; (VOID)index_out;
    (VOID)next_hop_out; (VOID)source_address_out;
    return FALSE;
}

UINT _nxe_udp_socket_create(NX_IP *ip, NX_UDP_SOCKET *s, CHAR *name,
                            ULONG tos, ULONG frag, UINT ttl, ULONG queue,
                            UINT size)
{
    (VOID)ip; (VOID)s; (VOID)name; (VOID)tos; (VOID)frag; (VOID)ttl;
    (VOID)queue; (VOID)size;
    return NX_SUCCESS;
}

UINT _nxe_udp_socket_bind(NX_UDP_SOCKET *socket_ptr, UINT port,
                          ULONG wait_option)
{
    (VOID)socket_ptr; (VOID)port; (VOID)wait_option;
    return NX_SUCCESS;
}

UINT _nxe_udp_socket_port_get(NX_UDP_SOCKET *socket_ptr, UINT *port_ptr)
{
    (VOID)socket_ptr;
    *port_ptr = 1234;
    return NX_SUCCESS;
}

/* Used by socket(), sockaddr writing and the handoff registry. */
ULONG _tx_time_get(VOID) { return 0; }
ULONG ami_millis(VOID) { return 0; }
VOID ami_mem_socket_delta(LONG delta) { (VOID)delta; }
VOID bsd_events_attach(AmiSocket *sock) { (VOID)sock; }
VOID bsd_mcast_close(AmiSocket *sock) { (VOID)sock; }
UINT _nxe_udp_socket_unbind(NX_UDP_SOCKET *s) { (VOID)s; return NX_SUCCESS; }
UINT _nxe_udp_socket_delete(NX_UDP_SOCKET *s) { (VOID)s; return NX_SUCCESS; }
VOID Signal(struct Task *task, ULONG mask) { (VOID)task; (VOID)mask; }
VOID ObtainSemaphore(struct SignalSemaphore *sem) { (VOID)sem; }
VOID ReleaseSemaphore(struct SignalSemaphore *sem) { (VOID)sem; }

VOID AddTail(struct List *list, struct Node *node)
{
    node->ln_Succ = (struct Node *)&list->lh_Tail;
    node->ln_Pred = list->lh_TailPred;
    list->lh_TailPred->ln_Succ = node;
    list->lh_TailPred = node;
}

VOID Remove(struct Node *node)
{
    node->ln_Pred->ln_Succ = node->ln_Succ;
    node->ln_Succ->ln_Pred = node->ln_Pred;
}

/* in6.c's, which pins the target's AF_INET6 and so is not built here. */
BOOL bsd_addr_is_v4mapped(const NXD_ADDRESS *addr, ULONG *v4)
{
    if (addr->nxd_ip_version != NX_IP_VERSION_V6 ||
        addr->nxd_ip_address.v6[0] != 0 || addr->nxd_ip_address.v6[1] != 0 ||
        addr->nxd_ip_address.v6[2] != 0xFFFFUL)
        return FALSE;
    *v4 = addr->nxd_ip_address.v6[3];
    return TRUE;
}

VOID bsd_addr_to_v4mapped(NXD_ADDRESS *addr, ULONG v4)
{
    addr->nxd_ip_version       = NX_IP_VERSION_V6;
    addr->nxd_ip_address.v6[0] = 0;
    addr->nxd_ip_address.v6[1] = 0;
    addr->nxd_ip_address.v6[2] = 0xFFFFUL;
    addr->nxd_ip_address.v6[3] = v4;
}

/* ---- fixtures -------------------------------------------------------------- */

static void h_reset(void)
{
    NXD_IPV6_ADDRESS *a = &h_ip.nx_ipv6_address[0];
    UINT              i;

    for (i = 0; i < H_FDS; i++)
    {
        free(h_table[i]);
        if (h_table2[i] != h_table[i])
            free(h_table2[i]);
    }

    memset(&h_base, 0, sizeof(h_base));
    memset(&h_other, 0, sizeof(h_other));
    memset(&h_table, 0, sizeof(h_table));
    memset(&h_table2, 0, sizeof(h_table2));
    memset(&h_ip, 0, sizeof(h_ip));

    h_base.sb_Table      = h_table;
    h_base.sb_TableSize  = H_FDS;
    h_base.sb_StackRefs  = 1;
    h_base.sb_StackIp    = &h_ip;
    h_other.sb_Table     = h_table2;
    h_other.sb_TableSize = H_FDS;
    h_other.sb_StackRefs = 1;
    h_other.sb_StackIp   = &h_ip;
    h_other.sb_Master    = &h_base;
    bsd_handoff_init(&h_base);

    h_ip.nx_ip_interface[0].nx_interface_valid = 1;
    a->nxd_ipv6_address_valid    = 1;
    a->nxd_ipv6_address_state    = NX_IPV6_ADDR_STATE_VALID;
    a->nxd_ipv6_address_attached = &h_ip.nx_ip_interface[0];
    memcpy(a->nxd_ipv6_address, h_local, sizeof(h_local));
}

/*
 * A bebbo sockaddr_in6 for fd00::1 port 53 scope 7: `byte0` and `pad` at
 * bytes 0 and 1. Port and scope go in as the library reads them on this
 * host (BSD_HTONS is the identity, as on the big-endian target).
 */
static void h_sa23(UBYTE sa[28], UBYTE byte0, UBYTE pad)
{
    struct sockaddr_in6 sin6;

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_port     = (in_port_t)BSD_HTONS(53);
    sin6.sin6_scope_id = 7;
    bsd_words_to_in6(h_peer, sin6.sin6_addr.s6_addr);
    memcpy(sa, &sin6, 28);
    sa[0] = byte0;
    sa[1] = pad;
}

/* The writer's answer: fd00::1 port 53 scope 7 in bytes 2..27. */
static BOOL h_is_sa_peer(const UBYTE *sa)
{
    UBYTE want[28];

    h_sa23(want, sa[0], sa[1]);
    return memcmp(sa, want, 28) == 0;
}

/* Ours: sin6_len 28 and the family at byte 1. */
static void h_sa28(UBYTE sa[28])
{
    h_sa23(sa, 28, (UBYTE)AF_INET6);
}

static BOOL h_is_peer(const NXD_ADDRESS *a)
{
    return a->nxd_ip_version == NX_IP_VERSION_V6 &&
           memcmp(a->nxd_ip_address.v6, h_peer, sizeof(h_peer)) == 0;
}

/* ---- the cases ------------------------------------------------------------- */

/* Which bytes read as which family. */
static void case_sa_family(void)
{
    UBYTE sa[28];

    h_sa28(sa);
    CHECK(bsd_sa_family((struct sockaddr *)sa, 28) == AF_INET6, "28 layout");

    h_sa23(sa, F23, 0);
    CHECK(bsd_sa_family((struct sockaddr *)sa, 28) == F23,
          "23 at byte 0, zeroed padding");
    h_sa23(sa, F23, 0x5A);
    CHECK(bsd_sa_family((struct sockaddr *)sa, 28) == F23,
          "23 at byte 0, uninitialised padding");
    h_sa23(sa, 0, F23);
    CHECK(bsd_sa_family((struct sockaddr *)sa, 28) == F23,
          "23 written through sa_family, byte 0 zero");
    h_sa23(sa, F23, F23);
    CHECK(bsd_sa_family((struct sockaddr *)sa, 28) == F23,
          "23 at both bytes, as this library writes it");
    h_sa23(sa, 28, F23);
    CHECK(bsd_sa_family((struct sockaddr *)sa, 28) == F23,
          "23 at byte 1 under a sin6_len");

    /* Padding that happens to be AF_INET: 23 there is no sin_len, and a
       sockaddr_in is 16 bytes, so 28 decides it. */
    h_sa23(sa, F23, AF_INET);
    CHECK(bsd_sa_family((struct sockaddr *)sa, 28) == F23,
          "23 at byte 0, padding 2, length 28: IPv6");
    CHECK(bsd_sa_family((struct sockaddr *)sa, 16) == AF_INET,
          "23 at byte 0, family 2, length 16: IPv4 as before");
    CHECK(bsd_sa_family((struct sockaddr *)sa, 128) == AF_INET,
          "23 at byte 0, family 2, storage length: IPv4 as before");

    h_sa23(sa, 16, AF_INET);
    CHECK(bsd_sa_family((struct sockaddr *)sa, 28) == AF_INET,
          "sockaddr_in in a 28-byte buffer stays IPv4");

    h_sa23(sa, F23, 0);
    CHECK(bsd_sa_family((struct sockaddr *)sa, 16) == -1,
          "23 layout shorter than a sockaddr_in6 is refused");
}

/* The reader behind bind, connect, sendto, sendmsg and getnameinfo. */
static void case_reader(void)
{
    UBYTE       sa[28];
    NXD_ADDRESS addr;
    UINT        port = 0;
    ULONG       scope = 0;

    h_reset();
    h_sa28(sa);
    CHECK(bsd_sockaddr_get(&h_base, (struct sockaddr *)sa, 28, &addr, &port,
                           &scope) == 0 &&
              h_is_peer(&addr) && port == 53 && scope == 7,
          "28 layout reads fd00::1 port 53 scope 7");
    CHECK(h_base.sb_Fam23 == 0, "28 layout leaves the opener at 28");

    h_sa23(sa, F23, AF_INET);
    memset(&addr, 0, sizeof(addr));
    CHECK(bsd_sockaddr_get(&h_base, (struct sockaddr *)sa, 28, &addr, &port,
                           &scope) == 0 &&
              h_is_peer(&addr) && port == 53 && scope == 7,
          "23 layout (padding 2) reads the same address, port and scope");
    CHECK(h_base.sb_Fam23 != 0, "23 layout flips the opener to 23");
}

/* The writer behind accept, getsockname, getpeername, recvfrom, recvmsg. */
static void case_writer(void)
{
    AmiSocket   s;
    NXD_ADDRESS addr;
    UBYTE       sa[32];
    socklen_t   len;

    memset(&s, 0, sizeof(s));
    addr.nxd_ip_version = NX_IP_VERSION_V6;
    memcpy(addr.nxd_ip_address.v6, h_peer, sizeof(h_peer));
    s.as_Flags = ASF_UDP | ASF_INET6;
    len = sizeof(sa);
    memset(sa, 0xEE, sizeof(sa));
    bsd_sockaddr_put(&s, (struct sockaddr *)sa, &len, &addr, 53, 7);
    CHECK(len == 28 && sa[0] == 28 && sa[1] == AF_INET6 && h_is_sa_peer(sa),
          "28 socket writes the 28 layout");

    s.as_Flags = ASF_UDP | ASF_INET6 | ASF_FAM23;
    len = sizeof(sa);
    memset(sa, 0xEE, sizeof(sa));
    bsd_sockaddr_put(&s, (struct sockaddr *)sa, &len, &addr, 53, 7);
    CHECK(len == 28 && sa[0] == F23 && sa[1] == F23 && h_is_sa_peer(sa),
          "23 socket writes 23 at sin6_family and at sa_family");

    bsd_addr_from_v4(&addr, 0xC0A80101UL);
    len = sizeof(sa);
    bsd_sockaddr_put(&s, (struct sockaddr *)sa, &len, &addr, 53, 0);
    CHECK(len == 28 && sa[0] == F23 && sa[1] == F23 && sa[18] == 0xFF &&
              sa[19] == 0xFF && sa[20] == 192 && sa[23] == 1,
          "23 socket writes an IPv4 peer as v4-mapped in the 23 layout");
}

/* socket(), bind() and connect() through the vectors, and what an accepted
   socket inherits. */
static void case_socket_api(void)
{
    UBYTE      sa[28];
    LONG       fd;
    AmiSocket *s;
    AmiSocket *child;

    h_reset();
    fd = bsd_socket(F23, SOCK_DGRAM, 0, &h_base);
    CHECK(fd >= 0, "socket(23, SOCK_DGRAM) succeeds");
    s = (fd >= 0) ? h_table[fd] : NULL;
    CHECK(s != NULL && (s->as_Flags & (ASF_INET6 | ASF_FAM23)) ==
                           (ASF_INET6 | ASF_FAM23),
          "socket(23) is an AF_INET6 socket answering in 23");
    CHECK(h_base.sb_Fam23 != 0, "socket(23) flips the opener");
    if (s == NULL)
        return;

    /* A listener's accepted sockets come from bsd_socket_alloc(as_Domain). */
    child = bsd_socket_alloc(&h_base, s->as_Domain, SOCK_STREAM, 0);
    CHECK(child != NULL && (child->as_Flags & ASF_FAM23) != 0,
          "a socket made from its domain (accept) answers in 23");
    free(child);

    h_sa23(sa, F23, 0);
    bsd_words_to_in6(h_local, &sa[8]);
    CHECK(bsd_bind(fd, (struct sockaddr *)sa, 28, &h_base) == 0,
          "bind() with the 23 layout");

    h_sa23(sa, F23, 0x5A);
    CHECK(bsd_connect(fd, (struct sockaddr *)sa, 28, &h_base) == 0 &&
              h_is_peer(&s->as_PeerAddr) && s->as_PeerPort == 53,
          "connect() with the 23 layout reaches fd00::1 port 53");

    h_reset();
    fd = bsd_socket(AF_INET6, SOCK_DGRAM, 0, &h_base);
    CHECK(fd >= 0 && (h_table[fd]->as_Flags & ASF_FAM23) == 0 &&
              h_base.sb_Fam23 == 0,
          "socket(28) answers in 28 and leaves the opener at 28");
    h_sa23(sa, F23, 0);
    CHECK(fd >= 0 &&
              bsd_connect(fd, (struct sockaddr *)sa, 28, &h_base) == 0 &&
              h_is_peer(&h_table[fd]->as_PeerAddr),
          "a 28 socket takes a 23-layout address too");
}

static struct addrinfo *h_gai(const char *node, const char *serv, int family)
{
    struct addrinfo  hints;
    struct addrinfo *res = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_NUMERICHOST;
    if (bsd_getaddrinfo((STRPTR)node, (STRPTR)serv, &hints, &res,
                        &h_base) != 0)
        return NULL;
    return res;
}

static void case_getaddrinfo(void)
{
    struct addrinfo *ai;
    UBYTE           *b;

    h_reset();
    ai = h_gai("fd00::1", "53", AF_UNSPEC);
    b  = (ai != NULL) ? (UBYTE *)ai->ai_addr : NULL;
    CHECK(ai != NULL && ai->ai_family == AF_INET6 && b[0] == 28 &&
              b[1] == AF_INET6 && ai->ai_addrlen == 28,
          "AF_UNSPEC on a fresh opener answers in 28");
    bsd_freeaddrinfo(ai, &h_base);

    ai = h_gai("fd00::1", "53", F23);
    b  = (ai != NULL) ? (UBYTE *)ai->ai_addr : NULL;
    CHECK(ai != NULL && ai->ai_family == F23 && b[0] == F23 &&
              b[1] == F23 &&
              ((struct sockaddr_in6 *)ai->ai_addr)->sin6_port ==
                  BSD_HTONS(53) &&
              ai->ai_addrlen == 28,
          "hints 23 answer in 23 (was EAI_FAMILY)");
    CHECK(h_base.sb_Fam23 != 0, "hints 23 flip the opener");
    bsd_freeaddrinfo(ai, &h_base);

    ai = h_gai("fd00::1", "53", AF_UNSPEC);
    CHECK(ai != NULL && ai->ai_family == F23 &&
              ((UBYTE *)ai->ai_addr)[0] == F23,
          "AF_UNSPEC after the flip answers in 23");
    bsd_freeaddrinfo(ai, &h_base);

    ai = h_gai("fd00::1", "53", AF_INET6);
    CHECK(ai != NULL && ai->ai_family == AF_INET6 &&
              ((UBYTE *)ai->ai_addr)[1] == AF_INET6,
          "hints 28 answer in 28 whatever the opener");
    bsd_freeaddrinfo(ai, &h_base);

    ai = h_gai("192.168.1.1", "53", AF_UNSPEC);
    CHECK(ai != NULL && ai->ai_family == AF_INET &&
              ((UBYTE *)ai->ai_addr)[1] == AF_INET,
          "IPv4 results are untouched after the flip");
    bsd_freeaddrinfo(ai, &h_base);
}

/*
 * The pass-through pattern: getaddrinfo(AF_UNSPEC), then socket() and
 * connect() with exactly what came back, before and after the flip.
 */
static void case_pass_through(void)
{
    struct addrinfo *ai;
    LONG             fd;
    int              round;

    for (round = 0; round < 2; round++)
    {
        h_reset();
        if (round == 1)
            h_base.sb_Fam23 = 1;

        ai = h_gai("fd00::1", "53", AF_UNSPEC);
        CHECK(ai != NULL, "getaddrinfo(AF_UNSPEC) answers");
        if (ai == NULL)
            continue;

        fd = bsd_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol,
                        &h_base);
        CHECK(fd >= 0, round ? "socket(ai_family 23) after the flip"
                             : "socket(ai_family 28) before the flip");
        CHECK(fd >= 0 &&
                  bsd_connect(fd, ai->ai_addr, ai->ai_addrlen, &h_base) == 0 &&
                  h_is_peer(&h_table[fd]->as_PeerAddr) &&
                  h_table[fd]->as_PeerPort == 53,
              round ? "connect(ai_addr) in 23 reaches fd00::1 port 53"
                    : "connect(ai_addr) in 28 reaches fd00::1 port 53");
        bsd_freeaddrinfo(ai, &h_base);
    }
}

static void case_getnameinfo(void)
{
    UBYTE sa[28];
    char  host[64];
    char  serv[16];

    h_reset();
    h_sa23(sa, F23, 0);
    sa[27] = 0;
    CHECK(bsd_getnameinfo((struct sockaddr *)sa, 28, (STRPTR)host,
                          sizeof(host), (STRPTR)serv, sizeof(serv),
                          NI_NUMERICHOST | NI_NUMERICSERV, &h_base) == 0 &&
              strcmp(host, "fd00::1") == 0 && strcmp(serv, "53") == 0,
          "getnameinfo() on the 23 layout");
    CHECK(h_base.sb_Fam23 != 0, "getnameinfo() on 23 flips the opener");
}

static void case_inet(void)
{
    UBYTE want[16];
    UBYTE out[16];
    char  text[64];

    h_reset();
    bsd_words_to_in6(h_peer, want);
    CHECK(bsd_inet_pton(F23, (STRPTR)"fd00::1", out, &h_base) == 1 &&
              memcmp(out, want, 16) == 0,
          "inet_pton(23)");
    CHECK(h_base.sb_Fam23 != 0, "inet_pton(23) flips the opener");

    h_reset();
    CHECK(bsd_inet_ntop(F23, want, (STRPTR)text, sizeof(text), &h_base) != NULL &&
              strcmp(text, "fd00::1") == 0,
          "inet_ntop(23)");
    CHECK(h_base.sb_Fam23 != 0, "inet_ntop(23) flips the opener");
}

/* ReleaseSocket() by one spelling, ObtainSocket() by the other. */
static void case_handoff(void)
{
    LONG       fd;
    LONG       id;
    AmiSocket *s;

    h_reset();
    fd = bsd_socket(F23, SOCK_DGRAM, 0, &h_base);
    s  = (fd >= 0) ? h_table[fd] : NULL;
    CHECK(s != NULL, "socket(23) for the handoff");
    if (s == NULL)
        return;

    id = bsd_ReleaseSocket(fd, -1, &h_base);
    fd = bsd_ObtainSocket(id, AF_INET6, SOCK_DGRAM, 0, &h_other);
    CHECK(fd >= 0 && h_table2[fd] == s, "a 23 socket is obtained as 28");
    CHECK((s->as_Flags & (ASF_INET6 | ASF_FAM23)) == ASF_INET6,
          "and answers its new holder in 28");

    id = bsd_ReleaseSocket(fd, -1, &h_other);
    fd = bsd_ObtainSocket(id, F23, SOCK_DGRAM, 0, &h_other);
    CHECK(fd >= 0 && h_table2[fd] == s, "a 28 socket is obtained as 23");
    CHECK((s->as_Flags & ASF_FAM23) != 0 && h_other.sb_Fam23 != 0,
          "and answers its new holder in 23");
    h_table[0] = NULL;
}

int main(void)
{
    case_sa_family();
    case_reader();
    case_writer();
    case_socket_api();
    case_getaddrinfo();
    case_pass_through();
    case_getnameinfo();
    case_inet();
    case_handoff();
    h_reset();

    printf("RESULT af_inet6_23 checks=%lu failures=%lu\n", h_checks,
           h_failures);
    return (h_failures == 0) ? 0 : 1;
}

/* One task here, so nothing to keep out. */
VOID Forbid(VOID) { }
VOID Permit(VOID) { }

#include "socket.c"
#include "cmsg.c"
#include "addrinfo.c"
#include "inet.c"
#include "handoff.c"
