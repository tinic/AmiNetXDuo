/*
 * bsdsocket.library, socket lifecycle and the per-opener descriptor table.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bsdsocket_vectors.h"
#include "aminetxduo/nxstatus.h"
#include "aminetxduo/config.h"
#include "aminetxduo/sana2.h"
#include "netmonitor.h"

#include "nx_tcp.h"

#include "nx_ip.h"
#ifdef AMINETXDUO_IPV6
#include "nx_ipv6.h"
#include "../ipv6/ipv6_srcsel.h"
#endif

#include "aminetxduo/random.h"

#include <proto/exec.h>

static char bsd_tcp_name[] = "AmiNetXDuo TCP";
static char bsd_udp_name[] = "AmiNetXDuo UDP";

/*
 * shutdown(SHUT_WR).  Call with the ThreadX scheduler lock held (bsd_nx_enter);
 * takes the IP protection mutex, as _nx_tcp_packet_send_fin() requires.
 */
#ifndef AMINETXDUO_TCP_CORK
static
#endif
VOID bsd_tcp_send_fin(AmiSocket *sock)
{
    NX_TCP_SOCKET *tcp = &sock->as_Nx.tcp;
    NX_IP         *ip  = tcp->nx_tcp_socket_ip_ptr;

    if (ip == NX_NULL)
        return;

    tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);

    if (tcp->nx_tcp_socket_state == NX_TCP_ESTABLISHED)
    {
        tcp->nx_tcp_socket_state = NX_TCP_FIN_WAIT_1;
    }
    else if (tcp->nx_tcp_socket_state == NX_TCP_CLOSE_WAIT)
    {
        tcp->nx_tcp_socket_state = NX_TCP_LAST_ACK;
    }
    else
    {
        tx_mutex_put(&ip->nx_ip_protection);
        return;
    }

    if (tcp->nx_tcp_socket_transmit_sent_head == NX_NULL)
    {
        tcp->nx_tcp_socket_timeout         = tcp->nx_tcp_socket_timeout_rate;
        tcp->nx_tcp_socket_timeout_retries = 0;
    }

    tcp->nx_tcp_socket_tx_sequence++;
    _nx_tcp_packet_send_fin(tcp, tcp->nx_tcp_socket_tx_sequence - 1);

    tx_mutex_put(&ip->nx_ip_protection);
}

static ULONG bsd_udp_queue_max(struct AmiSocketBase *base)
{
    NX_PACKET_POOL *pool = bsd_stack_pool(base);
    ULONG           queue;

    if (pool == NULL)
        return BSD_UDP_QUEUE_MIN;

    queue = pool->nx_packet_pool_total / BSD_UDP_POOL_SHARE;

    if (queue < BSD_UDP_QUEUE_MIN)
        queue = BSD_UDP_QUEUE_MIN;
    if (queue > BSD_UDP_QUEUE_CEILING)
        queue = BSD_UDP_QUEUE_CEILING;

    return queue;
}

/*
 * The sockets that can still draw on the receive budget: connecting or
 * connected, or closed by this end only (FIN_WAIT, the peer may still send).
 * One the peer has finished with (CLOSE_WAIT, CLOSING, LAST_ACK, TIMED_WAIT)
 * receives nothing more and does not halve the next socket's window --
 * every connection the web shell or a fetch has just finished sat in the
 * count for its 2MSL.
 */
static BOOL bsd_tcp_draws(UINT state)
{
    return ((state >= NX_TCP_SYN_SENT && state <= NX_TCP_ESTABLISHED) ||
            state == NX_TCP_FIN_WAIT_1 || state == NX_TCP_FIN_WAIT_2)
           ? TRUE : FALSE;
}

static ULONG bsd_tcp_consumer_count(NX_IP *ip)
{
    NX_TCP_SOCKET *tcp   = ip->nx_ip_tcp_created_sockets_ptr;
    ULONG          total = ip->nx_ip_tcp_created_sockets_count;
    ULONG          live  = 0;
    ULONG          i;

    for (i = 0; i < total && tcp != NX_NULL; i++)
    {
        if (bsd_tcp_draws(tcp->nx_tcp_socket_state))
            live++;

        tcp = tcp->nx_tcp_socket_created_next;
    }

    return live;
}

ULONG ami_bsd_tcp_window(struct AmiSocketBase *base)
{
    static ULONG    last_budget = 0;
    NX_PACKET_POOL *pool = bsd_stack_pool(base);
    NX_IP          *ip   = bsd_stack_ip(base);
    ULONG           budget;
    ULONG           cap;

    if (pool == NULL || ip == NULL)
        return (ULONG)BSD_TCP_WINDOW;

    budget = ami_bsd_tcp_budget(pool->nx_packet_pool_total,
                                pool->nx_packet_pool_payload_size);

    cap = (ULONG)BSD_TCP_WINDOW_CEILING;
    if (cap > budget)
        cap = budget;

    if (budget != last_budget)
    {
        last_budget = budget;
        AMI_INFO("bsdsocket: TCP window budget %ld bytes (pool %ld packets), "
                 "%ld..%ld per socket, %ld to open with",
                 (long)budget, (long)pool->nx_packet_pool_total,
                 (long)BSD_TCP_WINDOW, (long)cap, (long)BSD_TCP_WINDOW_LAN);
    }

    return ami_bsd_tcp_window_for(pool->nx_packet_pool_total,
                                  pool->nx_packet_pool_payload_size,
                                  bsd_tcp_consumer_count(ip));
}

/*
 * The window a socket may grow to once its handshake has measured the path
 * (bsdsocket_window.h).  Set on the NetX socket right after create, before
 * anything can send a SYN, because the fork negotiates the window scale from
 * it; the socket's advertised window stays at what it was created with until
 * bsd_tcp_window_grow() decides otherwise.
 */
static VOID bsd_tcp_window_maximum(struct AmiSocketBase *base,
                                   AmiSocket *sock)
{
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    NX_PACKET_POOL *pool = bsd_stack_pool(base);
    NX_IP          *ip   = bsd_stack_ip(base);
    ULONG           share, ceiling;

    if (pool == NULL || ip == NULL)
        return;                         /* create left it at the window */

    /* The same consumer count the window itself was drawn against: this
       socket is still CLOSED and is not among them.  The scale is
       negotiated for the autotuning ceiling where the pool has one. */
    share = ami_bsd_tcp_window_max_for(pool->nx_packet_pool_total,
                                       pool->nx_packet_pool_payload_size,
                                       bsd_tcp_consumer_count(ip));
    ceiling = ami_bsd_tcp_rx_auto_ceiling(pool->nx_packet_pool_total,
                                          pool->nx_packet_pool_payload_size);
    sock->as_RxShare = share;
    sock->as_Nx.tcp.nx_tcp_socket_rx_window_maximum =
        (ceiling > share) ? ceiling : share;
#else
    /* Without the scale option there is no field and nothing to grow to:
       sixteen bits is the ceiling and the socket was created at it. */
    (VOID)base;
    (VOID)sock;
#endif
}

/* What a socket may grow to: the maximum where the build keeps one, else
   the window it was created with. */
static ULONG bsd_tcp_window_top(const NX_TCP_SOCKET *tcp)
{
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    return tcp->nx_tcp_socket_rx_window_maximum;
#else
    return tcp->nx_tcp_socket_rx_window_default;
#endif
}

/* A connection that has taken no data for this long is not receiving: an
   open shell or a page held open keeps no part of the card. */
#define BSD_TCP_SHARE_IDLE_MS   2000UL

/*
 * The settled connections on `sana` whose window the card bounds (still able
 * to receive, burst-bound at the round trip they settled at, and taking data
 * now or settled within BSD_TCP_SHARE_IDLE_MS): what shares the card's
 * memory (ami_bsd_tcp_window_ring_share).  At least 1.
 */
static ULONG bsd_tcp_ring_sharers(NX_IP *ip, AmiSana2If *sana)
{
    NX_TCP_SOCKET *tcp   = ip->nx_ip_tcp_created_sockets_ptr;
    ULONG          total = ip->nx_ip_tcp_created_sockets_count;
    ULONG          bps   = ami_sana2_get_bps(sana);
    ULONG          grow  = ami_sana2_get_tcp_grow_rtt(sana);
    ULONG          now   = ami_millis();
    ULONG          n     = 0;
    ULONG          i;

    for (i = 0; i < total && tcp != NX_NULL; i++)
    {
        NX_INTERFACE *nxif = tcp->nx_tcp_socket_connect_interface;
        AmiSocket    *sock = (AmiSocket *)tcp->nx_tcp_socket_reserved_ptr;

        if (nxif != NX_NULL &&
            (AmiSana2If *)nxif->nx_interface_additional_link_info == sana &&
            sock != NULL && sock->as_SettleWant != 0 &&
            bsd_tcp_draws(tcp->nx_tcp_socket_state) &&
            ami_bsd_tcp_window_burst_bound(bps, sock->as_SettleRtt, grow))
        {
            if (tcp->nx_tcp_socket_rx_pass != sock->as_SharePass)
            {
                sock->as_SharePass   = tcp->nx_tcp_socket_rx_pass;
                sock->as_ShareMillis = now;
            }
            if (now - sock->as_ShareMillis <= BSD_TCP_SHARE_IDLE_MS)
                n++;
        }

        tcp = tcp->nx_tcp_socket_created_next;
    }

    return (n != 0) ? n : 1UL;
}

/*
 * Whether a connection on `sana` is taking data over a long path with the
 * settled window rather than the card's fit (bsdsocket_window.h,
 * ami_bsd_tcp_window_receive_bound): what an overrun is blamed on.
 */
static BOOL bsd_tcp_wan_unfitted(NX_IP *ip, AmiSana2If *sana)
{
    NX_TCP_SOCKET *tcp   = ip->nx_ip_tcp_created_sockets_ptr;
    ULONG          total = ip->nx_ip_tcp_created_sockets_count;
    ULONG          bps   = ami_sana2_get_bps(sana);
    ULONG          grow  = ami_sana2_get_tcp_grow_rtt(sana);
    ULONG          now   = ami_millis();
    ULONG          i;

    for (i = 0; i < total && tcp != NX_NULL; i++)
    {
        NX_INTERFACE *nxif = tcp->nx_tcp_socket_connect_interface;
        AmiSocket    *sock = (AmiSocket *)tcp->nx_tcp_socket_reserved_ptr;

        if (nxif != NX_NULL &&
            (AmiSana2If *)nxif->nx_interface_additional_link_info == sana &&
            sock != NULL && sock->as_SettleWant != 0 &&
            bsd_tcp_draws(tcp->nx_tcp_socket_state) &&
            ami_bsd_tcp_window_burst_bound(bps, sock->as_SettleRtt, grow) &&
            ami_bsd_tcp_window_long_path(sock->as_SettleRtt, grow) &&
            now - sock->as_ShareMillis <= BSD_TCP_SHARE_IDLE_MS)
            return TRUE;

        tcp = tcp->nx_tcp_socket_created_next;
    }

    return FALSE;
}

/*
 * Apply a window the settle chose (`want`, before the card's bound) for the
 * link and round trip the socket has: the card's bound becomes the cap the
 * sender is offered, the buffer goes to what is behind it.  Growth needs the
 * scale negotiated for the maximum at SYN time; a peer that offered no
 * scaling had both fields pinned at 65535 by the fork already and the
 * maximum is not expressible, so it stays.  Shrinking the buffer is for a
 * fresh socket only: on a live one only what is not yet spoken for could
 * come off, and the cap alone already keeps the sender within the card.
 */
static VOID bsd_tcp_window_apply(NX_TCP_SOCKET *tcp, ULONG want, ULONG rtt_ms,
                                 BOOL fresh, ULONG sharers)
{
    NX_INTERFACE *nxif = tcp->nx_tcp_socket_connect_interface;
    AmiSana2If   *sana = (nxif != NX_NULL)
                       ? (AmiSana2If *)nxif->nx_interface_additional_link_info
                       : NULL;
    ULONG cur  = tcp->nx_tcp_socket_rx_window_default;

    /* ... and never more than this connection's part (`sharers`) of what
       the card behind this interface can hold from the wire at once
       (bsdsocket_window.h, ami_bsd_tcp_window_fit), where
       the whole window can arrive at once -- a LAN, or a card slower than
       the path (ami_bsd_tcp_window_burst_bound).  The segment size is the
       one the handshake settled: nx_tcp_socket_mss is what the application
       asked for and stays 0 on an accepted socket. */
    /* Explicitly uncapped unless the card bounds it below: 0 would hand the
       connection back to its interface's cap (nx_api.h). */
    tcp->nx_tcp_socket_rx_window_cap = NX_TCP_RX_WINDOW_CAP_NONE;
    if (sana != NULL)
    {
        ULONG bound = ami_bsd_tcp_window_receive_bound(
                          want,
                          ami_bsd_tcp_window_ring_share(
                              ami_sana2_get_hw_rx_bytes(sana), sharers),
                          tcp->nx_tcp_socket_connect_mss,
                          ami_sana2_get_bps(sana), rtt_ms,
                          ami_sana2_get_tcp_grow_rtt(sana),
                          ami_sana2_get_tcp_wan_window(sana),
                          ami_sana2_get_wan_fitted(sana));

        /* A window cut to the card is what the sender is offered, over a
           buffer with room behind it (ami_bsd_tcp_window_buffer): with the
           buffer the size of the burst, the acknowledgment of every burst
           advertised zero and the sender waited for the application's read
           and a window update -- 0.7 ms of every 5.75 ms on an A3000 with an
           X-Surf 100.  The cap never pulls back a right edge already sent
           (_nx_tcp_socket_rx_window_open). */
        if (bound < want)
            tcp->nx_tcp_socket_rx_window_cap = bound;
        want = ami_bsd_tcp_window_buffer(want, bound);
    }

    if (want == cur)
        return;
    if (want > cur)
    {
#ifdef NX_ENABLE_TCP_WINDOW_SCALING
        if ((want >> tcp->nx_tcp_rcv_win_scale_value) > 65535UL)
            return;
#else
        return;
#endif
        tcp->nx_tcp_socket_rx_window_current += want - cur;
        tcp->nx_tcp_socket_rx_window_default  = want;
        return;
    }
    if (!fresh)
        return;

    /* Smaller: only what is not yet spoken for comes off, which before the
       first data is all of it. */
    if (tcp->nx_tcp_socket_rx_window_current > cur - want)
        tcp->nx_tcp_socket_rx_window_current -= cur - want;
    else
        tcp->nx_tcp_socket_rx_window_current = 0;
    tcp->nx_tcp_socket_rx_window_default = want;
}

/*
 * The interface's slow start (bsdsocket_window.h, ami_bsd_tcp_initial_window),
 * from the recap, which every settle also runs: the card's speed may be
 * known only once the link is up.
 */
static VOID bsd_tcp_slow_start_policy(NX_IP *ip, NX_INTERFACE *nxif,
                                      AmiSana2If *sana)
{
    ULONG pool = (ip->nx_ip_default_packet_pool != NX_NULL)
               ? ip->nx_ip_default_packet_pool->nx_packet_pool_total : 0UL;
    ULONG bps  = ami_sana2_get_bps(sana);

    nxif->nx_interface_tcp_initial_window =
        (UCHAR)ami_bsd_tcp_initial_window(pool, bps);
    nxif->nx_interface_tcp_abc_limit =
        (UCHAR)ami_bsd_tcp_abc_limit(pool, bps);
}

/*
 * Settle a freshly established socket's window (bsdsocket_window.h).  IP
 * thread, from the establish notify, with the receive queue still empty:
 * current and default move together, so what the next acknowledgment
 * advertises is the new buffer and nothing already queued is counted twice.
 */
VOID bsd_tcp_window_settle(NX_TCP_SOCKET *tcp, ULONG rtt_ms)
{
    NX_INTERFACE *nxif = tcp->nx_tcp_socket_connect_interface;
    AmiSana2If   *sana = (nxif != NX_NULL)
                       ? (AmiSana2If *)nxif->nx_interface_additional_link_info
                       : NULL;
    AmiSocket    *sock = (AmiSocket *)tcp->nx_tcp_socket_reserved_ptr;
    ULONG bps  = (sana != NULL) ? ami_sana2_get_bps(sana) : 0UL;
    ULONG grow = ami_sana2_get_tcp_grow_rtt(sana);  /* 0 = built-in line */
    ULONG top  = bsd_tcp_window_top(tcp);
    ULONG want;

#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    /* The share, not the autotuning ceiling the scale was negotiated for. */
    if (sock != NULL && sock->as_RxShare != 0UL && sock->as_RxShare < top)
        top = sock->as_RxShare;
#endif
    want = ami_bsd_tcp_window_settle(tcp->nx_tcp_socket_rx_window_default,
                                     top, bps, rtt_ms, grow);

    if (sock != NULL)
    {
        sock->as_SettleWant  = want;
        sock->as_SettleRtt   = rtt_ms;
        sock->as_SharePass   = tcp->nx_tcp_socket_rx_pass;
        sock->as_ShareMillis = ami_millis();
    }

    /* The card's memory is the unit's: this connection takes its part, and
       the others on the unit give up theirs before its data arrives. */
    bsd_tcp_window_apply(tcp, want, rtt_ms, TRUE,
                         (sana != NULL)
                         ? bsd_tcp_ring_sharers(tcp->nx_tcp_socket_ip_ptr, sana)
                         : 1UL);
    if (sana != NULL)
        bsd_tcp_window_recap(tcp->nx_tcp_socket_ip_ptr, sana);

#ifdef NX_ENABLE_TCP_WINDOW_SCALING
    /* Autotuning where the card does not bound the window and nobody sized
       it: a long path on a fast card, no TCPWANWINDOW, no SO_RCVBUF. */
    if (sock != NULL)
    {
        sock->as_RxAutoMark = 0UL;
        if (sana != NULL && sock->as_RcvBuf == 0 && rtt_ms != 0UL &&
            !ami_bsd_tcp_window_burst_bound(bps, rtt_ms, grow) &&
            ami_sana2_get_tcp_wan_window(sana) == 0UL &&
            tcp->nx_tcp_socket_rx_window_default <
                tcp->nx_tcp_socket_rx_window_maximum)
        {
            sock->as_RxAutoSeq    = tcp->nx_tcp_socket_rx_sequence;
            sock->as_RxAutoMillis = ami_millis();
            sock->as_RxAutoMark   = ami_bsd_tcp_rx_auto_mark(
                                        tcp->nx_tcp_socket_rx_window_default);
        }
    }
#endif
}

