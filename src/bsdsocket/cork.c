/*
 * bsdsocket.library, the small-write cork behind setsockopt(TCP_NODELAY, 0).
 *
 * A write shorter than a segment is copied into a packet held on the socket
 * and credited to the caller at once.  The packet goes to NetX Duo when it is
 * full, when a write does not fit, when the caller is about to wait for the
 * peer (recv, WaitSelect on the read set), on MSG_OOB, TCP_NODELAY=1, shutdown
 * and close -- or, failing all of those, from the IP thread within a tick.
 *
 * THE ACTORS.
 *
 *   A  an application task inside bsd_nx_enter()/leave() (the baton).
 *   I  the NetX IP thread.  It holds nx_ip_protection for its whole event pass
 *      and releases it only inside tx_event_flags_get(), so bsd_cork_ip_pass()
 *      runs with the mutex held.
 *   R  the SANA-II reader.  TCP input runs on it directly, under the mutex, so
 *      the window notify runs on I or R with the mutex held.
 *   T  the ThreadX timer callback, on the tick task under Forbid().  It only
 *      sets NX_IP_CORK_EVENT.
 *   F  the append fast path (transfer.c, AMINETXDUO_TCP_CORK_FASTPATH), an
 *      application task OUTSIDE the bracket.
 *   B  another task holding the same socket through handoff.c.
 *
 * THE RULES.
 *
 *   as_CorkState, as_CorkFlags, as_CorkPkt, as_CorkOwner and the armed list
 *   change only under Forbid().  Byte copies and NetX Duo calls happen outside
 *   it, covered by the state: APPEND is F's, FLUSH is as_CorkOwner's.
 *
 *   Only a FLUSH owner sends, and a packet goes to NetX Duo only while it is
 *   detached (as_CorkPkt NULL).  NX_SUCCESS means NetX owns it.  Any other
 *   status leaves the remainder with us, already trimmed by NetX
 *   (nx_tcp_socket_send_internal.c) -- the bytes were credited when they were
 *   appended and are never credited again.
 *
 *   While a packet is pending or the state is not IDLE, no new data goes to
 *   NetX Duo around it, even after TCP_NODELAY=1: it is appended, or waits for
 *   the pending segment to drain first.
 *
 * THE BARRIER.  An A that finds FLUSH owned by the IP thread takes and gives
 * nx_ip_protection.  I set FLUSH(IP) with the mutex held and puts the socket
 * back to IDLE before its pass returns, and it releases the mutex only between
 * passes, so the get returns after the pass is over.  That holds across every
 * Exec wait the send path can make (sana2_tx.c: a third-party BeginIO() may
 * ObtainSemaphore() or Wait(), and a log hook may too):
 *
 *   - An Exec wait that does NOT give up the baton (every one on the transmit
 *     path: BeginIO() and the driver's copy hooks are called straight, with no
 *     netstack_baton bracket around them) leaves I the only ThreadX thread
 *     that can run.  No A is inside the bracket at all while it lasts, so none
 *     can reach NetX Duo; F and non-bracketed tasks can run, and F touches only
 *     an IDLE socket, never a FLUSH one, and never calls NetX.
 *
 *   - An Exec wait that DOES give up the baton (sana2_device.c's DoIO bracket,
 *     used for device commands, not for CMD_WRITE) lets an A in, but I still
 *     holds nx_ip_protection.  Every NetX Duo entry point an A can reach takes
 *     that mutex, and so does the barrier: the A suspends in ThreadX until the
 *     pass is over.
 *
 * So the only NetX Duo access to a socket in FLUSH(IP) is I's own.
 *
 * LIFETIME.  bsd_cork_start() creates the timer and installs the handler when
 * the stack comes up (library.c, bsd_lib_open); bsd_cork_stop() deactivates
 * and deletes the timer, clears the handler under nx_ip_protection and empties
 * the armed list before netstack_shutdown() deletes the IP instance.  A socket
 * is unlinked under Forbid() by bsd_cork_drop() before socket.c frees it; a
 * parked closing socket stays linked with TICK (its reserved_ptr is NULL, so
 * the window notify cannot find it) until the sweep drops it.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "aminetxduo/nxstatus.h"

#include "nx_ip.h"
#include "nx_tcp.h"

#include <proto/exec.h>

/* Everything below is Forbid()-guarded: the armed list, the timer's state,
   and which IP instance the handler is installed on. */
