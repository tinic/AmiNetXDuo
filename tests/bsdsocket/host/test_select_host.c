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
#define H_USER2_SIG     (1UL << 15)     /* a second one the caller asks for  */
#define H_SIGEVENT_SIG  (1UL << 16)     /* sb_SigEventMask, SO_EVENTMASK     */
#define H_SIGIO_SIG     (1UL << 17)     /* sb_SigIOMask                      */
#define H_SIGURG_SIG    (1UL << 18)     /* sb_SigUrgMask                     */

#define H_TIMER_BIT     20              /* what ami_signal_alloc() hands out */
#define H_TIMER_SIG     (1UL << H_TIMER_BIT)

#define H_TX_QUEUE_MAX  8               /* nx_tcp_socket_transmit_queue_maximum */

static struct AmiSocketBase h_base;
static struct Task          h_task;
static struct Task          h_other_task;
static struct Task         *h_me;  /* who is calling; NULL = the opener */

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
    LONG         wait_nx_enter_result; /* value installed by the next Wait */
    ULONG        wait_post_signal;     /* a signal arriving right after Wait */

    ULONG        opens;             /* OpenDevice()                          */
    BYTE         open_result;
    ULONG        sendios;
    ULONG        abortios;
    ULONG        waitios;
    ULONG        checkios;
    BOOL         io_done;       /* CheckIO() says the request has replied  */
    ULONG        tick_jump;     /* ticks a planned timer wake advances     */
    ULONG        signal_calls;
    ULONG        last_signalled;
    struct Task *last_signal_task;  /* which Task Signal() was handed        */

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
    h_me = NULL;
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

/*
 * The one task this tier has.  bsd_timer_open() asks who is calling so that
 * the timer port signals whoever owns its bit, which on a shared base is not
 * the opener; here they are the same task and h_task is both.
 */
struct Task *FindTask(const char *name)
{
    (VOID)name;
    return (h_me != NULL) ? h_me : &h_task;
}

