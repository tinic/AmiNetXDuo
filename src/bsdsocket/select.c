/*
 * bsdsocket.library, WaitSelect() and the socket event plumbing.
 *
 * WaitSelect() is the central call in Amiga network programming and the one
 * conformance suites exercise most (docs/RESEARCH.md S3.1, risk 3). It waits
 * on socket readiness and Exec signal bits at the same time, so a single loop
 * can serve the network, Intuition, ARexx and timers.
 *
 * Documented behaviour honoured here:
 *   - nfds is highest-descriptor + 1
 *   - a NULL timeout blocks forever, a zero timeout polls
 *   - the fd sets are left completely unmodified when the call fails
 *   - the break mask (Ctrl-C by default) returns -1/EINTR and the break
 *     signal is reposted so the caller's own check still sees it
 *   - the caller's signal mask is an in/out parameter: on return it holds
 *     the subset that actually arrived
 *
 * Blocking is Exec Wait() on a signal that every NetX Duo receive/connect/
 * disconnect callback sets. The signal is cleared before each poll, so there
 * is no window in which a wakeup can be lost.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "connfail.h"

#include <proto/exec.h>
#include <proto/timer.h>


/* The largest timeout WaitSelect() accepts, from the autodoc. */
#define BSD_SELECT_MAX_SECS 100000000UL

/* fd_set is an array of 32-bit words, bit (fd % 32) of word (fd / 32). */
#define BSD_FD_WORD(fd)     ((ULONG)(fd) / BSD_FD_BITS)
#define BSD_FD_MASK(fd)     (1UL << ((ULONG)(fd) % BSD_FD_BITS))

/* --------------------------------------------------------- event callbacks, */

VOID bsd_event_post(AmiSocket *sock, ULONG events)
{
    struct AmiSocketBase *base;
    ULONG                 signals;

    if (sock == NULL)
        return;

    sock->as_Events |= events;

    base = sock->as_Owner;
    if (base == NULL || base->sb_Task == NULL)
        return;

    signals = base->sb_EventSigMask;

    if ((sock->as_EventMask & events) != 0)
        signals |= base->sb_SigEventMask;

    if ((events & (FD_READ | FD_WRITE)) != 0)
        signals |= base->sb_SigIOMask;

    if ((events & FD_OOB) != 0)
        signals |= base->sb_SigUrgMask;

    if (signals != 0)
        Signal(base->sb_Task, signals);
}

static VOID bsd_tcp_receive_notify(NX_TCP_SOCKET *socket_ptr)
{
    bsd_event_post((AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr, FD_READ);
}

/*
 * The peer closed, or the connection was reset. Handed to
 * nx_tcp_socket_create(). It fires from _nx_tcp_socket_connection_reset() and
 * from the FIN handling in the state machine, but only while the socket still
 * counts as connected.
 */
VOID bsd_tcp_disconnect_callback(NX_TCP_SOCKET *socket_ptr)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr;

    if (sock == NULL)
        return;

    sock->as_Flags |= ASF_EOF;

    /* A closed connection is readable (it returns 0) and writable (EPIPE). */
    bsd_event_post(sock, FD_CLOSE | FD_READ | FD_WRITE);
}

/*
 * The three-way handshake finished.
 *
 * Reachable only with NX_ENABLE_EXTENDED_NOTIFY_SUPPORT
 * (port/netxduo-amiga/inc/nx_user.h). Without it
 * nx_tcp_socket_establish_notify() returns NX_NOT_SUPPORTED and completion has
 * to be polled out of nx_tcp_socket_state. It fires from two places:
 *
 *   nx_tcp_socket_state_syn_sent.c, our connect() got its SYN+ACK
 *   nx_tcp_socket_state_syn_received.c , a client's handshake with a socket
 *                                          we parked on a listen port
 *
 * The second case matters beyond non-blocking connect: bsd_listen_callback()
 * fires when the SYN arrives, but a listener is not accept()-ready until the
 * handshake completes. Without this callback a WaitSelect() on a listening
 * descriptor can sleep through the ACK and only wake on the next unrelated
 * event.
 */
static VOID bsd_tcp_establish_notify(NX_TCP_SOCKET *socket_ptr)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr;

    if (sock == NULL)
        return;

    sock->as_Flags &= ~ASF_CONNECTING;
    sock->as_Flags |= ASF_CONNECTED;

    /* A socket still parked on a listen port belongs to its listener. The
       application selects on the listener, not on the parked socket. */
    if ((sock->as_Flags & ASF_INCOMING) != 0 && sock->as_Parent != NULL)
    {
        sock->as_Parent->as_Flags |= ASF_ACCEPTPEND;
        bsd_event_post(sock->as_Parent, FD_ACCEPT | FD_READ);
        return;
    }

    bsd_event_post(sock, FD_CONNECT | FD_WRITE);
}

