/*
 * src/bsdsocket/socket.c:bsd_bind() on the host, the #38 port-sharing opt-in.
 *
 * WHAT THIS PROVES AND WHAT IT DOES NOT.  bsd_bind() is the only place the
 * BSD layer decides whether a UDP socket opts into NetX Duo port sharing; the
 * decision is a single expression that sets as_Nx.udp.nx_udp_socket_share
 * from the SO_REUSEPORT/SO_REUSEADDR flags, the port, and the bound address.
 * Sharing is wildcard-only: a socket bound to INADDR_ANY (or ::) with an
 * explicit port opts in; a socket bound to a specific local address --
 * loopback, a multicast group, or one of the host's own addresses -- still
 * binds successfully but never opts in, because NetX binds UDP by port alone
 * and has no per-socket address/group filter, so fanning a multicast datagram
 * to such a co-bound sharer would over-deliver to a socket that never joined
 * the group.
 *
 * The fan-out itself is NetX Duo's and is under test in
 * tests/netstack/host/test_udp_share_host.c.  Here the two _nxe_udp_socket_bind
 * / port_get stubs reproduce NetX's co-bind rule against the flag bsd_bind set,
 * so the consequence of the flag -- a second non-sharing bind of a held port
 * fails EADDRINUSE -- is asserted end to end without linking NetX.
 *
 * socket.c is #included rather than linked, and -ffunction-sections plus the
 * linker's --gc-sections drop every LVO vector but bsd_bind and its callees,
 * so only the dozen externs below need stubs.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

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
            printf("  FAIL %s\n", (what));                                    \
        }                                                                     \
    } while (0)

#define H_FDS   2
#define PORT     5353

static struct AmiSocketBase h_base;
static AmiSocket            h_sock[H_FDS];
static AmiSocket           *h_table[H_FDS];
static NX_IP                h_ip;

/* The co-bind model: NetX's rule, against the flag bsd_bind just set. */
static ULONG h_last_port;
static ULONG h_bound_port;
static BOOL  h_bound_shared;

static void h_reset(void)
{
    memset(&h_base, 0, sizeof(h_base));
    memset(&h_sock, 0, sizeof(h_sock));
    memset(&h_table, 0, sizeof(h_table));
    memset(&h_ip, 0, sizeof(h_ip));

    h_base.sb_Table     = h_table;
    h_base.sb_TableSize = H_FDS;
    h_base.sb_StackRefs = 1;
    h_base.sb_StackIp   = &h_ip;

    h_last_port    = 0;
    h_bound_port   = 0;
    h_bound_shared = FALSE;
}

static AmiSocket *h_udp(LONG fd, ULONG flags)
{
    AmiSocket *s = &h_sock[fd];

    s->as_Owner = &h_base;
    s->as_Flags = ASF_UDP | flags;
    s->as_Type  = SOCK_DGRAM;
    h_table[fd] = s;
    return s;
}

/* One local address on interface 0, so bsd_bind_kind can call it SPECIFIC. */
static void h_iface(ULONG addr)
{
    h_ip.nx_ip_interface[0].nx_interface_valid      = 1;
    h_ip.nx_ip_interface[0].nx_interface_ip_address = addr;
}

/* A sockaddr in the Amiga's byte order: sin_len at 0, sin_family at 1.
   The host's struct puts sin_family first, and bsd_sa_family() reads b[1],
   so the family byte is planted where the shipping code looks for it. */
static void h_sin(struct sockaddr_in *sin, ULONG addr, UWORD port)
{
    UBYTE *b = (UBYTE *)sin;

    memset(sin, 0, sizeof(*sin));
    b[0] = (UBYTE)sizeof(*sin);   /* sin_len */
    b[1] = AF_INET;               /* sin_family, the Amiga's byte 1 */
    sin->sin_port        = port;  /* offset 2, value kept through BSD_NTOHS */
    sin->sin_addr.s_addr = addr;  /* offset 4, value kept through BSD_NTOHL */
}

/* ---- the externs bsd_bind's closure reaches ------------------------------ */

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;
    return -1;
}

BOOL bsd_netmon_have(LONG type) { (VOID)type; return FALSE; }

STRPTR bsd_netmon_caller(struct AmiSocketBase *base)
{
    return (base != NULL) ? (STRPTR)base->sb_LogTag : NULL;
}

LONG bsd_netmon_dispatch(LONG type, APTR message)
{
    (VOID)type; (VOID)message;
    return 0;
}

