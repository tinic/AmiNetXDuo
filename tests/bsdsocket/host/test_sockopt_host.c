/*
 * src/bsdsocket/options.c on the host: setsockopt, getsockopt and the two
 * ioctls, at the SOL_SOCKET and IPPROTO_TCP levels.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "interfaces.h"

/* TCP_USER_TIMEOUT.  host_prelude.h undefines glibc's, which is 18 where the
   Amiga's is 0x1001; this is the header options.c takes it from too. */
#include "aminetxduo/tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* socket.c's, which options.c now reads the stored zone through (#51); no
   case here stores a scope id. */
ULONG bsd_scope_live(ULONG scope, ULONG epoch)
{
    (VOID)epoch;
    return scope;
}

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
#define H_RATE  ((ULONG)NX_IP_PERIODIC_RATE)

static struct AmiSocketBase h_base;
static AmiSocket            h_sock[H_FDS];
static AmiSocket           *h_table[H_FDS];

static struct
{
    LONG   nx_enter_result;
    ULONG  nx_enters;
    ULONG  nx_leaves;
    ULONG  tcp_reuse;           /* last reuse_address_set value, +1; 0 = none */
    ULONG  delegated;           /* calls that left options.c for another file */
    LONG   delegate_result;
    ULONG  raw_available;
    ULONG  udp_available;
    ULONG  packet_length;
} h;

/* Forbid() depth, and how many times it fell back to 0: a lookup and a
   retain with no fall between them were one atomic step. */
static ULONG h_forbid;
static ULONG h_permitted;
static ULONG h_lookup_at;           /* h_permitted at the last lookup, +1 */
static ULONG h_split;               /* retains not in their lookup's Forbid */

static char h_cb[16];
static char h_veto[4];

/* Dup2Socket's fixtures: a stand-in descriptor callback, and the socket the
   last release freed. */
static LONG     (*h_fd_hook)(LONG fd);
static AmiSocket *h_freed;
static ULONG      h_after_free;
AmiSocket        *bsd_defer_head;

/* Match socket.c's deferred-release bookkeeping when the NetX bracket is
   unavailable; options.c now uses this on a refused Dup2Socket unwind. */
VOID bsd_socket_defer(AmiSocket *sock)
{
    if (sock->as_DeferRefs++ == 0)
    {
        sock->as_DeferNext = bsd_defer_head;
        bsd_defer_head = sock;
    }
}

static void h_reset(void)
{
    memset(&h_base, 0, sizeof(h_base));
    memset(&h_sock, 0, sizeof(h_sock));
    memset(&h_table, 0, sizeof(h_table));
    memset(&h, 0, sizeof(h));

    h_base.sb_Table     = h_table;
    h_base.sb_TableSize = H_FDS;
    h_fd_hook    = NULL;
    h_cb[0]      = '\0';
    h_veto[0]    = '\0';
    h_forbid     = 0;
    h_split      = 0;
    h_freed      = NULL;
    h_after_free = 0;
    bsd_defer_head = NULL;
}

static AmiSocket *h_tcp(LONG fd)
{
    AmiSocket *s = &h_sock[fd];

    s->as_Owner = &h_base;
    s->as_Flags = ASF_TCP | ASF_CONNECTED;
    s->as_Type  = SOCK_STREAM;
    h_table[fd] = s;
    return s;
}

static AmiSocket *h_udp(LONG fd)
{
    AmiSocket *s = &h_sock[fd];

    s->as_Owner = &h_base;
    s->as_Flags = ASF_UDP;
    s->as_Type  = SOCK_DGRAM;
    h_table[fd] = s;
    return s;
}

VOID Forbid(VOID) { h_forbid++; }
VOID Permit(VOID)
{
    if (h_forbid > 0 && --h_forbid == 0)
        h_permitted++;
}
VOID Signal(struct Task *task, ULONG mask) { (VOID)task; (VOID)mask; }

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;
    return -1;
}

AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd)
{
    (VOID)base;
    h_lookup_at = (h_forbid > 0) ? h_permitted + 1 : 0;
    if (fd < 0 || fd >= H_FDS || h_table[fd] == BSD_FD_RESERVED ||
        h_table[fd] == BSD_FD_BUSY)
        return NULL;
    return h_table[fd];
}

LONG bsd_table_size(struct AmiSocketBase *base) { (VOID)base; return H_FDS; }

VOID bsd_bcopy(CONST_APTR src, APTR dst, ULONG size)
{
    memmove(dst, src, size);
}

LONG bsd_nx_enter(struct AmiSocketBase *base)
{
    (VOID)base;
    h.nx_enters++;
    return h.nx_enter_result;
}

VOID bsd_nx_leave(struct AmiSocketBase *base) { (VOID)base; h.nx_leaves++; }

#ifdef AMINETXDUO_TCP_CORK
/* cork.c is test_cork's.  What options.c owes it is the call, inside the
   bracket, with the sense of the value inverted: 0 turns the cork on.  The
   stub keeps the flag the way cork.c does, so getsockopt reads it back. */
static ULONG h_cork_sets;
static LONG  h_cork_result;

LONG bsd_cork_set(struct AmiSocketBase *base, AmiSocket *sock, BOOL on)
{
    (VOID)base;
    h_cork_sets++;
    if (h_cork_result != 0)
        return h_cork_result;
    if (on)
        sock->as_CorkFlags |= BSD_CORKF_ON;
    else
        sock->as_CorkFlags &= (UBYTE)~BSD_CORKF_ON;
    return 0;
}
#endif

LONG bsd_cmsg_option(struct AmiSocketBase *base, AmiSocket *sock, LONG level,
                     LONG optname, APTR optval, socklen_t *optlen, BOOL set)
{
    (VOID)base; (VOID)sock; (VOID)level; (VOID)optname;
    (VOID)optval; (VOID)optlen; (VOID)set;
    return 1;
}

LONG bsd_setsockopt_ipv6(struct AmiSocketBase *base, AmiSocket *sock,
                         LONG level, LONG optname, APTR optval,
                         socklen_t optlen)
{
    (VOID)base; (VOID)sock; (VOID)level; (VOID)optname;
    (VOID)optval; (VOID)optlen;
    h.delegated++;
    return h.delegate_result;
}

LONG bsd_getsockopt_ipv6(struct AmiSocketBase *base, AmiSocket *sock,
                         LONG level, LONG optname, APTR optval,
                         socklen_t *optlen)
{
    (VOID)base; (VOID)sock; (VOID)level; (VOID)optname;
    (VOID)optval; (VOID)optlen;
    h.delegated++;
    return h.delegate_result;
}

