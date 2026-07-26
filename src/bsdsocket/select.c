/*
 * bsdsocket.library -- WaitSelect() and the socket event plumbing.
 *
 * WaitSelect() is the call Amiga network programming is built around, and the
 * one conformance suites hammer (docs/RESEARCH.md S3.1, risk 3). It waits on
 * socket readiness *and* Exec signal bits at the same time, so a single loop
 * can serve the network, Intuition, ARexx and timers.
 *
 * Documented behaviour honoured here:
 *   - nfds is highest-descriptor + 1;
 *   - a NULL timeout blocks forever, a zero timeout polls;
 *   - the fd sets are left completely unmodified when the call fails;
 *   - the break mask (Ctrl-C by default) returns -1/EINTR and the break
 *     signal is *reposted* so the caller's own check still sees it;
 *   - the caller's signal mask is an in/out parameter: on return it holds
 *     the subset that actually arrived.
 *
 * Blocking is Exec Wait() on a signal that every NetX Duo receive/connect/
 * disconnect callback sets, and the signal is cleared *before* each poll, so
 * there is no window in which a wakeup can be lost.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"

#include <proto/exec.h>
#include <proto/timer.h>

#define BSD_FD_BITS         32
#define BSD_FD_WORDS        ((BSD_MAX_DTABLESIZE + BSD_FD_BITS - 1) / BSD_FD_BITS)

/* fd_set is an array of 32-bit words, bit (fd % 32) of word (fd / 32). */
#define BSD_FD_WORD(fd)     ((ULONG)(fd) / BSD_FD_BITS)
#define BSD_FD_MASK(fd)     (1UL << ((ULONG)(fd) % BSD_FD_BITS))

/* --------------------------------------------------------- event callbacks -- */

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
 * The peer closed, or the connection was reset.
 *
 * This is the callback handed to nx_tcp_socket_create(). It fires from
 * _nx_tcp_socket_connection_reset() and from the FIN handling in the state
 * machine, but only while the socket still counts as connected.
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
 * NX_ENABLE_EXTENDED_NOTIFY_SUPPORT (port/netxduo-amiga/inc/nx_user.h) is what
 * makes this reachable; without it nx_tcp_socket_establish_notify() returns
 * NX_NOT_SUPPORTED and completion has to be polled out of
 * nx_tcp_socket_state. It fires from two places:
 *
 *   nx_tcp_socket_state_syn_sent.c      -- our connect() got its SYN+ACK
 *   nx_tcp_socket_state_syn_received.c  -- a client's handshake with a socket
 *                                          we parked on a listen port
 *
 * The second case is why this matters for more than non-blocking connect:
 * bsd_listen_callback() fires when the SYN *arrives*, but a listener is not
 * accept()-ready until the handshake completes, so without this callback a
 * WaitSelect() on a listening descriptor could sleep through the ACK and only
 * wake on the next unrelated event.
 */
static VOID bsd_tcp_establish_notify(NX_TCP_SOCKET *socket_ptr)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr;

    if (sock == NULL)
        return;

    sock->as_Flags &= ~ASF_CONNECTING;
    sock->as_Flags |= ASF_CONNECTED;

    /* A socket still parked on a listen port belongs to its listener; the
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
 * This is the other half of the extended notify set, and the only thing that
 * can tell a *pending* connect() it has failed -- the create-time disconnect
 * callback above is not invoked for a socket that never reached ESTABLISHED.
 */
