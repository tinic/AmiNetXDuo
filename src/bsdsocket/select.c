/*
 * bsdsocket.library, WaitSelect() and the socket event plumbing.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "aminetxduo/budget.h"
#include "connfail.h"

#include <proto/exec.h>
#include <proto/timer.h>


/* The largest timeout WaitSelect() accepts, from the autodoc. */
#define BSD_SELECT_MAX_SECS 100000000UL

/* fd_set is an array of 32-bit words, bit (fd % 32) of word (fd / 32). */
#define BSD_FD_WORD(fd)     ((ULONG)(fd) / BSD_FD_BITS)
#define BSD_FD_MASK(fd)     (1UL << ((ULONG)(fd) % BSD_FD_BITS))

/*
 * WaitSelect() uses *signals as both the caller's input mask and the mask of
 * caller signals it consumed.  An error does not undo Wait()/SetSignal(), so
 * an early return after either call must still publish those bits.  Otherwise
 * a signal arriving together with Ctrl-C, or immediately before ENETDOWN,
 * disappears from both the task and the caller's result.
 */
static LONG bsd_waitselect_fail(struct AmiSocketBase *base, ULONG *signals,
                                ULONG got_signals, LONG error)
{
    if (signals != NULL)
        *signals = got_signals;

    return bsd_fail(base, error);
}

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
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr;

#ifdef AMINETXDUO_RXPROBE
    ami_budget_notify(ami_budget_clock());
#endif

#ifdef AMINETXDUO_RX_DIRECT_COMPLETE
    if (sock != NULL && sock->as_RxDState == BSD_RXD_ARMED)
        bsd_rxdirect_pump(sock, FALSE);
#endif

    bsd_event_post(sock, FD_READ);
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

#ifdef AMINETXDUO_TCP_CORK
    /* A reset leaves a corked segment nowhere to go: the pass that finds the
       connection gone drops it.  A FIN leaves CLOSE_WAIT, which still sends. */
    if (sock->as_CorkPkt != NULL)
    {
        bsd_cork_window_open(sock);
        bsd_cork_wake(sock);
    }
#endif

    /* A closed connection is readable (it returns 0) and writable (EPIPE). */
    bsd_event_post(sock, FD_CLOSE | FD_READ | FD_WRITE);
}

/*
 * The three-way handshake finished.
 */
static VOID bsd_tcp_establish_notify(NX_TCP_SOCKET *socket_ptr)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr;

    if (sock == NULL)
        return;

    sock->as_Flags &= ~ASF_CONNECTING;
    sock->as_Flags |= ASF_CONNECTED;

    /* A socket still parked on a listen port belongs to its listener. The
       application selects on the listener, not on the parked socket.  Its
       window settles for the link it came up on and for the round trip the
       SYN cache measured from its SYN-ACK to the ACK
       (nx_tcp_socket_handshake_rtt; 0 when nothing was measured, which is
       the LAN policy, bsdsocket_window.h). */
    if ((sock->as_Flags & ASF_INCOMING) != 0 && sock->as_Parent != NULL)
    {
        bsd_tcp_window_settle(socket_ptr, socket_ptr->nx_tcp_socket_handshake_rtt);
        sock->as_Parent->as_Flags |= ASF_ACCEPTPEND;
        bsd_event_post(sock->as_Parent, FD_ACCEPT | FD_READ);
        return;
    }

    /*
     * The handshake just measured the path: SYN out at as_ConnectMillis,
     * SYN/ACK in now.  A long round trip is a link where the window is the
     * transfer rate, so the socket takes its maximum before any data
     * arrives; a short one is a LAN, where the window is sized to the link
     * it came up on (bsdsocket_window.h).  ami_millis() is E-clock time, so
     * this resolves the 1-5 ms a LAN takes from the 20 and up the Internet
     * takes, which the 50 Hz RTT estimator cannot.
     */
    bsd_tcp_window_settle(socket_ptr, ami_millis() - sock->as_ConnectMillis);

    bsd_event_post(sock, FD_CONNECT | FD_WRITE);
}

