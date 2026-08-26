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
    ULONG available = 0;

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

        if (nx_tcp_socket_bytes_available(&sock->as_Nx.tcp, &available)
                == NX_SUCCESS && available > 0)
            return TRUE;

        return FALSE;
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

    return (sock->as_Nx.tcp.nx_tcp_socket_transmit_sent_count <
            sock->as_Nx.tcp.nx_tcp_socket_transmit_queue_maximum);
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
        base->sb_TimerSignal  = -1;
        base->sb_TimerSigMask = 0;
        return FALSE;
    }

    base->sb_TimerOpen = TRUE;

    return TRUE;
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

            SetSignal(0, SocketBase->sb_EventSigMask);
            count = bsd_poll_sets(SocketBase, nfds, in_read, in_write,
                                  in_except, ready);

            if (count < 0)
                return bsd_fail(SocketBase, AMI_ENETDOWN);

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