static VOID bsd_tcp_disconnect_complete_notify(NX_TCP_SOCKET *socket_ptr)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr;

    if (sock == NULL)
        return;

    if ((sock->as_Flags & ASF_CONNECTING) != 0)
    {
        /* The connect attempt died. Leave the reason for SO_ERROR, which is
           how a non-blocking caller finds out what went wrong. */
        sock->as_Flags &= ~ASF_CONNECTING;
        sock->as_SoError = AMI_ECONNREFUSED;
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

VOID bsd_listen_callback(NX_TCP_SOCKET *socket_ptr, UINT port)
{
    AmiSocket *sock = (AmiSocket *)socket_ptr->nx_tcp_socket_reserved_ptr;

    (VOID)port;

    if (sock == NULL)
        return;

    /* The connection lands on the parked socket; the listener is what the
     * application selects on, so the event belongs to the parent. */
    if (sock->as_Parent != NULL)
    {
        sock->as_Parent->as_Flags |= ASF_ACCEPTPEND;
        bsd_event_post(sock->as_Parent, FD_ACCEPT | FD_READ);
    }
    else
    {
        sock->as_Flags |= ASF_ACCEPTPEND;
        bsd_event_post(sock, FD_ACCEPT | FD_READ);
    }
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
           (socket.c); everything else is a *_notify() setter. The last two
           need NX_ENABLE_EXTENDED_NOTIFY_SUPPORT and would silently return
           NX_NOT_SUPPORTED without it -- see port/netxduo-amiga/inc/nx_user.h. */
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
    }
}

/* ----------------------------------------------------------- readiness ---- */

ULONG bsd_wait_option(AmiSocket *sock, ULONG timeout_ticks)
{
    if ((sock->as_Flags & ASF_NONBLOCK) != 0)
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
             * test is ESTABLISHED and nothing weaker. The parked socket sits
             * in SYN_RECEIVED from the moment bsd_listen() arms it (see
             * socket.c), and ASF_ACCEPTPEND is set by the listen callback when
             * the SYN lands -- both are true well before the handshake
             * finishes, so neither can stand in for readiness.
             */
            return (sock->as_Incoming != NULL &&
                    sock->as_Incoming->as_Nx.tcp.nx_tcp_socket_state ==
                        NX_TCP_ESTABLISHED);
        }

        /* A closed or half-closed connection reads as end-of-file. */
        if ((sock->as_Flags & (ASF_EOF | ASF_RDSHUT)) != 0)
            return TRUE;

        /*
         * Same thing seen from the state machine, in case the peer's FIN
         * landed before the socket had a disconnect callback attached.
         *
         * The states are named rather than compared with >=, and that is the
         * whole point of this comment.  NetX Duo numbers them CLOSE_WAIT 6,
         * FIN_WAIT_1 7, FIN_WAIT_2 8 (nx_api.h), so ">= NX_TCP_CLOSE_WAIT"
         * also catches the two states that mean *we* sent the FIN and are
         * still waiting to hear the peer's -- which is precisely a socket
         * after shutdown(SHUT_WR), where nothing has arrived and nothing may
         * ever arrive.  Reporting that readable makes `nc -N` (half-close,
         * then select for the rest of the answer) spin at full speed on a
         * select() that returns immediately and a recv() that returns
         * EWOULDBLOCK.  Only the states in which the peer's FIN has actually
         * been received belong here.
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
     * A pending error makes a socket writable in BSD -- that is how a caller
     * waiting on a non-blocking connect is told to go and read SO_ERROR
     * rather than waiting out its whole timeout.
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

/* --------------------------------------------------------------- timeout -- */

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

    /* NewList() is amiga.lib; open-code the port. */
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
        ami_signal_free(sig);
        return FALSE;
    }

    base->sb_TimerOpen = TRUE;

    return TRUE;
}

/* ------------------------------------------------------------- WaitSelect -- */

typedef struct
{
    ULONG   read[BSD_FD_WORDS];
    ULONG   write[BSD_FD_WORDS];
    ULONG   except[BSD_FD_WORDS];
} BsdFdSets;