static AmiSocket *bsd_cork_armed;
static NX_IP     *bsd_cork_ip;
static TX_TIMER   bsd_cork_timer;
static BOOL       bsd_cork_timer_made;
static BOOL       bsd_cork_timer_live;

BOOL bsd_cork_running(VOID)
{
    return (bsd_cork_ip != NULL) ? TRUE : FALSE;
}

/* T.  The tick task, under Forbid(): the event and nothing else. */
VOID bsd_cork_tick(ULONG ip_arg)
{
    NX_IP *ip = (NX_IP *)ip_arg;

    AMI_NX_ONLY_SUCCESS(tx_event_flags_set(&ip->nx_ip_events, NX_IP_CORK_EVENT,
                                           TX_OR));
}

/* ------------------------------------------------------ the armed list -- */

/* Forbid() held.  Links the socket if it is not, and starts the tick if the
   socket asks for one and it is not running. */
static VOID bsd_cork_link_locked(AmiSocket *sock)
{
    if ((sock->as_CorkFlags & BSD_CORKF_LINKED) == 0)
    {
        sock->as_CorkNext   = bsd_cork_armed;
        bsd_cork_armed      = sock;
        sock->as_CorkFlags |= BSD_CORKF_LINKED;
    }

    if ((sock->as_CorkFlags & BSD_CORKF_TICK) != 0 &&
        bsd_cork_timer_made && !bsd_cork_timer_live)
    {
        if (tx_timer_activate(&bsd_cork_timer) == TX_SUCCESS)
            bsd_cork_timer_live = TRUE;
    }
}

/* Forbid() held. */
static VOID bsd_cork_unlink_locked(AmiSocket *sock)
{
    AmiSocket **link = &bsd_cork_armed;

    if ((sock->as_CorkFlags & BSD_CORKF_LINKED) == 0)
        return;

    while (*link != NULL)
    {
        if (*link == sock)
        {
            *link = sock->as_CorkNext;
            break;
        }
        link = &(*link)->as_CorkNext;
    }

    sock->as_CorkNext   = NULL;
    sock->as_CorkFlags &= (UBYTE)~BSD_CORKF_LINKED;
}

/* Forbid() held.  The tick stops when no linked socket needs it: a STALLED one
   is woken by the window notify instead. */
static VOID bsd_cork_tick_settle_locked(VOID)
{
    AmiSocket *sock;

    if (!bsd_cork_timer_live)
        return;

    for (sock = bsd_cork_armed; sock != NULL; sock = sock->as_CorkNext)
    {
        if ((sock->as_CorkFlags & BSD_CORKF_TICK) != 0)
            return;
    }

    AMI_NX_CLEANUP(tx_timer_deactivate(&bsd_cork_timer));
    bsd_cork_timer_live = FALSE;
}

/* ----------------------------------------------------------- lifetime -- */

VOID bsd_cork_start(NX_IP *ip)
{
    AmiNetCaller *caller;

    if (ip == NULL || bsd_cork_ip != NULL)
        return;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
        return;

    if (!bsd_cork_timer_made)
        bsd_cork_timer_made =
            (tx_timer_create(&bsd_cork_timer, (CHAR *)"AmiNetXDuo cork",
                             bsd_cork_tick, (ULONG)ip, 1UL, 1UL,
                             TX_NO_ACTIVATE) == TX_SUCCESS) ? TRUE : FALSE;

    if (bsd_cork_timer_made)
    {
        Forbid();
        ip->nx_ip_cork_handler = bsd_cork_ip_pass;
        bsd_cork_ip            = ip;
        Permit();
    }

    ami_netstack_leave_free(caller);
}

