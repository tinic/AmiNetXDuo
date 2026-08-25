/*
 * src/bsdsocket/select.c on the host: the readiness predicates and what
 * WaitSelect() returns.
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

#define H_FDS           4       /* descriptors the fake table holds          */

#define H_EVENT_SIG     (1UL << 12)     /* sb_EventSigMask                   */
#define H_BREAK_SIG     (1UL << 13)     /* sb_BreakMask, Ctrl-C's stand-in   */
#define H_USER_SIG      (1UL << 14)     /* the caller's own, in `signals`    */
#define H_SIGEVENT_SIG  (1UL << 16)     /* sb_SigEventMask, SO_EVENTMASK     */
#define H_SIGIO_SIG     (1UL << 17)     /* sb_SigIOMask                      */
#define H_SIGURG_SIG    (1UL << 18)     /* sb_SigUrgMask                     */

#define H_TIMER_BIT     20              /* what ami_signal_alloc() hands out */
#define H_TIMER_SIG     (1UL << H_TIMER_BIT)

#define H_TX_QUEUE_MAX  8               /* nx_tcp_socket_transmit_queue_maximum */

static struct AmiSocketBase h_base;
static struct Task          h_task;

static AmiSocket   h_sock[H_FDS];
static AmiSocket  *h_table[H_FDS];

/* Fake datagrams for the UDP receive queue.  nx_packet_length carries the
   source port the stub matches on; nothing here parses a header. */
#define H_PKTS 3
static NX_PACKET h_pkt[H_PKTS];

static struct
{
    ULONG        signals;           /* the task's pending signal set         */

    LONG         nx_enter_result;   /* 0 succeeds, -1 is "kernel is down"    */
    ULONG        nx_enters;
    ULONG        nx_leaves;

    ULONG        tcp_bytes_available;
    AmiSocket   *incoming_ready;    /* what bsd_incoming_first_ready() finds */

    /* Wait() is scripted: each call takes the next entry.  Running off the
       end is a defect in the test or in the code, not a value to invent. */
    ULONG        wait_plan[4];
    unsigned     wait_planned;
    unsigned     wait_calls;
    ULONG        last_wait_mask;

    /* A socket that becomes established while the caller is blocked, which is
       what a NetX Duo callback on the IP thread does to it.  Applied inside
       Wait(), because nothing else can change the fixture mid-call. */
    AmiSocket   *wait_establishes;

    ULONG        opens;             /* OpenDevice()                          */
    BYTE         open_result;
    ULONG        sendios;
    ULONG        abortios;
    ULONG        waitios;
    ULONG        signal_calls;
    ULONG        last_signalled;

    ULONG        notifies;          /* nx_*_notify() setters that were armed */

    BOOL         udp_from_peer;     /* what bsd_udp_from_peer() answers      */
    ULONG        ticks;
} h;

static void h_unreachable(const char *what)
{
    printf("  FAIL unreachable call: %s\n", what);
    h_failures++;
    abort();
}

static void h_reset(void)
{
    memset(&h_base, 0, sizeof(h_base));
    memset(&h_sock, 0, sizeof(h_sock));
    memset(&h_table, 0, sizeof(h_table));
    memset(&h_pkt, 0, sizeof(h_pkt));
    memset(&h, 0, sizeof(h));

    h_base.sb_Task          = &h_task;
    h_base.sb_Table         = h_table;
    h_base.sb_TableSize     = H_FDS;
    h_base.sb_EventSigMask  = H_EVENT_SIG;
    h_base.sb_BreakMask     = H_BREAK_SIG;
    h_base.sb_SigEventMask  = H_SIGEVENT_SIG;
    h_base.sb_SigIOMask     = H_SIGIO_SIG;
    h_base.sb_SigUrgMask    = H_SIGURG_SIG;

    h.open_result = 0;              /* timer.device opens                    */
}

static AmiSocket *h_udp(LONG fd, UINT peer)
{
    AmiSocket *s = &h_sock[fd];

    s->as_Owner   = &h_base;
    s->as_Flags   = ASF_UDP;
    s->as_PeerPort = peer;
    if (peer != 0)
        s->as_Flags |= ASF_CONNECTED;

    h_table[fd] = s;
    return s;
}