/*
 * One readiness sweep, inside a ThreadX context bracket.
 *
 * WaitSelect() is the one vector that cannot simply adopt on entry and orphan
 * on exit: it parks in Exec Wait() for as long as the caller asked for, and an
 * adopted task holding the ThreadX baton across that would stop the IP thread,
 * the timer and every other socket user until the wait ended. So the bracket
 * covers only the poll -- bsd_readable() reaches nx_tcp_socket_bytes_available()
 * and nx_udp_socket_bytes_available(), both THREADS_ONLY -- and the Wait() and
 * the timer IO below happen as an ordinary Exec task.
 */
static LONG bsd_poll_sets(struct AmiSocketBase *base, LONG nfds,
                          const ULONG *in_read, const ULONG *in_write,
                          const ULONG *in_except, BsdFdSets *out)
{
    LONG fd, count = 0;

    bsd_bzero(out, sizeof(*out));

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

/* Copy the caller's set in, or leave the local copy zeroed if it is NULL. */
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
    ULONG      in_read[BSD_FD_WORDS];
    ULONG      in_write[BSD_FD_WORDS];
    ULONG      in_except[BSD_FD_WORDS];
    BsdFdSets  ready;
    LONG       words, fd, count = 0;
    ULONG      user_mask   = (signals != NULL) ? *signals : 0;
    ULONG      break_mask  = SocketBase->sb_BreakMask & ~user_mask;
    ULONG      got_signals = 0;
    ULONG      wait_mask;
    BOOL       timer_running = FALSE;
    BOOL       poll_only     = FALSE;

    if (nfds < 0 || nfds > BSD_MAX_DTABLESIZE ||
        (SocketBase->sb_Table != NULL && nfds > SocketBase->sb_TableSize))
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (timeout != NULL)
    {
        if ((ULONG)timeout->tv_micro >= 1000000UL)
            return bsd_fail(SocketBase, AMI_EINVAL);

        poll_only = ((ULONG)timeout->tv_secs == 0 &&
                     (ULONG)timeout->tv_micro == 0);
    }

    words = (LONG)((nfds + BSD_FD_BITS - 1) / BSD_FD_BITS);

    bsd_fdset_in(in_read,   read_fds,   words);
    bsd_fdset_in(in_write,  write_fds,  words);
    bsd_fdset_in(in_except, except_fds, words);

    /*
     * Validate every descriptor named in the sets BEFORE anything is written
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

    if (timeout != NULL && !poll_only)
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

    for (;;)
    {
        ULONG received;

        /*
         * Clear first, then poll. An event that arrives after the clear sets
         * the signal again, so Wait() below returns immediately -- there is
         * no window in which a wakeup goes missing.
         */
        SetSignal(0, SocketBase->sb_EventSigMask);

        count = bsd_poll_sets(SocketBase, nfds, in_read, in_write, in_except,
                              &ready);
        if (count < 0)
        {
            /* The kernel is not running; nothing can ever become ready. */
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

        received = Wait(wait_mask);

        if ((received & break_mask) != 0)
        {
            /*
             * Break. Put the signal back so the caller's own Ctrl-C handling
             * still sees it, and leave the fd sets exactly as they were.
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
                                  in_except, &ready);
            break;
        }
    }

    if (timer_running)
    {
        AbortIO((struct IORequest *)&SocketBase->sb_TimerReq);
        WaitIO((struct IORequest *)&SocketBase->sb_TimerReq);
    }

    if (count <= 0)
        bsd_bzero(&ready, sizeof(ready));

    bsd_fdset_out(read_fds,   ready.read,   words);
    bsd_fdset_out(write_fds,  ready.write,  words);
    bsd_fdset_out(except_fds, ready.except, words);

    if (signals != NULL)
        *signals = got_signals;

    return (count > 0) ? count : 0;
}

/* -------------------------------------------------- the AmiTCP event API -- */

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

        if (sock == NULL)
            continue;

        events = sock->as_Events & sock->as_EventMask;
        if (events == 0)
            continue;

        sock->as_Events &= ~events;
        *event_ptr = events;

        return fd;
    }

    /* No events pending; not an error, so errno is left alone. */
    return -1;
}