/*
 * The connection is completely gone: an RST arrived, or the SYN/data retries
 * ran out (_nx_tcp_socket_connection_reset), or an orderly close finished.
 */
static VOID bsd_tcp_disconnect_complete_notify(NX_TCP_SOCKET *socket_ptr)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr;

    if (sock == NULL)
        return;

    if ((sock->as_Flags & ASF_CONNECTING) != 0)
    {
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

#ifdef AMINETXDUO_TCP_CORK
    if (sock->as_CorkPkt != NULL)
    {
        bsd_cork_window_open(sock);
        bsd_cork_wake(sock);
    }
#endif

    bsd_event_post(sock, FD_CLOSE | FD_READ | FD_WRITE);
}

/*
 * NetX Duo calls this from the IP thread on every acknowledgment that leaves
 * the transmit queue with room (the fork's nx_tcp_socket_state_transmit_check
 * runs for a registered notify, not only for a suspended sender).  Most of
 * those are of no interest to anybody, so the post is gated on a writer
 * having actually hit the wall: as_TxWait is what bsd_writable() and a short
 * non-blocking send leave behind.  Without the gate every ACK would Signal()
 * a task that asked for SIGIO.
 */
static VOID bsd_tcp_window_notify(NX_TCP_SOCKET *socket_ptr)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr;

#ifdef AMINETXDUO_TCP_CORK
    /* A corked segment the window stalled.  Woken, never sent from here:
       cork.c, bsd_cork_window_open(). */
    if (sock != NULL && ((sock->as_CorkFlags & BSD_CORKF_STALLED) != 0 ||
                         sock->as_CorkState != BSD_CORK_IDLE))
        bsd_cork_window_open(sock);
#endif

    if (sock == NULL || sock->as_TxWait == 0)
        return;
    sock->as_TxWait = 0;
    bsd_event_post(sock, FD_WRITE);
}

static VOID bsd_udp_receive_notify(NX_UDP_SOCKET *socket_ptr)
{
    bsd_event_post((AmiSocket *)socket_ptr->nx_udp_socket_reserved_ptr, FD_READ);
}

/*
 * An ICMP error named a datagram this socket's port sent. Whether it is ours
 * to take is the same question a received datagram asks: NetX Duo binds a UDP
 */
static UINT bsd_udp_icmp_error_notify(NX_UDP_SOCKET *socket_ptr, UINT error_code,
                                      NXD_ADDRESS *peer_address, UINT peer_port)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_udp_socket_reserved_ptr;

    if (sock == NULL || (sock->as_Flags & ASF_CONNECTED) == 0)
        return NX_FALSE;

    if (!bsd_udp_from_peer(sock, peer_address, peer_port, 0UL))
        return NX_FALSE;

    sock->as_SoError = bsd_errno_from_nx(error_code);
    bsd_event_post(sock, FD_ERROR | FD_READ);

    return NX_TRUE;
}

/*
 * A SYN has taken the socket that was on the port, so the listen request's
 * slot is free and the next reserve can go on it. Runs on the NetX Duo IP
 * thread: no Exec allocation, which is why bsd_listen() creates the reserves
 */