VOID Signal(struct Task *task, ULONG signalSet)
{
    h.last_signal_task = task;
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

    /* A timer wake is the request replying: CheckIO() sees it and the
       clock has moved on by the timeout. */
    if ((arrived & H_TIMER_SIG) != 0)
    {
        h.io_done = TRUE;
        h.ticks  += h.tick_jump;
    }

    if (h.wait_establishes != NULL)
    {
        h.wait_establishes->as_Nx.tcp.nx_tcp_socket_state = NX_TCP_ESTABLISHED;
        h.wait_establishes = NULL;
    }

    if (h.wait_nx_enter_result != 0)
        h.nx_enter_result = h.wait_nx_enter_result;

    /* A signal that arrives after Wait() returns, in the window before the
       caller's next SetSignal() read -- the pending-break case at the loop
       top.  One-shot, like wait_establishes. */
    if (h.wait_post_signal != 0)
    {
        h.signals |= h.wait_post_signal;
        h.wait_post_signal = 0;
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

VOID SendIO(struct IORequest *io)  { (VOID)io; h.sendios++; h.io_done = FALSE; }
LONG AbortIO(struct IORequest *io) { (VOID)io; h.abortios++; h.io_done = TRUE; return 0; }
LONG WaitIO(struct IORequest *io)  { (VOID)io; h.waitios++; h.io_done = FALSE; return 0; }
struct IORequest *CheckIO(struct IORequest *io)
{
    h.checkios++;
    return h.io_done ? io : NULL;
}

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

/* The establish notify settles the socket's receive window from the
   handshake's round trip (socket.c, bsdsocket_window.h); the policy has its
   own host test in tests/netstack/host/test_pool_window_host.c and this file
   only needs the notify to run, so both are inert here. */
VOID  bsd_tcp_window_settle(NX_TCP_SOCKET *tcp, ULONG rtt_ms)
{ (VOID)tcp; (VOID)rtt_ms; }
ULONG ami_millis(VOID) { return 0UL; }

#ifdef AMINETXDUO_TCP_CORK
/* The cork's hooks in select.c.  cork.c and its interplay with select.c are
   test_cork's (tests/bsdsocket/host/test_cork_host.c); no socket here holds a
   corked segment, so none of these is reached. */
VOID  bsd_cork_window_open(AmiSocket *sock) { (VOID)sock; h_unreachable("bsd_cork_window_open"); }
VOID  bsd_cork_wake(AmiSocket *sock) { (VOID)sock; h_unreachable("bsd_cork_wake"); }
ULONG bsd_cork_room(const AmiSocket *sock) { (VOID)sock; h_unreachable("bsd_cork_room"); return 0; }
VOID  bsd_cork_push(struct AmiSocketBase *base, AmiSocket *sock)
{ (VOID)base; (VOID)sock; h_unreachable("bsd_cork_push"); }
#endif

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

static NX_PACKET h_probe_packet;

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

    /* The old case here set tcp_bytes_available with the queue count at zero
       and asserted readable. That state is not reachable: the count is bumped
       for every segment put on the receive queue, in order or not, so a zero
       count means an empty queue and nx_tcp_socket_bytes_available() would
       return zero too. It asserted the mock's freedom, not the stack's.
       What bsd_readable() now tests is the queue head itself, so assert on
       that -- including the defensive case where head and count disagree,
       which must still report ready rather than lose the wakeup. */
    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    s->as_Nx.tcp.nx_tcp_socket_receive_queue_head = &h_probe_packet;
    CHECK(bsd_readable(s) == TRUE, "a packet on the queue is readable");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    h.tcp_bytes_available = 42;
    CHECK(bsd_readable(s) == FALSE,
          "an empty queue is not readable, whatever a byte count claims");

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
    CHECK(s->as_TxWait == 1,
          "and it leaves the FD_WRITE request behind for the next ACK");

    /* The request is taken back when there is room, so an acknowledgment
       later does not post FD_WRITE to a writer that never waited. */
    s->as_Nx.tcp.nx_tcp_socket_transmit_sent_count = 1;
    CHECK(bsd_writable(s) == TRUE, "room again is writable");
    CHECK(s->as_TxWait == 0, "and takes the FD_WRITE request back");

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

/*
 * F-065: a task that did not open the base is refused before any base-wide
 * scratch is touched -- a poll and an untimed wait as the timed one already
 * was.  Data is ready, so the old code answered 1 at once.  A base whose
 * opener is gone is not refused.
 */
static void t_waitselect_foreign_task(void)
{
    static const UINT one[] = { 7777 };        /* from the connected peer */
    HSets s, before;
    LONG  n;

    printf("WaitSelect(): only the task that opened the base\n");

    h_reset();
    h_queue(h_udp(0, 7777), one, 1);
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);
    before = s;
    memset(&h_base.sb_SelIn, 0xA5, sizeof(h_base.sb_SelIn));
    memset(&h_base.sb_SelReady, 0xA5, sizeof(h_base.sb_SelReady));
    h_me = &h_other_task;

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &h_poll, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EINVAL,
          "a poll from another task is EINVAL");
    CHECK(memcmp(&s, &before, sizeof(s)) == 0, "its sets come back as given");
    {
        UBYTE a5[sizeof(h_base.sb_SelIn)];

        memset(a5, 0xA5, sizeof(a5));
        CHECK(memcmp(&h_base.sb_SelIn, a5, sizeof(a5)) == 0 &&
                  memcmp(&h_base.sb_SelReady, a5,
                         sizeof(h_base.sb_SelReady)) == 0,
              "and the opener's scratch sets are not touched");
    }

    n = bsd_WaitSelect(1, s.read, NULL, NULL, NULL, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EINVAL,
          "an untimed wait from another task is EINVAL");

    /* The opener itself: as before. */
    h_me = NULL;
    n = bsd_WaitSelect(1, s.read, NULL, NULL, &h_poll, NULL, &h_base);
    CHECK(n == 1, "the opener's poll finds the datagram");

    /* A base whose opener is gone refuses nobody, as before. */
    h_reset();
    h_queue(h_udp(0, 7777), one, 1);
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);
    h_base.sb_Task = NULL;
    h_me = &h_other_task;
    n = bsd_WaitSelect(1, s.read, NULL, NULL, &h_poll, NULL, &h_base);
    CHECK(n == 1, "with no opener task, a poll is served");
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

    /* Wait() consumes every bit it returns.  A caller signal which arrives
       in the same wakeup as the break must therefore still be returned in
       *signals even though the function itself reports EINTR. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0] = H_BREAK_SIG | H_USER_SIG;
    h.wait_planned = 1;
    signals = H_USER_SIG;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, NULL, &signals, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EINTR,
          "a break and caller signal arriving together report EINTR");
    CHECK(signals == H_USER_SIG,
          "and the consumed caller signal is returned in the signal mask");
    CHECK((h.signals & H_BREAK_SIG) != 0,
          "and only the break signal is reposted");

    /* The same contract applies if an event wakes the task and the stack is
       gone by the readiness poll which follows it. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0] = H_EVENT_SIG | H_USER_SIG;
    h.wait_planned = 1;
    h.wait_nx_enter_result = -1;
    signals = H_USER_SIG;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, NULL, &signals, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_ENETDOWN,
          "a failed post-wakeup poll reports ENETDOWN");
    CHECK(signals == H_USER_SIG,
          "and preserves the caller signal consumed by that wakeup");

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

    /* N-084: a caller signal that Wait() returned on its own does not end the
       loop there; the loop top does.  A break that lands in between makes
       that top return EINTR, and the signal Wait() consumed must still be
       reported.  The caller asks for two bits so an untouched mask is told
       apart from the one bit that arrived. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0]     = H_USER_SIG;
    h.wait_planned     = 1;
    h.wait_post_signal = H_BREAK_SIG;      /* after the wake, before the top */
    signals = H_USER_SIG | H_USER2_SIG;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, NULL, &signals, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EINTR,
          "a break pending at the loop top after a caller wake is EINTR");
    CHECK(signals == H_USER_SIG,
          "and the caller signal Wait() consumed comes back in the mask");
    CHECK((h.signals & H_USER_SIG) == 0,
          "which is not left standing in the task as well");

    /* N-085: a base whose opener is gone (sb_Task NULL) is not refused, and a
       break Wait() took is put back on the caller, not on sb_Task. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h_base.sb_Task = NULL;
    h_me           = &h_other_task;
    h.wait_plan[0] = H_BREAK_SIG;
    h.wait_planned = 1;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, NULL, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EINTR,
          "a break during the wait on an orphaned base is EINTR");
    CHECK(h.signal_calls == 1 && h.last_signal_task == &h_other_task,
          "and the break goes back to the calling task, not to a NULL sb_Task");
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
    h.tick_jump    = TX_TIMER_TICKS_PER_SECOND;     /* the second, in ticks */
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
    CHECK(!h_base.sb_TimerArmed, "and nothing is left out at the device");
    CHECK(!h_isset(s.read, 0), "and the result set comes back cleared");

    /* A kept request that fires before this wait's own deadline -- it was
       armed for an earlier, shorter wait -- is re-armed for the remainder. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0] = H_TIMER_SIG;
    h.wait_plan[1] = H_TIMER_SIG;
    h.wait_planned = 2;
    h.tick_jump    = TX_TIMER_TICKS_PER_SECOND / 2; /* fires at half the second */
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);
    n = bsd_WaitSelect(1, s.read, NULL, NULL, &one_second, NULL, &h_base);
    CHECK(n == 0 && h.wait_calls == 2,
          "a request that fires early is followed by a second wait");
    CHECK(h.sendios == 2, "armed once more, for the remainder");
    CHECK(h_base.sb_TimerReq.tr_time.tv_secs == 0 &&
          h_base.sb_TimerReq.tr_time.tv_micro == 500000UL,
          "which is the half second still owed");

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
    CHECK(h.abortios == 0 && h.waitios == 0 && h_base.sb_TimerArmed,
          "and the timeout request is left out at the device for the next wait");

    /* The next timed wait with the same timeout keeps that request: no
       SendIO, no AbortIO.  A shorter timeout than the request's remainder
       aborts it and arms afresh.  One that fired in between is reaped on
       entry, then armed afresh. */
    h_sock[0].as_Nx.tcp.nx_tcp_socket_state = NX_TCP_SYN_SENT;
    h.wait_plan[0]     = H_EVENT_SIG;
    h.wait_planned     = 1;
    h.wait_calls       = 0;
    h.wait_establishes = &h_sock[0];
    h.sendios = h.abortios = h.waitios = 0;
    memset(&s, 0, sizeof(s));
    h_set(s.write, 0);
    n = bsd_WaitSelect(1, NULL, s.write, NULL, &one_second, NULL, &h_base);
    CHECK(n == 1 && h.sendios == 0 && h.abortios == 0 && h.waitios == 0 &&
          h_base.sb_TimerArmed,
          "the same timeout again reuses the request that is out");

    {
        struct timeval tenth = { 0, 100000 };

        h_sock[0].as_Nx.tcp.nx_tcp_socket_state = NX_TCP_SYN_SENT;
        h.wait_plan[0]     = H_EVENT_SIG;
        h.wait_planned     = 1;
        h.wait_calls       = 0;
        h.wait_establishes = &h_sock[0];
        h.sendios = h.abortios = h.waitios = 0;
        memset(&s, 0, sizeof(s));
        h_set(s.write, 0);
        n = bsd_WaitSelect(1, NULL, s.write, NULL, &tenth, NULL, &h_base);
        CHECK(n == 1 && h.abortios == 1 && h.waitios == 1 && h.sendios == 1,
              "a shorter timeout aborts the request that is out and arms its own");
        CHECK(h_base.sb_TimerReq.tr_time.tv_micro == 100000UL,
              "for the shorter time");
    }

    h_sock[0].as_Nx.tcp.nx_tcp_socket_state = NX_TCP_SYN_SENT;
    h.io_done          = TRUE;          /* it fired while nobody waited */
    h.wait_plan[0]     = H_EVENT_SIG;
    h.wait_planned     = 1;
    h.wait_calls       = 0;
    h.wait_establishes = &h_sock[0];
    h.sendios = h.abortios = h.waitios = 0;
    memset(&s, 0, sizeof(s));
    h_set(s.write, 0);
    n = bsd_WaitSelect(1, NULL, s.write, NULL, &one_second, NULL, &h_base);
    CHECK(n == 1 && h.waitios == 1 && h.abortios == 0 && h.sendios == 1,
          "a request that fired between waits is reaped, not aborted, and a new one armed");

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