VOID bsd_bzero(APTR p, ULONG size) { memset(p, 0, size); }

LONG bsd_nx_enter(struct AmiSocketBase *base) { (VOID)base; return 0; }

VOID bsd_nx_leave(struct AmiSocketBase *base) { (VOID)base; }

VOID bsd_raw_revalidate_endpoint(AmiSocket *sock) { (VOID)sock; }

/* The three IPv6 helpers bsd_bind reaches under AMINETXDUO_IPV6, which this
   build defines to match the shipping library.  They live in other units
   (in6.c, src/ipv6/ipv6_srcsel.c) and are compiled in but never called here:
   every case binds an IPv4 address, so bsd_sockaddr_get's AF_INET6 arm and
   bsd_bind_kind's zoned-scope arm are not taken.  Each traps on entry so a
   future IPv6 case cannot pass silently against a stub. */
BOOL bsd_addr_normalise(const AmiSocket *sock, NXD_ADDRESS *addr)
{
    (VOID)sock; (VOID)addr;
    abort();
}

VOID bsd_in6_to_words(const UBYTE bytes[16], ULONG words[4])
{
    (VOID)bytes; (VOID)words;
    abort();
}

UINT anx6_scope(const ULONG *addr)
{
    (VOID)addr;
    abort();
}

LONG bsd_errno_from_nx(UINT status)
{
    /* The one status the flag's consequence produces. */
    return (status == NX_PORT_UNAVAILABLE) ? AMI_EADDRINUSE : AMI_EIO;
}

UINT _nxe_udp_socket_bind(NX_UDP_SOCKET *socket_ptr, UINT port,
                          ULONG wait_option)
{
    BOOL share;

    (VOID)wait_option;
    share = (socket_ptr->nx_udp_socket_share == NX_TRUE);
    if (port == (UINT)NX_ANY_PORT)
        port = 4096;   /* NetX's ephemeral pick, for port 0 */

    if (h_bound_port != 0 && !(h_bound_shared && share))
        return NX_PORT_UNAVAILABLE;

    h_bound_port   = port;
    h_bound_shared = share;
    h_last_port    = port;
    return NX_SUCCESS;
}

UINT _nxe_udp_socket_port_get(NX_UDP_SOCKET *socket_ptr, UINT *port_ptr)
{
    (VOID)socket_ptr;
    *port_ptr = (UINT)h_last_port;
    return NX_SUCCESS;
}

UINT _nxe_tcp_client_socket_bind(NX_TCP_SOCKET *socket_ptr, UINT port,
                                 ULONG wait_option)
{
    (VOID)socket_ptr; (VOID)port; (VOID)wait_option;
    return NX_SUCCESS;
}

UINT _nxe_tcp_client_socket_port_get(NX_TCP_SOCKET *socket_ptr, UINT *port_ptr)
{
    (VOID)socket_ptr;
    *port_ptr = PORT;
    return NX_SUCCESS;
}

/* ---- the test ------------------------------------------------------------ */