static AmiSocket *h_tcp(LONG fd, UINT state)
{
    AmiSocket *s = &h_sock[fd];

    s->as_Owner = &h_base;
    s->as_Flags = ASF_TCP | ASF_CONNECTED;
    s->as_Nx.tcp.nx_tcp_socket_state                  = state;
    s->as_Nx.tcp.nx_tcp_socket_transmit_queue_maximum = H_TX_QUEUE_MAX;
    s->as_Nx.tcp.nx_tcp_socket_transmit_sent_count    = 0;

    h_table[fd] = s;
    return s;
}

static void h_queue(AmiSocket *s, const UINT *ports, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++)
    {
        h_pkt[i].nx_packet_length     = ports[i];
        h_pkt[i].nx_packet_queue_next = (i + 1 < n) ? &h_pkt[i + 1] : NX_NULL;
    }

    s->as_Nx.udp.nx_udp_socket_receive_head  = (n > 0) ? &h_pkt[0] : NX_NULL;
    s->as_Nx.udp.nx_udp_socket_receive_count = n;
}

ULONG SetSignal(ULONG newSignals, ULONG signalSet)
{
    ULONG old = h.signals;

    h.signals = (old & ~signalSet) | (newSignals & signalSet);

    return old;
}

VOID Signal(struct Task *task, ULONG signalSet)
{
    (VOID)task;
    h.signals       |= signalSet;
    h.signal_calls++;
    h.last_signalled = signalSet;
}

ULONG Wait(ULONG signalSet)
{
    ULONG arrived;

    h.last_wait_mask = signalSet;

    if (h.wait_calls >= h.wait_planned)
        h_unreachable("Wait");

    arrived = h.wait_plan[h.wait_calls++] & signalSet;
    h.signals &= ~arrived;

    if (h.wait_establishes != NULL)
    {
        h.wait_establishes->as_Nx.tcp.nx_tcp_socket_state = NX_TCP_ESTABLISHED;
        h.wait_establishes = NULL;
    }

    return arrived;
}

VOID Forbid(VOID) { }
VOID Permit(VOID) { }

BYTE OpenDevice(const UBYTE *devName, ULONG unit, struct IORequest *io,
                ULONG flags)
{
    (VOID)devName; (VOID)unit; (VOID)io; (VOID)flags;
    h.opens++;
    return h.open_result;
}

VOID SendIO(struct IORequest *io)  { (VOID)io; h.sendios++;  }
LONG AbortIO(struct IORequest *io) { (VOID)io; h.abortios++; return 0; }
LONG WaitIO(struct IORequest *io)  { (VOID)io; h.waitios++;  return 0; }

BYTE ami_signal_alloc(VOID)        { return (BYTE)H_TIMER_BIT; }
VOID ami_signal_free(BYTE sig)     { (VOID)sig; }

LONG bsd_fail(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;
    return -1;
}

VOID bsd_set_errno(struct AmiSocketBase *base, LONG code)
{
    base->sb_Errno = code;
}

AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd)
{
    (VOID)base;

    if (fd < 0 || fd >= H_FDS)
        return NULL;

    return h_table[fd];
}

LONG bsd_table_size(struct AmiSocketBase *base) { (VOID)base; return H_FDS; }

VOID bsd_bzero(APTR p, ULONG size) { memset(p, 0, size); }

LONG bsd_nx_enter(struct AmiSocketBase *base)
{
    (VOID)base;
    h.nx_enters++;
    return h.nx_enter_result;
}

VOID bsd_nx_leave(struct AmiSocketBase *base) { (VOID)base; h.nx_leaves++; }

AmiSocket *bsd_incoming_first_ready(const AmiSocket *listener)
{
    (VOID)listener;
    return h.incoming_ready;
}

LONG bsd_errno_from_nx(UINT status) { return (LONG)status; }

BOOL bsd_udp_from_peer(const AmiSocket *sock, const NXD_ADDRESS *src,
                       UINT src_port, ULONG src_scope)
{
    (VOID)sock; (VOID)src; (VOID)src_port; (VOID)src_scope;
    return h.udp_from_peer;
}

BOOL bsd_udp_accepts_packet(const AmiSocket *sock, const NX_PACKET *packet)
{
    if ((sock->as_Flags & ASF_CONNECTED) == 0)
        return TRUE;

    return (packet->nx_packet_length == sock->as_PeerPort) ? TRUE : FALSE;
}