static VOID bsd_listen_refill(AmiSocket *listener)
{
    NX_IP     *ip = bsd_stack_ip(listener->as_Owner);
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

        /* OPTIONAL.  With NX_NO_WAIT the arm answers NX_IN_PROGRESS, or
           NX_SUCCESS if a connection landed between the relisten above and
           here.  Anything else is NX_NOT_LISTEN_STATE: the socket left the
           listen state in that window and the slot is armed nowhere, so the
           next connection to this port is dropped with no other trace. */
        status = nx_tcp_server_socket_accept(&p->as_Nx.tcp, NX_NO_WAIT);
        if (status != NX_IN_PROGRESS && status != NX_SUCCESS)
            AMI_WARN("bsdsocket: port %ld was relistened but not armed (%ld); "
                     "the next connection to it is dropped",
                     (long)listener->as_ListenPort, (long)status);
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

#define BSD_BREAK_SLICE_TICKS   10

/*
 * Wait in slices, checking the break mask between them.
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

    if (wait == NX_NO_WAIT ||
        (wait == NX_WAIT_FOREVER && break_mask == 0))
        return call(arg, wait);

    for (;;)
    {
        ULONG slice;
        ULONG started = 0;

        /* bsd_break_signals(), not SetSignal(): in a green build this loop
           runs on the realm as the caller's proxy, and the owner's break
           bits are being collected by its parked side (netx_call.c). */
        if ((bsd_break_signals(base) & break_mask) != 0)
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

        if (wait != NX_WAIT_FOREVER)
            started = tx_time_get();

        status = call(arg, slice);
        if (status != NX_NO_PACKET && status != NX_TX_QUEUE_DEPTH &&
            status != NX_WINDOW_OVERFLOW)
            return status;

        if (wait != NX_WAIT_FOREVER)
        {
            ULONG elapsed = tx_time_get() - started;

            if (elapsed < slice)
                elapsed = slice;

            remaining = (elapsed >= remaining) ? 0 : remaining - elapsed;
        }
    }
}

ULONG bsd_wait_option(AmiSocket *sock, ULONG timeout_ticks, LONG flags)
{
    if ((sock->as_Flags & ASF_NONBLOCK) != 0 ||
        (flags & MSG_DONTWAIT) != 0)
        return NX_NO_WAIT;

    if (timeout_ticks != 0)
        return timeout_ticks;

    return NX_WAIT_FOREVER;
}

BOOL bsd_readable(AmiSocket *sock)
{
    if (sock == NULL)
        return FALSE;

    /* shutdown(SHUT_RD) is an immediate EOF for every connected socket type. */
    if ((sock->as_Flags & ASF_RDSHUT) != 0)
        return TRUE;

    if (sock->as_RxPending != NULL)
    {
#ifdef AMINETXDUO_RX_DIRECT_COMPLETE
        if ((sock->as_Flags & (ASF_TCP | ASF_RAW)) != ASF_TCP ||
            sock->as_RxPending->nx_packet_length > sock->as_RxOffset)
            return TRUE;
#else
        return TRUE;
#endif
    }

    if ((sock->as_Flags & ASF_RAW) != 0)
        return (sock->as_RawHead != NX_NULL);

    if ((sock->as_Flags & ASF_TCP) != 0)
    {
        if ((sock->as_Flags & ASF_LISTENING) != 0)
        {
            return (bsd_incoming_first_ready(sock) != NULL) ? TRUE : FALSE;
        }

        /* A closed or half-closed connection returns end-of-file. */
        if ((sock->as_Flags & ASF_EOF) != 0)
            return TRUE;

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

        /* Only reached with the count at zero, and the count is bumped for
           every queued segment, in order or not (nx_tcp_socket_state_data_
           check.c:589 and :935). nx_tcp_socket_bytes_available() would take
           nx_ip_protection and the THREADS_ONLY Forbid/FindTask/Permit to
           walk a queue it has just been told is empty, and return zero. Test
           the head instead: one aligned load, and it still reports ready if
           the count and the queue ever disagree. Select may over-report. */
        return (sock->as_Nx.tcp.nx_tcp_socket_receive_queue_head != NX_NULL)
                   ? TRUE : FALSE;
    }

    if (sock->as_SoError != 0)
        return TRUE;

    {
        const NX_PACKET *packet = sock->as_Nx.udp.nx_udp_socket_receive_head;

        while (packet != NX_NULL)
        {
            if (bsd_udp_accepts_packet(sock, packet))
                return TRUE;

            packet = packet->nx_packet_queue_next;
        }
    }

    return FALSE;
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

    if (sock->as_SoError != 0)
        return TRUE;

    if (sock->as_Nx.tcp.nx_tcp_socket_state != NX_TCP_ESTABLISHED)
        return FALSE;

    /* Ask to be told BEFORE looking: an acknowledgment that frees the queue
       between the look and the caller's Wait() then still posts FD_WRITE
       (bsd_tcp_window_notify).  A queue with room takes the request back. */
    sock->as_TxWait = 1;
#ifdef AMINETXDUO_TCP_CORK
    /* A corked segment with room takes the next write; one without it is a
       segment the queue has to take first. */
    if (sock->as_CorkPkt != NULL)
    {
        if (bsd_cork_room(sock) > 0 ||
            sock->as_Nx.tcp.nx_tcp_socket_transmit_sent_count + 1UL <
            sock->as_Nx.tcp.nx_tcp_socket_transmit_queue_maximum)
        {
            sock->as_TxWait = 0;
            return TRUE;
        }
        return FALSE;
    }
#endif
    if (sock->as_Nx.tcp.nx_tcp_socket_transmit_sent_count <
        sock->as_Nx.tcp.nx_tcp_socket_transmit_queue_maximum)
    {
        sock->as_TxWait = 0;
        return TRUE;
    }
    return FALSE;
}