VOID bsd_cork_stop(VOID)
{
    AmiNetCaller *caller;
    NX_IP        *ip = bsd_cork_ip;
    AmiSocket    *list, *sock, *next;

    if (ip == NULL)
        return;

    caller = ami_netstack_enter_alloc();
    if (caller == NULL)
    {
        /* No kernel to enter is no kernel to run a tick or a pass either;
           forget both so the next stack starts clean. */
        Forbid();
        ip->nx_ip_cork_handler = NX_NULL;
        bsd_cork_ip            = NULL;
        bsd_cork_timer_made    = FALSE;
        bsd_cork_timer_live    = FALSE;
        for (sock = bsd_cork_armed; sock != NULL; sock = next)
        {
            next = sock->as_CorkNext;
            sock->as_CorkNext   = NULL;
            sock->as_CorkFlags &= (UBYTE)~BSD_CORKF_LINKED;
        }
        bsd_cork_armed         = NULL;
        Permit();
        return;
    }

    /* The timer first, so no new event is set; then the handler, under the
       mutex, so no pass is running while it goes.  A bit already set finds
       NX_NULL. */
    if (bsd_cork_timer_made)
    {
        AMI_NX_CLEANUP(tx_timer_deactivate(&bsd_cork_timer));
        AMI_NX_CLEANUP(tx_timer_delete(&bsd_cork_timer));
    }

    AMI_NX_ONLY_SUCCESS(tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER));

    Forbid();
    ip->nx_ip_cork_handler = NX_NULL;
    bsd_cork_ip            = NULL;
    bsd_cork_timer_made    = FALSE;
    bsd_cork_timer_live    = FALSE;
    list                   = bsd_cork_armed;
    bsd_cork_armed         = NULL;
    for (sock = list; sock != NULL; sock = sock->as_CorkNext)
        sock->as_CorkFlags &= (UBYTE)~BSD_CORKF_LINKED;
    Permit();

    /* Whatever is still on the list belongs to a parked or leaked socket the
       stack teardown is about to take the connection of.  Its packets go back
       while the pool still exists. */
    for (sock = list; sock != NULL; sock = next)
    {
        NX_PACKET *pkt;

        next = sock->as_CorkNext;
        sock->as_CorkNext = NULL;

        Forbid();
        pkt = (sock->as_CorkState == BSD_CORK_IDLE) ? sock->as_CorkPkt : NULL;
        if (pkt != NULL)
            sock->as_CorkPkt = NULL;
        sock->as_CorkFlags &= (UBYTE)~(BSD_CORKF_STALLED | BSD_CORKF_TICK |
                                       BSD_CORKF_FIN | BSD_CORKF_KICK);
        Permit();

        if (pkt != NULL)
            AMI_NX_CLEANUP(nx_packet_release(pkt));
    }

    AMI_NX_ONLY_SUCCESS(tx_mutex_put(&ip->nx_ip_protection));

    ami_netstack_leave_free(caller);
}

/* ---------------------------------------------------------- ownership -- */

/*
 * Whether a socket's writes may be held at all: TCP_NODELAY 0, a connection
 * that can carry data, and an interface that is not the loopback -- the same
 * test as bsd_send_run_iface() (transfer.c).  A loopback send is received
 * directly, inside the sender's own call, so holding one only adds a tick.
 */
BOOL bsd_cork_corkable(const AmiSocket *sock)
{
    const NX_INTERFACE *nxif;
    UINT                state;

    if ((sock->as_CorkFlags & BSD_CORKF_ON) == 0 || bsd_cork_ip == NULL)
        return FALSE;

    state = sock->as_Nx.tcp.nx_tcp_socket_state;
    if (state != NX_TCP_ESTABLISHED && state != NX_TCP_CLOSE_WAIT)
        return FALSE;

    nxif = sock->as_Nx.tcp.nx_tcp_socket_connect_interface;

    return (nxif != NX_NULL && nxif->nx_interface_additional_link_info != NX_NULL)
               ? TRUE : FALSE;
}

/* Room left in the pending segment, 0 when there is none. */
ULONG bsd_cork_room(const AmiSocket *sock)
{
    const NX_PACKET *pkt = sock->as_CorkPkt;
    ULONG            room, tail;

    if (pkt == NULL || pkt->nx_packet_length >= sock->as_CorkCap)
        return 0;

    room = sock->as_CorkCap - pkt->nx_packet_length;
    tail = (ULONG)(pkt->nx_packet_data_end - pkt->nx_packet_append_ptr);

    return (tail < room) ? tail : room;
}

/* The barrier (see the header): returns once I's pass is over. */
static VOID bsd_cork_barrier(VOID)
{
    NX_IP *ip = bsd_cork_ip;

    if (ip == NULL)
        return;

    AMI_NX_ONLY_SUCCESS(tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER));
    AMI_NX_ONLY_SUCCESS(tx_mutex_put(&ip->nx_ip_protection));
}