/*
 * The connection is completely gone: an RST arrived, or the SYN/data retries
 * ran out (_nx_tcp_socket_connection_reset), or an orderly close finished.
 *
 * The other half of the extended notify set, and the only thing that can tell
 * a pending connect() it has failed. The create-time disconnect callback above
 * is not invoked for a socket that never reached ESTABLISHED.
 */
static VOID bsd_tcp_disconnect_complete_notify(NX_TCP_SOCKET *socket_ptr)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr;

    if (sock == NULL)
        return;

    if ((sock->as_Flags & ASF_CONNECTING) != 0)
    {
        /* The connect attempt failed. The reason is left for SO_ERROR, which
           is how a non-blocking caller finds out what went wrong. An ICMP
           message that named the SYN says more than "refused". A router's host
           unreachable is not the peer's RST, so it takes precedence. A spent
           retransmission budget is ETIMEDOUT, not a refusal: `whois -6'
           reported "connection refused" after 191 seconds with no answer,
           which sent the reader looking for a server. */
        sock->as_Flags &= ~ASF_CONNECTING;
        sock->as_SoError =
            (sock->as_Nx.tcp.nx_tcp_socket_icmp_error != NX_SUCCESS)
                ? bsd_errno_from_nx(sock->as_Nx.tcp.nx_tcp_socket_icmp_error)
          : bsd_connect_ladder_spent(&sock->as_Nx.tcp)
                ? AMI_ETIMEDOUT
                : AMI_ECONNREFUSED;
        bsd_event_post(sock, FD_CONNECT | FD_ERROR | FD_WRITE);
        return;
    }

    sock->as_Flags |= ASF_EOF;
    bsd_event_post(sock, FD_CLOSE | FD_READ | FD_WRITE);
}

static VOID bsd_tcp_window_notify(NX_TCP_SOCKET *socket_ptr)
{
    bsd_event_post((AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr, FD_WRITE);
}

static VOID bsd_udp_receive_notify(NX_UDP_SOCKET *socket_ptr)
{
    bsd_event_post((AmiSocket *)socket_ptr->nx_udp_socket_reserved_ptr, FD_READ);
}

/*
 * An ICMP error named a datagram this socket's port sent. Whether it is ours
 * to take is the same question a received datagram asks: NetX Duo binds a UDP
 * socket to a port and knows no peer, so it hands the decision here.
 *
 * RFC 1122 4.1.3.3 wants the error passed to the application. Only a connected
 * socket can be told which peer it belongs to, so an unconnected one declines.
 * bsd_udp_from_peer() answers TRUE for a socket that never connected, which is
 * the wrong answer here, hence the explicit ASF_CONNECTED test.
 *
 * Runs on the IP thread. as_SoError is what getsockopt(SO_ERROR) and
 * WaitSelect()'s FD_ERROR read. The receive that was blocked gets the status
 * back from nx_udp_socket_receive() directly and clears as_SoError there.
 */
static UINT bsd_udp_icmp_error_notify(NX_UDP_SOCKET *socket_ptr, UINT error_code,
                                      NXD_ADDRESS *peer_address, UINT peer_port)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_udp_socket_reserved_ptr;

    if (sock == NULL || (sock->as_Flags & ASF_CONNECTED) == 0)
        return NX_FALSE;

    if (!bsd_udp_from_peer(sock, peer_address, peer_port))
        return NX_FALSE;

    sock->as_SoError = bsd_errno_from_nx(error_code);
    bsd_event_post(sock, FD_ERROR | FD_READ);

    return NX_TRUE;
}

/*
 * A SYN has taken the socket that was on the port, so the listen request's
 * slot is free and the next reserve can go on it. Runs on the NetX Duo IP
 * thread: no Exec allocation, which is why bsd_listen() creates the reserves
 * and this only moves one.
 *
 * Without it the port answers nothing until the application next calls
 * accept(), which is what listen()'s backlog exists to prevent, and a
 * connection whose handshake never completes then holds the port for the
 * whole SYN/ACK retransmit ladder.
 *
 * ASF_RELISTENING is the recursion guard: relisten() with a connection
 * already queued hands it over and calls this function again from inside
 * itself.
 */
static VOID bsd_listen_refill(AmiSocket *listener)
{
    NX_IP     *ip = netstack_ip();
    AmiSocket *p;

    if (ip == NULL || (listener->as_Flags & ASF_RELISTENING) != 0)
        return;

    listener->as_Flags |= ASF_RELISTENING;

    for (p = listener->as_Incoming; p != NULL; p = p->as_IncomingNext)
    {
        UINT status;

        if (p->as_Nx.tcp.nx_tcp_socket_state != NX_TCP_CLOSED)
            continue;

        status = nx_tcp_server_socket_relisten(ip, listener->as_ListenPort,
                                               &p->as_Nx.tcp);
        if (status != NX_SUCCESS && status != NX_CONNECTION_PENDING)
            continue;

        (VOID)nx_tcp_server_socket_accept(&p->as_Nx.tcp, NX_NO_WAIT);
        break;
    }

    listener->as_Flags &= ~ASF_RELISTENING;
}

