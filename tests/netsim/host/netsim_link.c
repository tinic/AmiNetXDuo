/*
 * AmiNetXDuo, host network simulator: the wire, the drivers and the clock.
 * netsim.h says what is modelled.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nx_api.h"
#include "nx_ip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netsim.h"
#include "thread_priorities.h"

typedef struct NetSimFrame
{
    struct NetSimFrame *next;
    ULONG64             depart_ns;      /* last bit serialised            */
    ULONG64             arrive_ns;
    ULONG               len;
    UINT                lost;
    UCHAR               data[1];
} NetSimFrame;

typedef struct NetSimPath
{
    NetSimPathConfig    cfg;
    NetSimPathStats     st;
    ULONG64             busy_until;     /* serialiser free from           */
    NetSimFrame        *head;           /* in flight, in arrival order    */
    NetSimFrame        *tail;
    NetSimFrame        *queued;         /* first not yet serialised       */
    ULONG               backlog;        /* bytes from `queued` on         */
} NetSimPath;

typedef struct NetSimSide
{
    NX_IP              *ip;
    TX_THREAD           rx_thread;
    TX_SEMAPHORE        rx_wake;
    NetSimFrame        *inbox;
    NetSimFrame        *inbox_tail;
    ULONG               rx_nobuf;
    UCHAR               rx_stack[4096];
} NetSimSide;

static NetSimPath   ns_path[NETSIM_SIDES];  /* [n]: towards side n        */
static NetSimSide   ns_side[NETSIM_SIDES];
static ULONG64      ns_now;
static ULONG64      ns_next_tick = NETSIM_TICK_NS;
static ULONG64      ns_deadline  = ~0ULL;
static ULONG64      ns_rng;

ULONG64 netsim_now_ns(VOID)
{
    return ns_now;
}

VOID netsim_deadline_set(ULONG64 at_ns)
{
    ns_deadline = at_ns;
}

NetSimPathStats *netsim_stats_towards(UINT side)
{
    return &ns_path[side].st;
}

VOID netsim_stats_reset(VOID)
{
    memset(&ns_path[0].st, 0, sizeof(ns_path[0].st));
    memset(&ns_path[1].st, 0, sizeof(ns_path[1].st));
}

ULONG netsim_rx_nobuf(UINT side)
{
    return ns_side[side].rx_nobuf;
}

/* The SYN cache's handshake clock (nx_user.h, NX_TCP_SYNCACHE_CLOCK). */
ULONG _nx_amiga_handshake_millis(VOID)
{
    return (ULONG)(ns_now / 1000000ULL);
}

VOID netsim_configure(const NetSimPathConfig *to_1, const NetSimPathConfig *to_0,
                      ULONG64 seed)
{
    ns_path[1].cfg = *to_1;
    ns_path[0].cfg = *to_0;
    ns_rng = seed ? seed : 1;
}

/* xorshift64*: the same draws on every host. */
static ULONG netsim_draw_ppm(VOID)
{
    ns_rng ^= ns_rng >> 12;
    ns_rng ^= ns_rng << 25;
    ns_rng ^= ns_rng >> 27;

    return (ULONG)(((ns_rng * 0x2545F4914F6CDD1DULL) >> 32) % 1000000UL);
}

/* Payload and sequence of a TCP segment, from its IPv4 header on. */
static UINT netsim_tcp_data(const UCHAR *d, ULONG len, ULONG *seq, ULONG *bytes)
{
    ULONG ihl, total, doff;

    if (len < 40 || (d[0] >> 4) != 4 || d[9] != NX_PROTOCOL_TCP)
        return NX_FALSE;

    ihl   = (ULONG)(d[0] & 0x0F) * 4UL;
    total = ((ULONG)d[2] << 8) | d[3];
    doff  = (ULONG)(d[ihl + 12] >> 4) * 4UL;

    if (total > len || ihl + doff > total)
        return NX_FALSE;

    *seq   = ((ULONG)d[ihl + 4] << 24) | ((ULONG)d[ihl + 5] << 16) |
             ((ULONG)d[ihl + 6] << 8)  |  (ULONG)d[ihl + 7];
    *bytes = total - ihl - doff;

    return *bytes != 0;
}

static VOID netsim_account(NetSimPath *p, const NetSimFrame *f)
{
    NetSimPathStats *st = &p->st;
    ULONG seq, bytes, end;

    if (!netsim_tcp_data(f->data, f->len, &seq, &bytes))
        return;

    end = seq + bytes;

    if (!st->seen)
    {
        st->seen          = 1;
        st->high_seq      = end;
        st->first_data_ns = ns_now;
    }
    else
    {
        ULONG64 gap = ns_now - st->last_data_ns;

        if (gap > st->max_gap_ns)
        {
            st->max_gap_ns    = gap;
            st->max_gap_at_ns = ns_now;
        }

        if ((LONG)(end - st->high_seq) <= 0)
            st->retx_bytes += bytes;
        else
        {
            if ((LONG)(seq - st->high_seq) < 0)
                st->retx_bytes += st->high_seq - seq;
            st->high_seq = end;
        }
    }

    st->data_bytes   += bytes;
    st->last_data_ns  = ns_now;
}

