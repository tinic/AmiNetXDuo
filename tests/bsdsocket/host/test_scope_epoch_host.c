/*
 * Stored IPv6 zones against interface-slot reuse (#51).
 *
 * A zone is slot+1, stored at connect(), bind() and setsockopt(IPV6_PKTINFO).
 * After RemoveNetInterface k and AddNetInterface into the same slot, the
 * number names a different link.  The stored zone must read as gone, as the
 * multicast rows do (test_mcast_epoch_host.c).
 *
 * socket.c and cmsg.c are #included; --gc-sections keeps only what the
 * cases reach, so the stubs below are the whole closure.
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

#define H_FDS  2
#define ZONE   2UL      /* slot 1 */

static struct AmiSocketBase h_base;
static AmiSocket            h_sock[H_FDS];
static AmiSocket           *h_table[H_FDS];
static NX_IP                h_ip;
static ULONG                h_epoch[4];

static const ULONG h_ll_local[4] = { 0xFE800000UL, 0, 0, 5 };
static const ULONG h_ll_peer[4]  = { 0xFE800000UL, 0, 0, 1 };

static LONG bsd_connect_locked(struct AmiSocketBase *SocketBase,
                               AmiSocket *sock, const NXD_ADDRESS *addr,
                               UINT port, ULONG scope);

/* ---- the externs the cases reach ----------------------------------------- */

ULONG netstack_interface_epoch(UWORD index)
{
    return (index < 4) ? h_epoch[index] : 0;
}

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;
    return -1;
}

BOOL bsd_netmon_have(LONG type) { (VOID)type; return FALSE; }
STRPTR bsd_netmon_caller(struct AmiSocketBase *base) { (VOID)base; return NULL; }
LONG bsd_netmon_dispatch(LONG type, APTR message)
{
    (VOID)type; (VOID)message;
    return 0;
}

VOID bsd_bzero(APTR p, ULONG size) { memset(p, 0, size); }
VOID bsd_bcopy(CONST_APTR src, APTR dst, ULONG size) { memcpy(dst, src, size); }
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

/* fe80::/10 is link scope 2; everything else here is global. */
UINT anx6_scope(const ULONG *addr)
{
    return ((addr[0] & 0xFFC00000UL) == 0xFE800000UL) ? 2U : 0xEU;
}