/*
 * A, inside the bracket.  Takes the socket for this task: 0 with the state
 * FLUSH(this task), the socket off the armed list and *pkt the detached
 * pending segment (NULL when there is none).  AMI_EAGAIN when another task or
 * the fast path has it and `wait` is NX_NO_WAIT; AMI_EINTR when the break
 * arrives while waiting for one.
 */
LONG bsd_cork_claim(struct AmiSocketBase *base, AmiSocket *sock, ULONG wait,
                    NX_PACKET **pkt)
{
    APTR me = (APTR)FindTask(NULL);

    *pkt = NULL;

    for (;;)
    {
        BOOL ip_owned, mine;

        Forbid();
        if (sock->as_CorkState == BSD_CORK_IDLE)
        {
            bsd_cork_unlink_locked(sock);
            sock->as_CorkState  = BSD_CORK_FLUSH;
            sock->as_CorkOwner  = me;
            sock->as_CorkFlags &= (UBYTE)~(BSD_CORKF_STALLED | BSD_CORKF_TICK |
                                           BSD_CORKF_KICK);
            *pkt                = sock->as_CorkPkt;
            sock->as_CorkPkt    = NULL;
            Permit();
            return 0;
        }
        ip_owned = (sock->as_CorkState == BSD_CORK_FLUSH &&
                    sock->as_CorkOwner == BSD_CORK_OWNER_IP) ? TRUE : FALSE;
        mine     = (sock->as_CorkState == BSD_CORK_FLUSH &&
                    sock->as_CorkOwner == me) ? TRUE : FALSE;
        Permit();

        if (ip_owned && bsd_cork_ip != NULL)
        {
            bsd_cork_barrier();
            continue;
        }

        /* This task's own claim, taken again: a path that nests two.  Waiting
           for itself would never end. */
        if (mine)
            return AMI_EINVAL;

        if (wait == NX_NO_WAIT)
            return AMI_EAGAIN;

        if (base != NULL && (bsd_break_signals(base) & base->sb_BreakMask) != 0)
            return AMI_EINTR;

        /* F copying, or B inside a send of its own.  Either is short or has a
           NetX Duo wait of its own under it; a tick gives it the machine. */
        AMI_NX_ONLY_SUCCESS(tx_thread_sleep(1));
    }
}

/*
 * Give the socket back.  `pkt` is what is left of the segment (NULL when it
 * went), `why` the flag its remainder waits on: BSD_CORKF_STALLED for the
 * window notify, BSD_CORKF_TICK for the timer.  TRUE when the segment is gone
 * and a FIN was waiting behind it: the caller sends it.
 */
BOOL bsd_cork_unclaim(AmiSocket *sock, NX_PACKET *pkt, ULONG why)
{
    BOOL fin = FALSE;

    Forbid();
    sock->as_CorkState = BSD_CORK_IDLE;
    sock->as_CorkOwner = NULL;
    sock->as_CorkPkt   = pkt;

    if (pkt != NULL)
    {
        /* A window that opened while the socket was owned left KICK: the
           notify found nothing STALLED to wake, so the tick does it. */
        if ((why & BSD_CORKF_STALLED) != 0 &&
            (sock->as_CorkFlags & BSD_CORKF_KICK) != 0)
            why = BSD_CORKF_TICK;

        /* Parked, the notify cannot find it (reserved_ptr is NULL). */
        if ((sock->as_Flags & ASF_CLOSING) != 0)
            why |= BSD_CORKF_TICK;

        if ((why & (BSD_CORKF_STALLED | BSD_CORKF_TICK)) == 0)
            why = BSD_CORKF_TICK;

        sock->as_CorkFlags |= (UBYTE)(why & (BSD_CORKF_STALLED | BSD_CORKF_TICK));
        sock->as_CorkFlags &= (UBYTE)~BSD_CORKF_KICK;
        bsd_cork_link_locked(sock);
    }
    else
    {
        fin = ((sock->as_CorkFlags & BSD_CORKF_FIN) != 0) ? TRUE : FALSE;
        sock->as_CorkFlags &= (UBYTE)~(BSD_CORKF_STALLED | BSD_CORKF_TICK |
                                       BSD_CORKF_FIN | BSD_CORKF_KICK);
    }
    Permit();

    return fin;
}

/*
 * What a send's status leaves.  0 and *pkt NULL: gone, NetX Duo has it or it
 * was dropped.  Otherwise the flag the remainder, still in *pkt, waits on.
 * A connection that is gone drops the segment silently -- the close is
 * reported elsewhere; any other refusal drops it and becomes SO_ERROR.
 */
