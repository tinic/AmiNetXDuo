/*
 * src/bsdsocket/options.c on the host: setsockopt, getsockopt and the two
 * ioctls, at the SOL_SOCKET and IPPROTO_TCP levels.
 *
 * WHY THIS FILE EXISTS
 *
 *   options.c was outside the host tier until the struct timeval collision
 *   was resolved, for the same reason select.c was: SO_RCVTIMEO, SO_SNDTIMEO
 *   and their two conversion helpers read tv_secs and tv_micro, and the C
 *   library owns the tag.  host_prelude.h renames it, so the whole file
 *   compiles here now and the timeout conversions -- the part that was
 *   unreachable -- are what most of the assertions below are about.
 *
 *   Everything in this file used to be provable only on the emulator, which
 *   .github/workflows/emulator.yml runs on a nightly cron and on a tag, never
 *   on a push.
 *
 * WHAT IS COVERED AND WHAT IS NOT
 *
 *   COVERED: the SOL_SOCKET and IPPROTO_TCP options that are decided in this
 *   file -- the timeout conversions in both directions, SO_ERROR's read and
 *   clear, SO_LINGER's bounds, SO_EVENTMASK, the flag options, the refusals,
 *   and FIONBIO/FIONREAD.
 *
 *   NOT COVERED: anything options.c delegates.  IPPROTO_IPV6 goes to
 *   bsd_setsockopt_ipv6()/bsd_getsockopt_ipv6() in src/ipv6, the membership
 *   options to bsd_mcast_*() and the ancillary-data options to
 *   bsd_cmsg_option(); all are stubbed, and what a stub proves about the file
 *   it stands in for is nothing.  What IS proved is that options.c routes to
 *   them, which is the half that lives here.
 *
 *   The build is the shipping one: AMINETXDUO_IPV6 and AMINETXDUO_MULTICAST
 *   are set project-wide, so the switch statements compiled here have the same
 *   arms as the ones that ship.
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

/* --------------------------------------------------------------- reporting */

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

/* ------------------------------------------------------------ the fixture */

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
    ULONG  delegated;           /* calls that left options.c for another file */
    LONG   delegate_result;
    ULONG  raw_available;
    ULONG  udp_available;
    ULONG  packet_length;
} h;

static void h_reset(void)
{
    memset(&h_base, 0, sizeof(h_base));
    memset(&h_sock, 0, sizeof(h_sock));
    memset(&h_table, 0, sizeof(h_table));
    memset(&h, 0, sizeof(h));

    h_base.sb_Table     = h_table;
    h_base.sb_TableSize = H_FDS;
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

/* -------------------------------------------------------------- the stubs */

VOID Forbid(VOID) { }
VOID Permit(VOID) { }

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;
    return -1;
}

AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd)
{
    (VOID)base;
    if (fd < 0 || fd >= H_FDS)
        return NULL;
    return h_table[fd];
}

LONG bsd_table_size(struct AmiSocketBase *base) { (VOID)base; return H_FDS; }

VOID bsd_bcopy(const APTR src, APTR dst, ULONG size)
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

/*
 * The four files options.c hands an option to.  Each records that it was
 * reached and answers what the fixture set, so a test can assert the routing
 * without asserting anything about the file behind it.
 *
 * bsd_cmsg_option() answers 1 for "not mine, carry on", which is the contract
 * its declaration states; the tests that reach it leave it at that.
 */
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

/* The descriptor table, which only Dup2Socket reaches from this file. */
LONG bsd_fd_alloc(struct AmiSocketBase *base, AmiSocket *sock)
{
    LONG fd;

    for (fd = 0; fd < H_FDS; fd++)
    {
        if (h_table[fd] == NULL)
        {
            h_table[fd] = sock;
            return fd;
        }
    }

    (VOID)base;
    return -1;
}

LONG bsd_fd_reserve(struct AmiSocketBase *base, LONG fd)
{
    (VOID)base;
    return (fd >= 0 && fd < H_FDS) ? fd : -1;
}

BOOL bsd_fd_reserved(struct AmiSocketBase *base, LONG fd)
{
    (VOID)base; (VOID)fd;
    return FALSE;
}

LONG bsd_fd_free(struct AmiSocketBase *base, LONG fd)
{
    (VOID)base;
    if (fd >= 0 && fd < H_FDS)
        h_table[fd] = NULL;
    return 0;
}

VOID bsd_socket_retain(AmiSocket *sock) { sock->as_RefCount++; }
VOID bsd_socket_release(struct AmiSocketBase *base, AmiSocket *sock)
{
    (VOID)base;
    if (sock->as_RefCount > 0)
        sock->as_RefCount--;
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

/* --------------------------------------------- NetX Duo and ThreadX stubs */

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
    (VOID)socket_ptr; (VOID)reuse;
    return NX_SUCCESS;
}

UINT _nx_ip_route_find(NX_IP *ip_ptr, ULONG destination_address,
                       NX_INTERFACE **nx_interface, ULONG *next_hop_address)
{
    (VOID)ip_ptr; (VOID)destination_address;
    (VOID)nx_interface; (VOID)next_hop_address;
    return NX_NOT_SUCCESSFUL;
}

UINT _txe_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (VOID)mutex_ptr; (VOID)wait_option;
    return TX_SUCCESS;
}

UINT _txe_mutex_put(TX_MUTEX *mutex_ptr) { (VOID)mutex_ptr; return TX_SUCCESS; }

/* ------------------------------------------- SO_RCVTIMEO and SO_SNDTIMEO */

/*
 * The conversion in both directions, which is the whole of what the struct
 * timeval collision used to hide from this tier.
 */
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

    /*
     * Rounded up and not truncated, which is a different claim from the one
     * above: 30000 microseconds is a tick and a half at the Amiga's 50 Hz, and
     * a socket must never wait less than the caller asked for.  Written as the
     * contract rather than as a tick count so it holds at any tick rate.
     */
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

    /* Back out again. */
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

/* ------------------------------------------------------ SO_ERROR's clear */

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

    /*
     * Clearing before validating meant a bad optval answered EFAULT and
     * destroyed the pending error on the way out, and a non-blocking connect
     * has no other way to find out why it failed.
     */
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

/* ------------------------------------------------- SO_LINGER's two bounds */

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

/* ------------------------------------------ the flag options and the event
 * mask, which are what getsockopt has to agree with */

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

    /*
     * SO_EVENTMASK is the AmiTCP V4 async event API's, and the FD_* bits it
     * carries are select.c's.  A value, not a boolean.
     */
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

/* ---------------------------------------------------------- the refusals */

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

    /*
     * A TCP-level option on a socket with no TCP under it.  Answering 1 or 0
     * there described a level the socket does not have.
     */
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

/* ------------------------------------------- TCP_USER_TIMEOUT, ours alone */

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

/* ------------------------------------------------ FIONBIO and FIONREAD */

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

    /*
     * shutdown(SHUT_RD) makes every later receive an EOF even if packets were
     * queued beforehand, so FIONREAD must report what recv() can return and
     * not bytes that are now intentionally hidden.
     */
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

int main(void)
{
    printf("options.c host tests\n");

    t_timeouts();
    t_so_error();
    t_linger();
    t_flags();
    t_refusals();
    t_user_timeout();
    t_ioctls();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}