LONG bsd_mcast_setopt(struct AmiSocketBase *base, AmiSocket *sock,
                      LONG optname, APTR optval, socklen_t optlen)
{
    (VOID)base; (VOID)sock; (VOID)optname; (VOID)optval; (VOID)optlen;
    h.delegated++;
    return h.delegate_result;
}

LONG bsd_mcast_getopt(struct AmiSocketBase *base, AmiSocket *sock,
                      LONG optname, APTR optval, socklen_t *optlen)
{
    (VOID)base; (VOID)sock; (VOID)optname; (VOID)optval; (VOID)optlen;
    h.delegated++;
    return h.delegate_result;
}

LONG bsd_if_ioctl(ULONG req, APTR argp, struct AmiSocketBase *SocketBase)
{
    (VOID)req; (VOID)argp;
    h.delegated++;
    return bsd_fail(SocketBase, AMI_ENOTTY);
}

ULONG bsd_raw_available(AmiSocket *sock) { (VOID)sock; return h.raw_available; }
ULONG bsd_udp_available(const AmiSocket *sock)
{
    (VOID)sock;
    return h.udp_available;
}

VOID bsd_sockaddr_put(const AmiSocket *sock, struct sockaddr *sa,
                      socklen_t *len, const NXD_ADDRESS *addr, UINT port,
                      ULONG scope)
{
    (VOID)sock; (VOID)sa; (VOID)len; (VOID)addr; (VOID)port; (VOID)scope;
}

VOID bsd_addr_from_v4(NXD_ADDRESS *addr, ULONG v4)
{
    memset(addr, 0, sizeof(*addr));
    addr->nxd_ip_address.v4 = v4;
}

/* The descriptor table, which only Dup2Socket reaches from this file.  The
   hook stands in for an FDCB_ALLOC callback: it runs where the real one
   does, before the slot is published, and non-zero refuses the slot. */
LONG bsd_fd_alloc(struct AmiSocketBase *base, AmiSocket *sock)
{
    LONG fd;

    for (fd = 0; fd < H_FDS; fd++)
    {
        if (h_table[fd] == NULL)
        {
            if (h_fd_hook != NULL && h_fd_hook(fd) != 0)
                return bsd_fail(base, AMI_EMFILE);
            h_table[fd] = sock;
            return fd;
        }
    }

    return bsd_fail(base, AMI_EMFILE);
}

/* The descriptor callbacks the stubs below stand in for, in the order they
   ran: F = FDCB_FREE, C = FDCB_CHECK, A = FDCB_ALLOC, R = the FDCB_ALLOC that
   puts a slot back.  h_veto names the one to refuse, with H_VETO. */
#define H_VETO  77

static LONG h_callback(struct AmiSocketBase *base, char what)
{
    size_t n = strlen(h_cb);

    if (n + 1 < sizeof(h_cb))
    {
        h_cb[n] = what;
        h_cb[n + 1] = '\0';
    }
    if (strchr(h_veto, what) != NULL)
        return bsd_fail(base, H_VETO);
    return 0;
}

LONG bsd_fd_reserve(struct AmiSocketBase *base, LONG fd)
{
    if (fd < 0 || fd >= H_FDS || h_table[fd] != NULL)
        return -1;
    if (h_callback(base, 'C') != 0 || h_callback(base, 'A') != 0)
        return -1;
    h_table[fd] = BSD_FD_RESERVED;
    return fd;
}

/* socket.c's claim, settle and unclaim, as far as the slot and the
   callbacks go.  The hook runs inside the claim, where another task or a
   callback could act. */
LONG bsd_fd_claim(struct AmiSocketBase *base, LONG fd, AmiSocket **prev)
{
    AmiSocket *entry;

    if (fd < 0 || fd >= H_FDS)
        return bsd_fail(base, AMI_EBADF);
    entry = h_table[fd];
    if (entry == BSD_FD_BUSY)
        return bsd_fail(base, AMI_EBUSY);
    h_table[fd] = BSD_FD_BUSY;
    *prev = entry;
    if (entry != NULL && h_callback(base, 'F') != 0)
    {
        h_table[fd] = entry;
        return -1;
    }
    return 0;
}

LONG bsd_fd_settle(struct AmiSocketBase *base, LONG fd, AmiSocket *entry)
{
    if (h_fd_hook != NULL && h_fd_hook(fd) != 0)
        return bsd_fail(base, AMI_EMFILE);
    if (h_callback(base, 'C') != 0 || h_callback(base, 'A') != 0)
        return -1;
    h_table[fd] = entry;
    return 0;
}

LONG bsd_fd_unclaim(struct AmiSocketBase *base, LONG fd, AmiSocket *prev)
{
    if (prev != NULL && h_callback(base, 'R') != 0)
    {
        h_table[fd] = NULL;
        return -1;
    }
    h_table[fd] = prev;
    return 0;
}

BOOL bsd_fd_reserved(struct AmiSocketBase *base, LONG fd)
{
    (VOID)base;
    return (BOOL)(fd >= 0 && fd < H_FDS && h_table[fd] == BSD_FD_RESERVED);
}

LONG bsd_fd_free(struct AmiSocketBase *base, LONG fd)
{
    if (fd >= 0 && fd < H_FDS && h_table[fd] != NULL)
    {
        if (h_callback(base, 'F') != 0)
            return -1;
        h_table[fd] = NULL;
    }
    return 0;
}


/* socket.c's, as far as a reference goes: the last release frees the socket,
   and any other drops the owner once this base holds no descriptor for it
   (bsd_owner_drop, with no other opener to elect). */
VOID bsd_socket_retain(AmiSocket *sock)
{
    if (sock == h_freed)
        h_after_free++;
    if (h_forbid == 0 || h_lookup_at != h_permitted + 1)
        h_split++;
    sock->as_RefCount++;
}

VOID bsd_socket_release(struct AmiSocketBase *base, AmiSocket *sock)
{
    LONG fd;

    if (sock == h_freed)
        h_after_free++;
    if (sock->as_RefCount > 0)
        sock->as_RefCount--;
    if (sock->as_RefCount == 0)
    {
        h_freed = sock;
        return;
    }
    if (sock->as_Owner != base)
        return;
    for (fd = 0; fd < H_FDS; fd++)
    {
        if (h_table[fd] == sock)
            return;
    }
    sock->as_Owner = NULL;
}

NX_IP *netstack_ip(VOID) { return NX_NULL; }

/* getsockname() on a socket bound to the IPv6 wildcard asks these which
   source the packets would leave with.  Answering "no usable address" leaves
   the wildcard in place, which is what an unbound fixture should report. */