#ifdef NX_ENABLE_TCP_WINDOW_SCALING
/*
 * Receive autotuning (bsdsocket_window.h): the mark has arrived.  IP
 * thread, from the receive notify, like the settle.  The room is the pool's
 * receive budget less what the other receivers hold.
 */
VOID bsd_tcp_rx_autotune(NX_TCP_SOCKET *tcp)
{
    AmiSocket      *sock = (AmiSocket *)tcp->nx_tcp_socket_reserved_ptr;
    NX_IP          *ip   = tcp->nx_tcp_socket_ip_ptr;
    NX_PACKET_POOL *pool = ip->nx_ip_default_packet_pool;
    NX_TCP_SOCKET  *other = ip->nx_ip_tcp_created_sockets_ptr;
    ULONG           count = ip->nx_ip_tcp_created_sockets_count;
    ULONG           now   = ami_millis();
    ULONG           cur   = tcp->nx_tcp_socket_rx_window_default;
    ULONG           room, held = 0UL, want, i;

    room = tcp->nx_tcp_socket_rx_window_maximum;
    if (pool != NX_NULL)
    {
        ULONG budget = ami_bsd_tcp_budget(pool->nx_packet_pool_total,
                                          pool->nx_packet_pool_payload_size);

        for (i = 0; i < count && other != NX_NULL; i++)
        {
            if (other != tcp && bsd_tcp_draws(other->nx_tcp_socket_state))
                held += other->nx_tcp_socket_rx_window_default;
            other = other->nx_tcp_socket_created_next;
        }
        budget = (budget > held) ? budget - held : 0UL;
        if (room > budget)
            room = budget;
    }
    if ((room >> tcp->nx_tcp_rcv_win_scale_value) > 65535UL)
        room = 65535UL << tcp->nx_tcp_rcv_win_scale_value;

    want = ami_bsd_tcp_rx_auto_grow(cur, now - sock->as_RxAutoMillis,
                                    sock->as_SettleRtt, room);
    if (want > cur)
    {
        tcp->nx_tcp_socket_rx_window_current += want - cur;
        tcp->nx_tcp_socket_rx_window_default  = want;
        sock->as_SettleWant = want;
    }

    sock->as_RxAutoSeq    = tcp->nx_tcp_socket_rx_sequence;
    sock->as_RxAutoMillis = now;
    sock->as_RxAutoMark   = (want < tcp->nx_tcp_socket_rx_window_maximum)
                          ? ami_bsd_tcp_rx_auto_mark(want) : 0UL;
}
#endif

/*
 * What the card behind `sana` holds changed while the unit was online -- a
 * link renegotiated with or without PAUSE (anxs2ext.h, ANXD_CMD_RX_CAPACITY).
 * The settle ran once, at establishment, so a connection already moving
 * would keep a window sized for the old answer: one sized for a paused link
 * overruns the ring when PAUSE goes.  Every settled connection on this
 * interface takes the window the new answer gives, from the inputs its
 * settle had and its part of the card among the connections bounded by it
 * (bsd_tcp_ring_sharers; also run once a second and on every settle, so a
 * connection that ends gives its part back), and the interface's cap -- what a connection offers before it
 * is settled -- follows the answer too.  The SANA-II reader calls this with
 * nx_ip_protection held (sana2.h, AmiSana2CapacityHook): once when the unit
 * is attached, then on every change.
 */
AMIGA_ASM_ARGS VOID bsd_tcp_window_recap(NX_IP *ip, AmiSana2If *sana)
{
    NX_TCP_SOCKET *tcp   = ip->nx_ip_tcp_created_sockets_ptr;
    ULONG          total = ip->nx_ip_tcp_created_sockets_count;
    ULONG          sharers = bsd_tcp_ring_sharers(ip, sana);
    ULONG          i;

    /* The card overran while a long path ran with the settled window: from
       now on this interface fits long paths to the card too. */
    if (ami_sana2_take_overruns(sana) != 0UL &&
        !ami_sana2_get_wan_fitted(sana) &&
        ami_sana2_get_tcp_wan_window(sana) == 0UL &&
        bsd_tcp_wan_unfitted(ip, sana))
        ami_sana2_set_wan_fitted(sana);

    /* The interface's own cap first: what a connection on it offers from
       its SYN or SYN-ACK until its window is settled.  The round trip is not
       known then, so it is the LAN answer -- the card's fit at this link's
       segment size -- and the settle lifts it for a long path. */
    for (i = 0; i < (ULONG)NX_MAX_PHYSICAL_INTERFACES; i++)
    {
        NX_INTERFACE *nxif = &ip->nx_ip_interface[i];

        if (nxif->nx_interface_valid &&
            (AmiSana2If *)nxif->nx_interface_additional_link_info == sana)
        {
            ULONG hw  = ami_sana2_get_hw_rx_bytes(sana);
            ULONG mss = (nxif->nx_interface_ip_mtu_size > 40UL)
                      ? nxif->nx_interface_ip_mtu_size - 40UL : 0UL;

            nxif->nx_interface_tcp_rx_window_cap =
                (hw != 0UL && mss != 0UL)
                ? ami_bsd_tcp_window_fit(NX_TCP_RX_WINDOW_CAP_NONE - 1UL, hw, mss)
                : 0UL;
            bsd_tcp_slow_start_policy(ip, nxif, sana);
        }
    }

    for (i = 0; i < total && tcp != NX_NULL; i++)
    {
        NX_INTERFACE *nxif = tcp->nx_tcp_socket_connect_interface;
        AmiSocket    *sock = (AmiSocket *)tcp->nx_tcp_socket_reserved_ptr;

        if (nxif != NX_NULL &&
            (AmiSana2If *)nxif->nx_interface_additional_link_info == sana &&
            sock != NULL && sock->as_SettleWant != 0 &&
            tcp->nx_tcp_socket_state >= NX_TCP_ESTABLISHED)
            bsd_tcp_window_apply(tcp, sock->as_SettleWant, sock->as_SettleRtt,
                                 FALSE, sharers);

        tcp = tcp->nx_tcp_socket_created_next;
    }
}

/*
 * SO_KEEPALIVE must default off, and NetX Duo defaults it on.
 */
static VOID bsd_tcp_keepalive_default(NX_TCP_SOCKET *tcp)
{
#ifdef NX_ENABLE_TCP_KEEPALIVE
    tcp->nx_tcp_socket_keepalive_enabled = NX_FALSE;
#else
    (VOID)tcp;
#endif
}

/*
 * Bound the receive queue in packets, sized from this socket's own window.
 */
static VOID bsd_tcp_rx_queue_cap(NX_TCP_SOCKET *tcp)
{
#ifdef NX_ENABLE_LOW_WATERMARK
    /* From the maximum, not the default: the window may grow to it after the
       handshake and the queue must be able to hold what it then advertises. */
    ULONG cap = bsd_tcp_window_top(tcp) / BSD_TCP_RX_MSS_REF +
                BSD_TCP_RX_QUEUE_SLACK;

    if (cap < NX_TCP_MAXIMUM_RX_QUEUE)
        cap = NX_TCP_MAXIMUM_RX_QUEUE;

    tcp->nx_tcp_socket_receive_queue_maximum = cap;
#else
    (VOID)tcp;
#endif
}

/*
 * Bound the transmit queue in unacknowledged segments, from the pool.
 *
 * NetX Duo's compile-time NX_TCP_MAXIMUM_TX_QUEUE is the ceiling SO_SNDBUF may
 * ask for; what a socket starts with is a share of the pool, so a machine
 * with sixteen packets keeps the old eight and a machine with four thousand
 * can keep a round trip's worth in flight.  The number that matters is
 * segments per round trip: on genet.device (5 ms) eight segments cap a sender
 * at 17 Mbit/s whatever the link does, and 32 (the SANA-II write ring, which
 * this was pinned to until 2026-09-17) cap it at 46 KB a round trip -- 250
 * Mbit/s on a 1.5 ms LAN, 14 over 26 ms.  The ring no longer bounds it: a
 * write that finds the ring full waits in the shim's own queue for the next
 * completion instead of sleeping a tick (sana2_internal.h,
 * AMI_SANA2_TX_PEND).  The share is bsdsocket_window.h's; see nx_user.h.
 */
static VOID bsd_tcp_tx_queue_default(struct AmiSocketBase *base,
                                     NX_TCP_SOCKET *tcp)
{
    NX_PACKET_POOL *pool  = bsd_stack_pool(base);
    ULONG           depth;

    /* No pool yet: 0 packets, so the floor. */
    depth = ami_bsd_tcp_tx_queue_for(
                (pool != NULL) ? pool->nx_packet_pool_total : 0UL,
                (ULONG)NX_TCP_MAXIMUM_TX_QUEUE);
    tcp->nx_tcp_socket_transmit_queue_maximum         = depth;
    tcp->nx_tcp_socket_transmit_queue_maximum_default = depth;
}

/* The reserved pointer of a native NetX user is not an AmiSocket. All BSD
   sockets, including accepted ones, were created with this exact name. */
static AmiSocket *bsd_tcp_auto_socket(NX_TCP_SOCKET *tcp)
{
    return (tcp->nx_tcp_socket_name == bsd_tcp_name)
           ? (AmiSocket *)tcp->nx_tcp_socket_reserved_ptr : NULL;
}

static ULONG bsd_tcp_packet_credit(ULONG bytes, ULONG mss, ULONG total)
{
    ULONG n;
    if (mss == 0UL)
        mss = BSD_TCP_RX_MSS_REF;
    n = bytes / mss + ((bytes % mss) != 0UL ? 1UL : 0UL);
    return (n < total) ? n : total;
}

static ULONG bsd_tcp_credit_add(ULONG a, ULONG b, ULONG total)
{
    return (a >= total || b >= total - a) ? total : a + b;
}

/*
 * Grow only when a real write fills the old queue on a measured long path.
 * The queue limit is a byte/resource bound in the GSO core too, so retaining
 * its old 1024*MSS ceiling would still cap a 40ms upload at roughly300Mbit/s.
 * The automatic ceiling comes from this pool; the manual SO_SNDBUF ceiling
 * and a caller's explicit choice are unchanged.
 *
 * No allocation and no wait. An unavailable IP mutex leaves the old policy
 * in force; pressure is retried no more than once per measured RTT. The
 * receive windows' still-open credit is reserved in whole buffers before
 * any extra TX grant. Backlogged senders share the remaining room, and a
 * new sender can take back unused automatic credit from an older sender.
 * This function runs under the caller's bsd_nx_enter() bracket.
 */
VOID bsd_tcp_tx_autotune(NX_TCP_SOCKET *tcp, ULONG pending_bytes)
{
    NX_IP *ip;
    NX_PACKET_POOL *pool;
    NX_INTERFACE *nxif;
    NX_TCP_SOCKET *other;
    AmiSocket *sock;
    AmiSana2If *sana;
    ULONG mss, limit, outstanding, rtt, grow, bps, now;
    ULONG total, count, i, rx_credit = 0UL, tx_used = 0UL;
    ULONG contenders = 1UL, free_packets, capacity, fair, reserved = 0UL;
    ULONG own_used, quota, depth;

    sock = bsd_tcp_auto_socket(tcp);
    if (sock == NULL || sock->as_SndBuf != 0 || pending_bytes == 0UL)
        return;
    ip = tcp->nx_tcp_socket_ip_ptr;
    pool = (ip != NX_NULL) ? ip->nx_ip_default_packet_pool : NX_NULL;
    nxif = tcp->nx_tcp_socket_connect_interface;
    if (pool == NX_NULL || pool->nx_packet_pool_total < BSD_TCP_WINDOW_BIG_POOL ||
        nxif == NX_NULL || nxif->nx_interface_additional_link_info == NULL)
        return;
    sana = (AmiSana2If *)nxif->nx_interface_additional_link_info;
    rtt = sock->as_SettleRtt;
    grow = ami_sana2_get_tcp_grow_rtt(sana);
    if (!ami_bsd_tcp_window_long_path(rtt, grow))
        return;
    bps = ami_sana2_get_bps(sana);
    if (bps < BSD_TCP_WINDOW_FAST_BPS)
        return;
    mss = _nx_tcp_socket_payload_mss(tcp);
    if (mss == 0UL || mss > 65535UL)
        return;
    outstanding = tcp->nx_tcp_socket_tx_outstanding_bytes;
    limit = tcp->nx_tcp_socket_transmit_queue_maximum;
    limit = (limit > 0x7FFFFFFFUL / mss) ? 0x7FFFFFFFUL : limit * mss;
    if (outstanding < limit && pending_bytes <= limit - outstanding)
        return;

    now = ami_millis();
    if (sock->as_AutoTxNext != 0UL && (LONG)(now - sock->as_AutoTxNext) < 0)
        return;
    sock->as_AutoTxNext = now + ((rtt < 60000UL) ? rtt : 60000UL);
    if (tx_mutex_get(&ip->nx_ip_protection, TX_NO_WAIT) != TX_SUCCESS)
        return;

    total = pool->nx_packet_pool_total;
    count = ip->nx_ip_tcp_created_sockets_count;
    other = ip->nx_ip_tcp_created_sockets_ptr;
    for (i = 0; i < count && other != NX_NULL; i++)
    {
        ULONG smss = _nx_tcp_socket_payload_mss(other);
        ULONG used = bsd_tcp_packet_credit(other->nx_tcp_socket_tx_outstanding_bytes,
                                           smss, total);
        tx_used = bsd_tcp_credit_add(tx_used, used, total);
        if (bsd_tcp_draws(other->nx_tcp_socket_state))
            rx_credit = bsd_tcp_credit_add(rx_credit,
                          bsd_tcp_packet_credit(other->nx_tcp_socket_rx_window_current,
                                                 smss, total), total);
        if (other != tcp &&
            (used >= BSD_TCP_TX_QUEUE_MIN ||
             other->nx_tcp_socket_transmit_suspension_list != NX_NULL))
            contenders++;
        other = other->nx_tcp_socket_created_next;
    }
    free_packets = ami_bsd_tcp_tx_free_packets(total, pool->nx_packet_pool_available,
                                               rx_credit);
    capacity = bsd_tcp_credit_add(tx_used, free_packets, total);
    fair = capacity / contenders;
    if (fair == 0UL)
        fair = 1UL;                     /* never strand a zero-flight writer */

    other = ip->nx_ip_tcp_created_sockets_ptr;
    for (i = 0; i < count && other != NX_NULL; i++)
    {
        AmiSocket *os = bsd_tcp_auto_socket(other);
        if (other != tcp && os != NULL && os->as_SndBuf == 0 && os->as_AutoTxDepth != 0UL)
        {
            ULONG smss = _nx_tcp_socket_payload_mss(other);
            ULONG used = bsd_tcp_packet_credit(other->nx_tcp_socket_tx_outstanding_bytes,
                                                smss, total);
            ULONG grant = os->as_AutoTxDepth;
            if (used == 0UL && other->nx_tcp_socket_transmit_suspension_list == NX_NULL)
            {
                /* Small later writes retain the historical progress floor;
                   a new large request must ask for room again. */
                grant = BSD_TCP_TX_QUEUE_MIN;
                os->as_AutoTxDepth = 0UL;
            }
            else
            {
                if (grant > fair)
                    grant = fair;
                os->as_AutoTxDepth = grant;
                if (grant > used)
                    reserved = bsd_tcp_credit_add(reserved, grant - used, total);
            }
            other->nx_tcp_socket_transmit_queue_maximum = grant;
            other->nx_tcp_socket_transmit_queue_maximum_default = grant;
        }
        other = other->nx_tcp_socket_created_next;
    }
    free_packets = (free_packets > reserved) ? free_packets - reserved : 0UL;
    own_used = bsd_tcp_packet_credit(outstanding, mss, total);
    quota = bsd_tcp_credit_add(own_used, free_packets, total);
    if (quota > fair)
        quota = fair;
    depth = ami_bsd_tcp_tx_flight_depth(bps, rtt, mss, quota);
    if (depth == 0UL)
        depth = 1UL;
    /* An untouched legacy queue is not reduced just to try an optimization. */
    if (sock->as_AutoTxDepth != 0UL || depth > tcp->nx_tcp_socket_transmit_queue_maximum)
    {
        tcp->nx_tcp_socket_transmit_queue_maximum = depth;
        tcp->nx_tcp_socket_transmit_queue_maximum_default = depth;
        sock->as_AutoTxDepth = depth;
    }
    /* The successful nonblocking get above still belongs to this thread. */
    AMI_NX_ONLY_SUCCESS(tx_mutex_put(&ip->nx_ip_protection));
}