VOID bsd_listen_callback(NX_TCP_SOCKET *socket_ptr, UINT port)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr;
    AmiSocket *listener;

    (VOID)port;

    if (sock == NULL)
        return;

    /* The connection lands on the parked socket. The listener is what the
     * application selects on, so the event belongs to the parent. */
    listener = (sock->as_Parent != NULL) ? sock->as_Parent : sock;

    listener->as_Flags |= ASF_ACCEPTPEND;
    bsd_event_post(listener, FD_ACCEPT | FD_READ);

    bsd_listen_refill(listener);
}

VOID bsd_events_attach(AmiSocket *sock)
{
    /* A raw socket has no NetX Duo control block to hang callbacks on: its
       wakeups come from raw.c's IP-level filter, which calls bsd_event_post()
       directly. */
    if ((sock->as_Flags & ASF_RAW) != 0)
        return;

    if ((sock->as_Flags & ASF_TCP) != 0)
    {
        sock->as_Nx.tcp.nx_tcp_socket_reserved_ptr = sock;

        /* The disconnect callback is installed at nx_tcp_socket_create() time
           (socket.c). Everything else is a *_notify() setter. The last two
           need NX_ENABLE_EXTENDED_NOTIFY_SUPPORT. Without it they return
           NX_NOT_SUPPORTED silently, see port/netxduo-amiga/inc/nx_user.h. */
        nx_tcp_socket_receive_notify(&sock->as_Nx.tcp, bsd_tcp_receive_notify);
        nx_tcp_socket_window_update_notify_set(&sock->as_Nx.tcp,
                                               bsd_tcp_window_notify);
        nx_tcp_socket_establish_notify(&sock->as_Nx.tcp,
                                       bsd_tcp_establish_notify);
        nx_tcp_socket_disconnect_complete_notify(&sock->as_Nx.tcp,
                                                 bsd_tcp_disconnect_complete_notify);
    }
    else
    {
        sock->as_Nx.udp.nx_udp_socket_reserved_ptr = sock;
        nx_udp_socket_receive_notify(&sock->as_Nx.udp, bsd_udp_receive_notify);
        nx_udp_socket_icmp_error_notify(&sock->as_Nx.udp,
                                        bsd_udp_icmp_error_notify);
    }
}

/* ----------------------------------------------------------- readiness ---- */

/*
 * How long a NetX Duo call can wait, and what makes the errno mappings safe.
 *
 * NX_WAIT_FOREVER here is what keeps ten unconditional
 * NX_NO_PACKET -> EWOULDBLOCK mappings correct: with a non-zero wait option
 * NetX Duo suspends instead of returning NX_NO_PACKET.
 *
 *   nx_tcp_socket_receive.c:257       suspends. NX_NO_PACKET at :276 is the else
 *   nx_tcp_socket_send_internal.c:1354  suspends. NX_WINDOW_OVERFLOW (:1387)
 *                                     and NX_TX_QUEUE_DEPTH (:1399) are the else
 *   nx_packet_allocate.c:178          suspends on `if (wait_option)`.
 *                                     NX_NO_PACKET at :268 is the else
 *
 * So each of those returns needs wait_option == 0, a non-blocking socket where
 * EWOULDBLOCK is right, or the calling thread to be ip->nx_ip_thread. No
 * bsdsocket.library vector is ever entered on the NetX Duo IP thread.
 * bsd_nx_enter() (netx_call.c) brackets every vector with
 * ami_netstack_enter_cached(), which gives the calling Exec task a TX_THREAD
 * of its own, built once per base and resumed thereafter, but never
 * ip->nx_ip_thread. Only the callback set runs on the IP thread: the five
 * notify hooks above, bsd_listen_callback, bsd_tcp_disconnect_callback,
 * bsd_tcp_urgent_notify (oob.c), bsd_oob_ip_filter and bsd_raw_filter (raw.c).
 * None of them enters a vector and none can suspend.
 *
 * If that changes, a blocking recv() starts answering EAGAIN, the defect
 * English Amiga Board thread 122501 reports against AmiTCP and Roadshow
 * (docs/RESEARCH.md 37.1). nx_packet_allocate.c:178 has no IP-thread guard at
 * all: a vector called from the IP thread suspends the IP thread on the pool
 * only that thread can refill, which stops the stack rather than misreporting
 * an errno.
 *
 * bsd_wait_errno() (errno.c) checks instead of assuming. New call sites must
 * use it.
 */
