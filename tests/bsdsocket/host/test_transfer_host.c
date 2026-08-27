/*
 * src/bsdsocket/transfer.c on the host: send/sendto/sendmsg and
 * recv/recvfrom/recvmsg.
 *
 * WHY THIS FILE WAS SAID TO BE IMPOSSIBLE, AND WHY IT IS NOT.  transfer.c
 * opens with eleven _Static_asserts pinning `struct iovec` to eight bytes and
 * `struct msghdr` to the 4.4BSD twenty-eight byte shape, and those were read
 * as a wall: a host has 64-bit pointers, so the file could never compile
 * there.  It can.  The assertions are about the TARGET'S POINTER WIDTH, and at
 * that width glibc's own definitions are the same shape to the byte -- iovec
 * is {void *, size_t} at 0 and 4, and msghdr's seven members land on
 * 0,4,8,12,16,20,24.  So the whole translation unit is compiled here, at 32
 * bits, with nothing shimmed, nothing conditional and no assertion weakened:
 * the eleven of them RUN, as part of this build, against the shipping ABI.
 * They fail at 64 bits because the ABI genuinely is not that shape there,
 * which is what they are for.
 *
 * tests/bsdsocket/CMakeLists.txt puts this target behind
 * CMAKE_SIZEOF_VOID_P EQUAL 4 for that reason, and `tools/ci.sh host32` is
 * where it runs.
 *
 * WHAT IS ASSERTED.  The exported vectors, against the shipping translation
 * unit: the scatter/gather cursor across several iovecs, the argument
 * refusals, and the TCP send loop's crediting -- a send that is cut short
 * reports the bytes that reached the wire, which is where 10411a41 was.  The
 * NetX Duo packet layer is scripted below; the socket-layer helpers
 * transfer.c delegates to (cmsg.c, oob.c, raw.c, waitslice.c, the address
 * converters) are stubbed, and the header of each stub says what that does
 * and does not prove.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "udp_queue.h"
#include "netmonitor.h"

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

/*
 * The same eleven claims transfer.c makes, restated here so that a build of
 * this test on a host whose iovec is NOT the target's shape fails loudly
 * rather than being quietly excluded.  If these ever disagree with the ones in
 * transfer.c, one of the two is wrong about the ABI.
 */
_Static_assert(sizeof(struct iovec) == 8, "host iovec is not the target's");
_Static_assert(sizeof(struct msghdr) == 28, "host msghdr is not the target's");
_Static_assert(sizeof(void *) == 4, "this test needs the target's pointer width");

#define H_FDS       4
#define H_PKTS      8
#define H_PLAN      8

static struct AmiSocketBase h_base;
static AmiSocket            h_sock[H_FDS];
static NX_PACKET_POOL       h_pool;
static NX_IP                h_ip;

/* A fake packet.  Only the members transfer.c and the stubs read are used. */
typedef struct HPacket
{
    NX_PACKET   nx;
    ULONG       length;
    BOOL        in_use;
    BOOL        released;
} HPacket;

static HPacket h_pkt[H_PKTS];

static struct
{
    LONG        errno_value;
    ULONG       fails;              /* bsd_fail() calls                      */

    LONG        nx_enter_result;
    ULONG       nx_enters, nx_leaves;

    BOOL        no_pool;            /* netstack_pool() answers NULL          */

    ULONG       mss;

    /* nx_packet_allocate(): one status per call, then the last one repeats. */
    UINT        alloc_plan[H_PLAN];
    unsigned    alloc_planned, allocs;

    /* nx_packet_data_append() */
    UINT        append_status;
    ULONG       appended;           /* bytes accepted across all calls       */
    UBYTE       wire[256];          /* and the bytes themselves, in order    */
    ULONG       wire_len;

    /* nx_tcp_socket_send(): one status per call, then the last one repeats.
       NX_SUCCESS means the whole packet went. */
    UINT        send_plan[H_PLAN];
    unsigned    send_planned, sends;
    ULONG       sent_bytes;         /* what the successful sends carried     */

    ULONG       releases;

    /* What nx_tcp_socket_send() takes before it fails.  The real one is not
       all-or-nothing: it segments the packet itself and trims what went on the
       wire off the caller's packet before reporting the failure. */
    ULONG       send_trim;

    /* The socket-layer helpers */
    BOOL        monitor_present;
    LONG        monitor_verdict;
    ULONG       monitor_calls;
    LONG        cmsg_parse_result;
    ULONG       cmsg_builds;
    LONG        sockaddr_get_result;
    ULONG       sockaddr_puts;
    ULONG       wait_option;
} h;

static void h_reset(void)
{
    memset(&h, 0, sizeof(h));
    memset(&h_sock, 0, sizeof(h_sock));
    memset(&h_pkt, 0, sizeof(h_pkt));
    memset(&h_base, 0, sizeof(h_base));
    memset(&h_pool, 0, sizeof(h_pool));

    h.mss           = 536;
    h.append_status = NX_SUCCESS;
}