BOOL netstack_ipv6_source_for(const ULONG dest[4], LONG interface_index,
                              ULONG addr_out[4])
{
    (VOID)dest; (VOID)interface_index; (VOID)addr_out;
    return FALSE;
}

UINT anx6_scope(const ULONG *addr) { (VOID)addr; return 0; }

UINT _nxe_packet_length_get(NX_PACKET *packet_ptr, ULONG *length)
{
    (VOID)packet_ptr;
    *length = h.packet_length;
    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_bytes_available(NX_TCP_SOCKET *socket_ptr,
                                     ULONG *bytes_available)
{
    (VOID)socket_ptr;
    *bytes_available = 0;
    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_mss_get(NX_TCP_SOCKET *socket_ptr, ULONG *mss)
{
    (VOID)socket_ptr;
    *mss = 1460;
    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_mss_set(NX_TCP_SOCKET *socket_ptr, ULONG mss)
{
    (VOID)socket_ptr; (VOID)mss;
    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_receive_queue_max_set(NX_TCP_SOCKET *socket_ptr,
                                           UINT receive_queue_maximum)
{
    (VOID)socket_ptr; (VOID)receive_queue_maximum;
    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_reuse_address_set(NX_TCP_SOCKET *socket_ptr, UINT reuse)
{
    (VOID)socket_ptr;
    h.tcp_reuse = (ULONG)reuse + 1UL;
    return NX_SUCCESS;
}

BOOL netstack_ipv4_route(ULONG destination, LONG preferred_index,
                         UWORD *index_out, ULONG *next_hop_out,
                         ULONG *source_address_out)
{
    (VOID)destination;
    (VOID)preferred_index;
    (VOID)index_out;
    (VOID)next_hop_out;
    (VOID)source_address_out;
    return FALSE;
}

static void t_timeouts(void)
{
    AmiSocket     *s;
    struct timeval tv;
    socklen_t      len;
    LONG           rc;

    printf("SO_RCVTIMEO / SO_SNDTIMEO: struct timeval both ways\n");

    h_reset();
    s = h_tcp(0);
    tv.tv_secs = 1; tv.tv_micro = 0;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv), &h_base);
    CHECK(rc == 0 && s->as_RcvTimeout == H_RATE,
          "one second is one second's worth of ticks");

    /* Rounded up: the socket reads 0 as "no timeout", so the shortest timeout
       a caller can name must not become no timeout at all. */
    h_reset();
    s = h_tcp(0);
    tv.tv_secs = 0; tv.tv_micro = 1;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv), &h_base);
    CHECK(rc == 0 && s->as_RcvTimeout == 1,
          "one microsecond rounds up to one tick, not to none");

    h_reset();
    s = h_tcp(0);
    tv.tv_secs = 0; tv.tv_micro = 30000;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv), &h_base);
    CHECK(rc == 0 && s->as_RcvTimeout * (1000000UL / H_RATE) >= 30000UL,
          "a timeout between two ticks waits the longer time, never the shorter");

    h_reset();
    s = h_tcp(0);
    s->as_RcvTimeout = 99;
    tv.tv_secs = 0; tv.tv_micro = 0;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv), &h_base);
    CHECK(rc == 0 && s->as_RcvTimeout == 0, "a zero timeout clears the timeout");

    h_reset();
    s = h_tcp(0);
    tv.tv_secs = 0; tv.tv_micro = 1000000;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv), &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EINVAL,
          "a microsecond count of 1000000 is EINVAL");

    /* The fields are unsigned; the ABI value with the sign bit set is what
       makes this negative, and it is refused rather than becoming 497 days. */
    h_reset();
    s = h_tcp(0);
    tv.tv_secs = 0x80000000UL; tv.tv_micro = 0;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv), &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EINVAL,
          "a negative number of seconds is EINVAL");

    h_reset();
    s = h_tcp(0);
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv,
                        (socklen_t)(sizeof(tv) - 1), &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EINVAL,
          "a short option is EINVAL, not a partial read");

    h_reset();
    s = h_tcp(0);
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, NULL, sizeof(tv), &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EINVAL, "and a NULL one as well");

    /* The two are separate fields.  Setting one used to be the sort of thing
       a merge could point at the other. */
    h_reset();
    s = h_tcp(0);
    tv.tv_secs = 3; tv.tv_micro = 0;
    (VOID)bsd_setsockopt(0, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv), &h_base);
    CHECK(s->as_SndTimeout == 3 * H_RATE && s->as_RcvTimeout == 0,
          "SO_SNDTIMEO sets the send timeout and only that");

    h_reset();
    s = h_tcp(0);
    s->as_RcvTimeout = 2 * H_RATE;
    memset(&tv, 0xEE, sizeof(tv));
    len = (socklen_t)sizeof(tv);
    rc = bsd_getsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, &len, &h_base);
    CHECK(rc == 0 && tv.tv_secs == 2 && tv.tv_micro == 0,
          "two seconds of ticks reads back as two seconds");
    CHECK(len == (socklen_t)sizeof(tv), "and the length is the timeval's");

    /* A sub-second value survives the round trip at the Amiga's tick rate. */
    h_reset();
    s = h_tcp(0);
    tv.tv_secs = 0; tv.tv_micro = 500000;
    (VOID)bsd_setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv), &h_base);
    memset(&tv, 0xEE, sizeof(tv));
    len = (socklen_t)sizeof(tv);
    (VOID)bsd_getsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, &len, &h_base);
    CHECK(tv.tv_secs == 0 && tv.tv_micro == 500000,
          "half a second survives the round trip");

    h_reset();
    s = h_tcp(0);
    len = (socklen_t)(sizeof(tv) - 1);
    rc = bsd_getsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, &len, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EINVAL,
          "a short buffer is EINVAL on the way out too");
}

