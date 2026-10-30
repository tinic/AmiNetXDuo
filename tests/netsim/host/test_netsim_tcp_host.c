/*
 * AmiNetXDuo, TCP end to end over a simulated link: throughput and loss
 * recovery, two whole NetX Duo stacks, no hardware.
 *
 * The A1200 on 2026-10-29 (#354) showed four TCP defects that every unit test
 * and review had passed: a SACK recovery retransmit storm (3.6 MB sent as
 * 14 MB behind a 150 Mbit bottleneck), RTO-floor stalls of 0.9 s, eleven or
 * twelve a run, repair slices dropped for want of an egress interface, and an
 * upload that ran at a different rate from the download.  The unit tests feed
 * hand-made acknowledgments to one socket and read counters; none of them
 * puts two stacks on a wire whose queue drops what does not fit.  This does.
 *
 * Linked for real: all of NetX Duo and ThreadX, built with the shipping
 * nx_user.h and the tree's TCP options, and src/bsdsocket/bsdsocket_window.c
 * for the window and transmit queue bsdsocket.library gives a socket on a
 * 4,096-packet pool.  ThreadX runs on shim/tx_port.h's virtual clock, so a
 * run is deterministic and takes host CPU time only (netsim.h, the wire).
 *
 * The sender makes 32 KB blocking writes, as AmiSpeedTest does, cut into MSS
 * segments and sent with bsd_send_tcp_run()'s sequence of NetX Duo calls
 * (src/bsdsocket/transfer.c) -- a packet allocated without waiting and then
 * in BSD_BREAK_SLICE_TICKS slices, the send tried without waiting and then in
 * slices.  The receiver reads in slices too and checks every byte against
 * the pattern the sender wrote.
 *
 * WHAT IT IS NOT.  A portable gate on TCP and the wire, nothing more.  It
 * reproduces those NetX Duo calls but runs none of the BSD socket layer
 * (transfer.c compiles only where a pointer is four bytes, the host32 tier),
 * SANA-II, Exec, GSO segmentation or batched transmit, and the CPU is
 * infinitely fast.  It cannot stand in for hardware, driver or bsdsocket
 * coverage, and says nothing about full speed or audio and interrupt
 * latency.
 *
 * Side 0 is the Amiga.  Side 1 is the peer it transferred with, a Linux
 * host, which acknowledges every second segment; NetX Duo's own receive ramp
 * stretches its acknowledgments towards NX_TCP_ACK_THRESHOLD_MAX, and the
 * slow start at the other end counts acknowledgments rather than bytes, so
 * two NetX Duo stacks at their defaults take a second to fill 375 KB.
 * Goodput is therefore measured over the second half of the data, once slow
 * start is behind it.  A stall anywhere in the run is the receiver gap's
 * to catch: the longest wait between advances of the in-order data the
 * application reads.  The sender's longest silence is printed as tx_gap.
 *
 * WHAT IT CAUGHT, 2026-10-29, the bottleneck scenario against three NetX Duo
 * trees (AMINETXDUO_NETSIM_NETXDUO, tests/netsim/CMakeLists.txt):
 *
 *     NetX Duo    what                              goodput  TX gap  tx ratio
 *     eeda05d8    main: retransmissions not in      80.0%    32 ms   1.015
 *                 the SACK pipe
 *     eed79540    PRR; repair slices lose their     92.3%    945 ms  1.043
 *                 egress interface
 *     a2e205fb    egress kept                       92.5%    28 ms   1.025
 *
 * and shallow (64 KB): main 71.1%, 998 ms, 1.115; a2e205fb and d4d683ce
 * 80.6%, 20 ms, 1.001.  With 150 KB both fixed cores stall 913 ms on a
 * repair retransmission the queue dropped again (NETSIM_TRACE).
 *
 * One scenario per ctest case, named on the command line.  Output is
 * key=value, and the exit code is the verdict.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netsim.h"
#include "thread_priorities.h"
#include "aminetxduo/pool.h"
#include "bsdsocket_window.h"

/* The pool a 4,096-packet machine carves (pool.h, the clamp): the A1200. */
#define NS_POOL_PACKETS         AMI_POOL_MAX_PACKETS
#define NS_PAYLOAD              AMI_POOL_PAYLOAD

/* The gigabit card the A1200 transfers ran over (anxgenet.device). */
#define NS_CARD_BPS             1000000000UL