static VOID bsd_tcp_seed_isn(NX_TCP_SOCKET *tcp)
{
    ULONG seed = ami_random_ulong();

    /* Zero means "not seeded" to the code above, so a zero draw would restore
       the biased branch. One draw in 2^32, and it costs a comparison. */
    if (seed == 0)
        seed = 1;

    tcp->nx_tcp_socket_tx_sequence = seed;
}

/* The first table, published under Forbid() like every later one; a task
   that published first keeps its table and this one is given back. */
static LONG bsd_table_first(struct AmiSocketBase *base, LONG size)
{
    AmiSocket **table;

    table = (AmiSocket **)ami_alloc((ULONG)size * sizeof(AmiSocket *));
    if (table == NULL)
        return -1;

    Forbid();
    if (base->sb_Table == NULL)
    {
        base->sb_Table     = table;
        base->sb_TableSize = size;
        table = NULL;
    }
    Permit();

    if (table != NULL)
        ami_free(table);

    return 0;
}

static LONG bsd_table_ensure(struct AmiSocketBase *base)
{
    if (base->sb_Table != NULL)
        return 0;

    return bsd_table_first(base, BSD_DEFAULT_DTABLESIZE);
}

/*
 * THE ADVERTISED DEFAULT MAY NEVER EXCEED AN AmiTCP fd_set, AND THIS IS THE
 * CHECK THAT SAYS SO.
 *
 * getdtablesize() is what a caller sizes its select() by -- Sun RPC's
 * `select(_rpc_dtablesize(), &set, ...)` literally hands our answer back to us
 * as nfds -- and AmiTCP's FD_SETSIZE is 64, so an fd_set compiled against it
 * is eight bytes.  Answer more than 64 by DEFAULT and WaitSelect reads, and
 * used to write, past that object: 1c5a5809 raised this to 256 for a reason
 * that looked good, no test could see it, and it shipped through five releases
 * until a user's NFS mount failed with "RPC: Port mapper failure - Unable to
 * receive" (24a92828).
 *
 * A program may still raise its own table with SBTC_DTABLESIZE, up to
 * BSD_MAX_DTABLESIZE -- the conformance suite does exactly that
 * (third_party/bsdsocktest/src/test_waitselect.c:430) -- and by asking it
 * takes responsibility for its own fd_set being big enough.  What it cannot
 * do is have that responsibility handed to it silently by a default it never
 * chose.
 */
#define BSD_AMITCP_FD_SETSIZE 64
_Static_assert(BSD_DEFAULT_DTABLESIZE <= BSD_AMITCP_FD_SETSIZE,
               "getdtablesize()'s default must fit an AmiTCP fd_set: a caller "
               "passes it straight back as nfds and WaitSelect would touch "
               "memory past a 64-bit set");

LONG bsd_table_size(struct AmiSocketBase *base)
{
    if (base->sb_TableSize == 0)
        return BSD_DEFAULT_DTABLESIZE;

    return base->sb_TableSize;
}

LONG bsd_table_resize(struct AmiSocketBase *base, LONG size)
{
    AmiSocket **table;
    AmiSocket **old;
    LONG        i, copy;

    if (size < 1 || size > BSD_MAX_DTABLESIZE)
        return -1;

    /* A task that published a first table meanwhile is resized below. */
    if (base->sb_Table == NULL && bsd_table_first(base, size) != 0)
        return -1;

    if (size == base->sb_TableSize)
        return 0;

    /* No unlocked look at the slots past `size` first: another task can
       shrink the table under it.  The check below is the one that counts. */
    table = (AmiSocket **)ami_alloc((ULONG)size * sizeof(AmiSocket *));
    if (table == NULL)
        return -1;

    /* Checked, copied and published whole under Forbid() and the old one
       freed after: another opener's bsd_owner_elect() reads this table
       (F-043), and a slot an allocation holds BSD_FD_BUSY is stored back
       under Forbid() too, so it lands in whichever table is current (N-087). */
    Forbid();
    for (i = base->sb_TableSize - 1; i >= size; i--)
    {
        if (base->sb_Table[i] != NULL)
        {
            Permit();
            ami_free(table);
            return -1;
        }
    }

    copy = (size < base->sb_TableSize) ? size : base->sb_TableSize;
    for (i = 0; i < copy; i++)
        table[i] = base->sb_Table[i];

    old = base->sb_Table;
    base->sb_Table     = table;
    base->sb_TableSize = size;
    Permit();
    ami_free(old);

    return 0;
}

/* One slot, bound and read under one Forbid(): another task on the base can
   shrink the table with SBTC_DTABLESIZE between a bound test and the read
   (N-087).  NULL for a descriptor the table does not have. */
static AmiSocket *bsd_fd_peek(struct AmiSocketBase *base, LONG fd)
{
    AmiSocket *entry = NULL;

    Forbid();
    if (base->sb_Table != NULL && fd >= 0 && fd < base->sb_TableSize)
        entry = base->sb_Table[fd];
    Permit();

    return entry;
}

AmiSocket *bsd_lookup(struct AmiSocketBase *base, LONG fd)
{
    AmiSocket *sock = bsd_fd_peek(base, fd);

    return (sock == BSD_FD_RESERVED || sock == BSD_FD_BUSY) ? NULL : sock;
}

BOOL bsd_fd_reserved(struct AmiSocketBase *base, LONG fd)
{
    return (BOOL)(bsd_fd_peek(base, fd) == BSD_FD_RESERVED);
}

/* bsd_lookup() and a reference on what it found, under one Forbid(): a
   caller that yields while it uses the socket keeps it alive across a close
   of the descriptor meanwhile.  NULL for no socket; else the caller owes one
   bsd_socket_release(). */
static AmiSocket *bsd_lookup_pin(struct AmiSocketBase *base, LONG fd)
{
    AmiSocket *sock = NULL;

    Forbid();
    if (base->sb_Table != NULL && fd >= 0 && fd < base->sb_TableSize)
        sock = base->sb_Table[fd];
    if (sock == BSD_FD_RESERVED || sock == BSD_FD_BUSY)
        sock = NULL;
    if (sock != NULL)
        sock->as_RefCount++;
    Permit();

    return sock;
}

/*
 * SBTC_FDCALLBACK: tell the opener that a descriptor came or went.
 */
static LONG bsd_fd_callback(struct AmiSocketBase *base, LONG fd, LONG action)
{
    if (base->sb_FDCallback == NULL)
        return 0;

    return base->sb_FDCallback(fd, action);
}

/* The store that ends one: under Forbid(), so it reaches the current table
   even if a callback grew it meanwhile. */
static VOID bsd_fd_store(struct AmiSocketBase *base, LONG fd, AmiSocket *entry)
{
    Forbid();
    base->sb_Table[fd] = entry;
    Permit();
}

/*
 * Take an empty slot for the callbacks: BSD_FD_BUSY under Forbid(), as
 * bsd_fd_claim() and bsd_fd_free() do, so a callback that re-enters cannot
 * allocate it and SBTC_DTABLESIZE cannot shrink the table past it (N-087).
 * Forbid() covers the test-and-set only, never a callback.
 */
static BOOL bsd_fd_take_empty(struct AmiSocketBase *base, LONG fd)
{
    BOOL taken = FALSE;

    Forbid();
    if (fd < base->sb_TableSize && base->sb_Table[fd] == NULL)
    {
        base->sb_Table[fd] = BSD_FD_BUSY;
        taken = TRUE;
    }
    Permit();

    return taken;
}

LONG bsd_fd_alloc(struct AmiSocketBase *base, AmiSocket *sock)
{
    LONG fd;
    LONG error;

    if (bsd_table_ensure(base) != 0)
        return bsd_fail(base, AMI_EMFILE);

    for (fd = 0; fd < base->sb_TableSize; fd++)
    {
        if (!bsd_fd_take_empty(base, fd))
            continue;

        if (bsd_fd_callback(base, fd, FDCB_CHECK) != 0)
        {
            bsd_fd_store(base, fd, BSD_FD_RESERVED);
            continue;
        }

        error = bsd_fd_callback(base, fd, FDCB_ALLOC);
        if (error != 0)
        {
            bsd_fd_store(base, fd, NULL);
            return bsd_fail(base, error);
        }

        bsd_fd_store(base, fd, sock);
        return fd;
    }

    return bsd_fail(base, AMI_EMFILE);
}

LONG bsd_fd_reserve(struct AmiSocketBase *base, LONG fd)
{
    LONG error;

    if (bsd_table_ensure(base) != 0)
        return bsd_fail(base, AMI_EMFILE);

    if (fd < 0)
        return bsd_fd_alloc(base, BSD_FD_RESERVED);

    if (!bsd_fd_take_empty(base, fd))
        return bsd_fail(base, AMI_EMFILE);

    error = bsd_fd_callback(base, fd, FDCB_CHECK);
    if (error == 0)
        error = bsd_fd_callback(base, fd, FDCB_ALLOC);
    if (error != 0)
    {
        bsd_fd_store(base, fd, NULL);
        return bsd_fail(base, error);
    }

    bsd_fd_store(base, fd, BSD_FD_RESERVED);
    return fd;
}

/*
 * Dup2Socket()'s target, replaced in three steps with the callbacks in the
 * order they always ran (F-055).  bsd_fd_claim() marks the slot BSD_FD_BUSY
 * under Forbid() before any callback, so another task on a shared base can
 * neither close what is in it (EBADF: there is nothing to look up) nor
 * allocate it; what was there, and the reference a socket there held, now
 * belong to the claimer.  Then FDCB_FREE for it.  A veto puts it straight
 * back: the opener was told nothing had changed.
 */
LONG bsd_fd_claim(struct AmiSocketBase *base, LONG fd, AmiSocket **prev)
{
    AmiSocket *entry;
    LONG       error;

    if (bsd_table_ensure(base) != 0)
        return bsd_fail(base, AMI_EMFILE);

    Forbid();
    if (fd < 0 || fd >= base->sb_TableSize)
    {
        Permit();
        return bsd_fail(base, AMI_EBADF);
    }
    entry = base->sb_Table[fd];
    if (entry == BSD_FD_BUSY)
    {
        Permit();
        return bsd_fail(base, AMI_EBUSY);
    }
    base->sb_Table[fd] = BSD_FD_BUSY;
    Permit();

    *prev = entry;

    if (entry != NULL)
    {
        error = bsd_fd_callback(base, fd, FDCB_FREE);
        if (error != 0)
        {
            bsd_fd_store(base, fd, entry);
            return bsd_fail(base, error);
        }
    }

    return 0;
}

/* FDCB_CHECK and FDCB_ALLOC for the replacement, then it is published.  A
   veto leaves the slot claimed, for bsd_fd_unclaim(). */
LONG bsd_fd_settle(struct AmiSocketBase *base, LONG fd, AmiSocket *entry)
{
    LONG error;

    error = bsd_fd_callback(base, fd, FDCB_CHECK);
    if (error == 0)
        error = bsd_fd_callback(base, fd, FDCB_ALLOC);
    if (error != 0)
        return bsd_fail(base, error);

    bsd_fd_store(base, fd, entry);
    return 0;
}

/*
 * The replacement was refused: put back what the claim took.  An empty slot
 * is empty again.  Anything else was announced gone with FDCB_FREE, so
 * FDCB_ALLOC announces it back.  Non-zero when that is refused too: the slot
 * is then empty and a socket that was in it is the caller's to release.
 */
LONG bsd_fd_unclaim(struct AmiSocketBase *base, LONG fd, AmiSocket *prev)
{
    if (prev != NULL && bsd_fd_callback(base, fd, FDCB_ALLOC) != 0)
    {
        bsd_fd_store(base, fd, NULL);
        return -1;
    }

    bsd_fd_store(base, fd, prev);
    return 0;
}

/* bsd_fd_free(), and what it took out of the slot: NULL when there was
   nothing to free.  CloseSocket() releases that, not what it looked up. */
static LONG bsd_fd_take(struct AmiSocketBase *base, LONG fd, AmiSocket **taken)
{
    *taken = NULL;

    {
        AmiSocket *entry;
        LONG error;

        /* The bound inside the Forbid(), with the read (bsd_fd_peek()). */
        Forbid();
        if (base->sb_Table == NULL || fd < 0 || fd >= base->sb_TableSize)
        {
            Permit();
            return 0;
        }
        entry = base->sb_Table[fd];
        if (entry == NULL)
        {
            Permit();
            return 0;
        }
        if (entry == BSD_FD_BUSY)
        {
            Permit();
            return bsd_fail(base, AMI_EBUSY);
        }
        /* As with Dup2's claim, callback reentry must neither find the
           closing socket nor reuse its descriptor before the veto result. */
        base->sb_Table[fd] = BSD_FD_BUSY;
        Permit();

        error = bsd_fd_callback(base, fd, FDCB_FREE);
        if (error != 0)
        {
            bsd_fd_store(base, fd, entry);
            return bsd_fail(base, error);
        }

        bsd_fd_store(base, fd, NULL);
        *taken = entry;
    }

    return 0;
}

LONG bsd_fd_free(struct AmiSocketBase *base, LONG fd)
{
    AmiSocket *taken;

    return bsd_fd_take(base, fd, &taken);
}

static AmiSocket *bsd_socket_alloc(struct AmiSocketBase *base,
                                   UWORD domain, UWORD type, LONG protocol)
{
    AmiSocket *sock = (AmiSocket *)ami_alloc(sizeof(AmiSocket));
    ULONG      version = NX_IP_VERSION_V4;

    if (sock == NULL)
        return NULL;

    sock->as_Owner    = base;
    sock->as_RefCount = 1;
    sock->as_Domain   = domain;
    sock->as_Type     = type;
    sock->as_Protocol = protocol;
    sock->as_Ttl      = (LONG)NX_IP_TIME_TO_LIVE;

    /* Not only a zeroing: an ICMPv6 filter starts passing everything. */
    bsd_cmsg_reset(sock);

#ifdef AMINETXDUO_MULTICAST
    sock->as_McastTtl  = 1;
    sock->as_McastLoop = 1;
    sock->as_McastIf   = -1;
#ifdef AMINETXDUO_IPV6
    /* RFC 3493: one hop, same as IPv4. The NX_IP's own hop limit is 255, so
       this is applied per send rather than inherited. */
    sock->as_Mcast6Hops = 1;
    sock->as_Mcast6If   = -1;
#endif
#endif

    switch (type)
    {
        case SOCK_STREAM: sock->as_Flags = ASF_TCP; break;
        case SOCK_RAW:    sock->as_Flags = ASF_RAW; break;
        default:          sock->as_Flags = ASF_UDP; break;
    }

#ifdef AMINETXDUO_IPV6
    if (domain == AF_INET6)
    {
        sock->as_Flags |= ASF_INET6;
        version = NX_IP_VERSION_V6;

        /*
         * IPV6_V6ONLY defaults to off, so an AF_INET6 socket is dual-stack.
         */
    }
#endif

    /* ami_alloc clears both address payloads. Only their version tags need
       initialization: an unspecified address still has its family's tag. */
    sock->as_LocalAddr.nxd_ip_version = version;
    sock->as_PeerAddr.nxd_ip_version  = version;

    ami_mem_socket_delta(1);

    return sock;
}

static VOID bsd_socket_dispose(AmiSocket *sock)
{
    if (sock == NULL)
        return;

    ami_mem_socket_delta(-1);
    ami_free(sock);
}

#define BSD_CLOSING_DEADLINE    (60UL * NX_IP_PERIODIC_RATE)

AmiSocket *bsd_closing_head;

static BOOL bsd_socket_destroy(AmiSocket *sock);

static VOID bsd_closing_data_notify(NX_TCP_SOCKET *tcp)
{
    NX_TCP_HEADER header;

    /* A bare FIN queues nothing and must not be answered this way. Only
       unreadable data gets a reset. */
    if (tcp->nx_tcp_socket_receive_queue_count == 0)
        return;

    header.nx_tcp_header_word_3         = NX_TCP_ACK_BIT;
    header.nx_tcp_acknowledgment_number = tcp->nx_tcp_socket_tx_sequence;
    header.nx_tcp_sequence_number       = tcp->nx_tcp_socket_rx_sequence;
    _nx_tcp_packet_send_rst(tcp, &header);

    tcp->nx_tcp_socket_timeout_retries = tcp->nx_tcp_socket_timeout_max_retries;
    tcp->nx_tcp_socket_timeout         = 1;
}