/*
 * F-066: a large accepted timeout re-arms the remainder from ticks.  The old
 * `left * BSD_TICK_US` is 32-bit and wraps once the remainder passes ~214748
 * ticks (about 71 minutes), so a multi-hour wait came back hours early.
 */
static void t_waitselect_rearm_overflow(void)
{
    HSets s;
    LONG  n;
    struct timeval long_wait = { 100000, 0 };   /* 27.8 hours, accepted */

    printf("WaitSelect(): a large timeout re-arms the true remainder\n");

    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0]     = H_TIMER_SIG;           /* the kept request fires early */
    h.wait_planned     = 1;
    h.wait_establishes = &h_sock[0];            /* ends the wait after the re-arm */
    h.tick_jump        = TX_TIMER_TICKS_PER_SECOND / 2;   /* half a second */
    memset(&s, 0, sizeof(s));
    h_set(s.write, 0);

    n = bsd_WaitSelect(1, NULL, s.write, NULL, &long_wait, NULL, &h_base);
    CHECK(n == 1, "a connect completing after the re-arm ends the wait");
    CHECK(h_base.sb_TimerReq.tr_time.tv_secs == 99999UL &&
          h_base.sb_TimerReq.tr_time.tv_micro == 500000UL,
          "and the re-armed remainder is 99999.5 seconds, not a wrapped "
          "32-bit fraction");
}