/* netstack_ip() answering NULL is what keeps bsd_listen_refill() out of the
   two NetX server-socket calls below; a listen refill is a NetX Duo
   conversation and belongs on the emulator. */
NX_IP *netstack_ip(VOID) { return NX_NULL; }

UINT _nxe_tcp_socket_bytes_available(NX_TCP_SOCKET *socket_ptr,
                                     ULONG *bytes_available)
{
    (VOID)socket_ptr;
    *bytes_available = h.tcp_bytes_available;
    return NX_SUCCESS;
}

UINT _nxe_tcp_socket_receive_notify(NX_TCP_SOCKET *socket_ptr,
                                    VOID (*fn)(NX_TCP_SOCKET *socket_ptr))
{
    (VOID)socket_ptr; (VOID)fn; h.notifies++; return NX_SUCCESS;
}

UINT _nxe_tcp_socket_window_update_notify_set(NX_TCP_SOCKET *socket_ptr,
                                              VOID (*fn)(NX_TCP_SOCKET *socket_ptr))
{
    (VOID)socket_ptr; (VOID)fn; h.notifies++; return NX_SUCCESS;
}

UINT _nxe_tcp_socket_establish_notify(NX_TCP_SOCKET *socket_ptr,
                                      VOID (*fn)(NX_TCP_SOCKET *socket_ptr))
{
    (VOID)socket_ptr; (VOID)fn; h.notifies++; return NX_SUCCESS;
}

UINT _nxe_tcp_socket_disconnect_complete_notify(NX_TCP_SOCKET *socket_ptr,
                                                VOID (*fn)(NX_TCP_SOCKET *socket_ptr))
{
    (VOID)socket_ptr; (VOID)fn; h.notifies++; return NX_SUCCESS;
}

UINT _nxe_udp_socket_receive_notify(NX_UDP_SOCKET *socket_ptr,
                                    VOID (*fn)(NX_UDP_SOCKET *socket_ptr))
{
    (VOID)socket_ptr; (VOID)fn; h.notifies++; return NX_SUCCESS;
}

UINT _nxe_udp_socket_icmp_error_notify(NX_UDP_SOCKET *socket_ptr,
                                       UINT (*fn)(NX_UDP_SOCKET *socket_ptr,
                                                  UINT error_code,
                                                  NXD_ADDRESS *peer_address,
                                                  UINT peer_port))
{
    (VOID)socket_ptr; (VOID)fn; h.notifies++; return NX_SUCCESS;
}

UINT _nxe_tcp_server_socket_accept(NX_TCP_SOCKET *socket_ptr, ULONG wait_option)
{
    (VOID)socket_ptr; (VOID)wait_option;
    h_unreachable("nx_tcp_server_socket_accept");
    return NX_SUCCESS;
}

UINT _nxe_tcp_server_socket_relisten(NX_IP *ip_ptr, UINT port,
                                     NX_TCP_SOCKET *socket_ptr)
{
    (VOID)ip_ptr; (VOID)port; (VOID)socket_ptr;
    h_unreachable("nx_tcp_server_socket_relisten");
    return NX_SUCCESS;
}

ULONG _tx_time_get(VOID) { return h.ticks; }

typedef struct
{
    ULONG read[BSD_FD_WORDS];
    ULONG write[BSD_FD_WORDS];
    ULONG except[BSD_FD_WORDS];
} HSets;

static void h_set(ULONG *words, LONG fd)
{
    words[(ULONG)fd / 32] |= 1UL << ((ULONG)fd % 32);
}

static BOOL h_isset(const ULONG *words, LONG fd)
{
    return (words[(ULONG)fd / 32] & (1UL << ((ULONG)fd % 32))) != 0;
}

/* A zero timeout: the poll-only path, which returns without ever reaching
   Wait() or timer.device. */
static struct timeval h_poll = { 0, 0 };