static VOID bsd_closing_park(AmiSocket *sock)
{
    sock->as_Flags |= ASF_CLOSING;

    nx_tcp_socket_receive_notify(&sock->as_Nx.tcp, bsd_closing_data_notify);

    sock->as_Nx.tcp.nx_tcp_socket_reserved_ptr = NX_NULL;
    sock->as_Owner = NULL;

    sock->as_ClosingAt   = tx_time_get();
    sock->as_ClosingNext = bsd_closing_head;
    bsd_closing_head     = sock;
}

static VOID bsd_tcp_abort(NX_TCP_SOCKET *tcp)
{
    UINT state = tcp->nx_tcp_socket_state;

    if (state == NX_TCP_CLOSED || state == NX_TCP_LISTEN_STATE)
        return;

    if (state != NX_TCP_ESTABLISHED && state != NX_TCP_SYN_SENT &&
        state != NX_TCP_SYN_RECEIVED && state != NX_TCP_CLOSE_WAIT)
        tcp->nx_tcp_socket_state = NX_TCP_ESTABLISHED;

    nx_tcp_socket_disconnect(tcp, NX_NO_WAIT);
}

VOID bsd_closing_sweep(VOID)
{
    AmiSocket  *sock;
    AmiSocket **link = &bsd_closing_head;
    ULONG       now  = tx_time_get();

    while ((sock = *link) != NULL)
    {
        UINT state = sock->as_Nx.tcp.nx_tcp_socket_state;
        BOOL done  = (state == NX_TCP_CLOSED) || (state == NX_TCP_TIMED_WAIT) ||
                     (state == NX_TCP_LISTEN_STATE);
        BOOL late  = ((ULONG)(now - sock->as_ClosingAt) >= BSD_CLOSING_DEADLINE) ||
                     (sock->as_Nx.tcp.nx_tcp_socket_receive_queue_count != 0);

        if (!done && !late)
        {
            link = &sock->as_ClosingNext;
            continue;
        }

        *link = sock->as_ClosingNext;
        sock->as_ClosingNext = NULL;

        if (!done)
        {
            if (sock->as_Nx.tcp.nx_tcp_socket_receive_queue_count == 0)
                AMI_WARN("bsdsocket: close did not complete in %ld s "
                         "(state %ld); resetting",
                         (long)(BSD_CLOSING_DEADLINE / NX_IP_PERIODIC_RATE),
                         (long)state);

            bsd_tcp_abort(&sock->as_Nx.tcp);
        }

        if (bsd_socket_destroy(sock))
            bsd_socket_dispose(sock);
    }
}

AmiSocket *bsd_defer_head;

VOID bsd_socket_defer(AmiSocket *sock)
{
    if (sock->as_DeferRefs++ == 0)
    {
        sock->as_DeferNext = bsd_defer_head;
        bsd_defer_head     = sock;
    }
}

/*
 * The list is taken whole under Forbid(), so a close deferring meanwhile
 * starts a new one.  Each owed release is one bsd_socket_release(); the count
 * never exceeds the references, so only the last can free the socket, and
 * nothing here touches it after that.
 */
VOID bsd_defer_sweep(struct AmiSocketBase *base)
{
    AmiSocket *sock;

    Forbid();
    sock = bsd_defer_head;
    bsd_defer_head = NULL;
    Permit();

    while (sock != NULL)
    {
        AmiSocket *next;
        ULONG      owed;

        Forbid();
        next               = sock->as_DeferNext;
        owed               = sock->as_DeferRefs;
        sock->as_DeferNext = NULL;
        sock->as_DeferRefs = 0;
        Permit();

        while (owed-- > 0)
            bsd_socket_release(base, sock);

        sock = next;
    }
}

/*
 * The parked closes and owed releases a last opener could not drain, once the
 * stack they belong to is gone (F-059).  ami_ns_destroy() deleted their NX
 * sockets with the IP instance (ami_ns_sockets_delete()), and a stopped kernel
 * runs no thread or timer, so only their memory is left: it is freed, and
 * nothing else is done to them.  While the stack is still up the lists are
 * kept, and the next bracketed close pays them against the live IP.  Destroyed
 * but not provably quiet -- a kernel that would not stop, a device still
 * holding requests -- they are forgotten, not freed, as before.  Either way a
 * dead stack leaves both lists empty, so no sweep ever reaches its sockets.
 *
 * A socket is on at most one list: a parked close has no reference left to
 * owe.  An owed listener takes its unaccepted connections with it, as
 * bsd_socket_release() would: nothing else names them.  An owed socket that
 * still has a reference no release is owed for is named by something live:
 * the owed releases are paid as counts and it is left to that holder, whose
 * own close then owes the last one and comes back here.
 */
static VOID bsd_orphan_free(AmiSocket *sock)
{
    while (sock->as_Incoming != NULL)
    {
        AmiSocket *child = sock->as_Incoming;

        sock->as_Incoming = child->as_IncomingNext;
        bsd_socket_dispose(child);
    }

    bsd_socket_dispose(sock);
}

VOID bsd_orphans_reclaim(VOID)
{
    AmiSocket *closing;
    AmiSocket *owed;
    AmiSocket *sock;
    AmiSocket *next;

    if (netstack_get() != NULL)
        return;

    Forbid();
    closing          = bsd_closing_head;
    owed             = bsd_defer_head;
    bsd_closing_head = NULL;
    bsd_defer_head   = NULL;
    Permit();

    if (closing == NULL && owed == NULL)
        return;

    if (!netstack_can_unload())
    {
        AMI_WARN("bsdsocket: sockets of a stack that did not stop cleanly "
                 "are left in memory");
        return;
    }

    for (sock = owed; sock != NULL; sock = next)
    {
        next = sock->as_DeferNext;

        if ((sock->as_Flags & ASF_CLOSING) != 0 ||
            sock->as_RefCount > sock->as_DeferRefs)
        {
            Forbid();
            if ((sock->as_Flags & ASF_CLOSING) == 0)
                sock->as_RefCount -= sock->as_DeferRefs;
            sock->as_DeferNext = NULL;
            sock->as_DeferRefs = 0;
            Permit();
            continue;
        }

        bsd_orphan_free(sock);
    }

    for (sock = closing; sock != NULL; sock = next)
    {
        next = sock->as_ClosingNext;
        bsd_socket_dispose(sock);
    }
}

VOID bsd_closing_drain(VOID)
{
    AmiSocket *sock;

    while ((sock = bsd_closing_head) != NULL)
    {
        bsd_closing_head     = sock->as_ClosingNext;
        sock->as_ClosingNext = NULL;

        bsd_tcp_abort(&sock->as_Nx.tcp);

        if (bsd_socket_destroy(sock))
            bsd_socket_dispose(sock);
    }
}

/*
 * Start the close. TRUE means the socket is finished with and the caller can
 * delete it now. FALSE means the FIN is in flight and the block has been
 * parked, so it is no longer the caller's to free.
 */
static BOOL bsd_tcp_close_start(AmiSocket *sock)
{
    NX_TCP_SOCKET *tcp   = &sock->as_Nx.tcp;
    UINT           state = tcp->nx_tcp_socket_state;

    if (state == NX_TCP_SYN_SENT || state == NX_TCP_SYN_RECEIVED)
    {
        nx_tcp_socket_disconnect(tcp, NX_NO_WAIT);
        return TRUE;
    }

    /* RFC 1122 4.2.2.13: unread data turns a close into an abort. */
    if (sock->as_RxPending != NULL || tcp->nx_tcp_socket_receive_queue_count != 0)
    {
#ifdef AMINETXDUO_TCP_CORK
        bsd_cork_drop(sock);
#endif
        bsd_tcp_abort(tcp);
        return TRUE;
    }

    /* SO_LINGER, l_linger == 0: the documented abortive close. */
    if (sock->as_LingerOn != 0 && sock->as_LingerTime == 0)
    {
#ifdef AMINETXDUO_TCP_CORK
        bsd_cork_drop(sock);
#endif
        bsd_tcp_abort(tcp);
        return TRUE;
    }

    if (sock->as_LingerOn != 0)
    {
        ULONG linger = (ULONG)sock->as_LingerTime * NX_IP_PERIODIC_RATE;

#ifdef AMINETXDUO_TCP_CORK
        /* What is corked gets the linger time first, and the disconnect what
           is left of it.  Still held when the time is up: an abort, as a
           linger that expires with data unsent is. */
        if (!bsd_cork_close_linger(sock, linger, &linger))
        {
            bsd_tcp_abort(tcp);
            return TRUE;
        }
#endif
        if (nx_tcp_socket_disconnect(tcp, linger) != NX_NOT_CONNECTED)
            return TRUE;
    }
#ifdef AMINETXDUO_TCP_CORK
    /* The FIN goes behind what is corked: parked with it deferred, and the
       pass that sends the segment's last byte sends the FIN after it. */
    else if (bsd_cork_close_graceful(sock))
    {
        bsd_closing_park(sock);
        return FALSE;
    }
#endif
    else
    {
        bsd_tcp_send_fin(sock);
    }

    state = tcp->nx_tcp_socket_state;

    if (state == NX_TCP_CLOSED || state == NX_TCP_LISTEN_STATE)
        return TRUE;

    bsd_closing_park(sock);

    return FALSE;
}

/*
 * Tear the NetX Duo socket down. Safe to call more than once.
 */
static BOOL bsd_socket_destroy(AmiSocket *sock)
{
    UINT status;

    if ((sock->as_Flags & ASF_ORPHANED) != 0)
        return FALSE;                   /* already known to be un-deletable */

    if ((sock->as_Flags & ASF_DELETED) != 0)
        return TRUE;

#ifdef AMINETXDUO_MULTICAST
    bsd_mcast_close(sock);
#endif

    if ((sock->as_Flags & (ASF_TCP | ASF_RAW | ASF_CLOSING)) == ASF_TCP &&
        (sock->as_Flags & (ASF_CONNECTED | ASF_CONNECTING)) != 0)
    {
        if (!bsd_tcp_close_start(sock))
            return FALSE;
    }

#ifdef AMINETXDUO_TCP_CORK
    /* Off the armed list and the segment released before the socket can be
       freed: no pass and no tick reaches it after this.  The sweep's deadline,
       the drain and CloseLibrary all come through here. */
    if ((sock->as_Flags & (ASF_TCP | ASF_RAW)) == ASF_TCP)
        bsd_cork_drop(sock);
#endif

    if (sock->as_RxPending != NULL)
    {
        nx_packet_release(sock->as_RxPending);
        sock->as_RxPending = NULL;
    }

    if ((sock->as_Flags & ASF_RAW) != 0)
    {
        bsd_raw_close(sock);
        sock->as_Flags |= ASF_DELETED;

        return TRUE;
    }

    if ((sock->as_Flags & ASF_TCP) != 0)
    {
        if ((sock->as_Flags & ASF_SERVER) != 0)
            nx_tcp_server_socket_unaccept(&sock->as_Nx.tcp);
        else if ((sock->as_Flags & ASF_NXBOUND) != 0)
            nx_tcp_client_socket_unbind(&sock->as_Nx.tcp);

        status = nx_tcp_socket_delete(&sock->as_Nx.tcp);

        if (status == NX_STILL_BOUND)
        {
            bsd_tcp_abort(&sock->as_Nx.tcp);

            if ((sock->as_Flags & ASF_SERVER) != 0)
                nx_tcp_server_socket_unaccept(&sock->as_Nx.tcp);
            else
                nx_tcp_client_socket_unbind(&sock->as_Nx.tcp);

            status = nx_tcp_socket_delete(&sock->as_Nx.tcp);
        }
    }
    else
    {
        if ((sock->as_Flags & ASF_NXBOUND) != 0)
            nx_udp_socket_unbind(&sock->as_Nx.udp);

        status = nx_udp_socket_delete(&sock->as_Nx.udp);
    }

    if (status != NX_SUCCESS && status != NX_NOT_CREATED)
    {
        AMI_WARN("bsdsocket: %s_socket_delete refused (%ld) state %ld "
                 "flags 0x%lx port %ld. It leaks %ld bytes rather than "
                 "corrupting the created list",
                 ((sock->as_Flags & ASF_TCP) != 0) ? "nx_tcp" : "nx_udp",
                 (long)status,
                 ((sock->as_Flags & ASF_TCP) != 0)
                     ? (long)sock->as_Nx.tcp.nx_tcp_socket_state : 0L,
                 (long)sock->as_Flags, (long)sock->as_LocalPort,
                 (long)sizeof(AmiSocket));

        sock->as_Flags |= ASF_ORPHANED;

        if ((sock->as_Flags & ASF_TCP) != 0)
            sock->as_Nx.tcp.nx_tcp_socket_reserved_ptr = NX_NULL;
        else
            sock->as_Nx.udp.nx_udp_socket_reserved_ptr = NX_NULL;

        sock->as_Owner = NULL;

        return FALSE;
    }

    sock->as_Flags |= ASF_DELETED;

    return TRUE;
}

VOID bsd_socket_retain(AmiSocket *sock)
{
    if (sock == NULL)
        return;

    Forbid();
    sock->as_RefCount++;
    Permit();
}

VOID bsd_socket_release(struct AmiSocketBase *base, AmiSocket *sock)
{
    ULONG remaining;

    if (sock == NULL)
        return;

    Forbid();
    if (sock->as_RefCount > 0)
        sock->as_RefCount--;
    remaining = sock->as_RefCount;

    if (remaining > 0)
        bsd_owner_drop(base, sock);
    Permit();

    if (remaining > 0)
    {
        return;
    }

    if ((sock->as_Flags & ASF_LISTENING) != 0)
    {
        NX_IP     *ip = bsd_stack_ip(base);
        AmiSocket *p;

        for (p = sock->as_Incoming; p != NULL; p = p->as_IncomingNext)
            nx_tcp_socket_disconnect(&p->as_Nx.tcp, NX_NO_WAIT);

        if (ip != NULL)
            nx_tcp_server_socket_unlisten(ip, sock->as_ListenPort);

        sock->as_Flags &= ~ASF_LISTENING;
    }

    while (sock->as_Incoming != NULL)
    {
        AmiSocket *victim = sock->as_Incoming;

        sock->as_Incoming = victim->as_IncomingNext;
        victim->as_IncomingNext = NULL;
        if (sock->as_IncomingCount != 0)
            sock->as_IncomingCount--;

        if (bsd_socket_destroy(victim))
            bsd_socket_dispose(victim);
    }

    if (bsd_socket_destroy(sock))
        bsd_socket_dispose(sock);
}

VOID bsd_close_all(struct AmiSocketBase *base)
{
    LONG fd;
    BOOL bracketed;

    if (base->sb_Table == NULL)
        return;

    bracketed = (bsd_nx_enter(base) == 0);
    if (!bracketed)
    {
        AMI_WARN("bsdsocket: CloseLibrary with the kernel down. "
                 "Sockets are left to the stack teardown");
    }

    for (fd = 0; fd < base->sb_TableSize; fd++)
    {
        AmiSocket *sock = bsd_fd_peek(base, fd);

        if (sock == NULL)
            continue;

        if (bsd_fd_free(base, fd) != 0)
            bsd_fd_store(base, fd, NULL);

        if (sock == BSD_FD_RESERVED || sock == BSD_FD_BUSY)
            continue;

        if (bracketed)
            bsd_socket_release(base, sock);
        else
        {
            /* Its NetX callbacks reach this base through as_Owner long after
               the base is gone, so ownership passes to another holder or to
               nobody; the release is owed to the next bracket (F-059). */
            Forbid();
            bsd_owner_drop(base, sock);
            bsd_socket_defer(sock);
            Permit();
        }
    }

    if (!bracketed)
        return;

    bsd_defer_sweep(base);
    bsd_closing_sweep();

    bsd_nx_leave(base);
}

VOID bsd_addr_from_v4(NXD_ADDRESS *addr, ULONG v4)
{
    addr->nxd_ip_version       = NX_IP_VERSION_V4;
    addr->nxd_ip_address.v4    = v4;
}

/*
 * Both address families use the BSD length/family prefix. Inspect bytes only
 * after checking the supplied buffer size; reject the former beta IPv6 ABI.
 */
LONG bsd_sa_family(const struct sockaddr *sa, socklen_t len)
{
    const UBYTE *b = (const UBYTE *)sa;

    if (sa == NULL || len < (socklen_t)sizeof(struct sockaddr_in))
        return -1;

#ifdef AMINETXDUO_IPV6
    if (b[1] == AF_INET6)
        return (len >= (socklen_t)sizeof(struct sockaddr_in6)) ? AF_INET6 : -1;
#endif

    if (b[1] == AF_INET)
        return AF_INET;

    if (b[1] == AF_UNSPEC && b[0] <= (UBYTE)sizeof(struct sockaddr_in))
        return AF_UNSPEC;

    return -1;
}