static AmiSocket *h_tcp(LONG fd)
{
    AmiSocket *s = &h_sock[fd];

    s->as_Owner = &h_base;
    s->as_Flags = ASF_TCP | ASF_CONNECTED;

    return s;
}

static AmiSocket *h_udp(LONG fd)
{
    AmiSocket *s = &h_sock[fd];

    s->as_Owner = &h_base;
    s->as_Flags = ASF_UDP;

    return s;
}

/* ------------------------------------------------------- socket helpers -- */

AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd)
{
    (VOID)base;

    if (fd < 0 || fd >= H_FDS)
        return NULL;

    return (h_sock[fd].as_Owner != NULL) ? &h_sock[fd] : NULL;
}

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    (VOID)base;

    h.fails++;
    h.errno_value = code;

    return -1;
}

LONG bsd_errno_from_nx(UINT status)
{
    return (status == NX_NO_PACKET) ? AMI_ENOBUFS : AMI_EIO;
}

LONG bsd_wait_errno(ULONG wait, UINT status)
{
    (VOID)status;

    return (wait == NX_NO_WAIT) ? AMI_EWOULDBLOCK : AMI_ETIMEDOUT;
}

LONG bsd_nx_enter(struct AmiSocketBase *base)
{
    (VOID)base;

    h.nx_enters++;

    return h.nx_enter_result;
}

VOID bsd_nx_leave(struct AmiSocketBase *base)
{
    (VOID)base;

    h.nx_leaves++;
}

/*
 * One slice, run once.  The real one splits a long wait so Ctrl-C is noticed;
 * nothing here tests that, and `aborted` is always FALSE, so the break paths
 * in transfer.c are NOT covered by this harness.
 */
UINT bsd_wait_sliced(struct AmiSocketBase *base, ULONG wait,
                     BsdSlicedCall call, VOID *arg, BOOL *aborted)
{
    (VOID)base;

    if (aborted != NULL)
        *aborted = FALSE;

    return call(arg, wait);
}

ULONG bsd_wait_option(AmiSocket *sock, ULONG timeout_ticks, LONG flags)
{
    (VOID)timeout_ticks;

    h.wait_option = ((sock->as_Flags & ASF_NONBLOCK) != 0 ||
                     (flags & MSG_DONTWAIT) != 0) ? NX_NO_WAIT : 100UL;

    return h.wait_option;
}

NX_IP *netstack_ip(VOID)
{
    return &h_ip;
}

NX_PACKET_POOL *netstack_pool(VOID)
{
    return h.no_pool ? NULL : &h_pool;
}

BOOL bsd_netmon_have(LONG type)
{
    (VOID)type;

    return h.monitor_present;
}

LONG bsd_netmon_dispatch(LONG type, APTR message)
{
    (VOID)type;
    (VOID)message;

    h.monitor_calls++;

    return h.monitor_verdict;
}

STRPTR bsd_netmon_caller(struct AmiSocketBase *base)
{
    (VOID)base;

    return (STRPTR)"host";
}

LONG bsd_cmsg_parse(struct AmiSocketBase *base, AmiSocket *sock,
                    const struct msghdr *msg, BsdCmsgSource *out)
{
    (VOID)base;
    (VOID)sock;
    (VOID)msg;

    memset(out, 0, sizeof(*out));

    return h.cmsg_parse_result;
}

VOID bsd_cmsg_build(AmiSocket *sock, NX_PACKET *packet, struct msghdr *msg)
{
    (VOID)sock;
    (VOID)packet;
    (VOID)msg;

    h.cmsg_builds++;
}

LONG bsd_cmsg_source_index(NX_IP *ip, const BsdCmsgSource *src, BOOL v6)
{
    (VOID)ip;
    (VOID)src;
    (VOID)v6;

    return -1;
}

LONG bsd_sockaddr_get(struct AmiSocketBase *base, const struct sockaddr *sa,
                      socklen_t len, NXD_ADDRESS *addr, UINT *port,
                      ULONG *scope_id)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

    (VOID)base;
    (VOID)len;

    if (h.sockaddr_get_result != 0)
        return h.sockaddr_get_result;

    memset(addr, 0, sizeof(*addr));
    addr->nxd_ip_version      = NX_IP_VERSION_V4;
    addr->nxd_ip_address.v4   = sin->sin_addr.s_addr;
    *port                     = (UINT)BSD_NTOHS(sin->sin_port);
    *scope_id                 = 0UL;

    return 0;
}

VOID bsd_sockaddr_put(const AmiSocket *sock, struct sockaddr *sa,
                      socklen_t *len, const NXD_ADDRESS *addr, UINT port,
                      ULONG scope_id)
{
    (VOID)sock;
    (VOID)sa;
    (VOID)len;
    (VOID)addr;
    (VOID)port;
    (VOID)scope_id;

    h.sockaddr_puts++;
}

VOID bsd_addr_from_v4(NXD_ADDRESS *addr, ULONG v4)
{
    memset(addr, 0, sizeof(*addr));
    addr->nxd_ip_version    = NX_IP_VERSION_V4;
    addr->nxd_ip_address.v4 = v4;
}