/* bsdsocket's write and read size, and its wait slice (select.c). */
#define NS_WRITE                32768UL
#define NS_SLICE                10UL

/* bsdsocket_internal.h, the receive queue cap per window. */
#define NS_RX_MSS_REF           1200UL
#define NS_RX_QUEUE_SLACK       16UL

#define NS_MS                   1000000ULL

/* The peer's acknowledgment spacing: two full segments. */
#define NS_PEER_ACK_BYTES       (2UL * 1460UL)

typedef struct NsScenario
{
    const char         *name;
    NetSimPathConfig    fwd;        /* towards side 1                      */
    NetSimPathConfig    rev;        /* towards side 0                      */
    ULONG               bytes;
    UINT                reverse;    /* also run side 1 to side 0, compare  */
    ULONG               goodput_floor;  /* permille of the rate, 0: not   */
    ULONG64             gap_limit_ns;   /* 0: not asserted                */
    ULONG               ratio_limit;    /* permille of the payload, 0: not */
} NsScenario;

/* 1% random loss at 100 Mbit/s and 20 ms: Mathis et al. (1997) put a Reno
   flow at MSS / RTT * sqrt(3/2) / sqrt(p) = 1460 * 8 / 0.020 * 1.2247 / 0.1
   = 7.15 Mbit/s, 7.15% of the link.  SACK recovery should do no worse; the
   floor is half that, 3.5%, so the margin is for the 20 ms tick and the
   loss draw, and a run under it is a recovery that stalled, not one that
   was unlucky. */
#define NS_MATHIS_FLOOR_PERMILLE    35

static const NsScenario ns_scenarios[] =
{
    { "clean",
      { 150000000ULL, 10 * NS_MS, 0, 0 },
      { 150000000ULL, 10 * NS_MS, 0, 0 },
      32UL * 1024UL * 1024UL, 0, 900, 500 * NS_MS, 0 },

    /* One bandwidth-delay product of tail-drop queue: 150 Mbit/s x 20 ms. */
    { "bottleneck",
      { 150000000ULL, 10 * NS_MS, 375000, 0 },
      { 150000000ULL, 10 * NS_MS, 0, 0 },
      32UL * 1024UL * 1024UL, 0, 850, 500 * NS_MS, 1050 },

    { "loss",
      { 100000000ULL, 10 * NS_MS, 0, 10000 },
      { 100000000ULL, 10 * NS_MS, 0, 0 },
      4UL * 1024UL * 1024UL, 0, NS_MATHIS_FLOOR_PERMILLE, 0, 0 },

    /* The clean link both ways round: side 0 connects and sends, then side
       0 connects and side 1 sends.  Upload and download must agree. */
    { "reverse",
      { 150000000ULL, 10 * NS_MS, 0, 0 },
      { 150000000ULL, 10 * NS_MS, 0, 0 },
      32UL * 1024UL * 1024UL, 1, 900, 500 * NS_MS, 0 },

    /* A shallow queue, 64 KB: wanemu's default and an ISP uplink's.  The
       bottleneck's gap and ratio; goodput is printed only.  Its 80.6% is an
       open limit: Reno growth after the slow-start loss, which faster window
       growth would likely lift; no fix is proven yet. */
    { "shallow",
      { 150000000ULL, 10 * NS_MS, 65536, 0 },
      { 150000000ULL, 10 * NS_MS, 0, 0 },
      32UL * 1024UL * 1024UL, 0, 0, 500 * NS_MS, 1050 },
};

/* Slow start, printed and not yet asserted: two NetX Duo stacks at their
   defaults ramp at one segment per acknowledgment (RFC 5681's min(N, SMSS))
   while the receive ramp stretches the acknowledgments. */
#define NS_SS_BYTES             375000UL

/* One end of a connection. */
typedef struct NsEnd
{
    NX_TCP_SOCKET   tcp;
    UINT            side;
    UINT            active;         /* connects, rather than accepts       */
    UINT            sends;
    ULONG64         connect_ns;
    ULONG           settled;        /* the window the establish settled    */
    UINT            failed;
    const char     *why;
} NsEnd;

