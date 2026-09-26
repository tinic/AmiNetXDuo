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
#include "aminetxduo/sana2.h"

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

/*
 * A fake packet.  Only the members transfer.c and the stubs read are used.
 *
 * THE LENGTH IS `nx.nx_packet_length` AND NOT A MEMBER OF ITS OWN.  It used to
 * be, and the two then disagreed the moment anything read the field instead of
 * calling nx_packet_length_get(): `bsd_packet_length()` in packet_extract.h
 * does exactly that on a non-trace build, which is correct on target -- the
 * field IS the length there -- and read zero here, so every send that was cut
 * short credited the caller with nothing.  One truth, so a stub cannot drift
 * from the target again.
 */
typedef struct HPacket
{
    NX_PACKET   nx;
    BOOL        in_use;
    BOOL        released;
} HPacket;

#define h_len(p)    ((p)->nx.nx_packet_length)

static HPacket h_pkt[H_PKTS];

static struct
{
    LONG        errno_value;
    ULONG       fails;              /* bsd_fail() calls                      */

    LONG        nx_enter_result;
    ULONG       nx_enters, nx_leaves;

    BOOL        no_pool;            /* legacy stub state, see no-pool case   */

    ULONG       mss;
    ULONG       mss_gets;           /* locked nx_tcp_socket_mss_get() calls  */

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

    /* Interface epochs (#51): one counter for every slot, moved by the next
       bsd_nx_enter() when bump_on_enter is set, as a RemoveNetInterface and
       AddNetInterface into the same slot would between a caller's check and
       its bracket. */
    ULONG       live_epoch;
    BOOL        bump_on_enter;
    ULONG       seen_scope;         /* what bsd_source_select() was handed   */

    /* Another task's connect() while the send waits for a packet: the next
       nx_packet_allocate() moves this socket to a different peer. */
    AmiSocket  *reconnect;
    NXD_ADDRESS sent_addr;          /* where the last UDP datagram went      */
    UINT        sent_port;
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
    h_base.sb_StackRefs = 1;
    h_base.sb_StackIp   = &h_ip;
    h_base.sb_StackPool = &h_pool;
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

    if (h.bump_on_enter)
        h.live_epoch++;

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

#ifdef AMINETXDUO_TX_RUN
/* The transmit run (sana2.h): the sockets here have no connect interface,
   so transfer.c hands these NULL and never flushes.  Counted, not modelled. */
static ULONG h_runs_begun, h_runs_ended;

VOID ami_sana2_tx_run_begin(AmiSana2If *iface)
{
    (VOID)iface;
    h_runs_begun++;
}

VOID ami_sana2_tx_run_flush(AmiSana2If *iface)
{
    (VOID)iface;
}

VOID ami_sana2_tx_run_end(AmiSana2If *iface)
{
    (VOID)iface;
    h_runs_ended++;
}
#endif

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

    h.seen_scope = scope;

    if (index != NULL)
        *index = 0;

    /* The real one's bounds check, which BSD_SCOPE_GONE never passes. */
    return (scope > (ULONG)NX_MAX_PHYSICAL_INTERFACES) ? BSD_SOURCE_REFUSE
                                                       : BSD_SOURCE_ROUTE;
}

#ifdef AMINETXDUO_IPV6
/* socket.c's: a zone stored under another epoch is gone. */
ULONG bsd_scope_live(ULONG scope, ULONG epoch)
{
    return (scope == 0UL || epoch == h.live_epoch) ? scope : BSD_SCOPE_GONE;
}
#endif

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

#ifdef AMINETXDUO_TCP_CORK
/* The cork's entry points transfer.c calls.  cork.c is test_cork's
   (test_cork_host.c, which compiles this same transfer.c against it); no
   socket here turns it on, so none of these is reached. */
static void h_cork_unreachable(const char *what)
{
    printf("  FAIL unreachable call: %s\n", what);
    h_failures++;
    abort();
}

BOOL  bsd_cork_corkable(const AmiSocket *sock)
{ (VOID)sock; h_cork_unreachable("bsd_cork_corkable"); return FALSE; }
LONG  bsd_cork_claim(struct AmiSocketBase *base, AmiSocket *sock, ULONG wait,
                     NX_PACKET **pkt)
{ (VOID)base; (VOID)sock; (VOID)wait; (VOID)pkt;
  h_cork_unreachable("bsd_cork_claim"); return 0; }
BOOL  bsd_cork_unclaim(AmiSocket *sock, NX_PACKET *pkt, ULONG why)
{ (VOID)sock; (VOID)pkt; (VOID)why;
  h_cork_unreachable("bsd_cork_unclaim"); return FALSE; }
ULONG bsd_cork_settle(AmiSocket *sock, NX_PACKET **pkt, UINT status)
{ (VOID)sock; (VOID)pkt; (VOID)status;
  h_cork_unreachable("bsd_cork_settle"); return 0; }