BOOL bsd_addr_normalise(const AmiSocket *sock, NXD_ADDRESS *addr)
{
    (VOID)sock;
    (VOID)addr;

    return TRUE;
}

VOID bsd_in6_to_words(const UBYTE bytes[16], ULONG words[4])
{
    memcpy(words, bytes, 16);
}

BOOL bsd_bind_wants_interface(const AmiSocket *sock, const NX_INTERFACE *nxif)
{
    (VOID)sock;
    (VOID)nxif;

    return TRUE;
}

LONG bsd_oob_send(struct AmiSocketBase *base, AmiSocket *sock, UBYTE byte,
                  LONG flags)
{
    (VOID)base;
    (VOID)sock;
    (VOID)byte;
    (VOID)flags;

    return 1;
}

BOOL bsd_oob_take(AmiSocket *sock, UBYTE *out)
{
    (VOID)sock;
    (VOID)out;

    return FALSE;
}

NX_PACKET *bsd_raw_receive(AmiSocket *sock, ULONG wait, UINT *why)
{
    (VOID)sock;
    (VOID)wait;

    if (why != NULL)
        *why = NX_NO_PACKET;

    return NX_NULL;
}

LONG bsd_raw_send_packet(struct AmiSocketBase *base, AmiSocket *sock,
                         NX_PACKET *packet, const NXD_ADDRESS *addr,
                         ULONG scope, const BsdCmsgSource *src)
{
    (VOID)base;
    (VOID)sock;
    (VOID)packet;
    (VOID)addr;
    (VOID)scope;
    (VOID)src;

    return 0;
}

VOID bsd_raw_source(NX_PACKET *packet, NXD_ADDRESS *addr)
{
    (VOID)packet;
    (VOID)addr;
}

BsdSourceKind bsd_source_select(const AmiSocket *sock, const NXD_ADDRESS *dest,
                                ULONG scope, UINT *index)
{
    (VOID)sock;
    (VOID)dest;
    (VOID)scope;

    if (index != NULL)
        *index = 0;

    return BSD_SOURCE_ROUTE;
}

UINT bsd_udp_queue_info(const NX_PACKET *packet, UINT *source_port,
                        ULONG *payload_length)
{
    (VOID)packet;

    if (source_port != NULL)
        *source_port = 0;
    if (payload_length != NULL)
        *payload_length = 0;

    return NX_SUCCESS;
}

UINT anx6_scope(const ULONG *addr)
{
    (VOID)addr;

    return 0;
}

VOID bsd_bzero(APTR p, ULONG size)
{
    memset(p, 0, (size_t)size);
}

/* ------------------------------------------------------------ NetX Duo -- */

static HPacket *h_from_nx(NX_PACKET *p)
{
    unsigned i;

    for (i = 0; i < H_PKTS; i++)
    {
        if (&h_pkt[i].nx == p)
            return &h_pkt[i];
    }

    return NULL;
}

static UINT h_plan(const UINT *plan, unsigned planned, unsigned n)
{
    if (planned == 0)
        return NX_SUCCESS;

    return plan[(n < planned) ? n : planned - 1];
}

UINT _nxe_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                          ULONG packet_type, ULONG wait_option)
{
    unsigned i;
    UINT     status;

    (VOID)pool_ptr;
    (VOID)packet_type;
    (VOID)wait_option;

    status = h_plan(h.alloc_plan, h.alloc_planned, h.allocs);
    h.allocs++;

    if (status != NX_SUCCESS)
        return status;

    for (i = 0; i < H_PKTS; i++)
    {
        if (!h_pkt[i].in_use)
        {
            memset(&h_pkt[i], 0, sizeof(h_pkt[i]));
            h_pkt[i].in_use = TRUE;
            *packet_ptr     = &h_pkt[i].nx;

            return NX_SUCCESS;
        }
    }

    return NX_NO_PACKET;
}

UINT _nxe_packet_data_append(NX_PACKET *packet_ptr, VOID *data_start,
                             ULONG data_size, NX_PACKET_POOL *pool_ptr,
                             ULONG wait_option)
{
    HPacket *p = h_from_nx(packet_ptr);

    (VOID)pool_ptr;
    (VOID)wait_option;

    if (h.append_status != NX_SUCCESS)
        return h.append_status;

    if (p != NULL)
        p->length += data_size;

    h.appended += data_size;

    if (h.wire_len + data_size <= sizeof(h.wire))
    {
        memcpy(&h.wire[h.wire_len], data_start, data_size);
        h.wire_len += data_size;
    }

    return NX_SUCCESS;
}

UINT _nxe_packet_length_get(NX_PACKET *packet_ptr, ULONG *length)
{
    HPacket *p = h_from_nx(packet_ptr);

    *length = (p != NULL) ? p->length : 0UL;

    return NX_SUCCESS;
}