/*
 * F-066 (tick total past LONG_MAX): the deadline is kept in 32-bit *signed*
 * ticks, so a timeout whose tick total exceeds LONG_MAX cannot drive the
 * (wanted_due - now) re-arm or the keep/cancel comparison.  The documented
 * 100,000,000 s maximum saturates the tick count; select.c gives such a
 * timeout the terminal path instead -- cancel any kept request, arm the full
 * timeval, and let its reply be the timeout -- rather than reading the
 * remainder back negative and returning early.
 */
static void t_waitselect_terminal(void)
{
    struct timeval big = { 100000000UL, 0 };   /* the autodoc maximum */
    struct timeval one_second = { 1, 0 };
    HSets s;
    LONG  n;

    printf("WaitSelect(): a tick total past LONG_MAX is terminal, not early\n");

    /* The documented maximum is accepted and expires to 0, never EINVAL.  It
       arms the full timeval once and does not re-arm (no signed remainder). */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0]     = H_TIMER_SIG;
    h.wait_planned     = 1;
    h.tick_jump        = TX_TIMER_TICKS_PER_SECOND / 2;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &big, NULL, &h_base);
    CHECK(n == 0, "100000000 seconds is accepted and expires to 0, not EINVAL");
    CHECK(h.sendios == 1, "the full timeval is armed exactly once");
    CHECK(h_base.sb_TimerReq.tr_time.tv_secs == 100000000UL &&
          h_base.sb_TimerReq.tr_time.tv_micro == 0,
          "and it carried the caller's 100000000-second timeout");
    CHECK(h.waitios == 1 && h.abortios == 0,
          "the completed reply is collected, not aborted, and not re-armed");

    /* A large wait that ends on data takes its request back instead of
       leaving a wrapped due time the next wait's keep/cancel cannot judge. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0]     = H_EVENT_SIG;
    h.wait_planned     = 1;
    h.wait_establishes = &h_sock[0];
    memset(&s, 0, sizeof(s));
    h_set(s.write, 0);

    n = bsd_WaitSelect(1, NULL, s.write, NULL, &big, NULL, &h_base);
    CHECK(n == 1, "a large wait that ends on data returns the ready count");
    CHECK(h.abortios == 1 && !h_base.sb_TimerArmed,
          "and takes its request back, leaving no wrapped due time");

    /* The next short wait then arms its own request, not the leftover one. */
    h_sock[0].as_Nx.tcp.nx_tcp_socket_state = NX_TCP_SYN_SENT;
    h.wait_plan[0]     = H_TIMER_SIG;
    h.wait_planned     = 1;
    h.wait_calls       = 0;
    h.tick_jump        = TX_TIMER_TICKS_PER_SECOND;
    h.signals          = 0;      /* clear the reposted break, if any */
    h.nx_enter_result  = 0;      /* clear the transient poll failure */
    h.wait_nx_enter_result = 0;
    h.sendios = h.abortios = h.waitios = 0;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &one_second, NULL, &h_base);
    CHECK(n == 0 && h.sendios == 1,
          "a later short wait arms its own request, not the large one");
    CHECK(h_base.sb_TimerReq.tr_time.tv_secs == 1 &&
          h_base.sb_TimerReq.tr_time.tv_micro == 0,
          "for one second, not the leftover 100000000");

    /* The boundary is want == LONG_MAX ticks: one second under the largest
       whole-second tick-path timeout re-arms the remainder; one second over
       is terminal and does not. */
    {
        const ULONG max_tick_secs =
            0x7FFFFFFFUL / (ULONG)TX_TIMER_TICKS_PER_SECOND;
        struct timeval under = { max_tick_secs, 0 };
        struct timeval over  = { max_tick_secs + 1UL, 0 };

        h_reset();
        (void)h_tcp(0, NX_TCP_SYN_SENT);
        h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
        h.wait_plan[0]     = H_TIMER_SIG;
        h.wait_planned     = 1;
        h.wait_establishes = &h_sock[0];
        h.tick_jump        = TX_TIMER_TICKS_PER_SECOND / 2;
        memset(&s, 0, sizeof(s));
        h_set(s.write, 0);

        n = bsd_WaitSelect(1, NULL, s.write, NULL, &under, NULL, &h_base);
        CHECK(n == 1 &&
              h_base.sb_TimerReq.tr_time.tv_secs == max_tick_secs - 1UL &&
              h_base.sb_TimerReq.tr_time.tv_micro == 500000UL,
              "one second under the boundary re-arms the true remainder");

        h_reset();
        (void)h_tcp(0, NX_TCP_SYN_SENT);
        h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
        h.wait_plan[0]     = H_TIMER_SIG;
        h.wait_planned     = 1;
        h.tick_jump        = TX_TIMER_TICKS_PER_SECOND / 2;
        memset(&s, 0, sizeof(s));
        h_set(s.read, 0);

        n = bsd_WaitSelect(1, s.read, NULL, NULL, &over, NULL, &h_base);
        CHECK(n == 0 && h.sendios == 1,
              "one second over the boundary is terminal, not re-armed");
    }

    /* The tick total saturates, but its own addition can still wrap: the
       largest whole-second product plus a rounded-microsecond count carries
       past ULONG_MAX to a near-zero deadline (F-066).  The saturating total
       must hold the terminal path so the full timeval is armed, not an
       immediate fire. */
    {
        const ULONG sat_secs = 0xFFFFFFFFUL / (ULONG)TX_TIMER_TICKS_PER_SECOND;
        struct timeval wrap = { sat_secs, 999999UL };

        h_reset();
        (void)h_tcp(0, NX_TCP_SYN_SENT);
        h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
        h.wait_plan[0]     = H_TIMER_SIG;
        h.wait_planned     = 1;
        h.tick_jump        = TX_TIMER_TICKS_PER_SECOND / 2;
        memset(&s, 0, sizeof(s));
        h_set(s.read, 0);

        n = bsd_WaitSelect(1, s.read, NULL, NULL, &wrap, NULL, &h_base);
        CHECK(n == 0, "a timeout whose rounded ticks wrap ULONG_MAX is "
                      "terminal, not an early fire");
        CHECK(h.sendios == 1 &&
              h_base.sb_TimerReq.tr_time.tv_secs == sat_secs &&
              h_base.sb_TimerReq.tr_time.tv_micro == 999999UL,
              "the full timeval is armed, not a wrapped 32-bit remainder");
        CHECK(h_base.sb_TimerDue == 0,
              "and it carries no signed deadline, not the wrapped near-zero "
              "tick");
        CHECK(h.waitios == 1 && h.abortios == 0,
              "the completed reply is collected, not re-armed");
    }

    /* A break (Ctrl-C) landing while the terminal request is out must not
       leave it behind: it has no signed due time, so the next wait could not
       judge a zero sb_TimerDue.  It is taken back on the way to EINTR, and
       nothing is re-armed. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0]     = H_BREAK_SIG;
    h.wait_planned     = 1;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &big, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EINTR,
          "a break during a terminal wait is EINTR, not an early timeout");
    CHECK(h.sendios == 1, "the full timeval was armed once, nothing re-armed");
    CHECK(h.abortios == 1 && !h_base.sb_TimerArmed,
          "and its request is taken back on the break, not left out");

    /* An event wake with no ready descriptor sends the wait around the loop
       once more; if the stack is gone by the loop-top readiness poll the
       terminal request must not be left out on the ENETDOWN return. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0]     = H_EVENT_SIG;
    h.wait_planned     = 1;
    h.wait_nx_enter_result = -1;          /* the next poll cannot enter */
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &big, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_ENETDOWN,
          "a terminal wait whose post-wakeup poll fails is ENETDOWN");
    CHECK(h.abortios == 1 && !h_base.sb_TimerArmed,
          "and takes its request back on the poll failure, not leaving it out");

    /* The next short wait then arms its own request, not the leftover one. */
    h_sock[0].as_Nx.tcp.nx_tcp_socket_state = NX_TCP_SYN_SENT;
    h.wait_plan[0]     = H_TIMER_SIG;
    h.wait_planned     = 1;
    h.wait_calls       = 0;
    h.tick_jump        = TX_TIMER_TICKS_PER_SECOND;
    h.signals          = 0;      /* clear the reposted break, if any */
    h.nx_enter_result  = 0;      /* clear the transient poll failure */
    h.wait_nx_enter_result = 0;
    h.sendios = h.abortios = h.waitios = 0;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &one_second, NULL, &h_base);
    CHECK(n == 0 && h.sendios == 1,
          "a later short wait arms its own request, not the large one");
    CHECK(h_base.sb_TimerReq.tr_time.tv_secs == 1 &&
          h_base.sb_TimerReq.tr_time.tv_micro == 0,
          "for one second, not the leftover 100000000");

    /* A break that lands in the window before the loop-top re-check -- pending
       at the SetSignal read rather than delivered by Wait -- must also take
       the terminal request back. */
    h_reset();
    (void)h_tcp(0, NX_TCP_SYN_SENT);
    h_sock[0].as_Flags = ASF_TCP | ASF_CONNECTING;
    h.wait_plan[0]     = H_EVENT_SIG;
    h.wait_planned     = 1;
    h.wait_post_signal = H_BREAK_SIG;     /* after the wake, before the re-check */
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &big, NULL, &h_base);
    CHECK(n == -1 && h_base.sb_Errno == AMI_EINTR,
          "a break pending at the loop-top re-check is EINTR");
    CHECK(h.abortios == 1 && !h_base.sb_TimerArmed,
          "and takes its request back, not leaving it out");

    /* The next short wait then arms its own request, not the leftover one. */
    h_sock[0].as_Nx.tcp.nx_tcp_socket_state = NX_TCP_SYN_SENT;
    h.wait_plan[0]     = H_TIMER_SIG;
    h.wait_planned     = 1;
    h.wait_calls       = 0;
    h.tick_jump        = TX_TIMER_TICKS_PER_SECOND;
    h.signals          = 0;      /* clear the reposted break, if any */
    h.nx_enter_result  = 0;      /* clear the transient poll failure */
    h.wait_nx_enter_result = 0;
    h.sendios = h.abortios = h.waitios = 0;
    memset(&s, 0, sizeof(s));
    h_set(s.read, 0);

    n = bsd_WaitSelect(1, s.read, NULL, NULL, &one_second, NULL, &h_base);
    CHECK(n == 0 && h.sendios == 1,
          "a later short wait arms its own request, not the large one");
    CHECK(h_base.sb_TimerReq.tr_time.tv_secs == 1 &&
          h_base.sb_TimerReq.tr_time.tv_micro == 0,
          "for one second, not the leftover 100000000");
}