static void t_udp_readability(void)
{
    AmiSocket *s;
    UINT       one_wrong[1] = { 9999 };
    UINT       wrong_then_right[2] = { 9999, 7777 };
    UINT       one_right[1] = { 7777 };

    printf("bsd_readable(): a connected UDP socket and its peer\n");

    h_reset();
    s = h_udp(0, 7777);
    CHECK(bsd_readable(s) == FALSE, "an empty queue is not readable");

    h_reset();
    s = h_udp(0, 7777);
    h_queue(s, one_wrong, 1);
    CHECK(bsd_readable(s) == FALSE,
          "another peer's datagram is not this socket's readability");

    h_reset();
    s = h_udp(0, 7777);
    h_queue(s, wrong_then_right, 2);
    CHECK(bsd_readable(s) == TRUE,
          "a matching datagram behind a mismatch is still found");

    h_reset();
    s = h_udp(0, 7777);
    h_queue(s, one_right, 1);
    CHECK(bsd_readable(s) == TRUE, "the peer's own datagram is readable");

    /* Unconnected: NetX queues by port and there is no peer to disagree with,
       so every datagram on the port is this socket's. */
    h_reset();
    s = h_udp(0, 0);
    h_queue(s, one_wrong, 1);
    CHECK(bsd_readable(s) == TRUE,
          "an unconnected socket takes whatever the port queued");

    /* An ICMP error is reported ahead of any datagram, because that is what
       nx_udp_socket_receive() will hand back next. */
    h_reset();
    s = h_udp(0, 7777);
    s->as_SoError = 111;
    CHECK(bsd_readable(s) == TRUE, "a pending ICMP error is readability");

    h_reset();
    s = h_udp(0, 7777);
    s->as_Flags |= ASF_RDSHUT;
    h_queue(s, one_wrong, 1);
    CHECK(bsd_readable(s) == TRUE, "shutdown(SHUT_RD) is an immediate EOF");
}

static void t_tcp_readability(void)
{
    AmiSocket *s;

    printf("bsd_readable(): TCP state and the half-close\n");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    CHECK(bsd_readable(s) == FALSE, "an idle established socket is not readable");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    s->as_Nx.tcp.nx_tcp_socket_receive_queue_count = 1;
    CHECK(bsd_readable(s) == TRUE, "a queued segment is readable");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    h.tcp_bytes_available = 42;
    CHECK(bsd_readable(s) == TRUE, "bytes available are readable");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    s->as_Flags |= ASF_EOF;
    CHECK(bsd_readable(s) == TRUE, "a closed connection reads end-of-file");

    h_reset();
    s = h_tcp(0, NX_TCP_CLOSE_WAIT);
    CHECK(bsd_readable(s) == TRUE, "CLOSE_WAIT: the peer's FIN arrived");

    h_reset();
    s = h_tcp(0, NX_TCP_LAST_ACK);
    CHECK(bsd_readable(s) == TRUE, "LAST_ACK: the peer's FIN arrived");

    h_reset();
    s = h_tcp(0, NX_TCP_FIN_WAIT_1);
    CHECK(bsd_readable(s) == FALSE,
          "FIN_WAIT_1 after shutdown(SHUT_WR) is not readable");

    h_reset();
    s = h_tcp(0, NX_TCP_FIN_WAIT_2);
    CHECK(bsd_readable(s) == FALSE,
          "FIN_WAIT_2 after shutdown(SHUT_WR) is not readable");

    h_reset();
    s = &h_sock[0];
    s->as_Owner = &h_base;
    s->as_Flags = ASF_TCP | ASF_LISTENING | ASF_ACCEPTPEND;
    h_table[0]  = s;
    h.incoming_ready = NULL;
    CHECK(bsd_readable(s) == FALSE,
          "a listener with a SYN but no finished handshake is not readable");

    h.incoming_ready = &h_sock[1];
    CHECK(bsd_readable(s) == TRUE,
          "a listener with a finished handshake is readable");
}