/*
 * How long a blocking call sleeps before it looks at the break signal.
 *
 * The Roadshow autodoc requires a blocking operation to be abortable:
 * SetSocketSignals says SIGINT "is the signal to send to the process which
 * owns the socket in order to abort a blocking operation, such as recv()",
 * and recv() documents [EINTR] for it. Passing NX_WAIT_FOREVER and sampling
 * nothing hung any program with a "press Ctrl-C to stop" loop.
 * src/tools/nc.c:567 works around the same gap from the outside.
 *
 * Ten ticks is 200 ms: short enough that Ctrl-C feels responsive, long enough
 * that an idle 14 MHz 68020 is not paying for a bracket four times a second.
 */
#define BSD_BREAK_SLICE_TICKS   10

/*
 * Wait in slices, checking the break mask between them.
 *
 * Only for calls where a timeout is harmless. nx_tcp_socket_receive() and
 * nx_tcp_socket_send() answer NX_NO_PACKET / NX_TX_QUEUE_DEPTH on expiry and
 * change nothing, so slicing them is invisible to the peer.
 *
 * Not usable for nx_tcp_server_socket_accept(): a timeout there runs
 * _nx_tcp_connect_cleanup and winds the socket back to LISTEN, so the next
 * call re-enters the LISTEN block and sends a second SYN+ACK on a half-open
 * connection. docs/RESEARCH.md 32.10 and 43 have the detail. accept() arms
 * that call with NX_NO_WAIT and slices nx_tcp_socket_state_wait() instead.
 *
 * Returns NX_SUCCESS with *aborted set when the break arrived, so the caller
 * fails with EINTR. Otherwise it returns whatever the sliced call last said.
 */
UINT bsd_wait_sliced(struct AmiSocketBase *base, ULONG wait,
                     BsdSlicedCall call, VOID *arg, BOOL *aborted)
{
    ULONG break_mask = base->sb_BreakMask;
    ULONG remaining  = wait;
    UINT  status;

    *aborted = FALSE;

    /* A caller that asked not to block, or a base with no break signal, gets
       the plain call. There is nothing to interleave. */
    if (wait == NX_NO_WAIT || break_mask == 0)
        return call(arg, wait);

    for (;;)
    {
        ULONG slice;

        if ((SetSignal(0UL, 0UL) & break_mask) != 0)
        {
            *aborted = TRUE;
            return NX_SUCCESS;
        }

        if (wait == NX_WAIT_FOREVER)
            slice = BSD_BREAK_SLICE_TICKS;
        else if (remaining == 0)
            return call(arg, NX_NO_WAIT);
        else
            slice = (remaining < BSD_BREAK_SLICE_TICKS) ? remaining
                                                        : BSD_BREAK_SLICE_TICKS;

        status = call(arg, slice);
        if (status != NX_NO_PACKET && status != NX_TX_QUEUE_DEPTH &&
            status != NX_WINDOW_OVERFLOW)
            return status;

        if (wait != NX_WAIT_FOREVER)
            remaining -= slice;
    }
}

ULONG bsd_wait_option(AmiSocket *sock, ULONG timeout_ticks, LONG flags)
{
    /* MSG_DONTWAIT is per operation, not a request to change the descriptor.
       It has the same effect on this call's wait as ASF_NONBLOCK. */
    if ((sock->as_Flags & ASF_NONBLOCK) != 0 ||
        (flags & MSG_DONTWAIT) != 0)
        return NX_NO_WAIT;

    if (timeout_ticks != 0)
        return timeout_ticks;

    return NX_WAIT_FOREVER;
}

BOOL bsd_readable(AmiSocket *sock)
{
    ULONG available = 0;

    if (sock == NULL)
        return FALSE;

    if (sock->as_RxPending != NULL)
        return TRUE;

    if ((sock->as_Flags & ASF_RAW) != 0)
        return (sock->as_RawHead != NX_NULL);

    if ((sock->as_Flags & ASF_TCP) != 0)
    {
        if ((sock->as_Flags & ASF_LISTENING) != 0)
        {
            /*
             * "Readable" on a listener means accept() will not block, so the
             * test is a finished handshake and nothing weaker. The parked
             * socket sits in SYN_RECEIVED from the moment bsd_listen() arms it
             * (see socket.c), and ASF_ACCEPTPEND is set by the listen callback
             * when the SYN lands. Both are true well before the handshake
             * finishes, so neither can stand in for readiness.
             *
             * Nothing stronger either: bsd_incoming_ready() counts a peer that
             * has already closed, because accept() hands that connection over
             * as well.
             */
            return (bsd_incoming_first_ready(sock) != NULL) ? TRUE : FALSE;
        }

        /* A closed or half-closed connection returns end-of-file. */
        if ((sock->as_Flags & (ASF_EOF | ASF_RDSHUT)) != 0)
            return TRUE;

        /*
         * Same thing seen from the state machine, in case the peer's FIN
         * landed before the socket had a disconnect callback attached.
         *
         * The states are listed by name, not compared with >=. NetX Duo
         * numbers them CLOSE_WAIT 6, FIN_WAIT_1 7, FIN_WAIT_2 8 (nx_api.h), so
         * ">= NX_TCP_CLOSE_WAIT" also catches the two states that mean we
         * sent the FIN and are still waiting for the peer's, a socket after
         * shutdown(SHUT_WR), where nothing has arrived and nothing can ever
         * arrive. Reporting that readable makes `nc -N` (half-close, then
         * select for the rest of the answer) spin at full speed on a select()
         * that returns immediately and a recv() that returns EWOULDBLOCK. Only
         * the states in which the peer's FIN has been received belong here.
         */
        if ((sock->as_Flags & ASF_CONNECTED) != 0)
        {
            UINT state = sock->as_Nx.tcp.nx_tcp_socket_state;

            if (state == NX_TCP_CLOSE_WAIT || state == NX_TCP_CLOSING ||
                state == NX_TCP_TIMED_WAIT || state == NX_TCP_LAST_ACK ||
                state == NX_TCP_CLOSED)
                return TRUE;
        }

        if (sock->as_Nx.tcp.nx_tcp_socket_receive_queue_count > 0)
            return TRUE;

        if (nx_tcp_socket_bytes_available(&sock->as_Nx.tcp, &available)
                == NX_SUCCESS && available > 0)
            return TRUE;

        return FALSE;
    }

    if (sock->as_Nx.udp.nx_udp_socket_receive_count > 0)
        return TRUE;

    return (nx_udp_socket_bytes_available(&sock->as_Nx.udp, &available)
                == NX_SUCCESS && available > 0);
}