static void t_paired_setters(void)
{
    static const LONG time_options[] = { SO_RCVTIMEO, SO_SNDTIMEO };
    static const LONG ip_options[] = { IP_TTL, IP_TOS };
    static const LONG ip_values[] = { -2, -1, 0, 1, 255, 256 };
    unsigned i, j, protocol;

    printf("paired option setters: field selection and refusal ordering\n");
    for (i = 0; i < 2; i++)
    {
        AmiSocket *s;
        struct timeval tv;
        UBYTE bytes[sizeof(tv) + 1];
        LONG rc;

        h_reset();
        s = h_tcp(0);
        s->as_RcvTimeout = 11;
        s->as_SndTimeout = 17;
        tv.tv_secs = 2; tv.tv_micro = 0;
        memcpy(bytes + 1, &tv, sizeof(tv));
        rc = bsd_setsockopt(0, SOL_SOCKET, time_options[i], bytes + 1,
                            sizeof(tv), &h_base);
        CHECK(rc == 0 && s->as_RcvTimeout == (i == 0 ? 2 * H_RATE : 11) &&
              s->as_SndTimeout == (i == 1 ? 2 * H_RATE : 17),
              "unaligned timeval updates only the selected timeout");
        tv.tv_micro = 1000000;
        rc = bsd_setsockopt(0, SOL_SOCKET, time_options[i], &tv,
                            sizeof(tv), &h_base);
        CHECK(rc == -1 && h_base.sb_Errno == AMI_EINVAL &&
              s->as_RcvTimeout == (i == 0 ? 2 * H_RATE : 11) &&
              s->as_SndTimeout == (i == 1 ? 2 * H_RATE : 17),
              "invalid timeval preserves both timeout fields");
        rc = bsd_setsockopt(0, SOL_SOCKET, time_options[i], NULL,
                            sizeof(tv), &h_base);
        CHECK(rc == -1 && h_base.sb_Errno == AMI_EINVAL,
              "both timeout setters reject NULL with EINVAL");
        CHECK(h.nx_enters == 0 && h.nx_leaves == 0,
              "timeout setters do not enter NetX");
    }

    for (protocol = 0; protocol < 2; protocol++)
    {
        for (i = 0; i < 2; i++)
        {
            for (j = 0; j < sizeof(ip_values) / sizeof(ip_values[0]); j++)
            {
                AmiSocket *s;
                LONG value = ip_values[j];
                BOOL valid = value >= -1 && value <= 255;
                LONG ttl = i == 0 && valid ? (value < 0 ? (LONG)NX_IP_TIME_TO_LIVE : value) : 77;
                LONG tos = i == 1 && valid ? (value < 0 ? 0 : value) : 88;
                LONG rc;

                h_reset();
                s = protocol == 0 ? h_tcp(0) : h_udp(0);
                s->as_Ttl = 77;
                s->as_Tos = 88;
                rc = bsd_setsockopt(0, IPPROTO_IP, ip_options[i], &value,
                                    sizeof(value), &h_base);
                CHECK(rc == (valid ? 0 : -1), "TTL/TOS boundary verdict");
                CHECK(valid || h_base.sb_Errno == AMI_EINVAL,
                      "TTL/TOS invalid value is EINVAL");
                CHECK(s->as_Ttl == ttl && s->as_Tos == tos,
                      "TTL/TOS selects the right field and preserves rejected values");
                CHECK(h.nx_enters == (valid ? 1UL : 0UL) && h.nx_leaves == h.nx_enters,
                      "TTL/TOS brackets only accepted values");
                if (valid)
                {
                    CHECK(protocol == 0
                              ? s->as_Nx.tcp.nx_tcp_socket_time_to_live == (UINT)ttl &&
                                s->as_Nx.tcp.nx_tcp_socket_type_of_service == ((ULONG)tos << 16)
                              : s->as_Nx.udp.nx_udp_socket_time_to_live == (UINT)ttl &&
                                s->as_Nx.udp.nx_udp_socket_type_of_service == ((ULONG)tos << 16),
                          "TTL/TOS updates the selected live protocol");
                }
            }
        }
    }
    for (i = 0; i < 2; i++)
    {
        AmiSocket *s;
        LONG value = 3;
        LONG rc;

        h_reset();
        s = h_tcp(0);
        s->as_Ttl = 77;
        s->as_Tos = 88;
        s->as_Nx.tcp.nx_tcp_socket_time_to_live = 99;
        h.nx_enter_result = -1;
        rc = bsd_setsockopt(0, IPPROTO_IP, ip_options[i], &value,
                            sizeof(value), &h_base);
        CHECK(rc == -1 && h_base.sb_Errno == AMI_ENETDOWN,
              "TTL/TOS bracket refusal is ENETDOWN");
        CHECK(s->as_Ttl == (i == 0 ? 3 : 77) && s->as_Tos == (i == 1 ? 3 : 88),
              "TTL/TOS keeps original local assignment before bracket refusal");
        CHECK(s->as_Nx.tcp.nx_tcp_socket_time_to_live == 99 &&
              h.nx_enters == 1 && h.nx_leaves == 0,
              "TTL/TOS does not mutate live state or leave a refused bracket");
    }
}

static void t_so_error(void)
{
    AmiSocket *s;
    LONG       value;
    socklen_t  len;
    LONG       rc;

    printf("SO_ERROR: read and clear, in that order\n");

    h_reset();
    s = h_tcp(0);
    s->as_SoError = 111;
    value = 0;
    len = (socklen_t)sizeof(value);
    rc = bsd_getsockopt(0, SOL_SOCKET, SO_ERROR, &value, &len, &h_base);
    CHECK(rc == 0 && value == 111, "the pending error is reported");
    CHECK(s->as_SoError == 0, "and cleared by the read");

    h_reset();
    s = h_tcp(0);
    s->as_SoError = 111;
    len = (socklen_t)sizeof(value);
    rc = bsd_getsockopt(0, SOL_SOCKET, SO_ERROR, NULL, &len, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EFAULT, "a NULL buffer is EFAULT");
    CHECK(s->as_SoError == 111,
          "and a refused read does not consume the pending error");

    /* On UDP, NetX holds a second copy for its next receive.  Both go, or one
       ICMP message is reported twice. */
    h_reset();
    s = h_udp(0);
    s->as_SoError = 111;
    s->as_Nx.udp.nx_udp_socket_icmp_error = 42;
    len = (socklen_t)sizeof(value);
    (VOID)bsd_getsockopt(0, SOL_SOCKET, SO_ERROR, &value, &len, &h_base);
    CHECK(s->as_Nx.udp.nx_udp_socket_icmp_error == NX_SUCCESS,
          "and NetX Duo's own copy is cleared with it");
}

static void t_linger(void)
{
    AmiSocket    *s;
    struct linger lin;
    socklen_t     len;
    LONG          rc;

    printf("SO_LINGER: the bound that keeps CloseSocket() returning\n");

    h_reset();
    s = h_tcp(0);
    lin.l_onoff = 1;
    lin.l_linger = 5;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin), &h_base);
    CHECK(rc == 0 && s->as_LingerOn == 1 && s->as_LingerTime == 5,
          "a linger inside the bound is stored");

    memset(&lin, 0, sizeof(lin));
    len = (socklen_t)sizeof(lin);
    rc = bsd_getsockopt(0, SOL_SOCKET, SO_LINGER, &lin, &len, &h_base);
    CHECK(rc == 0 && lin.l_onoff == 1 && lin.l_linger == 5,
          "and read back unchanged");

    /* bsd_socket_close() turns l_linger into a tick count, so a negative one
       becomes about 497 days and CloseSocket() never returns. */
    h_reset();
    s = h_tcp(0);
    lin.l_onoff = 1;
    lin.l_linger = -1;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin), &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EINVAL,
          "a negative linger is EINVAL");

    h_reset();
    s = h_tcp(0);
    lin.l_onoff = 1;
    lin.l_linger = (LONG)(32767L / (LONG)H_RATE) + 1;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin), &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EINVAL,
          "and one past SHRT_MAX ticks is EINVAL, as 4.4BSD has it");

    h_reset();
    s = h_tcp(0);
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_LINGER, &lin,
                        (socklen_t)(sizeof(lin) - 1), &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EINVAL,
          "a short linger is EINVAL");
}

