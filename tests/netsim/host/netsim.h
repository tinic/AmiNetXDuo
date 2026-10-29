/*
 * AmiNetXDuo, host network simulator: two NetX Duo instances, one wire.
 *
 * Side 0 and side 1 each own an NX_IP whose only interface is a link driver
 * here.  What a driver is handed goes onto a path towards the other side:
 *
 *     driver -> tail-drop queue -> serialiser at rate -> delay -> peer
 *
 * The queue is the bottleneck's, in bytes, including the frame being
 * serialised; a frame that does not fit is dropped as it arrives.  Random
 * loss, when asked for, is drawn per frame from a fixed seed after the
 * queue, so a lost frame still took its turn on the wire.  Each frame is
 * charged its IP datagram plus 38 bytes of Ethernet (header, FCS, preamble,
 * gap), so a rate here is a line rate there.
 *
 * Delivery is the shipping SANA-II reader's (src/sana2/sana2_rx.c): one
 * receive thread per side at AMI_SANA2_RX_PRIORITY, which takes
 * nx_ip_protection for the run it drains, names itself _nx_ip_input_thread
 * and hands each frame to _nx_ip_packet_receive(), so TCP runs inline.  A
 * frame that finds the receiver's pool empty is dropped and counted.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETSIM_H
#define AMINETXDUO_NETSIM_H

#include "tx_api.h"

struct NX_IP_DRIVER_STRUCT;
struct NX_IP_STRUCT;
struct NX_TCP_SOCKET_STRUCT;

#define NETSIM_SIDES            2
#define NETSIM_TICK_NS          (1000000000ULL / TX_TIMER_TICKS_PER_SECOND)
#define NETSIM_WIRE_OVERHEAD    38UL

typedef struct NetSimPathConfig
{
    ULONG64 rate_bps;
    ULONG64 delay_ns;           /* one way                                */
    ULONG   queue_bytes;        /* 0: unbounded                           */
    ULONG   loss_ppm;           /* independent, per frame, 0: none        */
} NetSimPathConfig;

/* What one direction carried, read from the TCP headers that crossed it.
   "Data" is a segment with payload; its bytes are payload bytes. */
typedef struct NetSimPathStats
{
    ULONG   frames;
    ULONG   drops_queue;
    ULONG   drops_loss;
    ULONG   peak_queue;
    ULONG64 data_bytes;         /* payload, every transmission            */
    ULONG64 retx_bytes;         /* payload at or below the highest sent   */
    ULONG64 first_data_ns;
    ULONG64 last_data_ns;
    ULONG64 max_gap_ns;         /* longest silence between data segments  */
    ULONG64 max_gap_at_ns;
    UINT    seen;
    ULONG   high_seq;
    ULONG   data_frames;        /* segments with payload                  */
    ULONG   acks;               /* pure acknowledgments                   */
    ULONG   cwnd_50ms;          /* the sender's, 50 ms after its first    */
    UINT    cwnd_sampled;       /* data segment on this path              */
} NetSimPathStats;

/* Before nx_ip_create(): both paths, side 0 to 1 first, and the loss seed. */
VOID    netsim_configure(const NetSimPathConfig *to_1,
                         const NetSimPathConfig *to_0, ULONG64 seed);

/* The driver for each side, for nx_ip_create(). */
VOID    netsim_driver_0(struct NX_IP_DRIVER_STRUCT *request);
VOID    netsim_driver_1(struct NX_IP_DRIVER_STRUCT *request);

/* After nx_ip_create(): the receive thread for `side`. */
UINT    netsim_receiver_start(UINT side, struct NX_IP_STRUCT *ip);

/* Virtual time. */
ULONG64 netsim_now_ns(VOID);

/* The path towards `side`, its counters, and starting them again. */
NetSimPathStats *netsim_stats_towards(UINT side);
VOID    netsim_stats_reset(VOID);

/* Frames a side had no packet for. */
ULONG   netsim_rx_nobuf(UINT side);

/* The socket sending bulk data from `side`, for the cwnd sample and the
   trace; NULL when the transfer is over.
 *
 * NETSIM_TRACE=<file> writes one line per event, with the virtual time:
 * data sent (seq, len, retransmission or not), queue drop, random loss,
 * acknowledgment received by the sender (ack, window, SACK blocks, and the
 * sender's congestion state once NetX Duo has taken it), and the
 * retransmission timer firing.  Sequence numbers are relative to the first
 * data segment. */
VOID    netsim_sender_set(UINT side, struct NX_TCP_SOCKET_STRUCT *tcp);

/* No virtual time past this; the run fails with reason=deadline. */
VOID    netsim_deadline_set(ULONG64 at_ns);

/* The scheduler's idle hook, and the tick it raises (tx_sim_port.c). */
VOID    netsim_port_idle(VOID);
VOID    _tx_timer_interrupt(VOID);

#endif /* AMINETXDUO_NETSIM_H */