/* nx_packet_release(p) expands to _nxe_packet_release(&p), so the argument is
   the address of the caller's variable and the packet is one dereference in. */
UINT _nxe_packet_release(NX_PACKET **packet_ptr_ptr)
{
    HPacket *p = h_from_nx(*packet_ptr_ptr);

    h.releases++;

    if (p != NULL)
    {
        p->in_use   = FALSE;
        p->released = TRUE;
    }

    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_mss_get(NX_TCP_SOCKET *socket_ptr, ULONG *mss)
{
    (VOID)socket_ptr;

    *mss = h.mss;

    return NX_SUCCESS;
}

/*
 * NX_SUCCESS takes the packet and the caller must not release it; anything
 * else leaves it with the caller, which is the case transfer.c credits with
 * bsd_send_consumed().
 */
UINT _nxe_tcp_socket_send(NX_TCP_SOCKET *socket_ptr, NX_PACKET **packet_ptr_ptr,
                          ULONG wait_option)
{
    HPacket *p = h_from_nx(*packet_ptr_ptr);
    UINT     status;

    (VOID)socket_ptr;
    (VOID)wait_option;

    status = h_plan(h.send_plan, h.send_planned, h.sends);
    h.sends++;

    if (p == NULL)
        return status;

    if (status == NX_SUCCESS)
    {
        h.sent_bytes += p->length;
        p->in_use     = FALSE;

        return status;
    }

    /* A failure that still put some of it on the wire: trim what went, as
       _nx_tcp_socket_send_internal() does. */
    if (h.send_trim > 0)
    {
        ULONG gone = (h.send_trim < p->length) ? h.send_trim : p->length;

        p->length    -= gone;
        h.sent_bytes += gone;
    }

    return status;
}

UINT _nxe_tcp_socket_receive(NX_TCP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                             ULONG wait_option)
{
    (VOID)socket_ptr;
    (VOID)packet_ptr;
    (VOID)wait_option;

    return NX_NO_PACKET;
}

UINT _nxe_udp_socket_receive(NX_UDP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                             ULONG wait_option)
{
    (VOID)socket_ptr;
    (VOID)packet_ptr;
    (VOID)wait_option;

    return NX_NO_PACKET;
}

UINT _nxe_udp_socket_bind(NX_UDP_SOCKET *socket_ptr, UINT port,
                          ULONG wait_option)
{
    (VOID)socket_ptr;
    (VOID)port;
    (VOID)wait_option;

    return NX_SUCCESS;
}

UINT _nxe_udp_socket_port_get(NX_UDP_SOCKET *socket_ptr, UINT *port_ptr)
{
    (VOID)socket_ptr;

    *port_ptr = 1024;

    return NX_SUCCESS;
}

UINT _nxde_udp_socket_send(NX_UDP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                           NXD_ADDRESS *ip_address, UINT port)
{
    HPacket *p = h_from_nx(*packet_ptr);

    (VOID)socket_ptr;
    (VOID)ip_address;
    (VOID)port;

    h.sends++;

    if (p != NULL)
    {
        h.sent_bytes += p->length;
        p->in_use     = FALSE;
    }

    return NX_SUCCESS;
}

UINT _nxde_udp_socket_source_send(NX_UDP_SOCKET *socket_ptr,
                                  NX_PACKET *packet_ptr,
                                  NXD_ADDRESS *ip_address, UINT port,
                                  UINT address_index)
{
    (VOID)address_index;

    return _nxde_udp_socket_send(socket_ptr, &packet_ptr, ip_address, port);
}

UINT _nxde_udp_source_extract(NX_PACKET *packet_ptr, NXD_ADDRESS *ip_address,
                              UINT *port)
{
    (VOID)packet_ptr;

    memset(ip_address, 0, sizeof(*ip_address));
    *port = 0;

    return NX_SUCCESS;
}

UINT _nxe_packet_data_extract_offset(NX_PACKET *packet_ptr, ULONG offset,
                                     VOID *buffer_start, ULONG buffer_length,
                                     ULONG *bytes_copied)
{
    (VOID)packet_ptr;
    (VOID)offset;
    (VOID)buffer_start;
    (VOID)buffer_length;

    *bytes_copied = 0;

    return NX_SUCCESS;
}

ULONG _nx_ip_route_find(NX_IP *ip_ptr, ULONG destination_address,
                        NX_INTERFACE **nx_ip_interface, ULONG *next_hop_address)
{
    (VOID)ip_ptr;
    (VOID)destination_address;
    (VOID)nx_ip_interface;
    (VOID)next_hop_address;

    return NX_SUCCESS;
}

UINT _nxd_ipv6_interface_find(NX_IP *ip_ptr, ULONG *dest_address,
                              NXD_IPV6_ADDRESS **ipv6_addr, NX_INTERFACE *if_ptr)
{
    (VOID)ip_ptr;
    (VOID)dest_address;
    (VOID)ipv6_addr;
    (VOID)if_ptr;

    return NX_SUCCESS;
}

UINT _nxe_udp_socket_source_send(NX_UDP_SOCKET *socket_ptr,
                                 NX_PACKET **packet_ptr, ULONG ip_address,
                                 UINT port, UINT address_index)
{
    NXD_ADDRESS a;

    (VOID)address_index;

    bsd_addr_from_v4(&a, ip_address);

    return _nxde_udp_socket_send(socket_ptr, packet_ptr, &a, port);
}

/* mcast.c.  Every send here is unicast, so the prepare/finish pair is a
   no-op; what a multicast send does with the interface hop limit is mcast.c's
   claim and is not made here. */
LONG bsd_mcast_prepare_send(AmiSocket *sock, const NXD_ADDRESS *addr)
{
    (VOID)sock;
    (VOID)addr;

    return 0;
}

LONG bsd_mcast6_prepare_send(AmiSocket *sock, const NXD_ADDRESS *addr,
                             ULONG *saved)
{
    (VOID)sock;
    (VOID)addr;

    *saved = 0UL;

    return 0;
}

VOID bsd_mcast6_finish_send(ULONG saved)
{
    (VOID)saved;
}

UINT _txe_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    (VOID)mutex_ptr;
    (VOID)wait_option;

    return TX_SUCCESS;
}