int main(void)
{
    struct sockaddr_in sin;

    /* Wildcard + SO_REUSEADDR opts in. */
    h_reset();
    {
        AmiSocket *s = h_udp(0, ASF_REUSEADDR);
        h_sin(&sin, 0x00000000UL, PORT);
        CHECK(bsd_bind(0, (struct sockaddr *)&sin, sizeof(sin), &h_base) == 0,
              "wildcard + REUSEADDR binds");
        CHECK(s->as_Nx.udp.nx_udp_socket_share == NX_TRUE,
              "wildcard + REUSEADDR sets share");
    }

    /* The same through SO_REUSEPORT. */
    h_reset();
    {
        AmiSocket *s = h_udp(0, ASF_REUSEPORT);
        h_sin(&sin, 0x00000000UL, PORT);
        CHECK(bsd_bind(0, (struct sockaddr *)&sin, sizeof(sin), &h_base) == 0,
              "wildcard + REUSEPORT binds");
        CHECK(s->as_Nx.udp.nx_udp_socket_share == NX_TRUE,
              "wildcard + REUSEPORT sets share");
    }

    /* Two wildcard sharers hold one port (the flag drives the co-bind). */
    h_reset();
    {
        AmiSocket *a = h_udp(0, ASF_REUSEADDR);
        AmiSocket *b = h_udp(1, ASF_REUSEADDR);
        h_sin(&sin, 0x00000000UL, PORT);
        CHECK(bsd_bind(0, (struct sockaddr *)&sin, sizeof(sin), &h_base) == 0,
              "first wildcard sharer binds");
        CHECK(bsd_bind(1, (struct sockaddr *)&sin, sizeof(sin), &h_base) == 0,
              "second wildcard sharer co-binds");
        CHECK(a->as_Nx.udp.nx_udp_socket_share == NX_TRUE &&
              b->as_Nx.udp.nx_udp_socket_share == NX_TRUE,
              "both wildcard sharers opted in");
    }

    /* A specific address still binds with the flag set (the regression codex
       named): no share, but no failure. */
    h_reset();
    {
        AmiSocket *s = h_udp(0, ASF_REUSEADDR);
        h_iface(0x0A000005UL);
        h_sin(&sin, 0x0A000005UL, PORT);
        CHECK(bsd_bind(0, (struct sockaddr *)&sin, sizeof(sin), &h_base) == 0,
              "specific + REUSEADDR binds (no regression)");
        CHECK(s->as_Nx.udp.nx_udp_socket_share == NX_FALSE,
              "specific + REUSEADDR does not opt in");
    }

    /* Two specific sockets on one port: the second fails EADDRINUSE. */
    h_reset();
    {
        h_udp(0, ASF_REUSEADDR);
        h_udp(1, ASF_REUSEADDR);
        h_iface(0x0A000005UL);
        h_sin(&sin, 0x0A000005UL, PORT);
        CHECK(bsd_bind(0, (struct sockaddr *)&sin, sizeof(sin), &h_base) == 0,
              "first specific bind succeeds");
        CHECK(bsd_bind(1, (struct sockaddr *)&sin, sizeof(sin), &h_base) == -1,
              "second specific co-bind refused");
        CHECK(h_base.sb_Errno == AMI_EADDRINUSE,
              "second specific co-bind is EADDRINUSE");
    }

    /* Loopback is a specific address: binds, but never opts in. */
    h_reset();
    {
        AmiSocket *s = h_udp(0, ASF_REUSEADDR);
        h_sin(&sin, 0x7F000001UL, PORT);
        CHECK(bsd_bind(0, (struct sockaddr *)&sin, sizeof(sin), &h_base) == 0,
              "loopback + REUSEADDR binds");
        CHECK(s->as_Nx.udp.nx_udp_socket_share == NX_FALSE,
              "loopback + REUSEADDR does not opt in");
    }

    /* A multicast group address is specific: binds, never opts in. */
    h_reset();
    {
        AmiSocket *s = h_udp(0, ASF_REUSEPORT);
        h_sin(&sin, 0xEFFFFFFAUL, PORT);   /* 239.255.255.250 */
        CHECK(bsd_bind(0, (struct sockaddr *)&sin, sizeof(sin), &h_base) == 0,
              "group + REUSEPORT binds");
        CHECK(s->as_Nx.udp.nx_udp_socket_share == NX_FALSE,
              "group + REUSEPORT does not opt in");
    }

    /* Ephemeral (port 0) is never shared. */
    h_reset();
    {
        AmiSocket *s = h_udp(0, ASF_REUSEADDR);
        h_sin(&sin, 0x00000000UL, 0);
        CHECK(bsd_bind(0, (struct sockaddr *)&sin, sizeof(sin), &h_base) == 0,
              "wildcard ephemeral binds");
        CHECK(s->as_Nx.udp.nx_udp_socket_share == NX_FALSE,
              "ephemeral never opts in");
    }

    /* Without either flag, nothing opts in. */
    h_reset();
    {
        AmiSocket *s = h_udp(0, 0);
        h_sin(&sin, 0x00000000UL, PORT);
        CHECK(bsd_bind(0, (struct sockaddr *)&sin, sizeof(sin), &h_base) == 0,
              "wildcard without reuse binds");
        CHECK(s->as_Nx.udp.nx_udp_socket_share == NX_FALSE,
              "no reuse flag, no share");
    }

    printf("bind_share: %lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}

/* socket.c's stored-zone checks (#51); no slot is ever reused here. */
ULONG netstack_interface_epoch(UWORD index) { (VOID)index; return 0; }

#include "socket.c"

/* The scope checks in socket.c (#51) ask for a slot's epoch; no case here
   binds a scoped address. */
ULONG netstack_interface_epoch(UWORD index)
{
    (VOID)index;
    return 0UL;
}