LONG bsd_sockaddr_get(struct AmiSocketBase *base, const struct sockaddr *sa,
                      socklen_t len, NXD_ADDRESS *addr, UINT *port,
                      ULONG *scope_id)
{
    LONG family;

    if (scope_id != NULL)
        *scope_id = 0;

    if (sa == NULL)
        return bsd_fail(base, AMI_EFAULT);

    family = bsd_sa_family(sa, len);

    if (family == -1)
    {
        return bsd_fail(base,
                        (len < (socklen_t)sizeof(struct sockaddr_in))
                            ? AMI_EINVAL : AMI_EAFNOSUPPORT);
    }

#ifdef AMINETXDUO_IPV6
    if (family == AF_INET6)
    {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;

        addr->nxd_ip_version = NX_IP_VERSION_V6;
        bsd_in6_to_words(sin6->sin6_addr.s6_addr, addr->nxd_ip_address.v6);

        *port = (UINT)BSD_NTOHS(sin6->sin6_port);

        if (scope_id != NULL)
            *scope_id = (ULONG)sin6->sin6_scope_id;

        return 0;
    }
#endif

    {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;

        bsd_addr_from_v4(addr, BSD_NTOHL(sin->sin_addr.s_addr));
        *port = (UINT)BSD_NTOHS(sin->sin_port);
    }

    return 0;
}

VOID bsd_sockaddr_put(const AmiSocket *sock, struct sockaddr *sa,
                      socklen_t *len, const NXD_ADDRESS *addr, UINT port,
                      ULONG scope_id)
{
    socklen_t copy;

    if (sa == NULL || len == NULL)
        return;

#ifdef AMINETXDUO_IPV6
    if (sock != NULL && (sock->as_Flags & ASF_INET6) != 0)
    {
        struct sockaddr_in6 sin6;
        NXD_ADDRESS         mapped;
        const NXD_ADDRESS  *use = addr;

        if (addr->nxd_ip_version == NX_IP_VERSION_V4)
        {
            bsd_addr_to_v4mapped(&mapped, addr->nxd_ip_address.v4);
            use = &mapped;
        }

        bsd_bzero(&sin6, sizeof(sin6));
        sin6.sin6_len    = (UBYTE)sizeof(sin6);
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port   = (in_port_t)BSD_HTONS((UWORD)port);
        bsd_words_to_in6(use->nxd_ip_address.v6, sin6.sin6_addr.s6_addr);
        sin6.sin6_scope_id = scope_id;

        copy = *len;
        if (copy > (socklen_t)sizeof(sin6))
            copy = (socklen_t)sizeof(sin6);

        bsd_bcopy(&sin6, sa, (ULONG)copy);
        *len = (socklen_t)sizeof(struct sockaddr_in6);

        return;
    }
#else
    (VOID)sock;
    (VOID)scope_id;
#endif

    {
        struct sockaddr_in sin;

        bsd_bzero(&sin, sizeof(sin));
        sin.sin_len    = (UBYTE)sizeof(struct sockaddr_in);
        sin.sin_family = AF_INET;
        sin.sin_port   = (in_port_t)BSD_HTONS((UWORD)port);
        sin.sin_addr.s_addr =
            BSD_HTONL((addr->nxd_ip_version == NX_IP_VERSION_V4)
                          ? addr->nxd_ip_address.v4 : 0UL);

        copy = *len;
        if (copy > (socklen_t)sizeof(sin))
            copy = (socklen_t)sizeof(sin);

        bsd_bcopy(&sin, sa, (ULONG)copy);
        *len = (socklen_t)sizeof(struct sockaddr_in);
    }
}

LONG bsd_socket(register LONG domain   __asm("d0"),
                register LONG type     __asm("d1"),
                register LONG protocol __asm("d2"),
                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock;
    NX_IP     *ip = bsd_stack_ip(SocketBase);
    UINT       status;
    LONG       fd;

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

#ifdef AMINETXDUO_IPV6
    if (domain != AF_INET && domain != AF_INET6)
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);

    if (domain == AF_INET6 && !netstack_ipv6_enabled())
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
#else
    if (domain != AF_INET)
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
#endif

    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW)
        return bsd_fail(SocketBase, AMI_EPROTONOSUPPORT);

    if (type == SOCK_RAW)
    {
        if (protocol <= 0 || protocol > 255)
            return bsd_fail(SocketBase, AMI_EPROTONOSUPPORT);
    }
    else if (protocol != 0 &&
             protocol != ((type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP))
    {
        return bsd_fail(SocketBase, AMI_EPROTONOSUPPORT);
    }

    sock = bsd_socket_alloc(SocketBase, (UWORD)domain, (UWORD)type, protocol);
    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_ENOBUFS);

    if (bsd_nx_enter(SocketBase) != 0)
    {
        bsd_socket_dispose(sock);
        return bsd_fail(SocketBase, AMI_ENETDOWN);
    }

    bsd_closing_sweep();

    if (type == SOCK_RAW)
    {
        if (bsd_raw_open(SocketBase, sock) != 0)
        {
            bsd_nx_leave(SocketBase);
            bsd_socket_dispose(sock);
            return -1;                  /* bsd_raw_open set errno */
        }

        status = NX_SUCCESS;
    }
    else if (type == SOCK_STREAM)
    {
        status = nx_tcp_socket_create(ip, &sock->as_Nx.tcp, bsd_tcp_name,
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                      NX_IP_TIME_TO_LIVE,
                                      ami_bsd_tcp_window(SocketBase),
                                      bsd_tcp_urgent_notify,
                                      bsd_tcp_disconnect_callback);
        if (status == NX_SUCCESS)
        {
            bsd_tcp_seed_isn(&sock->as_Nx.tcp);
            bsd_tcp_keepalive_default(&sock->as_Nx.tcp);
            bsd_tcp_window_maximum(SocketBase, sock);
            bsd_tcp_rx_queue_cap(&sock->as_Nx.tcp);
            bsd_tcp_tx_queue_default(SocketBase, &sock->as_Nx.tcp);
        }
    }
    else
    {
        status = nx_udp_socket_create(ip, &sock->as_Nx.udp, bsd_udp_name,
                                      NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                      NX_IP_TIME_TO_LIVE,
                                      bsd_udp_queue_max(SocketBase));
    }

    if (status != NX_SUCCESS)
    {
        bsd_nx_leave(SocketBase);
        bsd_socket_dispose(sock);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    bsd_events_attach(sock);

    fd = bsd_fd_alloc(SocketBase, sock);
    if (fd < 0)
    {
        if (bsd_socket_destroy(sock))
            bsd_socket_dispose(sock);
        bsd_nx_leave(SocketBase);
        return -1;
    }

    bsd_nx_leave(SocketBase);

    return fd;
}

typedef enum
{
    BSD_BIND_FOREIGN = 0,   /* not an address this machine has              */
    BSD_BIND_ANY,           /* wildcard or loopback: nothing to enforce     */
    BSD_BIND_SOLE,          /* ours, and the only addressed interface, so
                               the socket ANY would give is the same one    */
    BSD_BIND_SPECIFIC       /* ours, but one of several: cannot be honoured */
} BsdBindKind;

static BOOL bsd_addr_is_unspecified(const NXD_ADDRESS *addr)
{
#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
        return (addr->nxd_ip_address.v6[0] == 0 &&
                addr->nxd_ip_address.v6[1] == 0 &&
                addr->nxd_ip_address.v6[2] == 0 &&
                addr->nxd_ip_address.v6[3] == 0) ? TRUE : FALSE;
#endif
    return (addr->nxd_ip_address.v4 == 0UL) ? TRUE : FALSE;
}

static BOOL bsd_addr_is_loopback(const NXD_ADDRESS *addr)
{
#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
        return (addr->nxd_ip_address.v6[0] == 0 &&
                addr->nxd_ip_address.v6[1] == 0 &&
                addr->nxd_ip_address.v6[2] == 0 &&
                addr->nxd_ip_address.v6[3] == 1UL) ? TRUE : FALSE;
#endif
    /* 127.0.0.0/8, all of it, as BSD treats it. */
    return ((addr->nxd_ip_address.v4 >> 24) == 127UL) ? TRUE : FALSE;
}

#ifdef AMINETXDUO_MULTICAST
static BOOL bsd_addr_is_multicast(const NXD_ADDRESS *addr)
{
#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
        return ((addr->nxd_ip_address.v6[0] & 0xFF000000UL) == 0xFF000000UL)
                   ? TRUE : FALSE;
#endif
    return ((addr->nxd_ip_address.v4 & 0xF0000000UL) == 0xE0000000UL)
               ? TRUE : FALSE;
}
#endif

static BsdBindKind bsd_bind_kind(struct AmiSocketBase *base,
                                 const NXD_ADDRESS *addr, ULONG scope)
{
    NX_IP *ip = bsd_stack_ip(base);
#ifdef AMINETXDUO_IPV6
    const NX_INTERFACE *zoned = NX_NULL;
#endif
    UWORD  matches = 0;
    UWORD  addressed = 0;
    UINT   i;

#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6 && scope != 0UL &&
        !bsd_addr_is_loopback(addr) &&
        anx6_scope(addr->nxd_ip_address.v6) < 0xEU)
    {
        if (ip == NULL || scope > (ULONG)NX_MAX_PHYSICAL_INTERFACES)
            return BSD_BIND_FOREIGN;

        zoned = &ip->nx_ip_interface[scope - 1UL];
        if (zoned->nx_interface_valid == 0)
            return BSD_BIND_FOREIGN;
    }
#else
    (VOID)scope;
#endif

    if (bsd_addr_is_unspecified(addr) || bsd_addr_is_loopback(addr))
        return BSD_BIND_ANY;

#ifdef AMINETXDUO_MULTICAST
    if (bsd_addr_is_multicast(addr))
        return BSD_BIND_ANY;
#endif

    if (ip == NULL)
        return BSD_BIND_FOREIGN;

#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
    {
        for (i = 0; i < (UINT)NX_MAX_IPV6_ADDRESSES; i++)
        {
            const NXD_IPV6_ADDRESS *a = &ip->nx_ipv6_address[i];

            if (a->nxd_ipv6_address_valid == 0)
                continue;

            addressed++;
            if (a->nxd_ipv6_address[0] == addr->nxd_ip_address.v6[0] &&
                a->nxd_ipv6_address[1] == addr->nxd_ip_address.v6[1] &&
                a->nxd_ipv6_address[2] == addr->nxd_ip_address.v6[2] &&
                a->nxd_ipv6_address[3] == addr->nxd_ip_address.v6[3] &&
                (zoned == NX_NULL || a->nxd_ipv6_address_attached == zoned))
                matches++;
        }
    }
    else
#endif
    {
        for (i = 0; i < (UINT)NX_MAX_PHYSICAL_INTERFACES; i++)
        {
            const NX_INTERFACE *nxif = &ip->nx_ip_interface[i];

            if (nxif->nx_interface_valid == 0 ||
                nxif->nx_interface_ip_address == 0UL)
                continue;

            addressed++;
            if (nxif->nx_interface_ip_address == addr->nxd_ip_address.v4)
                matches++;
        }
    }

    if (matches == 0)
        return BSD_BIND_FOREIGN;

    return (addressed == 1) ? BSD_BIND_SOLE : BSD_BIND_SPECIFIC;
}

LONG bsd_bind(register LONG sock_fd            __asm("d0"),
              register struct sockaddr *name   __asm("a0"),
              register socklen_t namelen       __asm("d1"),
              register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket  *sock = bsd_lookup(SocketBase, sock_fd);
    NXD_ADDRESS addr;
    ULONG       scope = 0;
    UINT        port = 0;
    UINT        status;
    BsdBindKind kind;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (bsd_netmon_have(MHT_Bind))
    {
        struct BindMonitorMsg bmm;
        LONG                  denied;

        bsd_bzero(&bmm, sizeof(bmm));
        bmm.bmm_Size    = (LONG)sizeof(bmm);
        bmm.bmm_Caller  = bsd_netmon_caller(SocketBase);
        bmm.bmm_Socket  = sock_fd;
        bmm.bmm_Name    = name;
        bmm.bmm_NameLen = (LONG)namelen;

        denied = bsd_netmon_dispatch(MHT_Bind, &bmm);
        if (denied > 0)
            return bsd_fail(SocketBase, denied);
    }

    if ((sock->as_Flags & ASF_BOUND) != 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if (bsd_sockaddr_get(SocketBase, name, namelen, &addr, &port, &scope) != 0)
        return -1;

#ifdef AMINETXDUO_IPV6
    if ((sock->as_Flags & ASF_INET6) != 0)
    {
        if (addr.nxd_ip_version == NX_IP_VERSION_V4)
            return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
        if (!bsd_addr_normalise(sock, &addr))
            return bsd_fail(SocketBase, AMI_EINVAL);
    }
    else if (addr.nxd_ip_version == NX_IP_VERSION_V6)
    {
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
    }
#endif

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);
    kind = bsd_bind_kind(SocketBase, &addr, scope);
    bsd_nx_leave(SocketBase);

    switch (kind)
    {
        case BSD_BIND_ANY:
        case BSD_BIND_SOLE:
            break;

        case BSD_BIND_SPECIFIC:
            break;

        case BSD_BIND_FOREIGN:
        default:
            return bsd_fail(SocketBase, AMI_EADDRNOTAVAIL);
    }

    if ((sock->as_Flags & ASF_RAW) != 0)
    {
        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        sock->as_LocalAddr    = addr;
        sock->as_LocalPort    = port;
        sock->as_LocalScopeId = scope;
        sock->as_LocalScopeEpoch = bsd_scope_epoch(scope);
        sock->as_Flags |= ASF_BOUND;
        bsd_raw_revalidate_endpoint(sock);

        bsd_nx_leave(SocketBase);

        return 0;
    }

    sock->as_LocalAddr    = addr;
    sock->as_LocalPort    = port;
    sock->as_LocalScopeId = scope;
    sock->as_LocalScopeEpoch = bsd_scope_epoch(scope);

    if ((sock->as_Flags & ASF_UDP) != 0)
    {
        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        /* Opt into SO_REUSEPORT (and SO_REUSEADDR, which #38 makes the same
           promise for legacy callers) sharing only for an unspecified
           (INADDR_ANY, or :: for IPv6) bind with an explicit port.  A socket
           bound to a specific local address -- loopback, a multicast group,
           or one of the host's own addresses -- never opts in: NetX UDP binds
           by port alone and has no per-socket address or group filter, so
           fanning a multicast datagram out to such a co-bound sharer would
           over-deliver to a socket that never joined the group.  The bind
           itself still succeeds (legacy SO_REUSEADDR callers must not
           regress); only the share flag is withheld.  Full BSD address/group
           filtering is tracked separately.  */
        sock->as_Nx.udp.nx_udp_socket_share =
            (((sock->as_Flags & (ASF_REUSEPORT | ASF_REUSEADDR)) != 0) &&
             (port != 0) &&
             bsd_addr_is_unspecified(&addr)) ? NX_TRUE : NX_FALSE;

        status = nx_udp_socket_bind(&sock->as_Nx.udp,
                                    (port != 0) ? port : NX_ANY_PORT,
                                    NX_NO_WAIT);
        if (status != NX_SUCCESS)
        {
            bsd_nx_leave(SocketBase);
            return bsd_fail(SocketBase, bsd_errno_from_nx(status));
        }

        nx_udp_socket_port_get(&sock->as_Nx.udp, &port);
        bsd_nx_leave(SocketBase);

        sock->as_LocalPort = port;
        sock->as_Flags |= ASF_NXBOUND;
    }
    else
    {
        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        status = nx_tcp_client_socket_bind(&sock->as_Nx.tcp,
                                           (port != 0) ? port : NX_ANY_PORT,
                                           NX_NO_WAIT);
        if (status != NX_SUCCESS)
        {
            bsd_nx_leave(SocketBase);
            return bsd_fail(SocketBase,
                            (status == NX_PORT_UNAVAILABLE ||
                             status == NX_ALREADY_BOUND)
                                ? AMI_EADDRINUSE
                                : bsd_errno_from_nx(status));
        }

        nx_tcp_client_socket_port_get(&sock->as_Nx.tcp, &port);
        bsd_nx_leave(SocketBase);

        sock->as_LocalPort = port;
        sock->as_Flags |= ASF_NXBOUND;
    }

    sock->as_Flags |= ASF_BOUND;

    return 0;
}

static VOID bsd_listen_unlink(AmiSocket *sock, AmiSocket *victim)
{
    AmiSocket **link = &sock->as_Incoming;

    while (*link != NULL)
    {
        if (*link == victim)
        {
            *link = victim->as_IncomingNext;
            victim->as_IncomingNext = NULL;
            if (sock->as_IncomingCount != 0)
                sock->as_IncomingCount--;
            return;
        }
        link = &(*link)->as_IncomingNext;
    }
}

/*
 * Add one socket to a listening descriptor's list. Call inside a ThreadX
 * bracket. TRUE means one was added.
 */