/* One transfer and what it measured. */
typedef struct NsPhase
{
    UINT            from;           /* sending side                        */
    UINT            port;
    ULONG           bytes;
    ULONG           got;
    UINT            corrupt;
    ULONG           excess;         /* bytes past the end: corruption too  */
    ULONG64         rx_gap_ns;      /* longest wait between advances of    */
    ULONG64         rx_gap_at_ns;   /* the in-order bytes the receiver has */
    ULONG64         half_ns;        /* half delivered                      */
    ULONG64         done_ns;
    ULONG64         ss_ns;          /* NS_SS_BYTES delivered               */
    ULONG           retransmits;    /* the sender's NetX Duo count         */
    NsEnd           end[NETSIM_SIDES];
    TX_SEMAPHORE    finished;
} NsPhase;

static const NsScenario *ns_sc;
static NsScenario        ns_run;
static NX_IP            ns_ip[NETSIM_SIDES];
static NX_PACKET_POOL   ns_pool[NETSIM_SIDES];
static UCHAR            ns_ip_stack[NETSIM_SIDES][4096];
static TX_THREAD        ns_control;
static UCHAR            ns_control_stack[4096];
static TX_THREAD        ns_worker[NETSIM_SIDES];
static UCHAR            ns_worker_stack[NETSIM_SIDES][4096];
static NsPhase          ns_phase;

static UCHAR ns_pattern(ULONG offset)
{
    return (UCHAR)((offset * 7UL) ^ (offset >> 9));
}

/* ------------------------------------------------- bsdsocket's socket -- */

/* bsd_tcp_establish_notify() and bsd_tcp_window_settle(): grow to the
   maximum on a long path, measured from the connect by the side that
   connected and by the SYN cache on the side that accepted. */
static VOID ns_establish_notify(NX_TCP_SOCKET *tcp)
{
    NsEnd *e   = (NsEnd *)tcp->nx_tcp_socket_reserved_ptr;
    ULONG  cur = tcp->nx_tcp_socket_rx_window_default;
    ULONG  rtt_ms, want;

    rtt_ms = e->active
           ? (ULONG)((netsim_now_ns() - e->connect_ns) / NS_MS)
           : tcp->nx_tcp_socket_handshake_rtt;

    want = ami_bsd_tcp_window_settle(cur, tcp->nx_tcp_socket_rx_window_maximum,
                                     NS_CARD_BPS, rtt_ms, 0);

    tcp->nx_tcp_socket_rx_window_cap = NX_TCP_RX_WINDOW_CAP_NONE;

    if (want > cur && (want >> tcp->nx_tcp_rcv_win_scale_value) <= 65535UL)
    {
        tcp->nx_tcp_socket_rx_window_current += want - cur;
        tcp->nx_tcp_socket_rx_window_default  = want;
    }

    e->settled = tcp->nx_tcp_socket_rx_window_default;
}

/* bsd_socket(SOCK_STREAM): the window, its maximum, the queue caps. */
static UINT ns_socket(NsEnd *e)
{
    NX_IP          *ip   = &ns_ip[e->side];
    NX_PACKET_POOL *pool = &ns_pool[e->side];
    NX_TCP_SOCKET  *tcp  = &e->tcp;
    ULONG           cap;
    UINT            status;

    status = nx_tcp_socket_create(ip, tcp, (CHAR *)"netsim", NX_IP_NORMAL,
                                  NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE,
                                  ami_bsd_tcp_window_for(pool->nx_packet_pool_total,
                                                         pool->nx_packet_pool_payload_size, 0),
                                  NX_NULL, NX_NULL);
    if (status != NX_SUCCESS)
        return status;

    tcp->nx_tcp_socket_keepalive_enabled  = NX_FALSE;
    tcp->nx_tcp_socket_rx_window_maximum  =
        ami_bsd_tcp_window_max_for(pool->nx_packet_pool_total,
                                   pool->nx_packet_pool_payload_size, 0);

    cap = tcp->nx_tcp_socket_rx_window_maximum / NS_RX_MSS_REF + NS_RX_QUEUE_SLACK;
    if (cap < NX_TCP_MAXIMUM_RX_QUEUE)
        cap = NX_TCP_MAXIMUM_RX_QUEUE;
    tcp->nx_tcp_socket_receive_queue_maximum = cap;

    tcp->nx_tcp_socket_transmit_queue_maximum =
        ami_bsd_tcp_tx_queue_for(pool->nx_packet_pool_total,
                                 (ULONG)NX_TCP_MAXIMUM_TX_QUEUE);
    tcp->nx_tcp_socket_transmit_queue_maximum_default =
        tcp->nx_tcp_socket_transmit_queue_maximum;

    tcp->nx_tcp_socket_reserved_ptr = e;

    return nx_tcp_socket_establish_notify(tcp, ns_establish_notify);
}