ULONG bsd_cork_settle(AmiSocket *sock, NX_PACKET **pkt, UINT status)
{
    switch (status)
    {
        case NX_SUCCESS:
            *pkt = NULL;
            return 0;

        case NX_WINDOW_OVERFLOW:
        case NX_TX_QUEUE_DEPTH:
            return BSD_CORKF_STALLED;

        case NX_NO_PACKET:
            return BSD_CORKF_TICK;

        case NX_NOT_CONNECTED:
            AMI_NX_CLEANUP(nx_packet_release(*pkt));
            *pkt = NULL;
            return 0;

        default:
            AMI_NX_CLEANUP(nx_packet_release(*pkt));
            *pkt = NULL;
            sock->as_SoError = bsd_errno_from_nx(status);
            bsd_event_post(sock, FD_ERROR | FD_WRITE);
            return 0;
    }
}

/* The connection a segment can still go out on.  An interface that went away
   under a corked socket takes the segment with it. */
static BOOL bsd_cork_can_send(const AmiSocket *sock)
{
    const NX_INTERFACE *nxif = sock->as_Nx.tcp.nx_tcp_socket_connect_interface;
    UINT                state = sock->as_Nx.tcp.nx_tcp_socket_state;

    if (state != NX_TCP_ESTABLISHED && state != NX_TCP_CLOSE_WAIT)
        return FALSE;

    return (nxif != NX_NULL && nxif->nx_interface_valid &&
            nxif->nx_interface_additional_link_info != NX_NULL) ? TRUE : FALSE;
}

/* One NX_NO_WAIT send of a detached segment: the status it came back with. */
static UINT bsd_cork_send_now(AmiSocket *sock, NX_PACKET *pkt)
{
    if (!bsd_cork_can_send(sock))
        return NX_NOT_CONNECTED;

    return nx_tcp_socket_send(&sock->as_Nx.tcp, pkt, NX_NO_WAIT);
}

/* ------------------------------------------------------------ the pass -- */

/* I, nx_ip_protection held. */
static VOID bsd_cork_ip_flush(AmiSocket *sock)
{
    NX_PACKET *pkt;
    UINT       status;
    ULONG      why = 0;

    Forbid();
    pkt              = sock->as_CorkPkt;
    sock->as_CorkPkt = NULL;         /* detached before NetX Duo sees it */
    Permit();

    if (pkt != NULL)
    {
        status = bsd_cork_send_now(sock, pkt);
        why    = bsd_cork_settle(sock, &pkt, status);
    }

    if (bsd_cork_unclaim(sock, pkt, why))
        bsd_tcp_send_fin(sock);
}

VOID bsd_cork_ip_pass(NX_IP *ip)
{
    AmiSocket *list, *sock, *next;
    AmiSocket *claimed = NULL;

    (VOID)ip;

    /* Take the list.  An IDLE socket with somewhere to go becomes FLUSH(IP);
       one being appended to is looked at again next tick; a STALLED one waits
       for its window. */
    Forbid();
    list           = bsd_cork_armed;
    bsd_cork_armed = NULL;

    for (sock = list; sock != NULL; sock = next)
    {
        next = sock->as_CorkNext;
        sock->as_CorkNext   = NULL;
        sock->as_CorkFlags &= (UBYTE)~BSD_CORKF_LINKED;

        /* STALLED waits for the notify, unless TICK says the notify cannot
           reach it: a parked closing socket has no reserved_ptr. */
        if (sock->as_CorkState == BSD_CORK_IDLE &&
            ((sock->as_CorkFlags & BSD_CORKF_STALLED) == 0 ||
             (sock->as_CorkFlags & BSD_CORKF_TICK) != 0) &&
            sock->as_CorkPkt != NULL)
        {
            sock->as_CorkState  = BSD_CORK_FLUSH;
            sock->as_CorkOwner  = BSD_CORK_OWNER_IP;
            sock->as_CorkFlags &= (UBYTE)~(BSD_CORKF_STALLED | BSD_CORKF_TICK |
                                           BSD_CORKF_KICK);
            sock->as_CorkNext   = claimed;
            claimed             = sock;
            continue;
        }

        if (sock->as_CorkState == BSD_CORK_APPEND)
            sock->as_CorkFlags |= BSD_CORKF_TICK;

        if (sock->as_CorkPkt != NULL || sock->as_CorkState != BSD_CORK_IDLE)
            bsd_cork_link_locked(sock);
        else
            sock->as_CorkFlags &= (UBYTE)~(BSD_CORKF_STALLED | BSD_CORKF_TICK);
    }
    Permit();

    /* The local chain reuses as_CorkNext: FLUSH(IP) keeps every other actor
       off it until bsd_cork_unclaim() hands it back. */
    for (sock = claimed; sock != NULL; sock = next)
    {
        next = sock->as_CorkNext;
        sock->as_CorkNext = NULL;
        bsd_cork_ip_flush(sock);
    }

    Forbid();
    bsd_cork_tick_settle_locked();
    Permit();
}