BOOL bsd_exception(AmiSocket *sock)
{
    if (sock == NULL)
        return FALSE;

    return ((sock->as_Events & FD_OOB) != 0 ||
            (sock->as_Flags & ASF_OOBHAVE) != 0 ||
            sock->as_SoError != 0);
}

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
    /*
     * THE TASK THAT OWNS THE BIT, WHICH IS NOT ALWAYS THE OPENER.
     *
     * AllocSignal() allocates in the CALLING task and this runs lazily, on the
     * first WaitSelect() with a timeout.  mp_SigTask was the opener, so on a
     * base shared between tasks -- which this library permits, because it
     * enforces no same-task rule anywhere -- timer.device signalled the OPENER
     * on a bit the WAITER had allocated, and the waiter sat in Wait() until a
     * socket event arrived or forever.  A timeout that never fires is the
     * hardest kind of hang to attribute.
     *
     * The port serves one task: whoever opens it here.  A second task asking
     * for a timeout is refused below rather than left to hang.
     */
    base->sb_TimerPort.mp_SigTask      = FindTask(NULL);
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
        ami_signal_free(sig);
        base->sb_TimerSignal  = -1;
        base->sb_TimerSigMask = 0;
        return FALSE;
    }

    base->sb_TimerOpen = TRUE;

    return TRUE;
}

/*
 * THE TIMER REQUEST STAYS OUT.
 *
 * WaitSelect() with a timeout used to SendIO() a timer.device request on the
 * way in and AbortIO()+WaitIO() it on the way out, on every call.  timer.device
 * reads the E-Clock inside each of those, and on an A1200 behind a PiStorm32
 * that is two traps and six CIA bus cycles a time -- the pair was 12% of a
 * receive profile and 13% of a transmit one (rx3/tx3.prof, 2026-09-17), at
 * the ~9,000 waits a second a streaming socket makes, none of which ever
 * reached its timeout.
 *
 * The request is left outstanding when a wait ends on data.  The next timed
 * wait keeps it when it fires no later than about a tick after what is asked
 * (TX_TIMER_TICKS_PER_SECOND, 20 ms), aborts it only when it would fire too
 * late, and a request that fired while nobody waited is reaped on entry.  An
 * early firing -- a kept request that was armed for less than this wait asked
 * -- re-arms for the remainder, so no wait ends before its time.  The
 * deadline arithmetic is on ThreadX's tick, one fast-RAM read, never the
 * E-Clock.
 */
#define BSD_TICK_US   (1000000UL / (ULONG)TX_TIMER_TICKS_PER_SECOND)