static BOOL bsd_listen_park_one(struct AmiSocketBase *base, AmiSocket *sock)
{
    NX_IP     *ip = bsd_stack_ip(base);
    AmiSocket *spare;
    UINT       status;

    if (ip == NULL || (sock->as_Flags & ASF_LISTENING) == 0)
        return FALSE;

    spare = bsd_socket_alloc(base, sock->as_Domain, sock->as_Type,
                             sock->as_Protocol);
    if (spare == NULL)
        return FALSE;

    status = nx_tcp_socket_create(ip, &spare->as_Nx.tcp, bsd_tcp_name,
                                  NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                  NX_IP_TIME_TO_LIVE,
                                  ami_bsd_tcp_window(base),
                                  bsd_tcp_urgent_notify,
                                  bsd_tcp_disconnect_callback);
    if (status != NX_SUCCESS)
    {
        bsd_socket_dispose(spare);
        return FALSE;
    }

    bsd_tcp_seed_isn(&spare->as_Nx.tcp);
    bsd_tcp_tx_queue_default(base, &spare->as_Nx.tcp);
    bsd_tcp_keepalive_default(&spare->as_Nx.tcp);
    bsd_tcp_window_maximum(base, spare);
    bsd_tcp_rx_queue_cap(&spare->as_Nx.tcp);

    spare->as_Flags    |= ASF_INCOMING | ASF_SERVER;
    spare->as_Parent    = sock;
    spare->as_LocalPort = sock->as_ListenPort;
    bsd_events_attach(spare);

    status = nx_tcp_server_socket_relisten(ip, sock->as_ListenPort,
                                           &spare->as_Nx.tcp);

    if (status != NX_SUCCESS && status != NX_CONNECTION_PENDING &&
        sock->as_Incoming == NULL)
    {
        AMI_WARN("bsdsocket: relisten on port %ld failed (%ld); rebuilding "
                 "the listen request",
                 (long)sock->as_ListenPort, (long)status);

        nx_tcp_server_socket_unlisten(ip, sock->as_ListenPort);

        status = nx_tcp_server_socket_listen(ip, sock->as_ListenPort,
                                             &spare->as_Nx.tcp,
                                             sock->as_Backlog,
                                             bsd_listen_callback);

        if (status != NX_SUCCESS && status != NX_CONNECTION_PENDING)
        {
            AMI_WARN("bsdsocket: port %ld has no listen request left (%ld). "
                     "The next accept() will try again",
                     (long)sock->as_ListenPort, (long)status);

            if (bsd_socket_destroy(spare))
                bsd_socket_dispose(spare);

            return FALSE;
        }
    }

    if (status == NX_SUCCESS || status == NX_CONNECTION_PENDING)
    {
        /* OPTIONAL, as in bsd_listen_return() below. */
        UINT armed = nx_tcp_server_socket_accept(&spare->as_Nx.tcp, NX_NO_WAIT);

        if (armed != NX_IN_PROGRESS && armed != NX_SUCCESS)
            AMI_WARN("bsdsocket: port %ld was relistened but not armed (%ld); "
                     "the next connection to it is dropped",
                     (long)sock->as_ListenPort, (long)armed);

        if (status == NX_CONNECTION_PENDING)
            sock->as_Flags |= ASF_ACCEPTPEND;
    }

    Forbid();
    spare->as_Owner        = sock->as_Owner;
    spare->as_IncomingNext = sock->as_Incoming;
    sock->as_Incoming      = spare;
    sock->as_IncomingCount++;
    Permit();

    return TRUE;
}

/*
 * Top the listener's parked list back up to the backlog it asked for.
 * TRUE means there is at least one socket on the port.
 */
static BOOL bsd_listen_rearm(struct AmiSocketBase *base, AmiSocket *sock)
{
    UINT limit = (sock->as_Backlog != 0) ? sock->as_Backlog : 1;

    while (sock->as_IncomingCount < limit)
    {
        if (!bsd_listen_park_one(base, sock))
            break;
    }

    return (sock->as_Incoming != NULL) ? TRUE : FALSE;
}

static VOID bsd_listen_return(struct AmiSocketBase *base, AmiSocket *sock,
                              AmiSocket *incoming);

/*
 * Reset `incoming`, take it off the port and put it back, with the listener's
 * refill held off throughout (N-088).  The unaccept releases data the peer
 * had queued, and a release can hand the baton on: the IP thread's listen
 * callback then finds this socket CLOSED and unbound, relistens it and gives
 * it a new connection, and the relisten here answers NX_NOT_CLOSED and
 * destroys that live connection.  ASF_RELISTENING makes bsd_listen_refill()
 * return at once; a handshake that finishes meanwhile waits in the SYN cache
 * for the relisten below, which delivers it.
 */
static VOID bsd_listen_hand_back(struct AmiSocketBase *base, AmiSocket *sock,
                                 AmiSocket *incoming)
{
    ULONG held = sock->as_Flags & ASF_RELISTENING;

    sock->as_Flags |= ASF_RELISTENING;

    /* NX_NO_WAIT on an established socket is NetX's reset. */
    nx_tcp_socket_disconnect(&incoming->as_Nx.tcp, NX_NO_WAIT);
    nx_tcp_server_socket_unaccept(&incoming->as_Nx.tcp);

    if (incoming->as_Parent == NULL)
    {
        /* accept() had already detached it: it belongs to the listener again
           before it goes back on the port. */
        Forbid();
        incoming->as_Flags &= ~ASF_CONNECTED;
        incoming->as_Flags |= ASF_INCOMING;
        incoming->as_Parent = sock;
        incoming->as_Owner  = sock->as_Owner;
        Permit();
    }

    bsd_listen_return(base, sock, incoming);

    if (held == 0)
        sock->as_Flags &= ~ASF_RELISTENING;
}

/*
 * Put a socket accept() has decided not to hand over back on the port. The
 * caller has already disconnected and unaccepted it.
 */
static VOID bsd_listen_return(struct AmiSocketBase *base, AmiSocket *sock,
                              AmiSocket *incoming)
{
    NX_IP *ip = bsd_stack_ip(base);
    UINT   status = NX_NOT_SUCCESSFUL;

    if (ip != NULL)
        status = nx_tcp_server_socket_relisten(ip, sock->as_ListenPort,
                                               &incoming->as_Nx.tcp);

    if (status == NX_SUCCESS || status == NX_CONNECTION_PENDING)
    {
        /* OPTIONAL.  NX_NO_WAIT arms rather than waits, so NX_IN_PROGRESS is
           the ordinary answer and NX_SUCCESS means a connection arrived in the
           window above.  Anything else is NX_NOT_LISTEN_STATE, and a slot that
           is on the incoming list but armed nowhere drops the next connection
           to this port silently. */
        UINT armed = nx_tcp_server_socket_accept(&incoming->as_Nx.tcp, NX_NO_WAIT);

        if (armed != NX_IN_PROGRESS && armed != NX_SUCCESS)
            AMI_WARN("bsdsocket: port %ld was relistened but not armed (%ld); "
                     "the next connection to it is dropped",
                     (long)sock->as_ListenPort, (long)armed);
        return;
    }

    bsd_listen_unlink(sock, incoming);
    if (bsd_socket_destroy(incoming))
        bsd_socket_dispose(incoming);

    (VOID)bsd_listen_rearm(base, sock);
}

LONG bsd_listen(register LONG sock_fd __asm("d0"),
                register LONG backlog __asm("d1"),
                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);
    AmiSocket *incoming;
    NX_IP     *ip = bsd_stack_ip(SocketBase);
    UINT       status;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_TCP) == 0)
        return bsd_fail(SocketBase, AMI_EOPNOTSUPP);

    if ((sock->as_Flags & ASF_LISTENING) != 0)
        return 0;                               /* idempotent, as in BSD */

    if ((sock->as_Flags & ASF_CONNECTED) != 0)
        return bsd_fail(SocketBase, AMI_EISCONN);

    if (sock->as_LocalPort == 0)
        return bsd_fail(SocketBase, AMI_EDESTADDRREQ);

    if (backlog < 1)
        backlog = 1;
    if (backlog > BSD_MAX_BACKLOG)
        backlog = BSD_MAX_BACKLOG;

    incoming = bsd_socket_alloc(SocketBase, sock->as_Domain, sock->as_Type,
                                sock->as_Protocol);
    if (incoming == NULL)
        return bsd_fail(SocketBase, AMI_ENOBUFS);

    if (bsd_nx_enter(SocketBase) != 0)
    {
        bsd_socket_dispose(incoming);
        return bsd_fail(SocketBase, AMI_ENETDOWN);
    }

    status = nx_tcp_socket_create(ip, &incoming->as_Nx.tcp, bsd_tcp_name,
                                  NX_IP_NORMAL, NX_FRAGMENT_OKAY,
                                  NX_IP_TIME_TO_LIVE,
                                  ami_bsd_tcp_window(SocketBase),
                                  bsd_tcp_urgent_notify,
                                  bsd_tcp_disconnect_callback);
    if (status == NX_SUCCESS)
    {
        bsd_tcp_seed_isn(&incoming->as_Nx.tcp);
        bsd_tcp_keepalive_default(&incoming->as_Nx.tcp);
        bsd_tcp_window_maximum(SocketBase, incoming);
        bsd_tcp_rx_queue_cap(&incoming->as_Nx.tcp);
        bsd_tcp_tx_queue_default(SocketBase, &incoming->as_Nx.tcp);
    }
    if (status != NX_SUCCESS)
    {
        bsd_nx_leave(SocketBase);
        bsd_socket_dispose(incoming);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    incoming->as_Flags     |= ASF_INCOMING | ASF_SERVER;
    incoming->as_Parent     = sock;
    incoming->as_LocalPort  = sock->as_LocalPort;
    bsd_events_attach(incoming);

    status = nx_tcp_server_socket_listen(ip, sock->as_LocalPort,
                                         &incoming->as_Nx.tcp,
                                         (UINT)backlog, bsd_listen_callback);
    if (status != NX_SUCCESS)
    {
        if (bsd_socket_destroy(incoming))
            bsd_socket_dispose(incoming);
        bsd_nx_leave(SocketBase);
        return bsd_fail(SocketBase, bsd_errno_from_nx(status));
    }

    status = nx_tcp_server_socket_accept(&incoming->as_Nx.tcp, NX_NO_WAIT);
    if (status != NX_IN_PROGRESS && status != NX_SUCCESS)
        AMI_WARN("bsdsocket: arming accept on port %ld failed (%ld)",
                 (long)sock->as_LocalPort, (long)status);

    Forbid();
    incoming->as_Owner        = sock->as_Owner;
    incoming->as_IncomingNext = NULL;
    sock->as_Incoming         = incoming;
    sock->as_IncomingCount    = 1;
    Permit();
    sock->as_ListenPort       = sock->as_LocalPort;
    sock->as_Backlog          = (UINT)backlog;
    sock->as_Flags           |= ASF_LISTENING;

    (VOID)bsd_listen_rearm(SocketBase, sock);

    bsd_nx_leave(SocketBase);

    return 0;
}

/*
 * Is the socket parked on a listener holding a connection accept() can hand
 * over?
 */
BOOL bsd_incoming_ready(const AmiSocket *incoming)
{
    UINT state;

    if (incoming == NULL)
        return FALSE;

    state = incoming->as_Nx.tcp.nx_tcp_socket_state;

    return (state == NX_TCP_ESTABLISHED || state == NX_TCP_CLOSE_WAIT ||
            state == NX_TCP_CLOSING || state == NX_TCP_TIMED_WAIT ||
            state == NX_TCP_LAST_ACK);
}

/*
 * Which parked socket, if any, has a connection ready to hand over. The list
 * is walked oldest-last, so this returns the one that has been waiting
 * longest, the accept queue's order.
 */
AmiSocket *bsd_incoming_first_ready(const AmiSocket *listener)
{
    AmiSocket *p;
    AmiSocket *ready = NULL;

    for (p = listener->as_Incoming; p != NULL; p = p->as_IncomingNext)
    {
        if (bsd_incoming_ready(p))
            ready = p;
    }

    return ready;
}

/*
 * Which parked socket is the one on the port.
 *
 */
static AmiSocket *bsd_incoming_on_port(const AmiSocket *listener)
{
    AmiSocket *p;

    for (p = listener->as_Incoming; p != NULL; p = p->as_IncomingNext)
    {
        UINT state = p->as_Nx.tcp.nx_tcp_socket_state;

        if (state == NX_TCP_LISTEN_STATE || state == NX_TCP_SYN_RECEIVED)
            return p;
    }

    return NULL;
}

typedef struct
{
    AmiSocket *listener;
    AmiSocket *ready;
} BsdAcceptArgs;

static UINT bsd_accept_once(VOID *arg, ULONG wait)
{
    BsdAcceptArgs *a = (BsdAcceptArgs *)arg;
    AmiSocket     *listening;
    UINT           status;

    a->ready = bsd_incoming_first_ready(a->listener);
    if (a->ready != NULL)
        return NX_SUCCESS;

    if (a->listener->as_Incoming == NULL)
        return NX_NOT_CONNECTED;

    listening = bsd_incoming_on_port(a->listener);
    if (listening == NULL)
        return NX_NOT_CONNECTED;

    /*
     * accept() is not a harmless wait primitive. A finite wait that expires
     * runs _nx_tcp_connect_cleanup(), moves SYN_RECEIVED back to LISTEN and
     * makes the next call send a fresh SYN+ACK with changed sequence state.
     * Arm the passive open without suspending, then use state_wait(), whose
     * timeout changes no TCP state, for the interruptible slice.
     */
    if (listening->as_Nx.tcp.nx_tcp_socket_state == NX_TCP_LISTEN_STATE)
    {
        status = nx_tcp_server_socket_accept(&listening->as_Nx.tcp,
                                             NX_NO_WAIT);
        if (status != NX_SUCCESS && status != NX_IN_PROGRESS &&
            status != NX_NOT_CONNECTED)
            return status;
    }

    status = nx_tcp_socket_state_wait(&listening->as_Nx.tcp,
                                      NX_TCP_ESTABLISHED, wait);

    a->ready = bsd_incoming_first_ready(a->listener);
    if (a->ready != NULL)
        return NX_SUCCESS;

    if (status == NX_NOT_SUCCESSFUL || status == NX_NOT_CONNECTED ||
        status == NX_IN_PROGRESS)
        return NX_NO_PACKET;

    return status;
}

typedef struct
{
    AmiSocket *sock;
} BsdConnectArgs;

static UINT bsd_connect_once(VOID *arg, ULONG wait)
{
    BsdConnectArgs *a    = (BsdConnectArgs *)arg;
    AmiSocket      *sock = a->sock;

    /* Asleep until the establish or disconnect notify wakes it
       (bsd_connect_wake), not nx_tcp_socket_state_wait(): that polls a tick
       at a time, and waits for ESTABLISHED exactly, so a peer that closes at
       once has the socket in CLOSE_WAIT before it looks and the whole slice
       ran out, 200 ms and more per connect().  The baton holds the IP thread
       off between the test and the sleep, so no wake falls in that gap.  A
       slice is a second at most, so a wake that never comes costs one. */
    while (wait != NX_NO_WAIT &&
           (sock->as_Flags & (ASF_CONNECTED | ASF_CONNECTING)) == ASF_CONNECTING)
    {
        ULONG slice = (wait < NX_IP_PERIODIC_RATE) ? wait : NX_IP_PERIODIC_RATE;

        sock->as_ConnectWaiter = tx_thread_identify();
        /* TX_SUCCESS, the slice ran out, or TX_WAIT_ABORTED, a notify woke
           it: the flags tested by the loop say which. */
        AMI_NX_EITHER_WAY(tx_thread_sleep(slice));
        sock->as_ConnectWaiter = TX_NULL;

        if (wait != NX_WAIT_FOREVER)
            wait -= slice;
    }

    if ((sock->as_Flags & ASF_CONNECTED) != 0 ||
        sock->as_Nx.tcp.nx_tcp_socket_state == NX_TCP_ESTABLISHED)
        return NX_SUCCESS;

    if ((sock->as_Flags & ASF_CONNECTING) == 0)
        return NX_NOT_CONNECTED;    /* died: a non-retry status, so we stop */

    return NX_NO_PACKET;            /* still connecting: keep slicing */
}

/*
 * Does a completed connection belong to the address the listener was bound to?
 *
 */