BOOL bsd_writable(AmiSocket *sock)
{
    if (sock == NULL)
        return FALSE;

    if ((sock->as_Flags & (ASF_TCP | ASF_RAW)) != ASF_TCP)
        return TRUE;                    /* UDP and raw always take a write */

    /* A pending non-blocking connect reports completion (or failure) as
     * writability, which is how BSD applications wait for connect(). */
    if ((sock->as_Flags & ASF_CONNECTING) != 0)
        return (sock->as_Nx.tcp.nx_tcp_socket_state == NX_TCP_ESTABLISHED ||
                sock->as_Nx.tcp.nx_tcp_socket_state == NX_TCP_CLOSED);

    if ((sock->as_Flags & (ASF_EOF | ASF_WRSHUT)) != 0)
        return TRUE;                    /* the write will fail immediately */

    /*
     * A pending error makes a socket writable in BSD, which is how a caller
     * waiting on a non-blocking connect is told to read SO_ERROR instead of
     * waiting out its whole timeout.
     */
    if (sock->as_SoError != 0)
        return TRUE;

    if (sock->as_Nx.tcp.nx_tcp_socket_state != NX_TCP_ESTABLISHED)
        return FALSE;

    return (sock->as_Nx.tcp.nx_tcp_socket_transmit_sent_count <
            sock->as_Nx.tcp.nx_tcp_socket_transmit_queue_maximum);
}

BOOL bsd_exception(AmiSocket *sock)
{
    if (sock == NULL)
        return FALSE;

    return ((sock->as_Events & (FD_OOB | FD_ERROR)) != 0 ||
            sock->as_SoError != 0);
}

/* --------------------------------------------------------------- timeout, */

static BOOL bsd_timer_open(struct AmiSocketBase *base)
{
    BYTE sig;

    if (base->sb_TimerOpen)
        return TRUE;

    sig = ami_signal_alloc();
    if (sig < 0)
        return FALSE;

    base->sb_TimerSignal  = sig;
    base->sb_TimerSigMask = 1UL << sig;

    /* NewList() is amiga.lib, so the port is open-coded. */
    base->sb_TimerPort.mp_Node.ln_Type = NT_MSGPORT;
    base->sb_TimerPort.mp_Flags        = PA_SIGNAL;
    base->sb_TimerPort.mp_SigBit       = sig;
    base->sb_TimerPort.mp_SigTask      = base->sb_Task;
    base->sb_TimerPort.mp_MsgList.lh_Head =
        (struct Node *)&base->sb_TimerPort.mp_MsgList.lh_Tail;
    base->sb_TimerPort.mp_MsgList.lh_Tail = NULL;
    base->sb_TimerPort.mp_MsgList.lh_TailPred =
        (struct Node *)&base->sb_TimerPort.mp_MsgList.lh_Head;

    base->sb_TimerReq.tr_node.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    base->sb_TimerReq.tr_node.io_Message.mn_ReplyPort    = &base->sb_TimerPort;
    base->sb_TimerReq.tr_node.io_Message.mn_Length       = sizeof(base->sb_TimerReq);

    if (OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ,
                   (struct IORequest *)&base->sb_TimerReq, 0) != 0)
    {
        /* Half-built, so the bit goes back and the mask is cleared. Otherwise
           the next open finds a mask naming a signal this task no longer
           holds. */
        ami_signal_free(sig);
        base->sb_TimerSignal  = -1;
        base->sb_TimerSigMask = 0;
        return FALSE;
    }

    base->sb_TimerOpen = TRUE;

    return TRUE;
}