VOID  bsd_cork_push(struct AmiSocketBase *base, AmiSocket *sock)
{ (VOID)base; (VOID)sock; h_cork_unreachable("bsd_cork_push"); }
ULONG bsd_cork_room(const AmiSocket *sock)
{ (VOID)sock; h_cork_unreachable("bsd_cork_room"); return 0; }
VOID  bsd_tcp_send_fin(AmiSocket *sock)
{ (VOID)sock; h_cork_unreachable("bsd_tcp_send_fin"); }
VOID  bsd_bcopy(CONST_APTR src, APTR dst, ULONG size)
{ (VOID)src; (VOID)dst; (VOID)size; h_cork_unreachable("bsd_bcopy"); }
#ifdef AMINETXDUO_TCP_CORK_FASTPATH
/* The fast path's lock and tick: it declines before either on a socket that
   never turned the cork on. */
VOID  Forbid(VOID) { h_cork_unreachable("Forbid"); }
VOID  Permit(VOID) { h_cork_unreachable("Permit"); }
VOID  bsd_cork_kick_tick(AmiSocket *sock)
{ (VOID)sock; h_cork_unreachable("bsd_cork_kick_tick"); }
#endif
#endif

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

    if (h.reconnect != NULL)
    {
        h.reconnect->as_PeerAddr.nxd_ip_address.v6[3] = 2UL;
        h.reconnect->as_PeerPort    = 54;
        h.reconnect->as_PeerScopeId = 3UL;
        h.reconnect                 = NULL;
    }

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
        h_len(p) += data_size;

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

    *length = (p != NULL) ? h_len(p) : 0UL;

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

/* The running ThreadX thread, which transfer.c compares against its own
   adopted caller before reading the MSS without the IP mutex. */
TX_THREAD *_tx_thread_current_ptr;

UINT _nxe_tcp_socket_mss_get(NX_TCP_SOCKET *socket_ptr, ULONG *mss)
{
    (VOID)socket_ptr;

    h.mss_gets++;
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
        h.sent_bytes += h_len(p);
        p->in_use     = FALSE;

        return status;
    }

    /* A failure that still put some of it on the wire: trim what went, as
       _nx_tcp_socket_send_internal() does. */
    if (h.send_trim > 0)
    {
        ULONG gone = (h.send_trim < h_len(p)) ? h.send_trim : h_len(p);

        h_len(p)     -= gone;
        h.sent_bytes += gone;
    }

    return status;
}

/* bsd_recv_once() calls the _nx_ entry point directly -- bsdsocket has
   already validated the socket and bsd_nx_need() has supplied the thread
   context, so the _nxe_ wrapper only re-checks what it proved.  Both spellings
   are stubbed: the wrapper is still what the rest of NetX Duo would reach. */
UINT _nx_tcp_socket_receive(NX_TCP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                            ULONG wait_option)
{
    (VOID)socket_ptr;
    (VOID)packet_ptr;
    (VOID)wait_option;

    return NX_NO_PACKET;
}