BOOL bsd_bind_wants_interface(const AmiSocket *listener,
                              const NX_INTERFACE *nxif)
{
    NX_IP *ip = bsd_stack_ip(listener->as_Owner);

#ifdef AMINETXDUO_IPV6
    ULONG scope = bsd_local_scope(listener);

    if (listener->as_LocalAddr.nxd_ip_version == NX_IP_VERSION_V6 &&
        scope != 0UL &&
        !bsd_addr_is_loopback(&listener->as_LocalAddr) &&
        anx6_scope(listener->as_LocalAddr.nxd_ip_address.v6) < 0xEU)
    {
        if (ip == NX_NULL ||
            scope > (ULONG)NX_MAX_PHYSICAL_INTERFACES ||
            nxif != &ip->nx_ip_interface[scope - 1UL])
            return FALSE;
    }
#endif

    if (bsd_addr_is_unspecified(&listener->as_LocalAddr))
        return TRUE;

#ifdef AMINETXDUO_MULTICAST
    if (bsd_addr_is_multicast(&listener->as_LocalAddr))
        return TRUE;
#endif

    if (nxif == NX_NULL || ip == NULL)
        return FALSE;

    if (bsd_addr_is_loopback(&listener->as_LocalAddr))
        return (nxif == &ip->nx_ip_interface[NX_LOOPBACK_INTERFACE])
                   ? TRUE : FALSE;

#ifdef AMINETXDUO_IPV6
    if (listener->as_LocalAddr.nxd_ip_version == NX_IP_VERSION_V6)
    {
        UINT i;

        for (i = 0; i < (UINT)NX_MAX_IPV6_ADDRESSES; i++)
        {
            const NXD_IPV6_ADDRESS *a = &ip->nx_ipv6_address[i];

            if (a->nxd_ipv6_address_valid == 0 ||
                a->nxd_ipv6_address_attached != nxif)
                continue;

            if (a->nxd_ipv6_address[0] == listener->as_LocalAddr.nxd_ip_address.v6[0] &&
                a->nxd_ipv6_address[1] == listener->as_LocalAddr.nxd_ip_address.v6[1] &&
                a->nxd_ipv6_address[2] == listener->as_LocalAddr.nxd_ip_address.v6[2] &&
                a->nxd_ipv6_address[3] == listener->as_LocalAddr.nxd_ip_address.v6[3])
                return TRUE;
        }

        return FALSE;
    }
#endif

    return (nxif->nx_interface_ip_address ==
            listener->as_LocalAddr.nxd_ip_address.v4) ? TRUE : FALSE;
}

static BOOL bsd_bind_accepts(const AmiSocket *listener, NX_TCP_SOCKET *conn)
{
    if (!bsd_bind_wants_interface(listener,
                                  conn->nx_tcp_socket_connect_interface))
        return FALSE;

    /* A wildcard IPv6 listener is deliberately dual-stack, but an IPv4
       listener is not. NetX's listen table is indexed by port alone, so this
       family check cannot be left to the stack. */
    if (listener->as_LocalAddr.nxd_ip_version == NX_IP_VERSION_V4)
        return (conn->nx_tcp_socket_connect_ip.nxd_ip_version ==
                NX_IP_VERSION_V4) ? TRUE : FALSE;

#ifdef AMINETXDUO_IPV6
    if (listener->as_LocalAddr.nxd_ip_version == NX_IP_VERSION_V6)
    {
        const NXD_IPV6_ADDRESS *local;

        if (bsd_addr_is_unspecified(&listener->as_LocalAddr))
            return TRUE;

        if (conn->nx_tcp_socket_connect_ip.nxd_ip_version !=
            NX_IP_VERSION_V6)
            return FALSE;

        local = conn->nx_tcp_socket_ipv6_addr;

        return (local != NX_NULL &&
                local->nxd_ipv6_address[0] ==
                    listener->as_LocalAddr.nxd_ip_address.v6[0] &&
                local->nxd_ipv6_address[1] ==
                    listener->as_LocalAddr.nxd_ip_address.v6[1] &&
                local->nxd_ipv6_address[2] ==
                    listener->as_LocalAddr.nxd_ip_address.v6[2] &&
                local->nxd_ipv6_address[3] ==
                    listener->as_LocalAddr.nxd_ip_address.v6[3])
                   ? TRUE : FALSE;
    }
#endif

    return FALSE;
}

#ifdef AMINETXDUO_IPV6
ULONG bsd_scope_epoch(ULONG scope)
{
    return (scope != 0UL) ? netstack_interface_epoch((UWORD)(scope - 1UL))
                          : 0UL;
}

ULONG bsd_scope_live(ULONG scope, ULONG epoch)
{
    return (scope == 0UL || bsd_scope_epoch(scope) == epoch)
               ? scope : BSD_SCOPE_GONE;
}
#endif

/*
 * Which source a send from this socket has to use.
 *
 */
BsdSourceKind bsd_source_select(const AmiSocket *sock, const NXD_ADDRESS *dest,
                                ULONG scope, UINT *index)
{
    NX_IP             *ip    = bsd_stack_ip(sock->as_Owner);
    const NXD_ADDRESS *local = &sock->as_LocalAddr;
    BOOL               bound;
    UINT               i;

    *index = 0;

    if (ip == NULL)
        return BSD_SOURCE_REFUSE;

    bound = (!bsd_addr_is_unspecified(local) &&
             local->nxd_ip_version == dest->nxd_ip_version) ? TRUE : FALSE;

#ifdef AMINETXDUO_MULTICAST
    if (bound && bsd_addr_is_multicast(local))
        bound = FALSE;
#endif

#ifdef AMINETXDUO_IPV6
    if (dest->nxd_ip_version == NX_IP_VERSION_V6)
    {
        NX_INTERFACE *zoned       = NX_NULL;
        NX_INTERFACE *local_zoned = NX_NULL;
        ULONG         local_scope = bsd_local_scope(sock);

        if (bound && local_scope != 0UL &&
            anx6_scope(local->nxd_ip_address.v6) < 0xEU)
        {
            if (local_scope > (ULONG)NX_MAX_PHYSICAL_INTERFACES)
                return BSD_SOURCE_REFUSE;

            local_zoned = &ip->nx_ip_interface[local_scope - 1UL];

            if (local_zoned->nx_interface_valid == 0)
                return BSD_SOURCE_REFUSE;
        }

        /*
         * RFC 4007 section 7 requires the upper layer to identify the zone
         * for every non-global destination when more than one is present.
         */
        if (scope != 0UL &&
            anx6_scope(dest->nxd_ip_address.v6) < 0xEU)
        {
            if (scope > (ULONG)NX_MAX_PHYSICAL_INTERFACES)
                return BSD_SOURCE_REFUSE;

            zoned = &ip->nx_ip_interface[scope - 1UL];

            if (zoned->nx_interface_valid == 0)
                return BSD_SOURCE_REFUSE;
        }

        if (local_zoned != NX_NULL && zoned != NX_NULL &&
            local_zoned != zoned)
            return BSD_SOURCE_UNREACH;

        if (zoned == NX_NULL)
            zoned = local_zoned;

        if (!bound && zoned == NX_NULL)
            return BSD_SOURCE_ROUTE;

        if (!bound)
        {
            LONG required = (zoned != NX_NULL)
                                ? (LONG)zoned->nx_interface_index : -1L;

            if (!netstack_ipv6_source_find(dest->nxd_ip_address.v6,
                                           required, NULL, index))
                return BSD_SOURCE_REFUSE;
            return BSD_SOURCE_INDEX;
        }

        for (i = 0;
             i < (UINT)(NX_MAX_IPV6_ADDRESSES + NX_LOOPBACK_IPV6_ENABLED);
             i++)
        {
            const NXD_IPV6_ADDRESS *a = &ip->nx_ipv6_address[i];

            if (a->nxd_ipv6_address_valid == 0 ||
                a->nxd_ipv6_address_state != NX_IPV6_ADDR_STATE_VALID)
                continue;

            if (zoned != NX_NULL)
            {
                if (a->nxd_ipv6_address_attached != zoned)
                    continue;
            }

            if (bound &&
                (a->nxd_ipv6_address[0] != local->nxd_ip_address.v6[0] ||
                 a->nxd_ipv6_address[1] != local->nxd_ip_address.v6[1] ||
                 a->nxd_ipv6_address[2] != local->nxd_ip_address.v6[2] ||
                 a->nxd_ipv6_address[3] != local->nxd_ip_address.v6[3]))
                continue;

            *index = (UINT)a->nxd_ipv6_address_index;
            return BSD_SOURCE_INDEX;
        }

        return BSD_SOURCE_REFUSE;
    }
#else
    (VOID)scope;
#endif

    if (!bound)
        return BSD_SOURCE_ROUTE;

    if (bsd_addr_is_loopback(local))
    {
        *index = (UINT)NX_LOOPBACK_INTERFACE;
    }
    else
    {
        for (i = 0; i < (UINT)NX_MAX_IP_INTERFACES; i++)
        {
            if (ip->nx_ip_interface[i].nx_interface_valid != 0 &&
                ip->nx_ip_interface[i].nx_interface_ip_address ==
                    local->nxd_ip_address.v4)
                break;
        }

        if (i == (UINT)NX_MAX_IP_INTERFACES)
            return BSD_SOURCE_REFUSE;

        *index = i;
    }

    {
        ULONG         next_hop = 0;

        if (!netstack_ipv4_route(dest->nxd_ip_address.v4, (LONG)*index,
                                 NULL, &next_hop, NULL) ||
            next_hop == 0UL)
            return BSD_SOURCE_UNREACH;
    }

    return BSD_SOURCE_INDEX;
}

/*
 * accept()'s pin on its listener, given back with no bracket held: a release
 * that would be the last is owed to the next bracketed close, as an
 * unbracketed CloseSocket() owes its own.
 */
static LONG bsd_accept_unpin_fail(struct AmiSocketBase *base, AmiSocket *sock,
                                  LONG error)
{
    Forbid();
    if (sock->as_RefCount > 1)
        sock->as_RefCount--;
    else
        bsd_socket_defer(sock);
    bsd_owner_drop(base, sock);
    Permit();

    return bsd_fail(base, error);
}

/* After a yield: still this descriptor's socket, and still owned?  A close
   of the descriptor meanwhile leaves only accept()'s pin, and nothing would
   wake a wait on it (bsd_owner_drop() has cleared the owner). */
static BOOL bsd_accept_still_open(struct AmiSocketBase *base, LONG fd,
                                  AmiSocket *sock)
{
    return (bsd_lookup(base, fd) == sock && sock->as_Owner != NULL)
               ? TRUE : FALSE;
}

LONG bsd_accept(register LONG sock_fd          __asm("d0"),
                register struct sockaddr *addr __asm("a0"),
                register socklen_t *addrlen    __asm("a1"),
                register struct AmiSocketBase *SocketBase __asm("a6"))
{
    /* Pinned: every wait below yields, and the listener must outlive a close
       of its descriptor and a deferred release swept by another opener. */
    AmiSocket  *sock = bsd_lookup_pin(SocketBase, sock_fd);
    AmiSocket  *incoming = NULL;
    NX_IP      *ip = bsd_stack_ip(SocketBase);
    NXD_ADDRESS peer;
    ULONG       peer_port = 0;
    ULONG       wait;
    UINT        status;
    LONG        fd;
    LONG        rc;
    BOOL        accepted = FALSE;   /* set with rc = fd, and only there */

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (addr != NULL && addrlen == NULL)
        return bsd_accept_unpin_fail(SocketBase, sock, AMI_EFAULT);

    if (ip == NULL)
        return bsd_accept_unpin_fail(SocketBase, sock, AMI_ENETDOWN);

    if ((sock->as_Flags & ASF_TCP) == 0)
        return bsd_accept_unpin_fail(SocketBase, sock, AMI_EOPNOTSUPP);

    if ((sock->as_Flags & ASF_LISTENING) == 0)
        return bsd_accept_unpin_fail(SocketBase, sock, AMI_EINVAL);

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_accept_unpin_fail(SocketBase, sock, AMI_ENETDOWN);

    wait = bsd_wait_option(sock, sock->as_RcvTimeout, 0);

    /*
     * A connection the bind refuses is reset and its slot goes back on the
     * port.  NetX's listen table is keyed by port alone, so one can arrive on
     * an address the listener is not bound to (#52).  Only a non-blocking
     * accept, or an SO_RCVTIMEO with nothing left, reports it as EWOULDBLOCK;
     * a blocking one waits on for the next.
     */
    for (;;)
    {
        BsdAcceptArgs args;
        BOOL          aborted;
        ULONG         started = tx_time_get();

        if (!bsd_listen_rearm(SocketBase, sock))
        {
            rc = bsd_fail(SocketBase, AMI_ENOBUFS);
            goto out;
        }

        args.listener = sock;
        args.ready    = NULL;

        status = bsd_wait_event(SocketBase, wait, bsd_accept_once, &args,
                                &aborted);
        if (aborted)
        {
            rc = bsd_fail(SocketBase, AMI_EINTR);
            goto out;
        }

        if (!bsd_accept_still_open(SocketBase, sock_fd, sock))
        {
            rc = bsd_fail(SocketBase, AMI_EBADF);
            goto out;
        }

        incoming = args.ready;

        if (status == NX_SUCCESS && incoming == NULL)
            status = NX_NO_PACKET;

        if (status == NX_NOT_CONNECTED || status == NX_IN_PROGRESS ||
            status == NX_NO_PACKET)
        {
            rc = bsd_fail(SocketBase, AMI_EWOULDBLOCK);
            goto out;
        }

        if (status == NX_WAIT_ABORTED)
        {
            rc = bsd_fail(SocketBase, AMI_EINTR);
            goto out;
        }

        if (status != NX_SUCCESS)
        {
            rc = bsd_fail(SocketBase, bsd_errno_from_nx(status));
            goto out;
        }

        nxd_tcp_socket_peer_info_get(&incoming->as_Nx.tcp, &peer, &peer_port);

        if (!bsd_bind_accepts(sock, &incoming->as_Nx.tcp))
        {
            AMI_DEBUG("bsdsocket: listener bound elsewhere refused a peer on port %ld",
                      (long)sock->as_ListenPort);
        }
#ifdef AMINETXDUO_IPV6
        else if ((sock->as_Flags & ASF_V6ONLY) != 0 &&
                 peer.nxd_ip_version == NX_IP_VERSION_V4)
        {
            AMI_DEBUG("bsdsocket: V6ONLY listener on port %ld refused an IPv4 peer",
                      (long)sock->as_ListenPort);
        }
#endif
        else
            break;

        bsd_listen_hand_back(SocketBase, sock, incoming);
        incoming = NULL;

        if (!bsd_accept_still_open(SocketBase, sock_fd, sock))
        {
            rc = bsd_fail(SocketBase, AMI_EBADF);
            goto out;
        }

        if (wait == NX_NO_WAIT)
        {
            rc = bsd_fail(SocketBase, AMI_EWOULDBLOCK);
            goto out;
        }

        if (wait != NX_WAIT_FOREVER)
        {
            ULONG elapsed = tx_time_get() - started;

            wait = (elapsed >= wait) ? NX_NO_WAIT : wait - elapsed;
        }
    }

    Forbid();
    incoming->as_Flags &= ~(ASF_INCOMING | ASF_ACCEPTPEND);
    incoming->as_Flags |= ASF_CONNECTED | ASF_BOUND;
    incoming->as_Parent = NULL;
    incoming->as_Owner  = SocketBase;
    Permit();

    incoming->as_LocalAddr    = sock->as_LocalAddr;
    incoming->as_LocalScopeId = sock->as_LocalScopeId;
    incoming->as_LocalScopeEpoch = sock->as_LocalScopeEpoch;

    if (bsd_addr_is_unspecified(&sock->as_LocalAddr))
    {
        if (peer.nxd_ip_version == NX_IP_VERSION_V4 &&
            incoming->as_Nx.tcp.nx_tcp_socket_connect_interface != NX_NULL)
        {
            bsd_addr_from_v4(
                &incoming->as_LocalAddr,
                incoming->as_Nx.tcp.nx_tcp_socket_connect_interface
                    ->nx_interface_ip_address);
            incoming->as_LocalScopeId = 0UL;
        }
#ifdef AMINETXDUO_IPV6
        else if (peer.nxd_ip_version == NX_IP_VERSION_V6 &&
                 incoming->as_Nx.tcp.nx_tcp_socket_ipv6_addr != NX_NULL)
        {
            const NXD_IPV6_ADDRESS *local =
                incoming->as_Nx.tcp.nx_tcp_socket_ipv6_addr;

            incoming->as_LocalAddr.nxd_ip_version = NX_IP_VERSION_V6;
            incoming->as_LocalAddr.nxd_ip_address.v6[0] =
                local->nxd_ipv6_address[0];
            incoming->as_LocalAddr.nxd_ip_address.v6[1] =
                local->nxd_ipv6_address[1];
            incoming->as_LocalAddr.nxd_ip_address.v6[2] =
                local->nxd_ipv6_address[2];
            incoming->as_LocalAddr.nxd_ip_address.v6[3] =
                local->nxd_ipv6_address[3];

            if (!bsd_addr_is_loopback(&incoming->as_LocalAddr) &&
                anx6_scope(incoming->as_LocalAddr.nxd_ip_address.v6) < 0xEU &&
                incoming->as_Nx.tcp.nx_tcp_socket_connect_interface != NX_NULL)
            {
                incoming->as_LocalScopeId =
                    (ULONG)incoming->as_Nx.tcp.nx_tcp_socket_connect_interface
                        ->nx_interface_index + 1UL;
                incoming->as_LocalScopeEpoch =
                    bsd_scope_epoch(incoming->as_LocalScopeId);
            }
            else
                incoming->as_LocalScopeId = 0UL;
        }
#endif
    }

    incoming->as_PeerAddr  = peer;
    incoming->as_PeerPort  = (UINT)peer_port;
    incoming->as_LocalPort = sock->as_ListenPort;

#ifdef AMINETXDUO_IPV6
    incoming->as_PeerScopeId  = 0UL;
    if (peer.nxd_ip_version == NX_IP_VERSION_V6 &&
        (peer.nxd_ip_address.v6[0] & 0xFFC00000UL) == 0xFE800000UL &&
        incoming->as_Nx.tcp.nx_tcp_socket_connect_interface != NX_NULL)
    {
        incoming->as_PeerScopeId =
            (ULONG)incoming->as_Nx.tcp.nx_tcp_socket_connect_interface
                ->nx_interface_index + 1UL;
        incoming->as_PeerScopeEpoch =
            bsd_scope_epoch(incoming->as_PeerScopeId);
    }
#endif

    fd = bsd_fd_alloc(SocketBase, incoming);
    if (fd < 0)
    {
        bsd_listen_hand_back(SocketBase, sock, incoming);
        incoming = NULL;
        rc = -1;
        goto out;
    }

    bsd_listen_unlink(sock, incoming);
    if (bsd_incoming_first_ready(sock) == NULL)
        sock->as_Flags &= ~ASF_ACCEPTPEND;

    (VOID)bsd_listen_rearm(SocketBase, sock);

    rc       = fd;
    accepted = TRUE;

out:
    /* The pin, inside the bracket: if it is the last reference, the listener
       is torn down here, where that is allowed. */
    bsd_socket_release(SocketBase, sock);

    bsd_nx_leave(SocketBase);

    /* Only the accepted socket's address: every failure leaves the caller's
       addr and addrlen as they were. */
    if (accepted && incoming != NULL && addr != NULL && addrlen != NULL)
        bsd_sockaddr_put(incoming, addr, addrlen, &incoming->as_PeerAddr,
                         incoming->as_PeerPort, incoming->as_PeerScopeId);

    return rc;
}