/* ------------------------------------------------------------- WaitSelect, */


/*
 * One readiness sweep, inside a ThreadX context bracket.
 *
 * WaitSelect() cannot adopt on entry and orphan on exit like the other
 * vectors. It parks in Exec Wait() for as long as the caller asked for, and an
 * adopted task holding the ThreadX scheduler lock across that stops the IP
 * thread, the timer and every other socket user until the wait ends. So the
 * bracket covers only the poll, where bsd_readable() reaches
 * nx_tcp_socket_bytes_available() and nx_udp_socket_bytes_available(), both
 * THREADS_ONLY. The Wait() and the timer IO below run as an ordinary Exec
 * task.
 */
static LONG bsd_poll_sets(struct AmiSocketBase *base, LONG nfds,
                          const ULONG *in_read, const ULONG *in_write,
                          const ULONG *in_except, BsdFdSets *out)
{
    LONG fd, count = 0;
    LONG words = (nfds + BSD_FD_BITS - 1) / BSD_FD_BITS;

    /*
     * Only the words the caller can name.  BsdFdSets is three arrays of
     * BSD_FD_WORDS, 384 bytes, sized for the largest table SBTC_DTABLESIZE
     * hands out.  bsd_fdset_out() copies `words` of each back and the loop
     * below writes no further.  A select on two descriptors was clearing all
     * 384 bytes on every call, which is 87 samples of 9432 in bsd_bzero.
     */
    if (words > BSD_FD_WORDS)
        words = BSD_FD_WORDS;

    if (words > 0)
    {
        bsd_bzero(out->read,   (ULONG)words * sizeof(ULONG));
        bsd_bzero(out->write,  (ULONG)words * sizeof(ULONG));
        bsd_bzero(out->except, (ULONG)words * sizeof(ULONG));
    }

    if (bsd_nx_enter(base) != 0)
        return -1;

    for (fd = 0; fd < nfds; fd++)
    {
        AmiSocket *sock;
        ULONG      word = BSD_FD_WORD(fd);
        ULONG      mask = BSD_FD_MASK(fd);
        BOOL       want_read   = (in_read   != NULL && (in_read[word]   & mask) != 0);
        BOOL       want_write  = (in_write  != NULL && (in_write[word]  & mask) != 0);
        BOOL       want_except = (in_except != NULL && (in_except[word] & mask) != 0);

        if (!want_read && !want_write && !want_except)
            continue;

        sock = bsd_lookup(base, fd);
        if (sock == NULL)
            continue;

        if (want_read && bsd_readable(sock))
        {
            out->read[word] |= mask;
            count++;
        }

        if (want_write && bsd_writable(sock))
        {
            out->write[word] |= mask;
            count++;
        }

        if (want_except && bsd_exception(sock))
        {
            out->except[word] |= mask;
            count++;
        }
    }

    bsd_nx_leave(base);

    return count;
}

/* Copy the caller's set in. If the caller passed NULL, the local copy is left
   zeroed. */
static VOID bsd_fdset_in(ULONG *dst, const APTR src, LONG words)
{
    LONG i;

    for (i = 0; i < words; i++)
        dst[i] = (src != NULL) ? ((const ULONG *)src)[i] : 0;
}

static VOID bsd_fdset_out(APTR dst, const ULONG *src, LONG words)
{
    LONG i;

    if (dst == NULL)
        return;

    for (i = 0; i < words; i++)
        ((ULONG *)dst)[i] = src[i];
}