UINT _nxe_tcp_socket_receive(NX_TCP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                             ULONG wait_option)
{
    return _nx_tcp_socket_receive(socket_ptr, packet_ptr, wait_option);
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

    h.sends++;
    h.sent_addr = *ip_address;
    h.sent_port = port;

    if (p != NULL)
    {
        h.sent_bytes += h_len(p);
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

/* The route resolves to a 1,500-byte Ethernet, so a send path that measured
   a datagram against the link MTU again -- the cap t_datagram_size() retired
   -- would refuse 1473 here and turn that case red. */
static NX_INTERFACE h_ether = { .nx_interface_ip_mtu_size = 1500 };

ULONG _nx_ip_route_find(NX_IP *ip_ptr, ULONG destination_address,
                        NX_INTERFACE **nx_ip_interface, ULONG *next_hop_address)
{
    (VOID)ip_ptr;
    (VOID)destination_address;

    if (nx_ip_interface != NULL)
        *nx_ip_interface = &h_ether;
    if (next_hop_address != NULL)
        *next_hop_address = destination_address;

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
   claim and is not made here.  -1 is mcast.c's "not multicast": 0 would
   route every send down the IPv4 multicast source path, which reads only
   addr->nxd_ip_address.v4. */
LONG bsd_mcast_prepare_send(AmiSocket *sock, const NXD_ADDRESS *addr)
{
    (VOID)sock;
    (VOID)addr;

    return -1;
}

/* This fixture sends only unicast packets; the multicast loop guard itself is
   exercised by test_mcast_loop. */
VOID bsd_mcast_loop_begin(NX_IP *ip, const AmiSocket *sock,
                          const NXD_ADDRESS *addr, BsdMcastLoopGuard *guard)
{
    (VOID)ip;
    (VOID)sock;
    (VOID)addr;
    guard->flag = NULL;
}

VOID bsd_mcast_loop_end(BsdMcastLoopGuard *guard)
{
    (VOID)guard;
}

LONG bsd_mcast6_prepare_send(struct AmiSocketBase *base, AmiSocket *sock,
                             const NXD_ADDRESS *addr, ULONG *saved)
{
    (VOID)base;
    (VOID)sock;
    (VOID)addr;

    *saved = 0UL;

    return -1;
}

VOID bsd_mcast6_finish_send(struct AmiSocketBase *base, ULONG saved)
{
    (VOID)base;
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
 * The MSS without the IP mutex: only when this base's own adopted bracket is
 * the running ThreadX thread.  The locked call answers 10 and the socket's own
 * fields answer 6, so the segment count says which one sized the send.
 */
static void t_mss_peek_guard(void)
{
    static const struct
    {
        BOOL        adopted;
        int         current;        /* 0 NULL, 1 the caller's, 2 another */
        BOOL        peek;
        const char *what;
    } cases[] =
    {
        { FALSE, 1, FALSE, "not adopted: the locked call" },
        { FALSE, 0, FALSE, "not adopted, no thread: the locked call" },
        { TRUE,  0, FALSE, "adopted, no running thread: the locked call" },
        { TRUE,  2, FALSE, "adopted, another thread running: the locked call" },
        { TRUE,  1, TRUE,  "adopted and running: the fields, no mutex" },
    };
    static TX_THREAD mine;              /* the port's slot, in production */
    static TX_THREAD other;
    char             buf[24];
    unsigned         i;

    printf("transfer: the MSS is read without the mutex only by the baton "
           "holder\n");

    memset(buf, 'x', sizeof(buf));

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        AmiSocket *s;

        h_reset();
        s = h_tcp(0);
        h.mss = 10;
        s->as_Nx.tcp.nx_tcp_socket_state       = NX_TCP_ESTABLISHED;
        s->as_Nx.tcp.nx_tcp_socket_connect_mss = 6;

        h_base.sb_NxCaller.nc_Adopted = cases[i].adopted;
        h_base.sb_NxCaller.nc_Thread  = &mine;
        _tx_thread_current_ptr =
            (cases[i].current == 1) ? &mine :
            (cases[i].current == 2) ? &other : NULL;

        CHECK(bsd_send(0, buf, 24, 0, &h_base) == 24, cases[i].what);
        if (cases[i].peek)
            CHECK(h.mss_gets == 0 && h.sends == 4,
                  "  sized by the socket's fields, and no mutex was taken");
        else
            CHECK(h.mss_gets == 1 && h.sends == 3,
                  "  sized by nx_tcp_socket_mss_get(), called once");
    }

    /* The fields before ESTABLISHED: the configured MSS, else 1460. */
    h_reset();
    (VOID)h_tcp(0);
    h_sock[0].as_Nx.tcp.nx_tcp_socket_state = NX_TCP_SYN_SENT;
    h_sock[0].as_Nx.tcp.nx_tcp_socket_mss   = 5;
    h_base.sb_NxCaller.nc_Adopted = TRUE;
    h_base.sb_NxCaller.nc_Thread  = &mine;
    _tx_thread_current_ptr = &mine;
    CHECK(bsd_send(0, buf, 24, 0, &h_base) == 24 && h.sends == 5 &&
          h.mss_gets == 0, "before ESTABLISHED the configured MSS sizes it");

    _tx_thread_current_ptr = NULL;
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
    h_base.sb_StackPool = NULL;

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
 * A datagram larger than the link goes out, fragmented by the stack; only one
 * larger than the protocol allows is refused.  The cap used to be the egress
 * MTU less the headers (1,472 on Ethernet), which refused what NFS over UDP
 * sends on every write and what `ping -s 1473' sends on purpose, while
 * nx_ip_fragment_enable() had been waiting for it since 19ab8d70.  Measured
 * 2026-09-19 on the A1200 through a 1,400-byte hop before the change: 1472
 * crossed, 1473 was "error 40" and never sent.
 */
static void t_datagram_size(void)
{
    static char        big[65508];
    struct sockaddr_in to;

    printf("transfer: a datagram larger than the link\n");

    memset(&to, 0, sizeof(to));
    to.sin_family      = AF_INET;
    to.sin_port        = BSD_HTONS(2049);
    to.sin_addr.s_addr = 0x0A000002UL;
    memset(big, 'x', sizeof(big));

    h_reset();
    (VOID)h_udp(1);
    CHECK(bsd_sendto(1, big, 1473, 0, (struct sockaddr *)&to, sizeof(to),
                     &h_base) == 1473 && h.sends == 1,
          "1473 bytes, one more than a full frame carries, are sent");

    h_reset();
    (VOID)h_udp(1);
    CHECK(bsd_sendto(1, big, 8192, 0, (struct sockaddr *)&to, sizeof(to),
                     &h_base) == 8192 && h.sends == 1,
          "an 8 KB NFS block is sent");

    h_reset();
    (VOID)h_udp(1);
    CHECK(bsd_sendto(1, big, 65507, 0, (struct sockaddr *)&to, sizeof(to),
                     &h_base) == 65507 && h.sends == 1,
          "65507 bytes, the most UDP over IPv4 carries, are sent");

    h_reset();
    (VOID)h_udp(1);
    CHECK(bsd_sendto(1, big, 65508, 0, (struct sockaddr *)&to, sizeof(to),
                     &h_base) == -1 &&
          h.errno_value == AMI_EMSGSIZE && h.sends == 0 && h.allocs == 0,
          "65508 is EMSGSIZE, and nothing was allocated for it");
}

#ifdef AMINETXDUO_IPV6
/*
 * A connected zone is resolved inside the bracket (#51).  The slot is
 * reused between the call starting and bsd_nx_enter(): send(), sendto() with
 * no address and sendmsg() with no name must all refuse, as the sticky
 * IPV6_PKTINFO and bound-zone paths do.
 */
static AmiSocket *h_udp6_connected(LONG fd)
{
    static const ULONG ll_peer[4] = { 0xFE800000UL, 0, 0, 1 };
    AmiSocket         *s          = h_udp(fd);

    s->as_Flags |= ASF_INET6 | ASF_CONNECTED | ASF_NXBOUND | ASF_BOUND;
    s->as_PeerAddr.nxd_ip_version = NX_IP_VERSION_V6;
    memcpy(s->as_PeerAddr.nxd_ip_address.v6, ll_peer, sizeof(ll_peer));
    s->as_PeerPort       = 53;
    s->as_PeerScopeId    = 2UL;
    s->as_PeerScopeEpoch = 0UL;

    return s;
}

static LONG h_send_shape(int shape, char *buf)
{
    struct iovec  iov;
    struct msghdr m;

    switch (shape)
    {
    case 0:
        return bsd_send(1, buf, 4, 0, &h_base);
    case 1:
        return bsd_sendto(1, buf, 4, 0, NULL, 0, &h_base);
    default:
        iov.iov_base = buf;
        iov.iov_len  = 4;
        memset(&m, 0, sizeof(m));
        m.msg_iov    = &iov;
        m.msg_iovlen = 1;
        return bsd_sendmsg(1, &m, 0, &h_base);
    }
}

static void t_peer_scope_in_bracket(void)
{
    static const char *const shape[3] = { "send", "sendto(NULL)",
                                          "sendmsg(no name)" };
    char buf[4] = { 1, 2, 3, 4 };
    char what[96];
    int  i;

    printf("transfer: a connected zone is resolved inside the bracket\n");

    for (i = 0; i < 3; i++)
    {
        h_reset();
        (VOID)h_udp6_connected(1);
        snprintf(what, sizeof(what), "%s: live zone sends on slot 1",
                 shape[i]);
        CHECK(h_send_shape(i, buf) == 4 && h.sends == 1 &&
                  h.seen_scope == 2UL, what);

        h_reset();
        (VOID)h_udp6_connected(1);
        h.bump_on_enter = TRUE;
        snprintf(what, sizeof(what),
                 "%s: slot reused before the bracket is EADDRNOTAVAIL",
                 shape[i]);
        CHECK(h_send_shape(i, buf) == -1 &&
                  h.errno_value == AMI_EADDRNOTAVAIL && h.sends == 0 &&
                  h.allocs == 0 && h.nx_enters == h.nx_leaves, what);

        /* connect() to fe80::2%3 port 54 while this send waits. */
        h_reset();
        h.reconnect = h_udp6_connected(1);
        snprintf(what, sizeof(what),
                 "%s: a connect() during the wait does not redirect it",
                 shape[i]);
        CHECK(h_send_shape(i, buf) == 4 && h.sends == 1 &&
                  h.sent_addr.nxd_ip_version == NX_IP_VERSION_V6 &&
                  h.sent_addr.nxd_ip_address.v6[0] == 0xFE800000UL &&
                  h.sent_addr.nxd_ip_address.v6[3] == 1UL &&
                  h.sent_port == 53 && h.seen_scope == 2UL, what);
    }
}
#endif

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
    t_mss_peek_guard();
    t_short_write_is_credited();
    t_no_packet();
    t_dontwait();
    t_shutdown();
    t_datagram_size();
#ifdef AMINETXDUO_IPV6
    t_peer_scope_in_bracket();
#endif
    t_recvmsg_outputs();
    t_send_monitor();

    printf("\n%lu checks, %lu failures\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