/* ---------------------------------------------------- the task flushes -- */

/*
 * A: one NX_NO_WAIT push of whatever is pending.  recv() about to wait,
 * WaitSelect() on a read set with nothing to read, TCP_NODELAY=1.  What does
 * not go stays pending for the window notify or the tick.
 */
VOID bsd_cork_push(struct AmiSocketBase *base, AmiSocket *sock)
{
    NX_PACKET *pkt;
    ULONG      why = 0;

    if (sock->as_CorkPkt == NULL)
        return;

    if (bsd_cork_claim(base, sock, NX_NO_WAIT, &pkt) != 0)
        return;                         /* its owner is already on it */

    if (pkt != NULL)
        why = bsd_cork_settle(sock, &pkt, bsd_cork_send_now(sock, pkt));

    if (bsd_cork_unclaim(sock, pkt, why))
        bsd_tcp_send_fin(sock);
}

/* setsockopt(TCP_NODELAY).  0 on success or an AMI_ errno. */
LONG bsd_cork_set(struct AmiSocketBase *base, AmiSocket *sock, BOOL on)
{
    if (on)
    {
        if (!bsd_cork_running())
            return AMI_ENOBUFS;

        Forbid();
        sock->as_CorkFlags |= BSD_CORKF_ON;
        Permit();
        return 0;
    }

    Forbid();
    sock->as_CorkFlags &= (UBYTE)~BSD_CORKF_ON;
    Permit();

    bsd_cork_push(base, sock);

    return 0;
}

/* The FIN behind a pending segment.  TRUE: it is deferred, and whoever sends
   the segment's last byte sends it. */
static BOOL bsd_cork_fin_behind(AmiSocket *sock, ULONG extra)
{
    NX_PACKET *pkt;
    ULONG      why = 0;

    if (sock->as_CorkPkt == NULL && sock->as_CorkState == BSD_CORK_IDLE)
        return FALSE;

    if (bsd_cork_claim(NULL, sock, NX_WAIT_FOREVER, &pkt) != 0)
        return FALSE;

    if (pkt != NULL)
        why = bsd_cork_settle(sock, &pkt, bsd_cork_send_now(sock, pkt));

    if (pkt != NULL)
    {
        Forbid();
        sock->as_CorkFlags |= BSD_CORKF_FIN;
        Permit();
        (VOID)bsd_cork_unclaim(sock, pkt, why | extra);
        return TRUE;
    }

    (VOID)bsd_cork_unclaim(sock, NULL, 0);
    return FALSE;
}

/* shutdown(SHUT_WR). */
BOOL bsd_cork_shut_write(AmiSocket *sock)
{
    return bsd_cork_fin_behind(sock, 0);
}

/* CloseSocket() without SO_LINGER.  The socket is parked next, and a parked
   socket keeps the tick. */
BOOL bsd_cork_close_graceful(AmiSocket *sock)
{
    return bsd_cork_fin_behind(sock, BSD_CORKF_TICK);
}

/*
 * CloseSocket() with a timed SO_LINGER: the pending segment gets up to the
 * linger time to leave.  TRUE with *left the ticks still to spend on the
 * disconnect; FALSE when the time ran out with data still held, which the
 * caller turns into an abort.
 */