/* ------------------------------------------------------- the transfer -- */

static VOID ns_fail(NsEnd *e, const char *why)
{
    if (!e->failed)
    {
        e->failed = 1;
        e->why    = why;
    }
}

/* One blocking write, as bsd_send_tcp_run() makes it. */
static UINT ns_write(NsEnd *e, ULONG offset, ULONG len)
{
    NX_PACKET_POOL *pool = &ns_pool[e->side];
    UCHAR           buf[NS_PAYLOAD];
    ULONG           mss  = 0;
    ULONG           sent = 0;

    nx_tcp_socket_mss_get(&e->tcp, &mss);
    if (mss == 0 || mss > sizeof(buf))
        mss = 536;

    while (sent < len)
    {
        NX_PACKET *packet = NX_NULL;
        ULONG      chunk  = len - sent;
        ULONG      i;
        UINT       status;

        if (chunk > mss)
            chunk = mss;

        status = nx_packet_allocate(pool, &packet, NX_TCP_PACKET, NX_NO_WAIT);
        while (status == NX_NO_PACKET)
            status = nx_packet_allocate(pool, &packet, NX_TCP_PACKET, NS_SLICE);
        if (status != NX_SUCCESS)
            return status;

        for (i = 0; i < chunk; i++)
            buf[i] = ns_pattern(offset + sent + i);

        status = nx_packet_data_append(packet, buf, chunk, pool, NS_SLICE);
        if (status != NX_SUCCESS)
        {
            nx_packet_release(packet);
            return status;
        }

        status = nx_tcp_socket_send(&e->tcp, packet, NX_NO_WAIT);
        while (status == NX_WINDOW_OVERFLOW || status == NX_TX_QUEUE_DEPTH ||
               status == NX_NO_PACKET)
            status = nx_tcp_socket_send(&e->tcp, packet, NS_SLICE);
        if (status != NX_SUCCESS)
        {
            nx_packet_release(packet);
            return status;
        }

        sent += chunk;
    }

    return NX_SUCCESS;
}

static VOID ns_send_all(NsPhase *ph, NsEnd *e)
{
    ULONG offset = 0;

    netsim_sender_set(e->side, &e->tcp);

    while (offset < ph->bytes)
    {
        ULONG len = ph->bytes - offset;

        if (len > NS_WRITE)
            len = NS_WRITE;

        if (ns_write(e, offset, len) != NX_SUCCESS)
        {
            ns_fail(e, "send");
            return;
        }
        offset += len;
    }

#ifndef NX_DISABLE_TCP_INFO
    ph->retransmits = e->tcp.nx_tcp_socket_retransmit_packets;
#endif
}

/* nx_tcp_socket_receive() hands over in-order data only, so the time
   between two deliveries is the time the connection made no progress --
   what a stall is, wherever in the path it came from. */
static VOID ns_receive_all(NsPhase *ph, NsEnd *e)
{
    ULONG64 last = netsim_now_ns();

    while (ph->got < ph->bytes)
    {
        NX_PACKET *packet;
        NX_PACKET *seg;
        UINT       status;
        ULONG64    now;

        status = nx_tcp_socket_receive(&e->tcp, &packet, NS_SLICE);
        if (status == NX_NO_PACKET)
            continue;
        if (status != NX_SUCCESS)
        {
            ns_fail(e, "receive");
            return;
        }

        for (seg = packet; seg != NX_NULL; seg = seg->nx_packet_next)
        {
            UCHAR *p = seg->nx_packet_prepend_ptr;

            while (p < seg->nx_packet_append_ptr)
            {
                if (ph->got >= ph->bytes)
                {
                    ph->excess  += (ULONG)(seg->nx_packet_append_ptr - p);
                    ph->corrupt  = 1;
                    break;
                }
                if (*p++ != ns_pattern(ph->got))
                    ph->corrupt = 1;
                ph->got++;
            }
        }
        nx_packet_release(packet);

        now = netsim_now_ns();
        if (now - last > ph->rx_gap_ns)
        {
            ph->rx_gap_ns    = now - last;
            ph->rx_gap_at_ns = now;
        }
        last = now;

        if (ph->ss_ns == 0 && ph->got >= NS_SS_BYTES)
            ph->ss_ns = netsim_now_ns();
        if (ph->half_ns == 0 && ph->got >= ph->bytes / 2)
            ph->half_ns = netsim_now_ns();
    }

    ph->done_ns = netsim_now_ns();
}