UINT _txe_mutex_put(TX_MUTEX *mutex_ptr)
{
    (VOID)mutex_ptr;

    return TX_SUCCESS;
}

/* ------------------------------------------------------------- tests ---- */

/*
 * The ABI the eleven assertions in transfer.c pin.  Restated as runtime checks
 * as well, because a compile-time assertion that holds says nothing in the
 * test's own output, and the point of bringing this file in is that the shape
 * is now checked somewhere a push runs.
 */
static void t_abi(void)
{
    struct msghdr m;

    printf("transfer: the scatter/gather ABI\n");

    h_reset();

    CHECK(sizeof(struct iovec) == 8, "iovec is eight bytes");
    CHECK(offsetof(struct iovec, iov_base) == 0, "iov_base is first");
    CHECK(offsetof(struct iovec, iov_len) == 4, "iov_len follows it");

    CHECK(sizeof(m) == 28, "msghdr is the 4.4BSD twenty-eight bytes");
    CHECK(offsetof(struct msghdr, msg_name)       ==  0, "msg_name");
    CHECK(offsetof(struct msghdr, msg_namelen)    ==  4, "msg_namelen");
    CHECK(offsetof(struct msghdr, msg_iov)        ==  8, "msg_iov");
    CHECK(offsetof(struct msghdr, msg_iovlen)     == 12, "msg_iovlen");
    CHECK(offsetof(struct msghdr, msg_control)    == 16, "msg_control");
    CHECK(offsetof(struct msghdr, msg_controllen) == 20, "msg_controllen");
    CHECK(offsetof(struct msghdr, msg_flags)      == 24, "msg_flags");
}

static void t_refusals(void)
{
    char buf[16];

    printf("transfer: the argument refusals\n");

    h_reset();
    (VOID)h_tcp(0);
    (VOID)h_udp(1);

    CHECK(bsd_send(3, buf, 4, 0, &h_base) == -1 &&
          h.errno_value == AMI_EBADF,
          "a descriptor nobody opened is EBADF");

    CHECK(bsd_send(0, buf, -1, 0, &h_base) == -1 &&
          h.errno_value == AMI_EINVAL,
          "a negative length is EINVAL");

    CHECK(bsd_send(0, NULL, 4, 0, &h_base) == -1 &&
          h.errno_value == AMI_EFAULT,
          "a null buffer with a length is EFAULT");

    CHECK(bsd_recv(0, NULL, 4, 0, &h_base) == -1 &&
          h.errno_value == AMI_EFAULT,
          "and so it is on the receive side");

    CHECK(bsd_send(1, buf, 4, MSG_OOB, &h_base) == -1 &&
          h.errno_value == AMI_EOPNOTSUPP,
          "MSG_OOB on a datagram socket is EOPNOTSUPP: there is no urgent "
          "data outside TCP");

    CHECK(bsd_sendmsg(0, NULL, 0, &h_base) == -1 &&
          h.errno_value == AMI_EFAULT,
          "a null msghdr is EFAULT");

    CHECK(bsd_recvmsg(0, NULL, 0, &h_base) == -1 &&
          h.errno_value == AMI_EFAULT,
          "on the receive side too");

    {
        struct msghdr m;

        memset(&m, 0, sizeof(m));

        CHECK(bsd_sendmsg(0, &m, MSG_OOB, &h_base) == -1 &&
              h.errno_value == AMI_EOPNOTSUPP,
              "sendmsg() cannot carry urgent data at all");
        CHECK(bsd_recvmsg(0, &m, MSG_OOB, &h_base) == -1 &&
              h.errno_value == AMI_EOPNOTSUPP,
              "nor can recvmsg()");
    }

    /* An unconnected datagram socket with no destination has nowhere to go. */
    CHECK(bsd_send(1, buf, 4, 0, &h_base) == -1 &&
          h.errno_value == AMI_EDESTADDRREQ,
          "send() on an unconnected datagram socket is EDESTADDRREQ");

    CHECK(bsd_sendto(1, buf, 4, 0, NULL, 0, &h_base) == -1 &&
          h.errno_value == AMI_EDESTADDRREQ,
          "and so is sendto() with no address");
}