static void t_writability(void)
{
    AmiSocket *s;

    printf("bsd_writable()\n");

    h_reset();
    s = h_udp(0, 7777);
    CHECK(bsd_writable(s) == TRUE, "a UDP socket always takes a write");

    h_reset();
    s = &h_sock[0];
    s->as_Owner = &h_base;
    s->as_Flags = ASF_RAW;
    CHECK(bsd_writable(s) == TRUE, "a raw socket always takes a write");

    h_reset();
    s = h_tcp(0, NX_TCP_SYN_SENT);
    s->as_Flags = (s->as_Flags & ~ASF_CONNECTED) | ASF_CONNECTING;
    CHECK(bsd_writable(s) == FALSE,
          "a connect still in the handshake is not writable");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    s->as_Flags = (s->as_Flags & ~ASF_CONNECTED) | ASF_CONNECTING;
    CHECK(bsd_writable(s) == TRUE,
          "a completed non-blocking connect reports as writable");

    h_reset();
    s = h_tcp(0, NX_TCP_CLOSED);
    s->as_Flags = (s->as_Flags & ~ASF_CONNECTED) | ASF_CONNECTING;
    CHECK(bsd_writable(s) == TRUE,
          "a failed non-blocking connect reports as writable too");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    s->as_Nx.tcp.nx_tcp_socket_transmit_sent_count = 1;
    CHECK(bsd_writable(s) == TRUE, "room in the transmit queue is writability");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    s->as_Nx.tcp.nx_tcp_socket_transmit_sent_count = H_TX_QUEUE_MAX;
    CHECK(bsd_writable(s) == FALSE, "a full transmit queue is not writable");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    s->as_SoError = 104;
    CHECK(bsd_writable(s) == TRUE,
          "a pending error is writable, so SO_ERROR gets read");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    s->as_Flags |= ASF_WRSHUT;
    CHECK(bsd_writable(s) == TRUE,
          "after shutdown(SHUT_WR) the write fails immediately, so it is writable");
}

static void t_exception(void)
{
    AmiSocket *s;

    printf("bsd_exception()\n");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    CHECK(bsd_exception(s) == FALSE, "a healthy socket is not exceptional");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    s->as_Events = FD_OOB;
    CHECK(bsd_exception(s) == TRUE, "an urgent-data event is exceptional");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    s->as_Flags |= ASF_OOBHAVE;
    CHECK(bsd_exception(s) == TRUE, "a held urgent byte is exceptional");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    s->as_SoError = 104;
    CHECK(bsd_exception(s) == TRUE, "a pending SO_ERROR is exceptional");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    s->as_Events = FD_ERROR;
    CHECK(bsd_exception(s) == FALSE,
          "the FD_ERROR latch alone is not an exceptional condition");
}

static void t_waitselect_count(void)
{
    HSets      s;
    AmiSocket *sock;
    LONG       n;
    UINT       one_right[1] = { 7777 };

    printf("WaitSelect(): the count is bits set, not descriptors\n");

    h_reset();
    sock = h_udp(0, 7777);
    h_queue(sock, one_right, 1);
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);
    h_set(s.write, 0);

    n = bsd_WaitSelect(1, s.read, s.write, NULL, &h_poll, NULL, &h_base);
    CHECK(n == 2, "one socket ready to read and to write counts 2, not 1");
    CHECK(h_isset(s.read, 0),  "and it is marked readable");
    CHECK(h_isset(s.write, 0), "and it is marked writable");

    /* A connected UDP socket holding an ICMP error is readable, writable and
       exceptional at once: 3 under AmiTCP. */
    h_reset();
    sock = h_udp(0, 7777);
    sock->as_SoError = 111;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);
    h_set(s.write, 0);
    h_set(s.except, 0);

    n = bsd_WaitSelect(1, s.read, s.write, s.except, &h_poll, NULL, &h_base);
    CHECK(n == 3, "readable, writable and exceptional at once counts 3");
    CHECK(h_isset(s.read, 0) && h_isset(s.write, 0) && h_isset(s.except, 0),
          "and all three bits are set");

    h_reset();
    (void)h_udp(0, 7777);
    (void)h_udp(1, 7777);
    h_queue(&h_sock[0], one_right, 1);
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);
    h_set(s.write, 1);

    n = bsd_WaitSelect(2, s.read, s.write, NULL, &h_poll, NULL, &h_base);
    CHECK(n == 2, "two descriptors ready in one set each also counts 2");

    /* Nothing ready and a zero timeout: 0, and the result sets are cleared
       rather than left holding the caller's request. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);
    h_set(s.write, 0);

    n = bsd_WaitSelect(1, s.read, s.write, NULL, &h_poll, NULL, &h_base);
    CHECK(n == 0, "a poll that finds nothing returns 0");
    CHECK(!h_isset(s.read, 0) && !h_isset(s.write, 0),
          "and the caller's sets come back cleared");
}

static void t_waitselect_refusals(void)
{
    HSets          s, before;
    LONG           n;
    struct timeval bad;

    printf("WaitSelect(): the refusals, and the sets they must not touch\n");

    h_reset();
    n = bsd_WaitSelect(-1, NULL, NULL, NULL, &h_poll, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EINVAL, "a negative nfds is EINVAL");

    /* A descriptor named in a set with no socket behind it fails the whole
       call, and the autodoc requires the sets to come back untouched. */
    h_reset();
    (void)h_udp(0, 7777);
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);
    h_set(s.read, 2);           /* nothing at 2 */
    h_set(s.write, 0);
    before = s;

    n = bsd_WaitSelect(3, s.read, s.write, NULL, &h_poll, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EBADF,
          "a descriptor with no socket is EBADF");
    CHECK(memcmp(&s, &before, sizeof(s)) == 0,
          "and a failed WaitSelect leaves the caller's sets exactly as they were");

    h_reset();
    bad.tv_secs = 0;
    bad.tv_micro = 1000000;
    n = bsd_WaitSelect(0, NULL, NULL, NULL, &bad, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EINVAL,
          "a microsecond count of 1000000 is EINVAL");

    h_reset();
    bad.tv_secs = 100000001UL;
    bad.tv_micro = 0;
    n = bsd_WaitSelect(0, NULL, NULL, NULL, &bad, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EINVAL,
          "more than 100000000 seconds is EINVAL");

    /* The kernel is down: nothing can ever become ready, and that is an error
       rather than an empty result. */
    h_reset();
    (void)h_udp(0, 7777);
    h.nx_enter_result = -1;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);
    before = s;

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &h_poll, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_ENETDOWN,
          "a poll that cannot enter the stack is ENETDOWN");
    CHECK(memcmp(&s, &before, sizeof(s)) == 0,
          "and that failure leaves the sets alone as well");
}