/* A timeout in ticks, rounded up: never shorter than asked.  Saturates. */
static ULONG bsd_timeout_ticks(const struct timeval *tv)
{
    ULONG secs  = (ULONG)tv->tv_secs;
    ULONG ticks = secs * (ULONG)TX_TIMER_TICKS_PER_SECOND;

    if (secs != 0 && ticks / secs != (ULONG)TX_TIMER_TICKS_PER_SECOND)
        return 0xFFFFFFFFUL;
    ticks += ((ULONG)tv->tv_micro + BSD_TICK_US - 1UL) / BSD_TICK_US;
    return ticks;
}

/* Take back a request that has fired with nobody waiting.  TRUE while one is
   still out. */
static BOOL bsd_timer_reap(struct AmiSocketBase *base)
{
    if (!base->sb_TimerArmed)
        return FALSE;

    if (CheckIO((struct IORequest *)&base->sb_TimerReq) != NULL)
    {
        WaitIO((struct IORequest *)&base->sb_TimerReq);
        base->sb_TimerArmed = FALSE;
    }

    return base->sb_TimerArmed;
}

static VOID bsd_timer_cancel(struct AmiSocketBase *base)
{
    if (!base->sb_TimerArmed)
        return;

    AbortIO((struct IORequest *)&base->sb_TimerReq);
    WaitIO((struct IORequest *)&base->sb_TimerReq);
    base->sb_TimerArmed = FALSE;
}

/* Called on the base's own teardown; a request out at the device would
   otherwise reply into freed memory. */
VOID bsd_timer_teardown(struct AmiSocketBase *base)
{
    bsd_timer_cancel(base);
}

static VOID bsd_timer_arm(struct AmiSocketBase *base, ULONG secs, ULONG micro,
                          ULONG due)
{
    base->sb_TimerReq.tr_node.io_Command = TR_ADDREQUEST;
    base->sb_TimerReq.tr_time.tv_secs    = secs;
    base->sb_TimerReq.tr_time.tv_micro   = micro;

    SendIO((struct IORequest *)&base->sb_TimerReq);
    base->sb_TimerArmed = TRUE;
    base->sb_TimerDue   = due;
}


/*
 * One readiness sweep, inside a ThreadX context bracket.
 */