static VOID ns_worker_entry(ULONG side)
{
    NsPhase *ph = &ns_phase;
    NsEnd   *e  = &ph->end[side];
    UINT     status;

    if (ns_socket(e) != NX_SUCCESS)
    {
        ns_fail(e, "socket");
        tx_semaphore_put(&ph->finished);
        return;
    }

    if (e->active)
    {
        e->connect_ns = netsim_now_ns();
        status = nx_tcp_client_socket_bind(&e->tcp, NX_ANY_PORT, NX_WAIT_FOREVER);
        if (status == NX_SUCCESS)
            status = nx_tcp_client_socket_connect(&e->tcp,
                         ns_ip[side ^ 1U].nx_ip_interface[0].nx_interface_ip_address,
                         ph->port, NX_WAIT_FOREVER);
    }
    else
    {
        status = nx_tcp_server_socket_listen(&ns_ip[side], ph->port, &e->tcp, 4,
                                             NX_NULL);
        if (status == NX_SUCCESS)
            status = nx_tcp_server_socket_accept(&e->tcp, NX_WAIT_FOREVER);
    }

    if (status != NX_SUCCESS)
        ns_fail(e, "connect");
    else if (e->sends)
        ns_send_all(ph, e);
    else
        ns_receive_all(ph, e);

    tx_semaphore_put(&ph->finished);
}

/* ------------------------------------------------------- the verdict -- */

static UINT ns_checks_failed;

static VOID ns_check(int ok, const char *phase, const char *what)
{
    if (!ok)
    {
        ns_checks_failed++;
        printf("check_fail=%s phase=%s\n", what, phase);
    }
}

/* Goodput in thousandths of the link rate.  The verdict reads it from half
   the data delivered to all of it; the whole run, from the first segment,
   is printed beside it. */
static ULONG ns_permille(ULONG64 bytes, ULONG64 ns, ULONG64 rate)
{
    if (ns == 0 || rate == 0)
        return 0;

    return (ULONG)((double)bytes * 8.0e12 / (double)ns / (double)rate);
}