static void t_flags(void)
{
    AmiSocket *s;
    LONG       value;
    socklen_t  len;
    LONG       rc;

    printf("the flag options, and getsockopt agreeing with setsockopt\n");

    h_reset();
    s = h_tcp(0);
    value = 1;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value),
                        &h_base);
    CHECK(rc == 0 && (s->as_Flags & ASF_REUSEADDR) != 0, "SO_REUSEADDR sets");
    value = 0; len = (socklen_t)sizeof(value);
    (VOID)bsd_getsockopt(0, SOL_SOCKET, SO_REUSEADDR, &value, &len, &h_base);
    CHECK(value == 1, "and reads back");

    value = 0;
    (VOID)bsd_setsockopt(0, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value),
                         &h_base);
    CHECK((s->as_Flags & ASF_REUSEADDR) == 0, "and clears again");

    h_reset();
    s = h_udp(0);
    value = 1;
    (VOID)bsd_setsockopt(0, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value),
                         &h_base);
    CHECK((s->as_Flags & ASF_BROADCAST) != 0, "SO_BROADCAST sets");

    h_reset();
    s = h_tcp(0);
    value = 1;
    (VOID)bsd_setsockopt(0, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value),
                         &h_base);
    CHECK((s->as_Flags & ASF_KEEPALIVE) != 0, "SO_KEEPALIVE sets");

    h_reset();
    s = h_tcp(0);
    value = FD_READ | FD_CLOSE;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_EVENTMASK, &value, sizeof(value),
                        &h_base);
    CHECK(rc == 0 && s->as_EventMask == (ULONG)(FD_READ | FD_CLOSE),
          "SO_EVENTMASK stores the FD_ bits it was given");
    value = 0; len = (socklen_t)sizeof(value);
    (VOID)bsd_getsockopt(0, SOL_SOCKET, SO_EVENTMASK, &value, &len, &h_base);
    CHECK(value == (FD_READ | FD_CLOSE), "and hands the same ones back");

    /* SO_OOBINLINE always answers 1: the urgent byte is delivered in the
       stream whatever the caller set, and echoing back their 0 would hide the
       one fact they cannot discover any other way. */
    h_reset();
    s = h_tcp(0);
    value = 0;
    (VOID)bsd_setsockopt(0, SOL_SOCKET, SO_OOBINLINE, &value, sizeof(value),
                         &h_base);
    len = (socklen_t)sizeof(value);
    (VOID)bsd_getsockopt(0, SOL_SOCKET, SO_OOBINLINE, &value, &len, &h_base);
    CHECK(value == 1, "SO_OOBINLINE reports the truth, not what was set");

    h_reset();
    s = h_tcp(0);
    s->as_Flags |= ASF_LISTENING;
    value = 0; len = (socklen_t)sizeof(value);
    (VOID)bsd_getsockopt(0, SOL_SOCKET, SO_ACCEPTCONN, &value, &len, &h_base);
    CHECK(value == 1, "SO_ACCEPTCONN answers for a listening socket");
}