static void t_events(void)
{
    AmiSocket *s;
    AmiSocket listener;

    printf("bsd_event_post() and bsd_events_attach()\n");

    h_reset();
    s = h_udp(0, 7777);
    bsd_event_post(s, FD_READ);
    CHECK((h.signals & H_EVENT_SIG) != 0, "every event wakes WaitSelect");
    CHECK((s->as_Events & FD_READ) != 0, "and is latched on the socket");

    h_reset();
    s = h_tcp(0, NX_TCP_ESTABLISHED);
    memset(&listener, 0, sizeof(listener));
    listener.as_Owner = &h_base;
    s->as_Flags |= ASF_INCOMING;
    s->as_Parent = &listener;
    s->as_Owner = NULL;
    bsd_event_post(s, FD_READ);
    CHECK((h.signals & H_EVENT_SIG) != 0,
          "a pending accept uses its listener's current event owner");
    listener.as_Owner = NULL;
    s->as_Owner = &h_base;
    h.signals = 0;
    bsd_event_post(s, FD_CLOSE);
    CHECK(h.signals == 0,
          "a parked listener cannot fall back to the child's earlier owner");
    s->as_Parent = NULL;
    s->as_Flags &= ~ASF_INCOMING;
    bsd_event_post(s, FD_READ);
    CHECK((h.signals & H_EVENT_SIG) != 0,
          "a detached accepted socket uses its own owner again");

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
    t_waitselect_foreign_task();
    t_waitselect_signals();
    t_waitselect_timeout();
    t_waitselect_rearm_overflow();
    t_waitselect_terminal();
    t_events();

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return h_failures == 0 ? 0 : 1;
}