static void t_waitselect_signals(void)
{
    HSets      s, before;
    LONG       n;
    ULONG      signals;
    UINT       one_right[1] = { 7777 };

    printf("WaitSelect(): the break mask and the caller's signals\n");

    h_reset();
    (void)h_udp(0, 7777);
    h_queue(&h_sock[0], one_right, 1);      /* ready at entry */
    h.signals = H_BREAK_SIG;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);
    before = s;

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &h_poll, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EINTR,
          "a break pending at entry is EINTR even with a socket ready");
    CHECK(memcmp(&s, &before, sizeof(s)) == 0,
          "and the sets are untouched");
    CHECK((h.signals & H_BREAK_SIG) != 0,
          "and the break signal is still set for the caller's own handling");

    /* The break arriving while blocked: same answer, and the signal is put
       back because Wait() consumed it. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0]  = H_BREAK_SIG;
    h.wait_planned  = 1;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);
    before = s;

    n = bsd_WaitSelect(1, s.read, NULL, NULL, NULL, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EINTR,
          "a break arriving during the wait is EINTR");
    CHECK((h.signals & H_BREAK_SIG) != 0,
          "and the break signal is reposted");
    CHECK(memcmp(&s, &before, sizeof(s)) == 0,
          "and the sets are untouched");

    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.signals = H_USER_SIG;
    signals   = H_USER_SIG;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, NULL, &signals, &h_base);
    CHECK(n == 0, "a caller's signal ends the wait with a count of 0");
    CHECK(signals == H_USER_SIG, "and comes back in the signal mask");
    CHECK((h.signals & H_USER_SIG) == 0,
          "and is consumed, because a signal reported and left standing "
          "is delivered twice");

    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0] = H_USER_SIG;
    h.wait_planned = 1;
    signals = H_USER_SIG;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, NULL, &signals, &h_base);
    CHECK(n == 0 && signals == H_USER_SIG,
          "a caller's signal arriving during the wait ends it the same way");
}

static void t_waitselect_timeout(void)
{
    HSets s;
    LONG  n;
    struct timeval one_second = { 1, 0 };
    UINT  one_right[1] = { 7777 };

    printf("WaitSelect(): timer.device, armed only when about to block\n");

    h_reset();
    (void)h_udp(0, 7777);
    h_queue(&h_sock[0], one_right, 1);
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &one_second, NULL, &h_base);
    CHECK(n == 1, "a ready socket with a timeout returns at once");
    CHECK(h.sendios == 0 && h.opens == 0,
          "and timer.device was never opened or sent to");

    /* Nothing ready: the request is armed once, its signal joins the wait
       mask, and the expiry is a timeout rather than an error. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0] = H_TIMER_SIG;
    h.wait_planned = 1;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &one_second, NULL, &h_base);
    CHECK(n == 0, "an expired timeout returns 0");
    CHECK(h.sendios == 1, "the timeout request was sent exactly once");
    CHECK(h_base.sb_TimerReq.tr_time.tv_secs == 1 &&
          h_base.sb_TimerReq.tr_time.tv_micro == 0,
          "and it carried the caller's timeout");
    CHECK((h.last_wait_mask & H_TIMER_SIG) != 0,
          "and its signal was in the wait mask");
    CHECK(h.waitios == 1 && h.abortios == 0,
          "an expired request is collected with WaitIO and not aborted");
    CHECK(!h_isset(s.read, 0), "and the result set comes back cleared");

    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0]      = H_EVENT_SIG;
    h.wait_planned      = 1;
    h.wait_establishes  = &h_sock[0];
    memset(&s, 0, sizeof(s));
    h_set(s.write, 0);

    n = bsd_WaitSelect(1, NULL, s.write, NULL, &one_second, NULL, &h_base);
    CHECK(n == 1, "a connect that completed during the wait ends it");
    CHECK(h_isset(s.write, 0), "and the descriptor comes back writable");
    CHECK(h.abortios == 1 && h.waitios == 1,
          "and the outstanding timeout request is aborted and collected");

    /* timer.device refusing to open is ENOMEM, not a silent block. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.open_result = -1;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &one_second, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_ENOMEM,
          "a timer.device that will not open is ENOMEM");
}

static void t_events(void)
{
    AmiSocket *s;

    printf("bsd_event_post() and bsd_events_attach()\n");

    h_reset();
    s = h_udp(0, 7777);
    bsd_event_post(s, FD_READ);
    CHECK((h.signals & H_EVENT_SIG) != 0, "every event wakes WaitSelect");
    CHECK((s->as_Events & FD_READ) != 0, "and is latched on the socket");

    /* SetSocketSignals: FD_READ and FD_WRITE are the IO mask's, FD_OOB the
       urgent mask's, and SO_EVENTMASK selects the event mask's. */
    h_reset();
    s = h_udp(0, 7777);
    bsd_event_post(s, FD_WRITE);
    CHECK((h.signals & H_SIGIO_SIG) != 0, "FD_WRITE signals SBTC_SIGIOMASK");
    CHECK((h.signals & H_SIGURG_SIG) == 0, "and not the urgent mask");

    h_reset();
    s = h_udp(0, 7777);
    bsd_event_post(s, FD_OOB);
    CHECK((h.signals & H_SIGURG_SIG) != 0, "FD_OOB signals SBTC_SIGURGMASK");
    CHECK((h.signals & H_SIGIO_SIG) == 0, "and not the IO mask");

    h_reset();
    s = h_udp(0, 7777);
    s->as_EventMask = FD_CLOSE;
    bsd_event_post(s, FD_CLOSE);
    CHECK((h.signals & H_SIGEVENT_SIG) != 0,
          "an event inside SO_EVENTMASK signals SBTC_SIGEVENTMASK");

    h_reset();
    s = h_udp(0, 7777);
    s->as_EventMask = FD_CLOSE;
    bsd_event_post(s, FD_ACCEPT);
    CHECK((h.signals & H_SIGEVENT_SIG) == 0,
          "and one outside it does not");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    bsd_events_attach(s);
    CHECK(h.notifies == 4, "a TCP socket arms four notify hooks");
    CHECK(s->as_Nx.tcp.nx_tcp_socket_reserved_ptr == s,
          "and the callbacks can find their AmiSocket");

    h_reset();
    s = h_udp(0, 7777);
    bsd_events_attach(s);
    CHECK(h.notifies == 2, "a UDP socket arms two");
    CHECK(s->as_Nx.udp.nx_udp_socket_reserved_ptr == s,
          "and the callbacks can find their AmiSocket");

    h_reset();
    s = &h_sock[0];
    s->as_Owner = &h_base;
    s->as_Flags = ASF_RAW;
    bsd_events_attach(s);
    CHECK(h.notifies == 0,
          "a raw socket has no NetX control block to hang one on");
}

int main(void)
{
    printf("select.c host tests\n");

    t_udp_readability();
    t_tcp_readability();
    t_writability();
    t_exception();
    t_waitselect_count();
    t_waitselect_refusals();
    t_waitselect_signals();
    t_waitselect_timeout();
    t_events();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}