BOOL bsd_cork_close_linger(AmiSocket *sock, ULONG ticks, ULONG *left)
{
    NX_PACKET *pkt;
    ULONG      started, spent;
    UINT       status;

    *left = ticks;

    if (sock->as_CorkPkt == NULL && sock->as_CorkState == BSD_CORK_IDLE)
        return TRUE;

    if (bsd_cork_claim(NULL, sock, NX_WAIT_FOREVER, &pkt) != 0)
        return TRUE;

    if (pkt == NULL)
    {
        (VOID)bsd_cork_unclaim(sock, NULL, 0);
        return TRUE;
    }

    started = tx_time_get();
    status  = bsd_cork_can_send(sock)
                  ? nx_tcp_socket_send(&sock->as_Nx.tcp, pkt, ticks)
                  : NX_NOT_CONNECTED;
    spent   = tx_time_get() - started;

    if (status == NX_WINDOW_OVERFLOW || status == NX_TX_QUEUE_DEPTH ||
        status == NX_NO_PACKET)
    {
        AMI_NX_CLEANUP(nx_packet_release(pkt));
        (VOID)bsd_cork_unclaim(sock, NULL, 0);
        *left = 0;
        return FALSE;
    }

    /* Gone, or dropped with the connection: the disconnect gets what is left
       of the linger, and at least a tick to say so. */
    (VOID)bsd_cork_settle(sock, &pkt, status);
    (VOID)bsd_cork_unclaim(sock, NULL, 0);

    *left = (spent >= ticks) ? 1UL : ticks - spent;

    return TRUE;
}

/*
 * The abortive paths and every teardown: linger 0, unread data, destroy, the
 * closing sweep's deadline, drain, CloseLibrary.  The segment is released and
 * the socket leaves the armed list, so no pass and no tick can reach it after
 * this returns.  Waits out F's copy and I's pass; another task's send (a
 * socket shared through handoff.c) is waited out the same way.
 */
VOID bsd_cork_drop(AmiSocket *sock)
{
    NX_PACKET *pkt = NULL;

    APTR       me  = (APTR)FindTask(NULL);

    for (;;)
    {
        BOOL ip_owned;

        Forbid();
        /* No pass can be running once the stack has stopped, and this task
           cannot be inside a claim of its own here: either way nobody else
           will hand the segment back, so take it. */
        if (sock->as_CorkState != BSD_CORK_IDLE &&
            ((sock->as_CorkOwner == BSD_CORK_OWNER_IP && bsd_cork_ip == NULL) ||
             sock->as_CorkOwner == me))
        {
            sock->as_CorkState = BSD_CORK_IDLE;
            sock->as_CorkOwner = NULL;
        }

        if (sock->as_CorkState == BSD_CORK_IDLE)
        {
            pkt              = sock->as_CorkPkt;
            sock->as_CorkPkt = NULL;
            bsd_cork_unlink_locked(sock);
            sock->as_CorkFlags &= (UBYTE)~(BSD_CORKF_STALLED | BSD_CORKF_TICK |
                                           BSD_CORKF_FIN | BSD_CORKF_KICK);
            Permit();
            break;
        }
        ip_owned = (sock->as_CorkOwner == BSD_CORK_OWNER_IP) ? TRUE : FALSE;
        Permit();

        if (ip_owned)
            bsd_cork_barrier();
        else
            AMI_NX_ONLY_SUCCESS(tx_thread_sleep(1));
    }

    if (pkt != NULL)
        AMI_NX_CLEANUP(nx_packet_release(pkt));
}

/* ------------------------------------------------------------ wakeups -- */

/* I or R, nx_ip_protection held: the window notify (select.c).  Never sends
   here -- on the loopback a send is received inside the call and re-enters
   this socket's packet processing -- it only wakes the pass. */
VOID bsd_cork_window_open(AmiSocket *sock)
{
    BOOL wake = FALSE;

    Forbid();
    if ((sock->as_CorkFlags & BSD_CORKF_STALLED) != 0)
    {
        sock->as_CorkFlags &= (UBYTE)~BSD_CORKF_STALLED;
        wake = TRUE;
    }
    else if (sock->as_CorkState != BSD_CORK_IDLE)
    {
        sock->as_CorkFlags |= BSD_CORKF_KICK;
    }
    Permit();

    if (wake)
        bsd_cork_wake(sock);
}

/* Set the event for a pass now rather than at the next tick. */
VOID bsd_cork_wake(AmiSocket *sock)
{
    NX_IP *ip = bsd_cork_ip;

    (VOID)sock;

    if (ip != NULL)
        AMI_NX_ONLY_SUCCESS(tx_event_flags_set(&ip->nx_ip_events, NX_IP_CORK_EVENT,
                                           TX_OR));
}