/*
 * bsd_iov_total() through sendmsg(): a malformed list is EINVAL and nothing
 * is sent, and the total is what the send loop is asked for.
 */
static void t_iov_total(void)
{
    struct msghdr m;
    struct iovec  iov[3];
    char          a[4], b[6];

    printf("transfer: the scatter/gather list\n");

    h_reset();
    (VOID)h_tcp(0);

    memset(&m, 0, sizeof(m));
    m.msg_iov = iov;

    /* A null base with a nonzero length. */
    iov[0].iov_base = NULL;
    iov[0].iov_len  = 4;
    m.msg_iovlen    = 1;

    CHECK(bsd_sendmsg(0, &m, 0, &h_base) == -1 &&
          h.errno_value == AMI_EINVAL,
          "an entry with no buffer is EINVAL");
    CHECK(h.sends == 0, "and nothing goes on the wire");

    /* A negative count. */
    iov[0].iov_base = a;
    m.msg_iovlen    = (int)-1;

    CHECK(bsd_sendmsg(0, &m, 0, &h_base) == -1 &&
          h.errno_value == AMI_EINVAL,
          "a negative entry count is EINVAL");

    /* A total that will not fit in a positive LONG. */
    iov[0].iov_base = a;
    iov[0].iov_len  = 0x7FFFFFFFUL;
    iov[1].iov_base = b;
    iov[1].iov_len  = 2;
    m.msg_iovlen    = 2;

    CHECK(bsd_sendmsg(0, &m, 0, &h_base) == -1 &&
          h.errno_value == AMI_EINVAL,
          "a total larger than send() can report is EINVAL, not truncated");

    /* A null list with a nonzero count. */
    m.msg_iov    = NULL;
    m.msg_iovlen = 1;

    CHECK(bsd_sendmsg(0, &m, 0, &h_base) == -1 &&
          h.errno_value == AMI_EINVAL,
          "a null list with entries in it is EINVAL");

    /* An empty list is a zero-byte send, not an error. */
    m.msg_iov    = iov;
    m.msg_iovlen = 0;

    CHECK(bsd_sendmsg(0, &m, 0, &h_base) == 0, "no entries sends no bytes");
    CHECK(h.sends == 0, "and puts no packet on the wire");
}

/*
 * The cursor across several entries, including an empty one in the middle:
 * the bytes must reach the wire in order and with no gap.
 */
static void t_iov_coalesce(void)
{
    struct msghdr m;
    struct iovec  iov[4];
    char          a[3], b[1], c[5];

    printf("transfer: several entries become one stream\n");

    h_reset();
    (VOID)h_tcp(0);

    memcpy(a, "abc", 3);
    memcpy(c, "defgh", 5);
    b[0] = 'z';

    iov[0].iov_base = a;  iov[0].iov_len = 3;
    iov[1].iov_base = b;  iov[1].iov_len = 0;      /* skipped entirely */
    iov[2].iov_base = c;  iov[2].iov_len = 5;

    memset(&m, 0, sizeof(m));
    m.msg_iov    = iov;
    m.msg_iovlen = 3;

    CHECK(bsd_sendmsg(0, &m, 0, &h_base) == 8, "eight bytes are sent");
    CHECK(h.wire_len == 8 && memcmp(h.wire, "abcdefgh", 8) == 0,
          "in order, with the empty entry contributing nothing");
    CHECK(h.sends == 1, "and in one segment, because they fit in one MSS");
}

/*
 * The segmentation: a send longer than the MSS becomes several packets, and
 * every byte still arrives once and in order.
 */
static void t_mss_segmentation(void)
{
    char buf[24];
    int  i;

    printf("transfer: a send longer than the segment size\n");

    h_reset();
    (VOID)h_tcp(0);
    h.mss = 10;

    for (i = 0; i < 24; i++)
        buf[i] = (char)('A' + i);

    CHECK(bsd_send(0, buf, 24, 0, &h_base) == 24, "all of it is sent");
    CHECK(h.sends == 3, "in three segments of at most ten bytes");
    CHECK(h.wire_len == 24 && memcmp(h.wire, buf, 24) == 0,
          "and the bytes are the ones the caller gave, in order");
    CHECK(h.nx_enters == 1 && h.nx_leaves == 1,
          "the whole send is one trip into the kernel, not one per segment");

    /* An MSS the socket has not negotiated falls back to 536. */
    h_reset();
    (VOID)h_tcp(0);
    h.mss = 0;

    CHECK(bsd_send(0, buf, 24, 0, &h_base) == 24, "sent");
    CHECK(h.sends == 1,
          "an unnegotiated MSS falls back to a segment big enough for this");
}