/*
 * Which source TCP is to connect from.
 * *pinned is TRUE with *index set when the connect has to name the source.
 * Zero to go ahead, -1 with the errno filed.
 */
static LONG bsd_tcp_source_check(struct AmiSocketBase *SocketBase,
                                 AmiSocket *sock, const NXD_ADDRESS *addr,
                                 ULONG scope, UINT *index, BOOL *pinned)
{
    NX_IP *ip = bsd_stack_ip(SocketBase);

    *index  = 0;
    *pinned = FALSE;

    if (ip == NULL)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    switch (bsd_source_select(sock, addr, scope, index))
    {
        case BSD_SOURCE_ROUTE:
            return 0;

        case BSD_SOURCE_INDEX:
            *pinned = TRUE;
            break;

        case BSD_SOURCE_UNREACH:
            return bsd_fail(SocketBase, AMI_ENETUNREACH);

        case BSD_SOURCE_REFUSE:
        default:
            return bsd_fail(SocketBase, AMI_EADDRNOTAVAIL);
    }

#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
    {
        NX_INTERFACE     *nxif   =
            ip->nx_ipv6_address[*index].nxd_ipv6_address_attached;

        if (nxif == NX_NULL)
            return bsd_fail(SocketBase, AMI_EADDRNOTAVAIL);

        if (!netstack_ipv6_source_find(addr->nxd_ip_address.v6,
                                       (LONG)nxif->nx_interface_index,
                                       NULL, index))
            return bsd_fail(SocketBase, AMI_ENETUNREACH);
    }
#endif

    return 0;
}

/*
 * connect()'s ephemeral port: RFC 6056 3.3.3, Algorithm 3, a keyed offset
 * per destination plus one counter, so a destination sees a port again only
 * after the whole range has gone by.  NetX picks one at random, and at tens
 * of connections a second a random port lands every few dozen connects on
 * one the peer still holds in TIME_WAIT: Linux answers that SYN with an ACK,
 * the stack resets, and connect() waits out the one-second SYN retransmit.
 * The key is HalfSipHash's, the SYN cache's hash.  NX_ANY_PORT when nothing
 * in the range is free, which leaves the failure to the bind.
 */
static UINT bsd_tcp_ephemeral_port(NX_IP *ip, const NXD_ADDRESS *addr,
                                   UINT port)
{
    static ULONG key[2];
    static ULONG next;
    ULONG        words[5];
    UINT         words_n = 0;
    UINT         found;

    if (ip == NULL)
        return NX_ANY_PORT;

    if (key[0] == 0 && key[1] == 0)
    {
        ami_random_bytes(key, sizeof(key));
        next = ami_random_ulong();
    }

#ifdef AMINETXDUO_IPV6
    if (addr->nxd_ip_version == NX_IP_VERSION_V6)
    {
        words[0] = addr->nxd_ip_address.v6[0];
        words[1] = addr->nxd_ip_address.v6[1];
        words[2] = addr->nxd_ip_address.v6[2];
        words[3] = addr->nxd_ip_address.v6[3];
        words_n  = 4;
    }
    else
#endif
        words[words_n++] = addr->nxd_ip_address.v4;
    words[words_n++] = (ULONG)port;

    found = NX_SEARCH_PORT_START +
            (UINT)((_nx_tcp_syncache_hash(key, words, words_n) + next++) %
                   ((NX_MAX_PORT + 1) - NX_SEARCH_PORT_START));

    if (nx_tcp_free_port_find(ip, found, &found) != NX_SUCCESS)
        return NX_ANY_PORT;

    return found;
}

/* The body of connect(), run inside a ThreadX context bracket. */
static LONG bsd_connect_locked(struct AmiSocketBase *SocketBase,
                               AmiSocket *sock, const NXD_ADDRESS *addr,
                               UINT port, ULONG scope)
{
    UINT status;
    UINT src_index  = 0;
    BOOL src_pinned = FALSE;

    if ((sock->as_Flags & ASF_RAW) != 0)
    {
        sock->as_PeerAddr = *addr;
        sock->as_PeerPort = port;
        sock->as_PeerScopeId = scope;
        sock->as_PeerScopeEpoch = bsd_scope_epoch(scope);
        sock->as_Flags   |= ASF_CONNECTED;

        bsd_raw_revalidate_endpoint(sock);

        return 0;
    }

    if ((sock->as_Flags & ASF_UDP) != 0)
    {
        if ((sock->as_Flags & ASF_NXBOUND) == 0)
        {
            /* An implicit ephemeral bind is never shared.  */
            sock->as_Nx.udp.nx_udp_socket_share = NX_FALSE;

            status = nx_udp_socket_bind(&sock->as_Nx.udp, NX_ANY_PORT,
                                        NX_NO_WAIT);
            if (status != NX_SUCCESS)
                return bsd_fail(SocketBase, bsd_errno_from_nx(status));

            nx_udp_socket_port_get(&sock->as_Nx.udp, &sock->as_LocalPort);
            sock->as_Flags |= ASF_NXBOUND | ASF_BOUND;
        }

        sock->as_PeerAddr = *addr;
        sock->as_PeerPort = port;
        sock->as_PeerScopeId = scope;
        sock->as_PeerScopeEpoch = bsd_scope_epoch(scope);
        sock->as_Flags   |= ASF_CONNECTED;

        if (sock->as_RxPending != NX_NULL &&
            !bsd_udp_accepts_received_packet(sock, sock->as_RxPending))
        {
            nx_packet_release(sock->as_RxPending);
            sock->as_RxPending = NX_NULL;
            sock->as_RxOffset  = 0;
        }

        return 0;
    }

    if ((sock->as_Flags & ASF_CONNECTED) != 0)
        return bsd_fail(SocketBase, AMI_EISCONN);

    if ((sock->as_Flags & ASF_LISTENING) != 0)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if ((sock->as_Flags & ASF_CONNECTING) != 0)
    {
        if (sock->as_Nx.tcp.nx_tcp_socket_state == NX_TCP_ESTABLISHED)
        {
            sock->as_Flags &= ~ASF_CONNECTING;
            sock->as_Flags |= ASF_CONNECTED;
            return 0;
        }

        return bsd_fail(SocketBase, AMI_EALREADY);
    }

    if (bsd_tcp_source_check(SocketBase, sock, addr, scope,
                             &src_index, &src_pinned) != 0)
        return -1;

    if ((sock->as_Flags & ASF_NXBOUND) == 0)
    {
        status = nx_tcp_client_socket_bind(
            &sock->as_Nx.tcp,
            (sock->as_LocalPort != 0)
                ? sock->as_LocalPort
                : bsd_tcp_ephemeral_port(bsd_stack_ip(SocketBase), addr, port),
            NX_NO_WAIT);
        if (status != NX_SUCCESS)
            return bsd_fail(SocketBase, bsd_errno_from_nx(status));

        nx_tcp_client_socket_port_get(&sock->as_Nx.tcp, &sock->as_LocalPort);
        sock->as_Flags |= ASF_NXBOUND | ASF_BOUND;
    }

    sock->as_PeerAddr = *addr;
    sock->as_PeerPort = port;
    sock->as_PeerScopeId = scope;
    sock->as_PeerScopeEpoch = bsd_scope_epoch(scope);
    sock->as_Flags   |= ASF_CONNECTING;

    /* When the SYN leaves; the establish notify reads the clock again and
       has the handshake's round trip, which is what sizes the window. */
    sock->as_ConnectMillis = ami_millis();

    status = src_pinned
                 ? nxd_tcp_client_socket_source_connect(
                       &sock->as_Nx.tcp, (NXD_ADDRESS *)addr, port, src_index,
                       NX_NO_WAIT)
                 : nxd_tcp_client_socket_connect(
                       &sock->as_Nx.tcp, (NXD_ADDRESS *)addr, port, NX_NO_WAIT);

    if (status == NX_SUCCESS)
    {
        sock->as_Flags |= ASF_CONNECTED;
        sock->as_Flags &= ~ASF_CONNECTING;
        return 0;
    }

    if (status == NX_IN_PROGRESS)
    {
        BsdConnectArgs args;
        BOOL           aborted;
        UINT           ws;

        if ((sock->as_Flags & ASF_CONNECTED) != 0)
        {
            sock->as_Flags &= ~ASF_CONNECTING;
            return 0;
        }

        if ((sock->as_Flags & ASF_CONNECTING) == 0)
        {
            if (sock->as_SoError == 0)
                sock->as_SoError = AMI_ECONNREFUSED;

            return bsd_fail(SocketBase, sock->as_SoError);
        }

        if ((sock->as_Flags & ASF_NONBLOCK) != 0)
            return bsd_fail(SocketBase, AMI_EINPROGRESS);

        args.sock = sock;
        ws = bsd_wait_sliced(SocketBase,
                             (sock->as_SndTimeout != 0) ? sock->as_SndTimeout
                                                        : NX_WAIT_FOREVER,
                             bsd_connect_once, &args, &aborted);

        if (aborted)
        {
            bsd_tcp_abort(&sock->as_Nx.tcp);
            sock->as_Flags &= ~(ASF_CONNECTING | ASF_CONNECTED);
            return bsd_fail(SocketBase, AMI_EINTR);
        }

        if (ws == NX_SUCCESS)
        {
            sock->as_Flags &= ~ASF_CONNECTING;
            sock->as_Flags |= ASF_CONNECTED;
            return 0;
        }

        if (ws == NX_NO_PACKET)
        {
            bsd_tcp_abort(&sock->as_Nx.tcp);
            sock->as_Flags &= ~(ASF_CONNECTING | ASF_CONNECTED);
            sock->as_SoError = AMI_ETIMEDOUT;
            return bsd_fail(SocketBase, AMI_ETIMEDOUT);
        }

        sock->as_Flags &= ~ASF_CONNECTING;

        if (sock->as_SoError == 0)
            sock->as_SoError = AMI_ECONNREFUSED;

        return bsd_fail(SocketBase, sock->as_SoError);
    }

    sock->as_Flags &= ~ASF_CONNECTING;

    if (status == NX_WAIT_ABORTED)
        return bsd_fail(SocketBase, AMI_EINTR);

    sock->as_SoError = (status == NX_NOT_CONNECTED)
                           ? AMI_ECONNREFUSED
                           : bsd_errno_from_nx(status);

    return bsd_fail(SocketBase, sock->as_SoError);
}

LONG bsd_connect(register LONG sock_fd          __asm("d0"),
                 register struct sockaddr *name __asm("a0"),
                 register socklen_t namelen     __asm("d1"),
                 register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket  *sock = bsd_lookup(SocketBase, sock_fd);
    NXD_ADDRESS addr;
    ULONG       scope = 0;
    UINT        port = 0;
    LONG        result;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (bsd_netmon_have(MHT_Connect))
    {
        struct ConnectMonitorMsg cmm;
        LONG                     denied;

        bsd_bzero(&cmm, sizeof(cmm));
        cmm.cmm_Size    = (LONG)sizeof(cmm);
        cmm.cmm_Caller  = bsd_netmon_caller(SocketBase);
        cmm.cmm_Socket  = sock_fd;
        cmm.cmm_Name    = name;
        cmm.cmm_NameLen = (LONG)namelen;

        denied = bsd_netmon_dispatch(MHT_Connect, &cmm);
        if (denied > 0)
            return bsd_fail(SocketBase, denied);
    }

    if ((sock->as_Flags & ASF_TCP) == 0 &&
        bsd_sa_family(name, namelen) == AF_UNSPEC)
    {
        ULONG version = sock->as_PeerAddr.nxd_ip_version;

        if (bsd_nx_enter(SocketBase) != 0)
            return bsd_fail(SocketBase, AMI_ENETDOWN);

        bsd_bzero(&sock->as_PeerAddr, sizeof(sock->as_PeerAddr));
        sock->as_PeerAddr.nxd_ip_version = version;
        sock->as_PeerPort = 0;
        sock->as_PeerScopeId = 0UL;
        sock->as_Flags   &= ~ASF_CONNECTED;

        bsd_nx_leave(SocketBase);

        return 0;
    }

    if (bsd_sockaddr_get(SocketBase, name, namelen, &addr, &port, &scope) != 0)
        return -1;

#ifdef AMINETXDUO_IPV6
    if ((sock->as_Flags & ASF_INET6) != 0)
    {
        if (addr.nxd_ip_version != NX_IP_VERSION_V6)
            return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);

        if (!bsd_addr_normalise(sock, &addr))
            return bsd_fail(SocketBase, AMI_ENETUNREACH);

    }
    else if (addr.nxd_ip_version == NX_IP_VERSION_V6)
    {
        return bsd_fail(SocketBase, AMI_EAFNOSUPPORT);
    }
#endif

    if (bsd_nx_enter(SocketBase) != 0)
        return bsd_fail(SocketBase, AMI_ENETDOWN);

    result = bsd_connect_locked(SocketBase, sock, &addr, port, scope);

    bsd_nx_leave(SocketBase);

    return result;
}

LONG bsd_shutdown(register LONG sock_fd __asm("d0"),
                  register LONG how     __asm("d1"),
                  register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock = bsd_lookup(SocketBase, sock_fd);

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (how < 0 || how > 2)
        return bsd_fail(SocketBase, AMI_EINVAL);

    if ((sock->as_Flags & ASF_CONNECTED) == 0)
        return bsd_fail(SocketBase, AMI_ENOTCONN);

    if (how == 0 || how == 2)
        sock->as_Flags |= ASF_RDSHUT;

    if (how == 1 || how == 2)
    {
        sock->as_Flags |= ASF_WRSHUT;

        if ((sock->as_Flags & (ASF_TCP | ASF_CONNECTED)) ==
            (ASF_TCP | ASF_CONNECTED))
        {
            if (bsd_nx_enter(SocketBase) != 0)
                return bsd_fail(SocketBase, AMI_ENETDOWN);

#ifdef AMINETXDUO_TCP_CORK
            /* Behind what is corked, if the window will not take it now. */
            if (!bsd_cork_shut_write(sock))
#endif
            bsd_tcp_send_fin(sock);

            bsd_nx_leave(SocketBase);
        }
    }

    return 0;
}

LONG bsd_CloseSocket(register LONG sock_fd __asm("d0"),
                     register struct AmiSocketBase *SocketBase __asm("a6"))
{
    AmiSocket *sock;

    if (bsd_fd_reserved(SocketBase, sock_fd))
    {
        if (bsd_fd_free(SocketBase, sock_fd) != 0)
            return -1;
        return 0;
    }

    if (bsd_lookup(SocketBase, sock_fd) == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    /* What the free took, not what the lookup saw: another task on this base
       may have closed the descriptor in between (N-087). */
    if (bsd_fd_take(SocketBase, sock_fd, &sock) != 0)
        return -1;

    if (sock == NULL)
        return bsd_fail(SocketBase, AMI_EBADF);

    if (sock == BSD_FD_RESERVED)
        return 0;

    if (bsd_nx_enter(SocketBase) == 0)
    {
        bsd_socket_release(SocketBase, sock);

        bsd_defer_sweep(SocketBase);
        bsd_closing_sweep();

        bsd_nx_leave(SocketBase);
    }
    else
    {
        /* No bracket, so no release now: it is owed to the next bracketed
           close.  The callbacks must not keep signalling this base, which
           can be closed and freed before they fire (F-059). */
        Forbid();
        bsd_owner_drop(SocketBase, sock);
        bsd_socket_defer(sock);
        Permit();

        AMI_WARN("bsdsocket: CloseSocket(%ld) with no ThreadX bracket. "
                 "The release waits for the next bracketed close",
                 (long)sock_fd);
    }

    return 0;
}