/* NETSIM_TRACE=1: every TCP segment as it is handed to the wire, for
   reading a failing run.  Off, it costs one test per frame. */
static int ns_trace = -1;

static VOID netsim_trace(UINT from, const NetSimFrame *f)
{
    const UCHAR *d = f->data;
    ULONG        ihl, total;

    if (ns_trace < 0)
        ns_trace = (getenv("NETSIM_TRACE") != NULL);
    if (!ns_trace || f->len < 40 || d[9] != NX_PROTOCOL_TCP)
        return;

    ihl   = (ULONG)(d[0] & 0x0F) * 4UL;
    total = ((ULONG)d[2] << 8) | d[3];
    d    += ihl;

    printf("seg t_us=%llu from=%u seq=%lu ack=%lu len=%lu win=%u flags=0x%02x\n",
           (unsigned long long)(ns_now / 1000ULL), from,
           (unsigned long)(((ULONG)d[4] << 24) | ((ULONG)d[5] << 16) |
                           ((ULONG)d[6] << 8) | d[7]),
           (unsigned long)(((ULONG)d[8] << 24) | ((ULONG)d[9] << 16) |
                           ((ULONG)d[10] << 8) | d[11]),
           (unsigned long)(total - ihl - (ULONG)(d[12] >> 4) * 4UL),
           ((UINT)d[14] << 8) | d[15], d[13]);
}

/* A datagram from `from`'s driver, onto the path towards the other side. */
static VOID netsim_transmit(UINT from, NX_PACKET *packet)
{
    NetSimPath  *p = &ns_path[from ^ 1U];
    NetSimFrame *f;
    NX_PACKET   *seg;
    ULONG        len = packet->nx_packet_length;
    ULONG        at  = 0;
    ULONG64      start, ser;

    f = (NetSimFrame *)malloc(sizeof(*f) + len);
    if (f == NULL)
    {
        printf("netsim=FAIL reason=host_out_of_memory\n");
        exit(1);
    }

    for (seg = packet; seg != NX_NULL && at < len; seg = seg->nx_packet_next)
    {
        ULONG chunk = (ULONG)(seg->nx_packet_append_ptr - seg->nx_packet_prepend_ptr);

        if (chunk > len - at)
            chunk = len - at;
        memcpy(f->data + at, seg->nx_packet_prepend_ptr, chunk);
        at += chunk;
    }

    f->len  = len;
    f->next = NULL;
    f->lost = 0;

    p->st.frames++;
    netsim_account(p, f);
    netsim_trace(from, f);

    /* What has been serialised by now has left the queue. */
    while (p->queued != NULL && p->queued->depart_ns <= ns_now)
    {
        p->backlog -= p->queued->len + NETSIM_WIRE_OVERHEAD;
        p->queued   = p->queued->next;
    }

    if (p->cfg.queue_bytes != 0 &&
        p->backlog + len + NETSIM_WIRE_OVERHEAD > p->cfg.queue_bytes)
    {
        p->st.drops_queue++;
        free(f);
        return;
    }

    start = (p->busy_until > ns_now) ? p->busy_until : ns_now;
    ser   = ((ULONG64)(len + NETSIM_WIRE_OVERHEAD) * 8ULL * 1000000000ULL +
             p->cfg.rate_bps - 1ULL) / p->cfg.rate_bps;

    f->depart_ns  = start + ser;
    f->arrive_ns  = f->depart_ns + p->cfg.delay_ns;
    p->busy_until = f->depart_ns;

    if (p->cfg.loss_ppm != 0 && netsim_draw_ppm() < p->cfg.loss_ppm)
    {
        f->lost = 1;
        p->st.drops_loss++;
    }

    if (p->tail != NULL)
        p->tail->next = f;
    else
        p->head = f;
    p->tail = f;

    if (p->queued == NULL)
        p->queued = f;
    p->backlog += len + NETSIM_WIRE_OVERHEAD;
    if (p->backlog > p->st.peak_queue)
        p->st.peak_queue = p->backlog;
}

static VOID netsim_driver(UINT side, NX_IP_DRIVER *req)
{
    NX_INTERFACE *nxif = req->nx_ip_driver_interface;

    req->nx_ip_driver_status = NX_SUCCESS;

    switch (req->nx_ip_driver_command)
    {
    case NX_LINK_INTERFACE_ATTACH:
        break;

    case NX_LINK_INITIALIZE:
        nxif->nx_interface_ip_mtu_size            = 1500;
        nxif->nx_interface_address_mapping_needed = NX_FALSE;
        nxif->nx_interface_link_up                = NX_FALSE;
        break;

    case NX_LINK_ENABLE:
        nxif->nx_interface_link_up = NX_TRUE;
        break;

    case NX_LINK_DISABLE:
        nxif->nx_interface_link_up = NX_FALSE;
        break;

    case NX_LINK_PACKET_SEND:
    case NX_LINK_PACKET_BROADCAST:
        /* The frame is copied onto the wire and the packet goes back at
           once, as a card that has taken it would give it back. */
        netsim_transmit(side, req->nx_ip_driver_packet);
        nx_packet_transmit_release(req->nx_ip_driver_packet);
        break;

    case NX_LINK_ARP_SEND:
    case NX_LINK_ARP_RESPONSE_SEND:
    case NX_LINK_RARP_SEND:
        nx_packet_transmit_release(req->nx_ip_driver_packet);
        req->nx_ip_driver_status = NX_NOT_SUCCESSFUL;
        break;

    default:
        req->nx_ip_driver_status = NX_UNHANDLED_COMMAND;
        break;
    }
}