/* ipv6_srcsel.c: any valid address attached to the required interface. */
BOOL netstack_ipv6_source_find(const ULONG dest[4], LONG required,
                               ULONG chosen[4], UINT *index)
{
    UINT i;

    (VOID)dest; (VOID)chosen;
    for (i = 0; i < (UINT)NX_MAX_IPV6_ADDRESSES; i++)
    {
        const NXD_IPV6_ADDRESS *a = &h_ip.nx_ipv6_address[i];

        if (a->nxd_ipv6_address_valid == 0 ||
            (required >= 0 &&
             a->nxd_ipv6_address_attached != &h_ip.nx_ip_interface[required]))
            continue;
        *index = (UINT)a->nxd_ipv6_address_index;
        return TRUE;
    }
    return FALSE;
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

/* connect()'s TCP arm and wait loop: linked, never reached. */
UINT _nxde_tcp_client_socket_connect(NX_TCP_SOCKET *s, NXD_ADDRESS *ip,
                                     UINT port, ULONG wait)
{ (VOID)s; (VOID)ip; (VOID)port; (VOID)wait; abort(); }
UINT _nxde_tcp_client_socket_source_connect(NX_TCP_SOCKET *s, NXD_ADDRESS *ip,
                                            UINT port, UINT index, ULONG wait)
{ (VOID)s; (VOID)ip; (VOID)port; (VOID)index; (VOID)wait; abort(); }
UINT _nxe_tcp_client_socket_bind(NX_TCP_SOCKET *s, UINT port, ULONG wait)
{ (VOID)s; (VOID)port; (VOID)wait; abort(); }
UINT _nxe_tcp_client_socket_port_get(NX_TCP_SOCKET *s, UINT *port)
{ (VOID)s; (VOID)port; abort(); }
UINT _nxe_tcp_socket_disconnect(NX_TCP_SOCKET *s, ULONG wait)
{ (VOID)s; (VOID)wait; abort(); }
UINT _nxe_tcp_socket_state_wait(NX_TCP_SOCKET *s, UINT state, ULONG wait)
{ (VOID)s; (VOID)state; (VOID)wait; abort(); }
UINT _nxe_packet_release(NX_PACKET **p) { (VOID)p; abort(); }
ULONG ami_millis(VOID) { return 0; }
UINT bsd_wait_sliced(struct AmiSocketBase *base, ULONG wait,
                     BsdSlicedCall call, VOID *arg, BOOL *aborted)
{ (VOID)base; (VOID)wait; (VOID)call; (VOID)arg; (VOID)aborted; abort(); }
BOOL netstack_ipv4_route(ULONG destination, LONG preferred_index,
                         UWORD *index_out, ULONG *next_hop_out,
                         ULONG *source_address_out)
{
    (VOID)destination; (VOID)preferred_index; (VOID)index_out;
    (VOID)next_hop_out; (VOID)source_address_out;
    abort();
}

/* ---- fixtures -------------------------------------------------------------- */

static void h_reset(void)
{
    NXD_IPV6_ADDRESS *a = &h_ip.nx_ipv6_address[0];

    memset(&h_base, 0, sizeof(h_base));
    memset(&h_sock, 0, sizeof(h_sock));
    memset(&h_table, 0, sizeof(h_table));
    memset(&h_ip, 0, sizeof(h_ip));
    memset(&h_epoch, 0, sizeof(h_epoch));

    h_base.sb_Table     = h_table;
    h_base.sb_TableSize = H_FDS;
    h_base.sb_StackRefs = 1;
    h_base.sb_StackIp   = &h_ip;

    /* Slot 1 holds a link with fe80::5. */
    h_ip.nx_ip_interface[1].nx_interface_valid = 1;
    h_ip.nx_ip_interface[1].nx_interface_index = 1;
    a->nxd_ipv6_address_valid    = 1;
    a->nxd_ipv6_address_state    = NX_IPV6_ADDR_STATE_VALID;
    a->nxd_ipv6_address_attached = &h_ip.nx_ip_interface[1];
    a->nxd_ipv6_address_index    = 0;
    memcpy(a->nxd_ipv6_address, h_ll_local, sizeof(h_ll_local));
}

/* RemoveNetInterface 1 then AddNetInterface into slot 1.  Only the epoch
   moves: the new occupant is valid and has the same kind of address. */
static void h_reuse_slot1(void)
{
    h_epoch[1]++;
}

static AmiSocket *h_udp6(LONG fd, ULONG flags)
{
    AmiSocket *s = &h_sock[fd];

    s->as_Owner = &h_base;
    s->as_Flags = ASF_UDP | ASF_INET6 | flags;
    s->as_Type  = SOCK_DGRAM;
    s->as_LocalAddr.nxd_ip_version = NX_IP_VERSION_V6;
    s->as_PeerAddr.nxd_ip_version  = NX_IP_VERSION_V6;
    h_table[fd] = s;
    return s;
}

static void h_v6(NXD_ADDRESS *a, const ULONG w[4])
{
    a->nxd_ip_version = NX_IP_VERSION_V6;
    memcpy(a->nxd_ip_address.v6, w, 4 * sizeof(ULONG));
}

/* Is a datagram that arrived by slot+1 == zone from the connected peer's
   zone?  The comparison bsd_udp_from_peer() makes (transfer.c). */
static BOOL h_from_zone(const AmiSocket *s, ULONG zone)
{
    return zone == bsd_peer_scope(s);
}

/* ---- the cases ------------------------------------------------------------- */

static void case_connect(void)
{
    NXD_ADDRESS   peer;
    AmiSocket    *s;
    UINT          index = 99;
    ULONG         k;
    BOOL          any;

    h_reset();
    s = h_udp6(0, ASF_NXBOUND | ASF_BOUND);
    h_v6(&peer, h_ll_peer);

    CHECK(bsd_connect_locked(&h_base, s, &peer, 53, ZONE) == 0,
          "connect fe80::1%2");
    CHECK(bsd_source_select(s, &peer, bsd_peer_scope(s), &index) ==
              BSD_SOURCE_INDEX && index == 0,
          "connect: live zone sends from slot 1");
    CHECK(h_from_zone(s, ZONE), "connect: live zone accepts its link");

    h_reuse_slot1();
    CHECK(bsd_source_select(s, &peer, bsd_peer_scope(s), &index) ==
              BSD_SOURCE_REFUSE,
          "connect: reused slot refuses send (EADDRNOTAVAIL)");

    any = FALSE;
    for (k = 0; k <= (ULONG)NX_MAX_IP_INTERFACES; k++)
        if (h_from_zone(s, k))
            any = TRUE;
    CHECK(!any, "receive: reused slot accepts no zone");

    /* A fresh connect to the new occupant is live again. */
    CHECK(bsd_connect_locked(&h_base, s, &peer, 53, ZONE) == 0 &&
              bsd_source_select(s, &peer, bsd_peer_scope(s), &index) ==
                  BSD_SOURCE_INDEX,
          "connect: reconnect after reuse sends");
}

static void case_bind(void)
{
    struct sockaddr_in6 sin6;
    NXD_ADDRESS         peer;
    AmiSocket          *s;
    UINT                index = 99;

    h_reset();
    s = h_udp6(0, 0);
    h_v6(&peer, h_ll_peer);

    memset(&sin6, 0, sizeof(sin6));
    bsd_words_to_in6(h_ll_local, sin6.sin6_addr.s6_addr);
    sin6.sin6_port     = 1234;
    sin6.sin6_scope_id = ZONE;
    ((UBYTE *)&sin6)[0] = AF_INET6;

    CHECK(bsd_bind(0, (struct sockaddr *)&sin6, sizeof(sin6), &h_base) == 0,
          "bind fe80::5%2");
    CHECK(bsd_source_select(s, &peer, 0UL, &index) == BSD_SOURCE_INDEX &&
              index == 0,
          "bind: live zone sends from fe80::5");
    CHECK(bsd_bind_wants_interface(s, &h_ip.nx_ip_interface[1]),
          "bind: live zone accepts its link");

    h_reuse_slot1();
    CHECK(bsd_source_select(s, &peer, 0UL, &index) == BSD_SOURCE_REFUSE,
          "bind: reused slot refuses send (EADDRNOTAVAIL)");
    CHECK(!bsd_bind_wants_interface(s, &h_ip.nx_ip_interface[1]),
          "bind: reused slot accepts no connection");
}

static LONG h_sticky(AmiSocket *s, ULONG ifindex)
{
    struct in6_pktinfo info;
    socklen_t          len = (socklen_t)sizeof(info);

    memset(&info, 0, sizeof(info));
    info.ipi6_ifindex = ifindex;
    return bsd_cmsg_option(&h_base, s, IPPROTO_IPV6, IPV6_PKTINFO, &info,
                           &len, TRUE);
}

static LONG h_send_index(AmiSocket *s)
{
    struct msghdr msg;
    BsdCmsgSource src;

    memset(&msg, 0, sizeof(msg));
    if (bsd_cmsg_parse(&h_base, s, &msg, &src) != 0)
        return -2;
    return bsd_cmsg_source_index(&h_ip, &src, TRUE);
}

static void case_sticky(void)
{
    AmiSocket *s;

    h_reset();
    s = h_udp6(0, 0);

    CHECK(h_sticky(s, ZONE) == 0, "setsockopt IPV6_PKTINFO{ifindex=2}");
    CHECK(h_send_index(s) == 0, "sticky: live ifindex sends from slot 1");

    h_reuse_slot1();
    CHECK(h_send_index(s) == -1,
          "sticky: reused slot refuses send (EADDRNOTAVAIL)");

    CHECK(h_sticky(s, ZONE) == 0 && h_send_index(s) == 0,
          "sticky: set again after reuse sends");
}

static void case_pktinfo4(void)
{
    BsdCmsgSource src;

    h_reset();
    memset(&src, 0, sizeof(src));
    src.cs_Have    = TRUE;
    src.cs_Ifindex = ZONE;

    CHECK(bsd_cmsg_source_index(&h_ip, &src, FALSE) == 1,
          "IP_PKTINFO: live ifindex is slot 1");

    h_ip.nx_ip_interface[1].nx_interface_valid = 0;
    CHECK(bsd_cmsg_source_index(&h_ip, &src, FALSE) == -1,
          "IP_PKTINFO: empty slot refused (EADDRNOTAVAIL)");
}

int main(void)
{
    case_connect();
    case_bind();
    case_sticky();
    case_pktinfo4();

    printf("scope epoch: %lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}

#include "socket.c"
#include "cmsg.c"