/*
 * WHERE 10411a41 WAS.  A send that is cut short after some of it reached the
 * wire must report the bytes that went, not -1.  Reporting the failure loses
 * the caller's place in its own buffer and it resends what the peer already
 * has.
 */
static void t_short_write_is_credited(void)
{
    char buf[24];

    printf("transfer: a send cut short reports what went\n");

    h_reset();
    (VOID)h_tcp(0);
    h.mss = 10;
    memset(buf, 'x', sizeof(buf));

    /* The first two segments go, the third is refused. */
    h.send_plan[0] = NX_SUCCESS;
    h.send_plan[1] = NX_SUCCESS;
    h.send_plan[2] = NX_WINDOW_OVERFLOW;
    h.send_planned = 3;

    CHECK(bsd_send(0, buf, 24, 0, &h_base) == 20,
          "the twenty bytes that reached the wire are reported");
    CHECK(h.fails == 0, "and it is not reported as a failure");
    CHECK(h.sent_bytes == 20, "the wire agrees");
    CHECK(h.releases == 1, "the segment that did not go is released");

    /*
     * The same failure, but the segment that was refused had already put part
     * of itself on the wire.  nx_tcp_socket_send() is not all-or-nothing: it
     * segments the packet, queues what fits, trims that off the caller's
     * packet and only then reports the window as full.  Those bytes are in the
     * peer's sequence space, so a caller told they did not go resends them.
     */
    h_reset();
    (VOID)h_tcp(0);
    h.mss          = 10;
    h.send_plan[0] = NX_SUCCESS;
    h.send_plan[1] = NX_WINDOW_OVERFLOW;
    h.send_planned = 2;
    h.send_trim    = 4;         /* four of the second segment's ten went */

    CHECK(bsd_send(0, buf, 24, 0, &h_base) == 14,
          "the four bytes the refused segment had already queued are credited");
    CHECK(h.sent_bytes == 14, "and that is what the wire carried");

    /* And with nothing at all queued before the refusal, the credit is zero
       rather than the whole segment. */
    h_reset();
    (VOID)h_tcp(0);
    h.mss          = 10;
    h.send_plan[0] = NX_SUCCESS;
    h.send_plan[1] = NX_WINDOW_OVERFLOW;
    h.send_planned = 2;
    h.send_trim    = 0;

    CHECK(bsd_send(0, buf, 24, 0, &h_base) == 10,
          "a refusal that queued nothing credits nothing");

    /* The same failure with nothing sent yet IS a failure. */
    h_reset();
    (VOID)h_tcp(0);
    h.mss = 10;
    h.send_plan[0] = NX_WINDOW_OVERFLOW;
    h.send_planned = 1;

    CHECK(bsd_send(0, buf, 24, 0, &h_base) == -1,
          "a first segment that cannot go is an error");
    CHECK(h.errno_value == AMI_ETIMEDOUT || h.errno_value == AMI_EWOULDBLOCK,
          "and the errno says the send timed out rather than failing");

    /* A peer that has gone away is EPIPE, and the socket stops claiming to be
       connected. */
    h_reset();
    (VOID)h_tcp(0);
    h.send_plan[0] = NX_NOT_CONNECTED;
    h.send_planned = 1;

    CHECK(bsd_send(0, buf, 8, 0, &h_base) == -1 &&
          h.errno_value == AMI_EPIPE,
          "a disconnected peer is EPIPE");
    CHECK((h_sock[0].as_Flags & ASF_CONNECTED) == 0,
          "and the socket stops claiming to be connected");
}

/*
 * A packet that cannot be allocated at all, with nothing sent, is the pool
 * being empty rather than a send failure.
 */
static void t_no_packet(void)
{
    char buf[8];

    printf("transfer: no packet to put it in\n");

    h_reset();
    (VOID)h_tcp(0);
    h.alloc_plan[0] = NX_NO_PACKET;
    h.alloc_planned = 1;

    CHECK(bsd_send(0, buf, 8, 0, &h_base) == -1, "the send fails");
    CHECK(h.sends == 0, "with nothing on the wire");

    /* And with the pool gone entirely. */
    h_reset();
    (VOID)h_tcp(0);
    h.no_pool = TRUE;

    CHECK(bsd_send(0, buf, 8, 0, &h_base) == -1 &&
          h.errno_value == AMI_ENETDOWN,
          "no packet pool at all is ENETDOWN");

    /* A kernel that will not let us in. */
    h_reset();
    (VOID)h_tcp(0);
    h.nx_enter_result = -1;

    CHECK(bsd_send(0, buf, 8, 0, &h_base) == -1 &&
          h.errno_value == AMI_ENETDOWN,
          "a kernel that cannot be entered is ENETDOWN");
    CHECK(h.nx_leaves == 0, "and nothing is left held");
}

/*
 * MSG_DONTWAIT is per transfer: it must not be remembered, and it must reach
 * the wait option for this call only.
 */