VOID netsim_driver_0(NX_IP_DRIVER *request)
{
    netsim_driver(0, request);
}

VOID netsim_driver_1(NX_IP_DRIVER *request)
{
    netsim_driver(1, request);
}

/* One receive thread per side: drain what has arrived, under the IP lock,
   as ami_sana2_rx_drain() does. */
static VOID netsim_receiver(ULONG side_index)
{
    NetSimSide *s  = &ns_side[side_index];
    NX_IP      *ip = s->ip;

    for (;;)
    {
        TX_THREAD *outer;

        tx_semaphore_get(&s->rx_wake, TX_WAIT_FOREVER);

        tx_mutex_get(&ip->nx_ip_protection, TX_WAIT_FOREVER);
        outer = _nx_ip_input_thread;
        _nx_ip_input_thread = tx_thread_identify();

        while (s->inbox != NULL)
        {
            NetSimFrame *f = s->inbox;
            NX_PACKET   *packet;

            s->inbox = f->next;
            if (s->inbox == NULL)
                s->inbox_tail = NULL;

            if (nx_packet_allocate(ip->nx_ip_default_packet_pool, &packet,
                                   NX_RECEIVE_PACKET, NX_NO_WAIT) != NX_SUCCESS)
            {
                s->rx_nobuf++;
                free(f);
                continue;
            }

            memcpy(packet->nx_packet_prepend_ptr, f->data, f->len);
            packet->nx_packet_append_ptr = packet->nx_packet_prepend_ptr + f->len;
            packet->nx_packet_length     = f->len;
            packet->nx_packet_address.nx_packet_interface_ptr = &ip->nx_ip_interface[0];
            free(f);

            _nx_ip_packet_receive(ip, packet);
        }

        _nx_ip_input_thread = outer;
        tx_mutex_put(&ip->nx_ip_protection);
    }
}

UINT netsim_receiver_start(UINT side, NX_IP *ip)
{
    NetSimSide *s = &ns_side[side];
    UINT status;

    s->ip = ip;

    status = tx_semaphore_create(&s->rx_wake, (CHAR *)"netsim rx", 0);
    if (status != TX_SUCCESS)
        return status;

    return tx_thread_create(&s->rx_thread, (CHAR *)"netsim rx", netsim_receiver,
                            side, s->rx_stack, sizeof(s->rx_stack),
                            AMI_SANA2_RX_PRIORITY, AMI_SANA2_RX_PRIORITY,
                            TX_NO_TIME_SLICE, TX_AUTO_START);
}

/* Hand every frame that has arrived by now to its side's receive thread. */
static VOID netsim_deliver(VOID)
{
    UINT side;

    for (side = 0; side < NETSIM_SIDES; side++)
    {
        NetSimPath *p = &ns_path[side];
        NetSimSide *s = &ns_side[side];
        UINT        woke = 0;

        while (p->head != NULL && p->head->arrive_ns <= ns_now)
        {
            NetSimFrame *f = p->head;

            p->head = f->next;
            if (p->head == NULL)
                p->tail = NULL;
            if (p->queued == f)
            {
                p->backlog -= f->len + NETSIM_WIRE_OVERHEAD;
                p->queued   = f->next;
            }

            if (f->lost || s->ip == NX_NULL)
            {
                free(f);
                continue;
            }

            f->next = NULL;
            if (s->inbox_tail != NULL)
                s->inbox_tail->next = f;
            else
                s->inbox = f;
            s->inbox_tail = f;
            woke = 1;
        }

        if (woke && s->rx_wake.tx_semaphore_count == 0)
            tx_semaphore_put(&s->rx_wake);
    }
}

VOID netsim_port_idle(VOID)
{
    ULONG64 next = ns_next_tick;
    UINT    side;

    for (side = 0; side < NETSIM_SIDES; side++)
    {
        if (ns_path[side].head != NULL && ns_path[side].head->arrive_ns < next)
            next = ns_path[side].head->arrive_ns;
    }

    if (next > ns_deadline)
    {
        printf("netsim=FAIL reason=deadline now_ms=%llu\n",
               (unsigned long long)(ns_now / 1000000ULL));
        fflush(stdout);
        exit(1);
    }

    if (next > ns_now)
        ns_now = next;

    netsim_deliver();

    if (ns_now >= ns_next_tick)
    {
        ns_next_tick += NETSIM_TICK_NS;
        _tx_timer_interrupt();
    }
}