static LONG bsd_poll_sets(struct AmiSocketBase *base, LONG nfds,
                          const ULONG *in_read, const ULONG *in_write,
                          const ULONG *in_except, BsdFdSets *out)
{
    LONG fd, count = 0;
    LONG words = (nfds + BSD_FD_BITS - 1) / BSD_FD_BITS;

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
        BOOL       want_read, want_write, want_except;

        /* Callers pass nfds near the table size with only a handful of bits
           set. At each word boundary, step over the whole word when none of
           the three sets watches anything in it. */
        if (mask == 1UL)
        {
            ULONG any = 0;

            if (in_read != NULL)
                any |= in_read[word];
            if (in_write != NULL)
                any |= in_write[word];
            if (in_except != NULL)
                any |= in_except[word];

            if (any == 0)
            {
                fd += BSD_FD_BITS - 1;   /* the loop's ++ completes the step */
                continue;
            }
        }

        want_read   = (in_read   != NULL && (in_read[word]   & mask) != 0);
        want_write  = (in_write  != NULL && (in_write[word]  & mask) != 0);
        want_except = (in_except != NULL && (in_except[word] & mask) != 0);

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
#ifdef AMINETXDUO_TCP_CORK
        /* Waiting to read with a write held: the peer may be waiting for it. */
        else if (want_read && sock->as_CorkPkt != NULL)
        {
            bsd_cork_push(base, sock);
        }
#endif

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
static VOID bsd_fdset_in(ULONG *dst, CONST_APTR src, LONG words)
{
    LONG i;

    for (i = 0; i < words; i++)
        dst[i] = (src != NULL) ? ((const ULONG *)src)[i] : 0;
}

/*
 * ONLY WHERE IT CHANGES, WHICH IS THE ONLY BOUND WE HAVE.
 *
 * `words` comes from nfds, and nfds is the caller's word about how many bits
 * its fd_set holds.  When that word is wrong -- and the Sun RPC idiom
 * `select(getdtablesize(), &set, ...)` makes it wrong whenever the table is
 * larger than the caller's FD_SETSIZE -- writing every word puts zeroes into
 * memory past the caller's object.  We cannot learn the object's real size,
 * so we do the next best thing and never write a word we would not change.
 *
 * `in` is the copy bsd_fdset_in() took of that same memory, so src[i] == in[i]
 * means the store would put back the value already there.  Skipping it is
 * observably identical and, in the one case this is reachable in -- trailing
 * memory that is all zero, so the validity scan above found no bits to reject
 * and no descriptor there can be ready -- it is the difference between
 * clearing 24 bytes of somebody else's stack and touching nothing.
 *
 * Where the caller's nfds is honest this changes nothing: a word that differs
 * is still written, and one that does not never needed to be.
 */
static VOID bsd_fdset_out(APTR dst, const ULONG *src, const ULONG *in,
                          LONG words)
{
    LONG i;

    if (dst == NULL)
        return;

    for (i = 0; i < words; i++)
    {
        if (src[i] != in[i])
            ((ULONG *)dst)[i] = src[i];
    }
}

LONG bsd_WaitSelect(register LONG nfds                __asm("d0"),
                    register APTR read_fds            __asm("a0"),
                    register APTR write_fds           __asm("a1"),
                    register APTR except_fds          __asm("a2"),
                    register struct timeval *timeout  __asm("a3"),
                    register ULONG *signals           __asm("d1"),
                    register struct AmiSocketBase *SocketBase __asm("a6"))
{
    ULONG     *in_read   = SocketBase->sb_SelIn.read;
    ULONG     *in_write  = SocketBase->sb_SelIn.write;
    ULONG     *in_except = SocketBase->sb_SelIn.except;
    BsdFdSets *ready     = &SocketBase->sb_SelReady;
    LONG       words, fd, count = 0;
    ULONG      user_mask   = (signals != NULL) ? *signals : 0;
    ULONG      break_mask  = SocketBase->sb_BreakMask & ~user_mask;
    ULONG      got_signals = 0;
    ULONG      wait_mask;
    BOOL       timer_running = FALSE;   /* this wait relies on the request */
    ULONG      wanted_due    = 0UL;     /* the tick this wait's timeout is at */
    BOOL       poll_only     = FALSE;

    if (nfds < 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (nfds > bsd_table_size(SocketBase))
        nfds = bsd_table_size(SocketBase);

    if (timeout != NULL)
    {
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

        pending = SetSignal(0UL, 0UL);

        if ((pending & break_mask) != 0)
            return bsd_fail(SocketBase, AMI_EINTR);   /* the request stays out */

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
            return bsd_waitselect_fail(SocketBase, signals, got_signals,
                                       AMI_ENETDOWN);

        if (count > 0 || got_signals != 0)
            break;

        if (poll_only)
            break;

        if (timeout != NULL && !timer_running)
        {
            /*
             * One timer, one task, and this refusal is LOAD-BEARING.
             *
             * The signal bit belongs to whichever task opened the port and one
             * IORequest cannot serve two concurrent waits.  Measured on the
             * rig, with this guard removed and a child process doing the first
             * timed WaitSelect() on its parent's base: the child's own call
             * returned 0 and its timeout fired, and from then on the OPENER's
             * socket() returned ENETDOWN -- while the child was still alive,
             * so it is the timer being opened from a foreign task that does
             * it, not the task exiting afterwards.  Thirteen unrelated claims
             * in the same probe failed behind it.
             *
             * Per-task timer state is reopening the library, which is what
             * Roadshow's own autodoc recommends for sharing.  Until there is
             * any, a second task is told no.
             */
            {
                struct Task *me = FindTask(NULL);

                /*
                 * The OPENER's timer, whether or not it has been opened yet.
                 * Refusing only once another task holds it left the hole
                 * open: the first timed WaitSelect() on a base still got to
                 * open the port wherever it was called from, and if that was
                 * not the opener it took the base down with it.
                 */
                if (SocketBase->sb_Task != NULL && me != SocketBase->sb_Task)
                {
                    AMI_WARN("bsdsocket: WaitSelect timeout from a task "
                             "that did not open this base. Open "
                             "bsdsocket.library in the task that waits: one "
                             "base per task is what the timer, the signals "
                             "and errno are all per");
                    return bsd_fail(SocketBase, AMI_EINVAL);
                }

                if (SocketBase->sb_TimerOpen &&
                    SocketBase->sb_TimerPort.mp_SigTask != me)
                {
                    AMI_WARN("bsdsocket: WaitSelect timeout from a second "
                             "task; the timer belongs to another one");
                    return bsd_fail(SocketBase, AMI_EINVAL);
                }
            }

            if (!bsd_timer_open(SocketBase))
                return bsd_fail(SocketBase, AMI_ENOMEM);

            {
                ULONG now   = tx_time_get();
                ULONG want  = bsd_timeout_ticks(timeout);

                wanted_due = now + want;

                /* A request still out is kept unless it would fire more than
                   a tick after this wait's deadline. */
                if (bsd_timer_reap(SocketBase) &&
                    (LONG)(SocketBase->sb_TimerDue - now) > (LONG)want + 1L)
                    bsd_timer_cancel(SocketBase);

                /* Reaped or cancelled, so a set bit is stale; a kept request
                   sets it again when it fires. */
                SetSignal(0, SocketBase->sb_TimerSigMask);

                if (!SocketBase->sb_TimerArmed)
                    bsd_timer_arm(SocketBase, (ULONG)timeout->tv_secs,
                                  (ULONG)timeout->tv_micro, wanted_due);
            }

            timer_running = TRUE;
            wait_mask    |= SocketBase->sb_TimerSigMask;
        }

        received = Wait(wait_mask);

        got_signals |= received & user_mask;

        if ((received & break_mask) != 0)
        {
            Signal(SocketBase->sb_Task, received & break_mask);

            return bsd_waitselect_fail(SocketBase, signals, got_signals,
                                       AMI_EINTR);
        }

        if (timer_running && (received & SocketBase->sb_TimerSigMask) != 0)
        {
            ULONG now;
            LONG  left;

            /* The bit without the reply is stale (a previous wait's request
               fired after it left): the request now out is still running. */
            if (CheckIO((struct IORequest *)&SocketBase->sb_TimerReq) == NULL)
                continue;

            WaitIO((struct IORequest *)&SocketBase->sb_TimerReq);
            SocketBase->sb_TimerArmed = FALSE;

            /* A kept request armed for an earlier wait fires early for this
               one: arm the remainder and keep waiting. */
            now  = tx_time_get();
            left = (LONG)(wanted_due - now);
            if (left > 0)
            {
                ULONG us = (ULONG)left * BSD_TICK_US;

                bsd_timer_arm(SocketBase, us / 1000000UL, us % 1000000UL,
                              wanted_due);
                continue;
            }

            timer_running = FALSE;

            SetSignal(0, SocketBase->sb_EventSigMask);
            count = bsd_poll_sets(SocketBase, nfds, in_read, in_write,
                                  in_except, ready);

            if (count < 0)
                return bsd_waitselect_fail(SocketBase, signals, got_signals,
                                           AMI_ENETDOWN);

            break;
        }
    }

    /* A wait that ended on data leaves the request out for the next one. */

    if (count <= 0 && words > 0)
    {
        bsd_bzero(ready->read,   (ULONG)words * sizeof(ULONG));
        bsd_bzero(ready->write,  (ULONG)words * sizeof(ULONG));
        bsd_bzero(ready->except, (ULONG)words * sizeof(ULONG));
    }

    bsd_fdset_out(read_fds,   ready->read,   in_read,   words);
    bsd_fdset_out(write_fds,  ready->write,  in_write,  words);
    bsd_fdset_out(except_fds, ready->except, in_except, words);

    if (signals != NULL)
        *signals = got_signals;

    return (count > 0) ? count : 0;
}

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