static ULONG ns_run_phase(const char *label, UINT from, UINT port)
{
    NsPhase         *ph   = &ns_phase;
    const NetSimPathConfig *path = (from == 0) ? &ns_sc->fwd : &ns_sc->rev;
    NetSimPathStats *st;
    ULONG64          start, steady_ns, all_ns;
    ULONG            steady, all, ratio;
    UINT             side;

    memset(ph, 0, sizeof(*ph));
    ph->from  = from;
    ph->port  = port;
    ph->bytes = ns_sc->bytes;
    tx_semaphore_create(&ph->finished, (CHAR *)"netsim phase", 0);

    for (side = 0; side < NETSIM_SIDES; side++)
    {
        ph->end[side].side   = side;
        ph->end[side].active = (side == 0);
        ph->end[side].sends  = (side == from);
    }

    netsim_stats_reset();
    start = netsim_now_ns();
    netsim_deadline_set(start + 60000ULL * NS_MS);

    for (side = 0; side < NETSIM_SIDES; side++)
        tx_thread_create(&ns_worker[side], (CHAR *)"netsim app", ns_worker_entry,
                         side, ns_worker_stack[side], sizeof(ns_worker_stack[side]),
                         AMI_CALLER_PRIORITY, AMI_CALLER_PRIORITY,
                         TX_NO_TIME_SLICE, TX_AUTO_START);

    tx_semaphore_get(&ph->finished, TX_WAIT_FOREVER);
    tx_semaphore_get(&ph->finished, TX_WAIT_FOREVER);
    netsim_sender_set(from, NX_NULL);

    st        = netsim_stats_towards(from ^ 1U);
    steady_ns = ph->done_ns - ph->half_ns;
    all_ns    = ph->done_ns - st->first_data_ns;
    steady    = ns_permille(ph->bytes - ph->bytes / 2, steady_ns, path->rate_bps);
    all       = ns_permille(ph->bytes, all_ns, path->rate_bps);
    ratio     = (ULONG)(st->data_bytes * 1000ULL / ph->bytes);

    printf("phase=%s from=%u bytes=%lu delivered=%lu data_ok=%u"
           " goodput_permille=%lu goodput_all_permille=%lu"
           " sent_bytes=%llu retx_bytes=%llu tx_ratio_permille=%lu"
           " rx_gap_ms=%llu rx_gap_at_ms=%llu tx_gap_ms=%llu tx_gap_at_ms=%llu"
           " excess_bytes=%lu"
           " drops_queue=%lu drops_loss=%lu peak_queue=%lu rx_nobuf=%lu"
           " nx_retransmits=%lu window=%lu elapsed_ms=%llu\n",
           label, from, (unsigned long)ph->bytes, (unsigned long)ph->got,
           (unsigned)(!ph->corrupt), (unsigned long)steady, (unsigned long)all,
           (unsigned long long)st->data_bytes, (unsigned long long)st->retx_bytes,
           (unsigned long)ratio,
           (unsigned long long)(ph->rx_gap_ns / NS_MS),
           (unsigned long long)((ph->rx_gap_at_ns - start) / NS_MS),
           (unsigned long long)(st->max_gap_ns / NS_MS),
           (unsigned long long)((st->max_gap_at_ns - start) / NS_MS),
           (unsigned long)ph->excess,
           (unsigned long)st->drops_queue, (unsigned long)st->drops_loss,
           (unsigned long)st->peak_queue, (unsigned long)netsim_rx_nobuf(from ^ 1U),
           (unsigned long)ph->retransmits,
           (unsigned long)ph->end[from ^ 1U].settled,
           (unsigned long long)((ph->done_ns - start) / NS_MS));

    {
        ULONG acks = netsim_stats_towards(from)->acks;

        printf("slowstart phase=%s ss_375k_ms=%llu cwnd_50ms=%lu"
               " data_segments=%lu acks=%lu segs_per_ack_x100=%lu\n",
               label,
               (unsigned long long)(ph->ss_ns ? (ph->ss_ns - st->first_data_ns) / NS_MS : 0),
               (unsigned long)st->cwnd_50ms, (unsigned long)st->data_frames,
               (unsigned long)acks,
               (unsigned long)(acks ? st->data_frames * 100UL / acks : 0));
    }

    for (side = 0; side < NETSIM_SIDES; side++)
    {
        if (ph->end[side].failed)
            printf("end_fail=%s side=%u phase=%s\n", ph->end[side].why, side, label);
        ns_check(!ph->end[side].failed, label, "connection");
    }

    ns_check(ph->got == ph->bytes && !ph->corrupt, label, "data");

    if (ns_sc->goodput_floor != 0)
        ns_check(steady >= ns_sc->goodput_floor, label, "goodput");
    if (ns_sc->gap_limit_ns != 0)
        ns_check(ph->rx_gap_ns < ns_sc->gap_limit_ns, label, "gap");
    if (ns_sc->ratio_limit != 0)
        ns_check(ratio <= ns_sc->ratio_limit, label, "tx_ratio");

    /* Both connections closed before the next phase, so it starts from an
       idle stack. */
    for (side = 0; side < NETSIM_SIDES; side++)
    {
        NsEnd *e = &ph->end[side];

        tx_thread_terminate(&ns_worker[side]);
        tx_thread_delete(&ns_worker[side]);
        nx_tcp_socket_disconnect(&e->tcp, NX_NO_WAIT);
        if (e->active)
            nx_tcp_client_socket_unbind(&e->tcp);
        else
        {
            nx_tcp_server_socket_unaccept(&e->tcp);
            nx_tcp_server_socket_unlisten(&ns_ip[side], port);
        }
        nx_tcp_socket_delete(&e->tcp);
    }
    tx_semaphore_delete(&ph->finished);

    return steady;
}