static void t_reuse(void)
{
    AmiSocket *s;
    LONG       value;
    socklen_t  len;
    LONG       rc;

    printf("SO_REUSEPORT / SO_REUSEADDR: sharing vs TIME-WAIT, and post-bind\n");

    /* A TCP socket that sets SO_REUSEPORT keeps the option's pre-split
       meaning: it was an alias for SO_REUSEADDR, so it still opts into
       TIME-WAIT reuse.  Both flags go on and the NetX reuse call runs inside
       the bracket. */
    h_reset();
    s = h_tcp(0);
    value = 1;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value),
                        &h_base);
    CHECK(rc == 0, "TCP SO_REUSEPORT=1 is accepted");
    CHECK((s->as_Flags & ASF_REUSEPORT) != 0, "and sets ASF_REUSEPORT");
    CHECK((s->as_Flags & ASF_REUSEADDR) != 0, "and still sets ASF_REUSEADDR");
    CHECK(h.nx_enters == 1 && h.nx_leaves == 1,
          "and the TIME-WAIT reuse call ran inside the bracket");

    value = 0; len = (socklen_t)sizeof(value);
    (VOID)bsd_getsockopt(0, SOL_SOCKET, SO_REUSEADDR, &value, &len, &h_base);
    CHECK(value == 1, "SO_REUSEADDR reads back 1 after SO_REUSEPORT");

    /* Clearing SO_REUSEPORT clears the TIME-WAIT alias too. */
    h_reset();
    s = h_tcp(0);
    value = 1;
    (VOID)bsd_setsockopt(0, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value),
                         &h_base);
    value = 0;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value),
                        &h_base);
    CHECK(rc == 0 && (s->as_Flags & ASF_REUSEADDR) == 0,
          "SO_REUSEPORT=0 clears the TIME-WAIT alias");

    /* The two are one flag on TCP, whichever is set: SO_REUSEADDR=1 reads
       back through SO_REUSEPORT too. */
    h_reset();
    s = h_tcp(0);
    value = 1;
    (VOID)bsd_setsockopt(0, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value),
                         &h_base);
    value = 0; len = (socklen_t)sizeof(value);
    (VOID)bsd_getsockopt(0, SOL_SOCKET, SO_REUSEPORT, &value, &len, &h_base);
    CHECK(value == 1 && (s->as_Flags & ASF_REUSEPORT) != 0,
          "TCP SO_REUSEADDR=1 reads back through SO_REUSEPORT");
    CHECK(h.tcp_reuse == (ULONG)NX_TRUE + 1UL, "and NetX reuse is on");

    /* And the other order: SO_REUSEPORT=1 then SO_REUSEADDR=0 leaves neither
       reading 1 while NetX's TIME-WAIT reuse is off. */
    h_reset();
    s = h_tcp(0);
    value = 1;
    (VOID)bsd_setsockopt(0, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value),
                         &h_base);
    value = 0;
    (VOID)bsd_setsockopt(0, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value),
                         &h_base);
    value = 1; len = (socklen_t)sizeof(value);
    (VOID)bsd_getsockopt(0, SOL_SOCKET, SO_REUSEPORT, &value, &len, &h_base);
    CHECK(value == 0 && (s->as_Flags & (ASF_REUSEPORT | ASF_REUSEADDR)) == 0,
          "TCP SO_REUSEPORT=1 then SO_REUSEADDR=0 reads 0 through both");
    CHECK(h.tcp_reuse == (ULONG)NX_FALSE + 1UL, "and NetX reuse is off");

    /* On a UDP socket SO_REUSEPORT is the sharing flag alone, and SO_REUSEADDR
       opts into the same sharing; neither sets the other. */
    h_reset();
    s = h_udp(0);
    value = 1;
    (VOID)bsd_setsockopt(0, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value),
                         &h_base);
    CHECK((s->as_Flags & ASF_REUSEPORT) != 0 &&
          (s->as_Flags & ASF_REUSEADDR) == 0,
          "UDP SO_REUSEPORT sets sharing, not SO_REUSEADDR");

    h_reset();
    s = h_udp(0);
    value = 1;
    (VOID)bsd_setsockopt(0, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value),
                         &h_base);
    CHECK((s->as_Flags & ASF_REUSEADDR) != 0 &&
          (s->as_Flags & ASF_REUSEPORT) == 0,
          "UDP SO_REUSEADDR sets sharing, not SO_REUSEPORT");

    /* On a bound UDP socket the flag was already read into nx_udp_socket_share
       at bind, so setting it now records the flag and returns 0 while the live
       share is left untouched; the flag only affects the next bind. */
    h_reset();
    s = h_udp(0);
    s->as_Flags |= ASF_BOUND;
    s->as_Nx.udp.nx_udp_socket_share = NX_TRUE;
    value = 1;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value),
                        &h_base);
    CHECK(rc == 0 && (s->as_Flags & ASF_REUSEPORT) != 0,
          "post-bind UDP SO_REUSEPORT records the flag, rc 0");
    CHECK(s->as_Nx.udp.nx_udp_socket_share == NX_TRUE,
          "and leaves the live share untouched");

    h_reset();
    s = h_udp(0);
    s->as_Flags |= ASF_BOUND;
    s->as_Nx.udp.nx_udp_socket_share = NX_TRUE;
    value = 1;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value),
                        &h_base);
    CHECK(rc == 0 && (s->as_Flags & ASF_REUSEADDR) != 0,
          "post-bind UDP SO_REUSEADDR records the flag, rc 0");
    CHECK(s->as_Nx.udp.nx_udp_socket_share == NX_TRUE,
          "and leaves the live share untouched");

    /* The live-share exemption is UDP-only: a TCP socket's reuse flag is live,
       not a one-shot bind promise. */
    h_reset();
    s = h_tcp(0);
    s->as_Flags |= ASF_BOUND;
    value = 1;
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_REUSEPORT, &value, sizeof(value),
                        &h_base);
    CHECK(rc == 0, "post-bind TCP SO_REUSEPORT is still accepted");
}

static void t_refusals(void)
{
    AmiSocket *s;
    LONG       value = 1;
    socklen_t  len;
    LONG       rc;

    printf("the refusals\n");

    h_reset();
    rc = bsd_setsockopt(0, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value),
                        &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EBADF,
          "a descriptor with no socket is EBADF");

    h_reset();
    (VOID)h_tcp(0);
    rc = bsd_setsockopt(0, SOL_SOCKET, 0x7FFF, &value, sizeof(value), &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_ENOPROTOOPT,
          "an option this level does not have is ENOPROTOOPT");

    h_reset();
    (VOID)h_tcp(0);
    len = (socklen_t)sizeof(value);
    rc = bsd_getsockopt(0, SOL_SOCKET, 0x7FFF, &value, &len, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_ENOPROTOOPT,
          "and on the way out as well");

    h_reset();
    s = h_udp(0);
    len = (socklen_t)sizeof(value);
    rc = bsd_getsockopt(0, IPPROTO_TCP, TCP_NODELAY, &value, &len, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_ENOPROTOOPT,
          "TCP_NODELAY on a UDP socket is ENOPROTOOPT");
    (VOID)s;

    /* And on a TCP socket it is 1, for the life of the socket: there is no
       Nagle in the stack to turn off. */
    h_reset();
    (VOID)h_tcp(0);
    value = 0; len = (socklen_t)sizeof(value);
    rc = bsd_getsockopt(0, IPPROTO_TCP, TCP_NODELAY, &value, &len, &h_base);
    CHECK(rc == 0 && value == 1, "and on a TCP socket it is always 1");

    /* A level nothing answers. */
    h_reset();
    (VOID)h_tcp(0);
    value = 1;
    rc = bsd_setsockopt(0, 0x7EEE, 1, &value, sizeof(value), &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_ENOPROTOOPT,
          "an unknown level is ENOPROTOOPT");
}

/*
 * TCP_NODELAY.  Built without AMINETXDUO_TCP_CORK there is no hold in the
 * stack to turn on, so 0 is EINVAL and 1 is what it always reads.  With it, 0
 * turns on the small-write cork (cork.c) and the option reads back as set.
 */
static void t_nodelay(void)
{
    LONG      value;
    socklen_t len;
    LONG      rc;

    printf("options.c: TCP_NODELAY\n");

    h_reset();
#ifdef AMINETXDUO_TCP_CORK
    h_cork_sets = 0;
#endif
    (VOID)h_tcp(0);
    value = 1;
    rc = bsd_setsockopt(0, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value),
                        &h_base);
    CHECK(rc == 0, "TCP_NODELAY 1 is accepted");

    value = 0;
    rc = bsd_setsockopt(0, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value),
                        &h_base);
#ifndef AMINETXDUO_TCP_CORK
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EINVAL,
          "TCP_NODELAY 0 is EINVAL: there is no hold to turn on");
    value = 0; len = (socklen_t)sizeof(value);
    rc = bsd_getsockopt(0, IPPROTO_TCP, TCP_NODELAY, &value, &len, &h_base);
    CHECK(rc == 0 && value == 1, "and it still reads 1");