LONG bsd_WaitSelect(register LONG nfds                __asm("d0"),
                    register APTR read_fds            __asm("a0"),
                    register APTR write_fds           __asm("a1"),
                    register APTR except_fds          __asm("a2"),
                    register struct timeval *timeout  __asm("a3"),
                    register ULONG *signals           __asm("d1"),
                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    /* In the base, not on the caller's stack, see sb_SelIn. */
    ULONG     *in_read   = SocketBase->sb_SelIn.read;
    ULONG     *in_write  = SocketBase->sb_SelIn.write;
    ULONG     *in_except = SocketBase->sb_SelIn.except;
    BsdFdSets *ready     = &SocketBase->sb_SelReady;
    LONG       words, fd, count = 0;
    ULONG      user_mask   = (signals != NULL) ? *signals : 0;
    ULONG      break_mask  = SocketBase->sb_BreakMask & ~user_mask;
    ULONG      got_signals = 0;
    ULONG      wait_mask;
    BOOL       timer_running = FALSE;
    BOOL       poll_only     = FALSE;

    if (nfds < 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    /*
     * "The 'nfds' parameter may be truncated if it covers more sockets than
     * are currently in use. This has the side-effect of filling in only as
     * many socket bits in the fd_set parameters you provide as are currently
     * in use, and not as many as you asked for.", so an over-large nfds is
     * clamped, not refused. It only affects a program that lowered
     * SBTC_DTABLESIZE and still passes FD_SETSIZE. At the default table size
     * of 256, which is FD_SETSIZE, nothing is truncated.
     *
     * Clamping also bounds `words` to BSD_FD_WORDS, since the table can never
     * exceed BSD_MAX_DTABLESIZE.
     */
    if (nfds > bsd_table_size(SocketBase))
        nfds = bsd_table_size(SocketBase);

    if (timeout != NULL)
    {
        /* "the number of microseconds must be smaller than 1000000 and the
           number of seconds must not be larger than 100000000". The seconds
           bound catches a negative tv_secs too, because the field is
           unsigned. */
        if ((ULONG)timeout->tv_micro >= 1000000UL ||
            (ULONG)timeout->tv_secs > BSD_SELECT_MAX_SECS)
            return bsd_fail(SocketBase, AMI_EINVAL);

        poll_only = ((ULONG)timeout->tv_secs == 0 &&
                     (ULONG)timeout->tv_micro == 0);
    }

    words = (LONG)((nfds + BSD_FD_BITS - 1) / BSD_FD_BITS);

    bsd_fdset_in(in_read,   read_fds,   words);
    bsd_fdset_in(in_write,  write_fds,  words);
    bsd_fdset_in(in_except, except_fds, words);

    /*
     * Check every descriptor named in the sets before anything is written
     * back: a failing WaitSelect() must leave the caller's sets untouched.
     */
    for (fd = 0; fd < nfds; fd++)
    {
        ULONG word = BSD_FD_WORD(fd);
        ULONG mask = BSD_FD_MASK(fd);

        if (((in_read[word] | in_write[word] | in_except[word]) & mask) == 0)
            continue;

        if (bsd_lookup(SocketBase, fd) == NULL)
            return bsd_fail(SocketBase, AMI_EBADF);
    }

    wait_mask = SocketBase->sb_EventSigMask | user_mask | break_mask;


    for (;;)
    {
        ULONG received;
        ULONG pending;

        /*
         * The break is tested before anything can return, not only after
         * Wait().
         *
         * The autodoc's BUGS section covers this: "WaitSelect() may ignore
         * the break signal altogether if a zero length timeout is given, or
         * sockets are ready at the time the function is entered. This
         * behaviour was changed in bsdsocket.library V4.289, which will always
         * make sure that the break signal is tested and acted upon, note
         * that no other AmiTCP-alike Amiga TCP/IP stack may check the break
         * signal under these circumstances".
         *
         * Both escapes are below: `count > 0` and `poll_only` each leave the
         * loop without reaching the check after Wait(), so a program polling a
         * busy socket never noticed Ctrl-C. A quiet socket blocks in Wait() and
         * was always fine.
         *
         * SetSignal(0,0) observes without consuming, so the break stays set for
         * the caller's own handling, as the post-Wait path leaves it. User
         * signals are consumed, because the caller is given them in `signals`
         * and a signal reported but left standing is delivered twice.
         */
        pending = SetSignal(0UL, 0UL);

        if ((pending & break_mask) != 0)
        {
            if (timer_running)
            {
                AbortIO((struct IORequest *)&SocketBase->sb_TimerReq);
                WaitIO((struct IORequest *)&SocketBase->sb_TimerReq);
            }

            return bsd_fail(SocketBase, AMI_EINTR);
        }

        if ((pending & user_mask) != 0)
            got_signals |= SetSignal(0UL, user_mask) & user_mask;

        /*
         * Clear first, then poll. An event that arrives after the clear sets
         * the signal again, so Wait() below returns immediately. There is no
         * window in which a wakeup goes missing.
         */
        SetSignal(0, SocketBase->sb_EventSigMask);

        count = bsd_poll_sets(SocketBase, nfds, in_read, in_write, in_except,
                              ready);
        if (count < 0)
        {
            /* The kernel is not running. Nothing can ever become ready. */
            if (timer_running)
            {
                AbortIO((struct IORequest *)&SocketBase->sb_TimerReq);
                WaitIO((struct IORequest *)&SocketBase->sb_TimerReq);
            }

            return bsd_fail(SocketBase, AMI_ENETDOWN);
        }

        if (count > 0 || got_signals != 0)
            break;

        if (poll_only)
            break;

        /*
         * The timeout is armed here rather than before the first poll.  A
         * select that finds a socket ready never blocks, and it was still
         * paying SendIO, AbortIO and WaitIO to timer.device for a request it
         * never waited on: 423 samples of 9376, 4.5% of an A1200 wire
         * transfer, because a bulk reader selects once per recv and the data
         * is normally already there.
         *
         * The timeout is what the call can block for, so starting it at the
         * point of blocking is also the closer reading of select(2).  Guarded
         * on timer_running: a Wait() that returns to a poll finding nothing
         * comes back round, and the request is still counting.
         */
        if (timeout != NULL && !timer_running)
        {
            if (!bsd_timer_open(SocketBase))
                return bsd_fail(SocketBase, AMI_ENOMEM);

            SocketBase->sb_TimerReq.tr_node.io_Command = TR_ADDREQUEST;
            SocketBase->sb_TimerReq.tr_time.tv_secs    = timeout->tv_secs;
            SocketBase->sb_TimerReq.tr_time.tv_micro   = timeout->tv_micro;

            SetSignal(0, SocketBase->sb_TimerSigMask);
            SendIO((struct IORequest *)&SocketBase->sb_TimerReq);

            timer_running = TRUE;
            wait_mask    |= SocketBase->sb_TimerSigMask;
        }

        received = Wait(wait_mask);

        if ((received & break_mask) != 0)
        {
            /*
             * Break. The signal goes back so the caller's own Ctrl-C handling
             * still sees it, and the fd sets are left exactly as they were.
             */
            if (timer_running)
            {
                AbortIO((struct IORequest *)&SocketBase->sb_TimerReq);
                WaitIO((struct IORequest *)&SocketBase->sb_TimerReq);
            }

            Signal(SocketBase->sb_Task, received & break_mask);

            return bsd_fail(SocketBase, AMI_EINTR);
        }

        got_signals |= received & user_mask;

        if (timer_running && (received & SocketBase->sb_TimerSigMask) != 0)
        {
            WaitIO((struct IORequest *)&SocketBase->sb_TimerReq);
            timer_running = FALSE;

            /* One last look before giving up on the timeout. */
            SetSignal(0, SocketBase->sb_EventSigMask);
            count = bsd_poll_sets(SocketBase, nfds, in_read, in_write,
                                  in_except, ready);
            break;
        }
    }

    if (timer_running)
    {
        AbortIO((struct IORequest *)&SocketBase->sb_TimerReq);
        WaitIO((struct IORequest *)&SocketBase->sb_TimerReq);
    }

    if (count <= 0 && words > 0)
    {
        /* Same bound as bsd_poll_sets(): only what bsd_fdset_out() reads. */
        bsd_bzero(ready->read,   (ULONG)words * sizeof(ULONG));
        bsd_bzero(ready->write,  (ULONG)words * sizeof(ULONG));
        bsd_bzero(ready->except, (ULONG)words * sizeof(ULONG));
    }

    bsd_fdset_out(read_fds,   ready->read,   words);
    bsd_fdset_out(write_fds,  ready->write,  words);
    bsd_fdset_out(except_fds, ready->except, words);

    if (signals != NULL)
        *signals = got_signals;

    return (count > 0) ? count : 0;
}

/* -------------------------------------------------- the AmiTCP event API, */

VOID bsd_SetSocketSignals(register ULONG int_mask    __asm("d0"),
                          register ULONG io_mask     __asm("d1"),
                          register ULONG urgent_mask __asm("d2"),
                          register struct AmiSocketBase *SocketBase __asm("a6"))
{
    SocketBase->sb_BreakMask  = int_mask;
    SocketBase->sb_SigIOMask  = io_mask;
    SocketBase->sb_SigUrgMask = urgent_mask;
}

LONG bsd_GetSocketEvents(register ULONG *event_ptr __asm("a0"),
                         register struct AmiSocketBase *SocketBase __asm("a6"))
{
    LONG fd;

    if (event_ptr == NULL)
        return bsd_fail(SocketBase, AMI_EFAULT);

    for (fd = 0; fd < SocketBase->sb_TableSize; fd++)
    {
        AmiSocket *sock = bsd_lookup(SocketBase, fd);
        ULONG      events;
        LONG       socket_error;

        if (sock == NULL)
            continue;

        /* Event callbacks run from the IP task. Read-and-clear must be one
           scheduler-atomic operation or a bit posted between the read and
           the write is overwritten by the stale value below. */
        Forbid();
        events = sock->as_Events & sock->as_EventMask;
        if (events == 0)
        {
            Permit();
            continue;
        }

        sock->as_Events &= ~events;
        socket_error = sock->as_SoError;
        Permit();

        *event_ptr = events;

        /*
         * "When this event is found, the 'errno' variable will be set to the
         * error code associated with the socket which triggered the error
         * event. The error code associated with the socket is not cleared. Use
         * the getsockopt(socket,SO_ERROR,..) function to do that, which will
         * read and clear the error code."
         *
         * So this is a peek: as_SoError stays put, and getsockopt(SO_ERROR)
         * remains the only read that clears it.
         */
        if ((events & FD_ERROR) != 0 && socket_error != 0)
            bsd_set_errno(SocketBase, socket_error);

        return fd;
    }

    /* No events pending. This is not an error, so errno is left alone. */
    return -1;
}