static void t_dontwait(void)
{
    char buf[8];

    printf("transfer: MSG_DONTWAIT is per call\n");

    h_reset();
    (VOID)h_tcp(0);

    CHECK(bsd_send(0, buf, 8, MSG_DONTWAIT, &h_base) == 8, "the send goes");
    CHECK(h.wait_option == NX_NO_WAIT, "with no wait");

    CHECK(bsd_send(0, buf, 8, 0, &h_base) == 8, "and the next one goes");
    CHECK(h.wait_option != NX_NO_WAIT,
          "with the socket's own timeout, because the flag was not kept");
    CHECK((h_sock[0].as_Flags & ASF_NONBLOCK) == 0,
          "and the socket is not left non-blocking");
}

/*
 * The write half being shut down is EPIPE on a datagram socket, and a stream
 * socket keeps its own check inside the send loop.
 */
static void t_shutdown(void)
{
    char buf[8];

    printf("transfer: a shut-down write half\n");

    h_reset();
    {
        AmiSocket *s = h_udp(1);

        s->as_Flags |= ASF_CONNECTED | ASF_WRSHUT;
    }

    CHECK(bsd_send(1, buf, 8, 0, &h_base) == -1 &&
          h.errno_value == AMI_EPIPE,
          "a datagram socket with the write half shut is EPIPE");

    h_reset();
    {
        AmiSocket *s = h_tcp(0);

        s->as_Flags |= ASF_WRSHUT;
    }

    CHECK(bsd_send(0, buf, 8, 0, &h_base) == -1 &&
          h.errno_value == AMI_EPIPE,
          "and so is a stream socket");

    /* The read half shut is end of file, not an error. */
    h_reset();
    {
        AmiSocket *s = h_udp(1);

        s->as_Flags |= ASF_RDSHUT;
    }

    CHECK(bsd_recv(1, buf, 8, 0, &h_base) == 0,
          "a datagram socket with the read half shut is end of file");
}

/*
 * recvmsg()'s out parameters.  msg_flags is not an input and whatever the
 * caller left there must not survive; msg_controllen is value-result and a
 * failed call leaves nothing claimed in it.
 */
static void t_recvmsg_outputs(void)
{
    struct msghdr m;
    struct iovec  iov;
    char          buf[8];

    printf("transfer: recvmsg() output fields\n");

    h_reset();
    (VOID)h_tcp(0);

    iov.iov_base = buf;
    iov.iov_len  = sizeof(buf);

    memset(&m, 0, sizeof(m));
    m.msg_iov        = &iov;
    m.msg_iovlen     = 1;
    m.msg_flags      = 0x5A5A;
    m.msg_controllen = 64;

    (VOID)bsd_recvmsg(0, &m, 0, &h_base);

    CHECK(m.msg_flags != 0x5A5A,
          "msg_flags is an output, so what the caller left there is gone");
    CHECK(m.msg_controllen == 0,
          "and nothing is claimed in msg_controllen when nothing was attached");

    /* A malformed list is refused before any of that. */
    h_reset();
    (VOID)h_tcp(0);
    memset(&m, 0, sizeof(m));
    m.msg_iov        = NULL;
    m.msg_iovlen     = 1;
    m.msg_controllen = 64;

    CHECK(bsd_recvmsg(0, &m, 0, &h_base) == -1 &&
          h.errno_value == AMI_EINVAL,
          "a malformed list is EINVAL");
}

/*
 * The MHT_Send monitoring hook can refuse a send before any of it happens,
 * and the refusal is the caller's errno.
 */
static void t_send_monitor(void)
{
    char buf[8];

    printf("transfer: the send monitoring hook\n");

    h_reset();
    (VOID)h_tcp(0);
    h.monitor_present = TRUE;
    h.monitor_verdict = AMI_EACCES;

    CHECK(bsd_send(0, buf, 8, 0, &h_base) == -1 &&
          h.errno_value == AMI_EACCES,
          "a hook that refuses sets the errno it names");
    CHECK(h.sends == 0, "and nothing reaches the wire");
    CHECK(h.monitor_calls == 1, "the hook was asked once");

    /* A hook that allows it does not change the outcome. */
    h_reset();
    (VOID)h_tcp(0);
    h.monitor_present = TRUE;
    h.monitor_verdict = 0;

    CHECK(bsd_send(0, buf, 8, 0, &h_base) == 8, "a hook that allows it lets "
          "the send through");

    /* No hook installed at all: not asked. */
    h_reset();
    (VOID)h_tcp(0);

    CHECK(bsd_send(0, buf, 8, 0, &h_base) == 8, "sent");
    CHECK(h.monitor_calls == 0, "with no hook, nothing is dispatched");
}

int main(void)
{
    printf("transfer.c host checks\n\n");

    t_abi();
    t_refusals();
    t_iov_total();
    t_iov_coalesce();
    t_mss_segmentation();
    t_short_write_is_credited();
    t_no_packet();
    t_dontwait();
    t_shutdown();
    t_recvmsg_outputs();
    t_send_monitor();

    printf("\n%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