#else
    CHECK(rc == 0 && h_cork_sets == 2, "TCP_NODELAY 0 turns the cork on");
    CHECK(h.nx_enters == h.nx_leaves && h.nx_enters == 2,
          "inside the bracket, each time");
    value = 1; len = (socklen_t)sizeof(value);
    rc = bsd_getsockopt(0, IPPROTO_TCP, TCP_NODELAY, &value, &len, &h_base);
    CHECK(rc == 0 && value == 0, "and it reads back 0");

    value = 1;
    rc = bsd_setsockopt(0, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value),
                        &h_base);
    value = 0; len = (socklen_t)sizeof(value);
    rc = bsd_getsockopt(0, IPPROTO_TCP, TCP_NODELAY, &value, &len, &h_base);
    CHECK(rc == 0 && value == 1, "1 turns it off, and reads back 1");

    h_cork_result = AMI_ENOBUFS;
    value = 0;
    rc = bsd_setsockopt(0, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value),
                        &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_ENOBUFS,
          "a cork that cannot run says so");
    h_cork_result = 0;
#endif
}

static void t_user_timeout(void)
{
    AmiSocket *s;
    LONG       value;
    socklen_t  len;
    LONG       rc;

    printf("TCP_USER_TIMEOUT\n");

    h_reset();
    s = h_tcp(0);
    value = 30000;
    rc = bsd_setsockopt(0, IPPROTO_TCP, TCP_USER_TIMEOUT, &value,
                        sizeof(value), &h_base);
    CHECK(rc == 0 && s->as_UserTimeout == 30000,
          "a deadline in milliseconds is stored as it was given");

    value = 0; len = (socklen_t)sizeof(value);
    rc = bsd_getsockopt(0, IPPROTO_TCP, TCP_USER_TIMEOUT, &value, &len,
                        &h_base);
    CHECK(rc == 0 && value == 30000, "and read back unchanged");

    h_reset();
    s = h_udp(0);
    value = 30000;
    rc = bsd_setsockopt(0, IPPROTO_TCP, TCP_USER_TIMEOUT, &value,
                        sizeof(value), &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_ENOPROTOOPT,
          "and a socket with no TCP under it is refused");
}

static void t_ioctls(void)
{
    AmiSocket *s;
    LONG       value;
    LONG       rc;

    printf("IoctlSocket(): FIONBIO and FIONREAD\n");

    h_reset();
    s = h_tcp(0);
    value = 1;
    rc = bsd_IoctlSocket(0, FIONBIO, &value, &h_base);
    CHECK(rc == 0 && (s->as_Flags & ASF_NONBLOCK) != 0, "FIONBIO sets");

    value = 0;
    rc = bsd_IoctlSocket(0, FIONBIO, &value, &h_base);
    CHECK(rc == 0 && (s->as_Flags & ASF_NONBLOCK) == 0, "and clears");

    h_reset();
    (VOID)h_tcp(0);
    rc = bsd_IoctlSocket(0, FIONBIO, NULL, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EFAULT,
          "with no argument it is EFAULT");

    h_reset();
    s = h_udp(0);
    s->as_Flags |= ASF_RDSHUT;
    h.udp_available = 512;
    value = -1;
    rc = bsd_IoctlSocket(0, FIONREAD, &value, &h_base);
    CHECK(rc == 0 && value == 0,
          "after shutdown(SHUT_RD) FIONREAD is 0, not what is still queued");

    h_reset();
    s = h_udp(0);
    h.udp_available = 512;
    value = -1;
    rc = bsd_IoctlSocket(0, FIONREAD, &value, &h_base);
    CHECK(rc == 0 && value == 512, "and otherwise it is what is queued");

    h_reset();
    (VOID)h_udp(0);
    h.nx_enter_result = -1;
    value = -1;
    rc = bsd_IoctlSocket(0, FIONREAD, &value, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_ENETDOWN,
          "and a stack that cannot be entered is ENETDOWN");

    h_reset();
    rc = bsd_IoctlSocket(0, FIONBIO, &value, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EBADF,
          "a descriptor with no socket is EBADF");
}

/*
 * Dup2Socket() while the source is closed under it (F-054): by a descriptor
 * callback, or by another task on a shared base.  The hook closes descriptor
 * 0 the way CloseSocket() does, before the new slot is published.
 */
static LONG h_hook_result;

static LONG h_close_source(LONG fd)
{
    AmiSocket *s = h_table[0];

    (VOID)fd;
    h_fd_hook = NULL;
    if (s != NULL)
    {
        h_table[0] = NULL;
        bsd_socket_release(&h_base, s);
    }
    return h_hook_result;
}

/* Another task on the base, looking at the target while it is replaced. */
static AmiSocket *h_seen;

static LONG h_peek_target(LONG fd)
{
    h_seen = bsd_lookup(&h_base, fd);
    return 0;
}

static LONG h_refuse(LONG fd)
{
    (VOID)fd;
    return 1;
}