static VOID ns_control_entry(ULONG unused)
{
    ULONG up, down;

    (VOID)unused;

    printf("scenario=%s rate_bps=%llu delay_ms=%llu queue_bytes=%lu loss_ppm=%lu"
           " payload=%lu write=%lu tick_hz=%lu\n",
           ns_sc->name, (unsigned long long)ns_sc->fwd.rate_bps,
           (unsigned long long)(ns_sc->fwd.delay_ns / NS_MS),
           (unsigned long)ns_sc->fwd.queue_bytes, (unsigned long)ns_sc->fwd.loss_ppm,
           (unsigned long)ns_sc->bytes, (unsigned long)NS_WRITE,
           (unsigned long)TX_TIMER_TICKS_PER_SECOND);

    up = ns_run_phase("upload", 0, 5001);

    if (ns_sc->reverse)
    {
        ULONG hi, lo;

        down = ns_run_phase("download", 1, 5002);
        hi   = (up > down) ? up : down;
        lo   = (up > down) ? down : up;

        printf("symmetry_permille=%lu\n",
               (unsigned long)(hi ? lo * 1000UL / hi : 0));
        ns_check(lo * 100UL >= hi * 95UL, "both", "symmetry");
    }

    printf("result=%s checks_failed=%u sim_ms=%llu\n",
           ns_checks_failed ? "FAIL" : "PASS", ns_checks_failed,
           (unsigned long long)(netsim_now_ns() / NS_MS));
    fflush(stdout);

    exit(ns_checks_failed ? 1 : 0);
}

/* ------------------------------------------------------------ bring-up -- */

static VOID ns_stack(UINT side, ULONG address)
{
    ULONG  bytes = NS_POOL_PACKETS *
                   (ULONG)((sizeof(NX_PACKET) + NX_PACKET_ALIGNMENT - 1 + NS_PAYLOAD + 15) & ~15UL);
    VOID  *memory = malloc(bytes);
    UINT   status;

    if (memory == NULL)
    {
        printf("netsim=FAIL reason=host_out_of_memory\n");
        exit(1);
    }

    status = nx_packet_pool_create(&ns_pool[side], (CHAR *)"netsim", NS_PAYLOAD,
                                   memory, bytes);
    if (status == NX_SUCCESS)
        status = nx_ip_create(&ns_ip[side], (CHAR *)"netsim", address,
                              0xFFFFFF00UL, &ns_pool[side],
                              side ? netsim_driver_1 : netsim_driver_0,
                              ns_ip_stack[side], sizeof(ns_ip_stack[side]),
                              AMI_IP_THREAD_PRIORITY);
    if (status == NX_SUCCESS)
        status = nx_tcp_enable(&ns_ip[side]);

    /* Side 1 is the peer the A1200 transferred with, a Linux host: it
       acknowledges every second full segment (RFC 1122 4.2.3.2), where
       NetX Duo's receive ramp would stretch to the port's ceiling. */
    if (status == NX_SUCCESS && side == 1)
        ns_ip[side].nx_ip_interface[0].nx_interface_tcp_ack_threshold_max =
            NS_PEER_ACK_BYTES;
    if (status == NX_SUCCESS)
        status = netsim_receiver_start(side, &ns_ip[side]);

    if (status != NX_SUCCESS)
    {
        printf("netsim=FAIL reason=bring_up side=%u status=0x%x\n", side, status);
        exit(1);
    }
}

VOID tx_application_define(VOID *first_unused_memory)
{
    (VOID)first_unused_memory;

    nx_system_initialize();

    ns_stack(0, 0x0A000001UL);
    ns_stack(1, 0x0A000002UL);

    tx_thread_create(&ns_control, (CHAR *)"netsim control", ns_control_entry, 0,
                     ns_control_stack, sizeof(ns_control_stack),
                     AMI_CALLER_PRIORITY, AMI_CALLER_PRIORITY,
                     TX_NO_TIME_SLICE, TX_AUTO_START);
}

int main(int argc, char **argv)
{
    size_t i;

    if (argc == 2 || argc == 3)
    {
        for (i = 0; i < sizeof(ns_scenarios) / sizeof(ns_scenarios[0]); i++)
        {
            if (strcmp(argv[1], ns_scenarios[i].name) == 0)
                ns_sc = &ns_scenarios[i];
        }
    }

    if (ns_sc == NULL)
    {
        printf("netsim=FAIL reason=usage"
               " want=clean|bottleneck|loss|reverse|shallow [queue_bytes]\n");
        return 2;
    }

    /* A second argument replaces the forward queue, for probing the depth
       at which a scenario breaks; ctest never passes one. */
    ns_run = *ns_sc;
    if (argc == 3)
        ns_run.fwd.queue_bytes = (ULONG)strtoul(argv[2], NULL, 10);
    ns_sc = &ns_run;

    /* NetX Duo's initial sequence numbers come from NX_RAND. */
    srand(1);

    netsim_configure(&ns_sc->fwd, &ns_sc->rev, 0x354ULL);
    tx_kernel_enter();

    return 1;
}