static void t_dup2_interleave(void)
{
    AmiSocket *s;
    LONG       rc;

    /* Dup2Socket(fd, -1): the lowest free descriptor. */
    h_reset();
    s = h_tcp(0);
    s->as_RefCount = 1;
    h_fd_hook = h_close_source;
    h_hook_result = 0;
    rc = bsd_Dup2Socket(0, -1, &h_base);
    CHECK(rc == 1 && h_table[1] == s, "dup(-1) publishes the socket");
    CHECK(h_freed == NULL && h_after_free == 0,
          "dup(-1): a source closed in the callback is not freed under the new descriptor");
    CHECK(s->as_RefCount == 1, "dup(-1): the new descriptor holds the one reference left");
    CHECK(s->as_Owner == &h_base, "dup(-1): this base still owns the socket it holds");

    /* Dup2Socket(fd, n). */
    h_reset();
    s = h_tcp(0);
    s->as_RefCount = 1;
    h_fd_hook = h_close_source;
    h_hook_result = 0;
    rc = bsd_Dup2Socket(0, 1, &h_base);
    CHECK(rc == 1 && h_table[1] == s, "dup2(n) publishes the socket");
    CHECK(h_freed == NULL && h_after_free == 0,
          "dup2(n): a source closed in the callback is not freed under the new descriptor");
    CHECK(s->as_RefCount == 1, "dup2(n): the new descriptor holds the one reference left");
    CHECK(s->as_Owner == &h_base, "dup2(n): this base still owns the socket it holds");

    /* No interleaving: two descriptors, two references. */
    h_reset();
    s = h_tcp(0);
    s->as_RefCount = 1;
    rc = bsd_Dup2Socket(0, 1, &h_base);
    CHECK(rc == 1 && s->as_RefCount == 2 && s->as_Owner == &h_base,
          "a plain dup2 takes one reference");
    CHECK(h_split == 0,
          "the source is looked up and retained inside one Forbid()");
    CHECK(h_forbid == 0, "and every Forbid() is paired");
    rc = bsd_Dup2Socket(0, 0, &h_base);
    CHECK(rc == 0 && s->as_RefCount == 2, "dup2 onto itself takes none");
    rc = bsd_Dup2Socket(1, -1, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EMFILE && s->as_RefCount == 2,
          "a full table is EMFILE and keeps no reference");
    h_table[1] = NULL;
    rc = bsd_Dup2Socket(1, 0, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EBADF && h_forbid == 0,
          "an empty source is EBADF, out of the Forbid()");

    /* A refused slot gives the reference back, with the refusal's errno. */
    h_reset();
    s = h_tcp(0);
    s->as_RefCount = 1;
    h_fd_hook = h_refuse;
    rc = bsd_Dup2Socket(0, -1, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EMFILE, "a refused dup(-1) is its errno");
    CHECK(s->as_RefCount == 1 && h.nx_enters == h.nx_leaves,
          "and gives its reference back inside a bracket");

    h_reset();
    s = h_tcp(0);
    s->as_RefCount = 1;
    h_fd_hook = h_refuse;
    rc = bsd_Dup2Socket(0, 1, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EMFILE && s->as_RefCount == 1,
          "a refused dup2(n) likewise");

    /* No bracket: a reference that is not the last still goes. */
    h_reset();
    s = h_tcp(0);
    s->as_RefCount = 1;
    h_fd_hook = h_refuse;
    h.nx_enter_result = -1;
    rc = bsd_Dup2Socket(0, 1, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EMFILE && s->as_RefCount == 1 &&
          s->as_Owner == &h_base,
          "a refusal with the stack down gives its reference back too");

    /* Refused after the callback closed the source: the release is the last. */
    h_reset();
    s = h_tcp(0);
    s->as_RefCount = 1;
    h_fd_hook = h_close_source;
    h_hook_result = 1;
    rc = bsd_Dup2Socket(0, -1, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EMFILE, "closed and refused is the refusal");
    CHECK(h_freed == s && h_after_free == 0 && s->as_RefCount == 0,
          "and the socket is freed once, by the unwind");

    h_reset();
    s = h_tcp(0);
    s->as_RefCount = 1;
    h_fd_hook = h_close_source;
    h_hook_result = 1;
    h.nx_enter_result = -1;
    rc = bsd_Dup2Socket(0, -1, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == AMI_EMFILE,
          "closed and refused with the stack down keeps the refusal errno");
    CHECK(bsd_defer_head == s && s->as_DeferRefs == 1 &&
          s->as_RefCount == 1 && h_freed == NULL,
          "its last release waits for a NetX bracket instead of being lost");
}

/*
 * Dup2Socket() onto an open descriptor whose replacement the callback refuses
 * (F-055): FDCB_FREE has already run for the old one, and it comes back.
 */
static void h_pair(AmiSocket **a, AmiSocket **b)
{
    h_reset();
    *a = h_tcp(0);
    (*a)->as_RefCount = 1;
    *b = h_udp(1);
    (*b)->as_RefCount = 1;
}

static void t_dup2_target_rollback(void)
{
    AmiSocket *a;
    AmiSocket *b;
    LONG       rc;

    h_pair(&a, &b);
    strcpy(h_veto, "C");
    rc = bsd_Dup2Socket(0, 1, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == H_VETO, "a CHECK veto is the caller's errno");
    CHECK(h_table[1] == b && b->as_RefCount == 1 && h_freed == NULL,
          "and the socket it would have replaced is still there, open");
    CHECK(strcmp(h_cb, "FCR") == 0, "FREE, CHECK, then ALLOC to put it back");
    CHECK(a->as_RefCount == 1 && b->as_Owner == &h_base,
          "the source keeps one reference and the target its owner");

    h_pair(&a, &b);
    strcpy(h_veto, "A");
    rc = bsd_Dup2Socket(0, 1, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == H_VETO && h_table[1] == b &&
          b->as_RefCount == 1 && strcmp(h_cb, "FCAR") == 0,
          "an ALLOC veto puts it back likewise");

    h_pair(&a, &b);
    strcpy(h_veto, "AR");
    rc = bsd_Dup2Socket(0, 1, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == H_VETO, "a refused put-back keeps the veto's errno");
    CHECK(h_table[1] == NULL && h_freed == b && strcmp(h_cb, "FCAR") == 0,
          "and cannot be undone: the slot is empty and its socket released");
    CHECK(a->as_RefCount == 1, "the source keeps one reference");

    h_pair(&a, &b);
    strcpy(h_veto, "F");
    rc = bsd_Dup2Socket(0, 1, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == H_VETO && h_table[1] == b &&
          b->as_RefCount == 1 && strcmp(h_cb, "F") == 0,
          "a FREE veto changes nothing");

    h_pair(&a, &b);
    rc = bsd_Dup2Socket(0, 1, &h_base);
    CHECK(rc == 1 && h_table[1] == a && a->as_RefCount == 2 && h_freed == b &&
          strcmp(h_cb, "FCA") == 0,
          "an accepted dup2 replaces and releases, callbacks in the old order");

    h_pair(&a, &b);
    h_seen = b;
    h_fd_hook = h_peek_target;
    strcpy(h_veto, "C");
    rc = bsd_Dup2Socket(0, 1, &h_base);
    CHECK(h_seen == NULL, "while it is replaced the target has no socket to look up");
    CHECK(rc == -1 && h_table[1] == b && b->as_RefCount == 1,
          "and a veto still finds it to put back");

    /* Dup2Socket(-1, n): reserve n. */
    h_pair(&a, &b);
    strcpy(h_veto, "C");
    rc = bsd_Dup2Socket(-1, 1, &h_base);
    CHECK(rc == -1 && h_base.sb_Errno == H_VETO && h_table[1] == b &&
          b->as_RefCount == 1 && strcmp(h_cb, "FCR") == 0,
          "a refused reserve puts the socket back too");

    h_pair(&a, &b);
    rc = bsd_Dup2Socket(-1, 1, &h_base);
    CHECK(rc == 1 && h_table[1] == BSD_FD_RESERVED && h_freed == b,
          "an accepted reserve replaces and releases");

    h_pair(&a, &b);
    h_table[1] = BSD_FD_RESERVED;
    strcpy(h_veto, "C");
    rc = bsd_Dup2Socket(0, 1, &h_base);
    CHECK(rc == -1 && h_table[1] == BSD_FD_RESERVED && strcmp(h_cb, "FCR") == 0,
          "a reserved target is put back reserved");
}

int main(void)
{
    printf("options.c host tests\n");

    t_timeouts();
    t_paired_setters();
    t_so_error();
    t_linger();
    t_flags();
    t_reuse();
    t_refusals();
    t_user_timeout();
    t_nodelay();
    t_ioctls();
    t_dup2_interleave();
    t_dup2_target_rollback();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}
