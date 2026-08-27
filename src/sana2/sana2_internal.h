/*
 * AmiNetXDuo, SANA-II shim internals.
 *
 * Private to src/sana2/. Include "tx_api.h" and "nx_api.h" before any exec
 * header: exec/types.h turns VOID into a macro, which breaks tx_port.h's
 * `typedef void VOID`.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_SANA2_INTERNAL_H
#define AMINETXDUO_SANA2_INTERNAL_H

#include "tx_api.h"
#include "nx_api.h"

#include "aminetxduo/sana2.h"
#include "aminetxduo/config.h"
#include "aminetxduo/compat.h"

#include "sana2_device.h"

/* --------------------------------------------------------------- tunables */

/*
 * CMD_READ is per packet type and the device has no buffers of its own, so a
 * reader's depth is its receive window in frames and each outstanding read pins
 * one NX_PACKET.  These are floors; ami_sana2_rx_plan() decides the rest.
 */
#ifndef AMI_SANA2_RX_DEPTH_IPV4
#define AMI_SANA2_RX_DEPTH_IPV4     4
#endif
#ifndef AMI_SANA2_RX_DEPTH_ARP
#define AMI_SANA2_RX_DEPTH_ARP      2
#endif
#ifndef AMI_SANA2_RX_DEPTH_IPV6
#define AMI_SANA2_RX_DEPTH_IPV6     2
#endif

/*
 * AMI_SANA2_RX_POOL_SHARE must move together with
 * src/bsdsocket/bsdsocket_internal.h's BSD_TCP_WINDOW_POOL_SHARE: the window is
 * the bytes a peer may have in flight and the read queue is where they land.
 * sana2 is below bsdsocket and cannot include its header.
 */
#ifndef AMI_SANA2_RX_POOL_SHARE
#define AMI_SANA2_RX_POOL_SHARE     8
#endif

/*
 * There is deliberately NO MINIMUM here above AMI_SANA2_RX_DEPTH_IPV4.  The
 * read queue and the socket receive queue come out of one pool, so past a point
 * a packet is worth more free than posted: on a 47-packet pool a floor of eight
 * cost four fifths of the datagrams the machine could catch.  A want, not a
 * floor.
 */
/*
 * The deepest the IPv6 reader is planned, however much the pool can spare.
 * IPv4 is not capped this way because a dual-stack machine is not receiving at
 * full rate on both protocols at once: one CPU, one wire, one pool.
 */
#ifndef AMI_SANA2_RX_WANT_IPV6
#define AMI_SANA2_RX_WANT_IPV6      8
#endif

/*
 * One in this many pool packets may be PINNED by the readers, all of them
 * together and not one each, divided again by how many interfaces are attached.
 * The per-reader floors override it: a reader below its floor cannot absorb the
 * smallest burst there is.  Twice the window share above.
 */
#ifndef AMI_SANA2_RX_BUDGET_SHARE
#define AMI_SANA2_RX_BUDGET_SHARE   4
#endif

#ifndef AMI_SANA2_RX_MAX_DEPTH
#define AMI_SANA2_RX_MAX_DEPTH      32
#endif

/*
 * The line rate assumed for a device that does not report one.  S2_DEVICEQUERY
 * answers into a zeroed block, so "did not fill BPS in" and "supplied a short
 * block that stopped before it" both arrive here as 0.  A nonsense rate needs
 * no case: the ladder's bottom step covers every wire slower than the reader.
 */
#ifndef AMI_SANA2_BPS_DEFAULT
#define AMI_SANA2_BPS_DEFAULT       10000000UL
#endif

/*
 * How long ami_sana2_rx_drain() waits for nx_ip_protection before it looks at
 * rx->stop again.  Not a tuning knob and not a timeout on the lock: the mutex
 * is retaken at once unless a stop is in flight, and the stop holds it for as
 * long as it waits for this thread.  One tick, so the join sees the reader
 * gone in its first sleep.
 */
#ifndef AMI_SANA2_RX_LOCK_TICKS
#define AMI_SANA2_RX_LOCK_TICKS     1
#endif

/*
 * How long a reader waits for the device to give its queued CMD_READs back.
 * 25 x 2 ticks is one second: generous for a driver that honours AbortIO(),
 * bounded for one that does not.
 */
#ifndef AMI_SANA2_RX_REAP_TRIES
#define AMI_SANA2_RX_REAP_TRIES     25
#endif

/*
 * How many completed reads one reader may take before it must release the
 * ThreadX baton once, even with another completion already on its port.
 *
 * The bracket around the reader's Wait() is not free: it takes a Forbid(),
 * suspends the ThreadX thread, dispatches whatever is ready and reverses all
 * of it afterwards (src/netstack/netstack_baton.c).  During a burst the device
 * has usually replied again by the time the last frame has been delivered, so
 * the Wait() would return at once and the whole bracket bought nothing.
 *
 * The bound is not a batch target, it is a fairness boundary.  The reader
 * outranks everything (src/thread_priorities.h) and now runs TCP itself, so
 * the thread that empties the socket only runs when the reader lets go.  Eight
 * is a little over half the read depth an interface at 10 Mbit is planned
 * with, which puts the release inside a full ring rather than after it.
 */
#ifndef AMI_SANA2_RX_RUN_MAX
#define AMI_SANA2_RX_RUN_MAX        8
#endif

#if AMI_SANA2_RX_RUN_MAX < 1
#error "AMI_SANA2_RX_RUN_MAX must be at least one"
#endif

#ifndef AMI_SANA2_TX_SLOTS
#define AMI_SANA2_TX_SLOTS          8
#endif

#include "../thread_priorities.h"
/*
 * The reader runs IP input, and TCP input with it, itself
 * (ami_sana2_rx_drain), so its stack has to cover what the IP thread's covers
 * -- AMI_IP_STACK_SIZE, 4096 -- plus the reader's own frames underneath it:
 * the thread body, the drain, the completion, the delivery, and the device's
 * BeginIO(), which runs on whichever stack calls it.  There is no MMU, so a
 * frame that does not fit writes over what lies below and the machine dies
 * somewhere unrelated.
 *
 * tools/check-stack-frames.sh measures the worst case the compiler will admit
 * to and checks ami_sana2_rx_thread against a budget.  It went from 540 bytes
 * to 1820 when IP and TCP input moved on to the reader, and the script's own
 * header says its figures run about 800 light against what tests/stack
 * measures in a guest, because a call through a function pointer is not a
 * symbol in the assembly.
 */
#ifndef AMI_SANA2_RX_STACK_SIZE
#define AMI_SANA2_RX_STACK_SIZE     8192
#endif

/* Ticks to spin on a full TX ring before dropping the frame. */
#ifndef AMI_SANA2_TX_WAIT_TICKS
#define AMI_SANA2_TX_WAIT_TICKS     4
#endif

/*
 * Probe for raw-frame support at open time (see ami_sana2_probe_raw).  Off:
 * the probe's WaitIO() has no deadline and a2065.device 2.16 does not answer
 * the AbortIO() before it, so ami_sana2_open() never returns.
 */
#ifndef AMI_SANA2_PROBE_RAW
#define AMI_SANA2_PROBE_RAW         0
#endif

/* Use raw framing when the probe says it is available. Off: see sana2_device.c. */
#ifndef AMI_SANA2_RAW_DEFAULT
#define AMI_SANA2_RAW_DEFAULT       0
#endif

/* Offer the 16-bit buffer-management tags. Off: the tag numbers could not be
   checked against any header on this toolchain, see sana2_device.h. */
#ifndef AMI_SANA2_OFFER_COPY16
#define AMI_SANA2_OFFER_COPY16      0
#endif

#ifdef AMINETXDUO_IPV6
#define AMI_SANA2_RX_READERS        3
#else
#define AMI_SANA2_RX_READERS        2
#endif

/*
 * How deep each read queue goes on this machine, on this wire.  Three numbers
 * rather than one, because the pool pays for all three at once.  Filled by
 * ami_sana2_rx_plan(); see it for what decides each.
 */
typedef struct AmiRxDepths
{
    UWORD   ipv4;
    UWORD   arp;
    UWORD   ipv6;       /* 0 when the plan was asked for a single stack */
} AmiRxDepths;

/*
 * `bps` is what S2_DEVICEQUERY reported (0 when the device did not say),
 * `pool_total` is nx_packet_pool_total (0 when there is no pool yet), `ifaces`
 * is how many interfaces share that pool (0 read as 1).  Scalars rather than
 * the interface, so the arithmetic runs under tests/sana2/host.
 */
VOID ami_sana2_rx_plan(ULONG bps, ULONG pool_total, BOOL dual_stack,
                       UWORD ifaces, AmiRxDepths *out);

/* How many interfaces are bound to an NX_IP right now, which is how many
   readers' worth of pool packets are already spoken for.  In sana2_driver.c,
   where the bindings are. */
UWORD ami_sana2_bound_count(VOID);

/* ---------------------------------------------------------- receive probe */

/*
 * Off by default:
 *   cmake -B build/rxprobe -DAMINETXDUO_RXPROBE=ON ...
 * Two ReadEClock() calls per drain; the totals go to the serial log at rx stop.
 */
#ifdef AMINETXDUO_RXPROBE

/* Gap records kept for the bulk flow. */
#ifndef AMI_RXPROBE_GAPS
#define AMI_RXPROBE_GAPS            48
#endif

/* Baton-wait buckets, log2 in E-Clock ticks: 0, 1, 2-3, 4-7 ... */
#define AMI_RXPROBE_BUCKETS         16

/* Deepest backlogs kept, smallest first. */
#define AMI_RXPROBE_WORST           12

typedef struct AmiRxProbe
{
    ULONG   posts;              /* SendIO(CMD_READ) issued                 */
    ULONG   drains;             /* wakes that found at least one reply     */
    ULONG   live;               /* posted and not yet dequeued             */

    /* Per drain: reads the device still held, and replies already waiting. */
    ULONG   avail_hist[AMI_SANA2_RX_MAX_DEPTH + 1];
    ULONG   backlog_hist[AMI_SANA2_RX_MAX_DEPTH + 1];
    ULONG   dry;                /* drains that found the device holding 0  */
    ULONG   post_zero;          /* nothing could be posted                 */
    ULONG   post_partial;       /* posted fewer than depth                 */

    /* E-Clock ticks spent reacquiring the ThreadX baton after the Wait(). */
    ULONG   baton_max;
    ULONG   baton_sum;
    ULONG   baton_hist[AMI_RXPROBE_BUCKETS];

    /* The worst wakes by backlog, to separate a held reader from a burst. */
    ULONG   worst_when[AMI_RXPROBE_WORST];   /* ticks since the last wake  */
    ULONG   worst_baton[AMI_RXPROBE_WORST];
    UWORD   worst_backlog[AMI_RXPROBE_WORST];
    UWORD   worst_avail[AMI_RXPROBE_WORST];
    ULONG   last_wake;
    ULONG   baton_last;
} AmiRxProbe;


/*
 * One bulk TCP flow, latched on the first segment carrying 512 bytes or more.
 * `next` is the sequence that wire order produces, so a segment starting beyond
 * it is a hole that already existed when the frame reached this line.
 */
typedef struct AmiRxSeqProbe
{
    ULONG   peer;
    UWORD   sport;
    UWORD   dport;
    ULONG   next;
    BOOL    armed;

    ULONG   inorder;
    ULONG   ahead;              /* hole: the frame before it never arrived */
    ULONG   ahead_bytes;
    ULONG   behind;             /* retransmission or reorder               */
    ULONG   pure_ack;
    ULONG   other;

    ULONG   gap_want[AMI_RXPROBE_GAPS];
    ULONG   gap_got[AMI_RXPROBE_GAPS];
    ULONG   gap_open[AMI_RXPROBE_GAPS];   /* E-Clock when the hole opened  */
    ULONG   gap_fill[AMI_RXPROBE_GAPS];   /* ticks until `want` turned up  */
    UWORD   gap_avail[AMI_RXPROBE_GAPS];
    UWORD   gaps;

    /* Every TCP segment with a payload, whatever the flow, so the total can
       be compared with a capture without assuming what `other` counted. */
    ULONG   data_frames;

    UWORD   avail;              /* reads outstanding during this drain     */
} AmiRxSeqProbe;

VOID ami_sana2_rxprobe_deliver(AmiSana2If *iface, const UCHAR *frame,
                               ULONG length);
VOID ami_sana2_rxprobe_report(const AmiSana2If *iface);

#endif /* AMINETXDUO_RXPROBE */

/*
 * Two bytes of slack in front of the synthesised Ethernet header so the IP
 * header lands on a 4-byte boundary; the 68020 pays for misaligned longword
 * reads.  NetX Duo's NX_PHYSICAL_HEADER is 16 for the same reason.
 */
#define AMI_SANA2_RX_PAD            2

#define AMI_ETHERTYPE_RARP          0x8035

/* --------------------------------------------------------------- RX slots */

struct AmiSana2Rx;

/*
 * One outstanding CMD_READ.  `req` must stay first: completed requests come
 * back as struct Message * from GetMsg() and are cast straight back to the slot.
 */
typedef struct AmiRxSlot
{
    struct IOSana2Req   req;
    struct AmiSana2Rx  *owner;
    NX_PACKET          *packet;     /* pinned for the life of the request  */
    UCHAR              *dst;        /* where S2_CopyToBuff must write      */
    ULONG               capacity;   /* bytes available at dst              */
    ULONG               copied;     /* set by the copy hook                */
#ifdef AMINETXDUO_RX_VERIFY
    /*
     * The frame's ones-complement sum, computed out of the loads the copy is
     * already doing.  Written by the copy hook and read by the drain one
     * request later, re-armed before the next.  Not in NX_PACKET: growing it to
     * carry the same value hung the stack.
     */
    ULONG               sum;
    BOOL                summed;
#endif
    BOOL                posted;
} AmiRxSlot;

/*
 * What the copy hook accumulated, lifted out of the slot.
 *
 * ami_sana2_rx_complete() re-arms and re-posts the slot BEFORE it delivers the
 * frame, so the ring is never short across a delivery. Re-arming clears
 * slot->copied and slot->summed -- it has to, before BeginIO(), or a device
 * that does not call the copy hook leaves the previous frame's verdict where
 * the next frame's length is resolved from. The delivery still needs the
 * values that belong to the frame in hand, so they travel in one of these
 * rather than in the slot.
 */
typedef struct AmiRxSum
{
    ULONG   sum;        /* ones-complement sum the copy carried            */
    ULONG   copied;     /* over how many bytes                             */
    BOOL    summed;     /* whether the copy took the summing path          */
} AmiRxSum;

typedef struct AmiSana2Rx
{
    AmiSana2If         *iface;
    ULONG               packet_type;
    UWORD               depth;

    TX_THREAD           thread;
    TX_SEMAPHORE        ready;
    TX_SEMAPHORE        exited;
    APTR                stack;

    struct Task        *task;
    struct MsgPort     *port;
    ULONG               wake_mask;

    /*
     * Exactly one reader carries the TX reaping duty (see
     * ami_sana2_tx_reap_bind()), and only that reader has a nonzero reap_mask.
     */
    BOOL                reap_tx;        /* this reader has the duty         */
    BYTE                reap_sigbit;    /* -1 when none is held             */
    ULONG               reap_mask;

    volatile BOOL       started;    /* tx_thread_create succeeded          */
    volatile BOOL       running;
    volatile BOOL       stop;
    volatile BOOL       failed;

    /* Reads the device would not give back at teardown. Nonzero means this
       reader's slots, pinned packets and reply port are still reachable by the
       device, so none of them can be freed, see ami_sana2_rx_teardown(). */
    volatile UWORD      orphans;

#ifdef AMINETXDUO_RXPROBE
    AmiRxProbe          probe;
#endif

    AmiRxSlot           slot[AMI_SANA2_RX_MAX_DEPTH];
} AmiSana2Rx;

/* --------------------------------------------------------------- TX slots */

/* `req` must stay first, same GetMsg() cast as AmiRxSlot. */
typedef struct AmiTxSlot
{
    struct IOSana2Req   req;
    AmiSana2If         *iface;

    NX_PACKET          *packet;     /* released when the write completes   */
    NX_PACKET          *cursor;     /* chain walk state for S2_CopyFromBuff*/
    ULONG               cursor_off;
    ULONG               consumed;
    ULONG               total;
    UWORD               hdr_len;    /* Ethernet bytes prepended in raw mode*/
    UWORD               pad_len;    /* zero bytes appended to reach 60     */
    volatile BOOL       busy;

#ifdef AMINETXDUO_RXPROBE
    /* The ack leg's opening stamp: E-Clock at BeginIO, 0 when unarmed.  The
       reply's reaper closes the pair; per slot, not latest-stamp, because
       several writes are in flight by design. */
    ULONG               write_at;
#endif
} AmiTxSlot;

/* ------------------------------------------------------------- interface */

/*
 * offline_state below: never online, online with S2_OFFLINE not yet issued, and
 * issued.  Only the middle one records a skipped offline.
 */
#define AMI_SANA2_OFFLINE_NEVER     0
#define AMI_SANA2_OFFLINE_UP        1
#define AMI_SANA2_OFFLINE_ISSUED    2

struct AmiSana2If
{
    /* Device. The opened request doubles as the template every cloned request
       is copied from. It carries io_Device, io_Unit and the device-side
       ios2_BufferManagement cookie. */
    struct IOSana2Req   templ;
    BOOL                device_open;
    char                device[AMI_CFG_PATH_LEN];
    ULONG               unit;
    /* S2_AnxCardType points here, not at the caller's AmiIfConfig: the tag
       list is an input to OpenDevice and outlives the open. */
    char                card[AMI_CFG_NAME_LEN];
    struct TagItem      buffer_tags[12];

    /* Hardware facts from S2_DEVICEQUERY / S2_GETSTATIONADDRESS. */
    UCHAR               mac[AMI_ETH_ADDR_SIZE];
    ULONG               mtu;
    ULONG               bps;
    ULONG               hw_type;
    UWORD               addr_bits;
    UWORD               addr_bytes;     /* 6 for Ethernet, 0 if addressless */

    BOOL                online;
    /*
     * Whether S2_OFFLINE has been issued since this interface last went online.
     * For the event ring and nothing else: a teardown calls ami_sana2_offline()
     * six or seven times over, and only the first means anything.
     */
    UBYTE               offline_state;
    /* Whether this interface currently holds a share of its unit's use count.
       See the per-unit block in sana2_device.c. */
    BOOL                unit_counted;
    /* Administrative state: the stack's intent, not the wire's condition.
       Only the driver entry's enable/disable cases write it. */
    BOOL                admin_up;
    BOOL                raw_supported;
    BOOL                raw_mode;
    /* Set once a device answers an error to a raw CMD_WRITE this shim sent on
       its own initiative. See the EtherType note in ami_sana2_tx_send(). */
    BOOL                raw_tx_refused;

    /* NetX Duo binding. */
    NX_IP              *ip;
    UINT                index;
    NX_INTERFACE       *interface_ptr;
    NX_PACKET_POOL     *pool;

    /* TX ring. The reply port raises a signal on the reader that carries the
       reaping duty, and falls back to PA_IGNORE when no reader holds it. The
       sending thread never blocks on it either way. */
    struct MsgPort      tx_port;
    AmiTxSlot           tx[AMI_SANA2_TX_SLOTS];

#ifdef AMINETXDUO_TX_LAZY_COLLECT
    /*
     * Lazy completion collection, see ami_sana2_tx_lazy_tick().  Parking only
     * engages while the timer exists (tx_lazy_timer_up).  tx_lazy_parked means
     * "PA_IGNORE because of a send", never "because no reader is bound", and
     * every transition of it happens under the same Disable() as the port flags.
     */
    TX_TIMER            tx_lazy_timer;
    BOOL                tx_lazy_timer_up;
    volatile BOOL       tx_lazy_parked;
    volatile ULONG      tx_lazy_last_send;
#endif

    /* RX readers, one per packet type. */
    AmiSana2Rx          rx[AMI_SANA2_RX_READERS];
    BOOL                rx_running;

    /* Set when a reader could not be reclaimed, either it never exited or
       the device kept its CMD_READs. The whole interface is then unfreeable
       and unrestartable, because the device holds pointers into it. */
    BOOL                rx_orphaned;

    /* Same on the transmit side: set while ami_sana2_tx_drain() has left the
       device holding a CMD_WRITE. Those requests and their reply port are
       inside this allocation too. Cleared by a later drain that succeeds. */
    BOOL                tx_orphaned;

    AmiSana2Stats       stats;

#ifdef AMINETXDUO_RXPROBE
    AmiRxSeqProbe       seq;
    ULONG               probe_dev_rx;   /* S2_GETGLOBALSTATS PacketsReceived */
    ULONG               probe_dev_tx;
#endif
};

/* ------------------------------------------------------------- internals */

/* sana2_copy.c, called by the device in m68k register convention. */
UBYTE *ami_sana2_rx_direct(APTR ios2_data, ULONG len);
VOID   ami_sana2_rx_filled(APTR ios2_data, ULONG len, ULONG sum, UBYTE summed);
BOOL ami_sana2_copy_to_buff(register APTR to    __asm("a0"),
                            register APTR from  __asm("a1"),
                            register ULONG len  __asm("d0"));
BOOL ami_sana2_copy_from_buff(register APTR to   __asm("a0"),
                              register APTR from __asm("a1"),
                              register ULONG len __asm("d0"));
VOID ami_sana2_copy_bytes(UCHAR *to, const UCHAR *from, ULONG len);

/* sana2_device.c */
VOID ami_sana2_block_enter(VOID);
VOID ami_sana2_block_leave(VOID);
VOID ami_sana2_port_init(struct MsgPort *port, struct Task *task, BYTE sigbit,
                         UBYTE flags);
LONG ami_sana2_do_io(struct IORequest *req);
LONG ami_sana2_command(AmiSana2If *iface, struct IOSana2Req *req, UWORD command);
LONG ami_sana2_online(AmiSana2If *iface);
LONG ami_sana2_offline(AmiSana2If *iface);
LONG ami_sana2_multicast(AmiSana2If *iface, UWORD command,
                         ULONG addr_msw, ULONG addr_lsw);
VOID ami_sana2_refresh_stats(AmiSana2If *iface);

/* sana2_driver.c */
VOID ami_sana2_unbind(AmiSana2If *iface);

/* sana2_rx.c */
LONG ami_sana2_rx_start(AmiSana2If *iface);
VOID ami_sana2_rx_stop(AmiSana2If *iface);
BOOL ami_sana2_rx_resolve_length(AmiRxSlot *slot, ULONG *length);
/*
 * `slot` is the request the frame arrived on, or NULL when there is none.  It
 * carries the sum the copy hook computed, so the check below does not walk the
 * payload a second time.
 */
VOID ami_sana2_rx_deliver(AmiSana2If *iface, NX_PACKET *packet,
                          const AmiRxSum *sum);

/* sana2_tx.c */
VOID ami_sana2_tx_init(AmiSana2If *iface);
VOID ami_sana2_tx_reap(AmiSana2If *iface);
VOID ami_sana2_tx_reap_bind(AmiSana2If *iface, struct Task *task, BYTE sigbit);
VOID ami_sana2_tx_reap_unbind(AmiSana2If *iface);
VOID ami_sana2_tx_defer(AmiSana2If *iface);
#ifdef AMINETXDUO_TX_LAZY_COLLECT
VOID ami_sana2_tx_lazy_start(AmiSana2If *iface);
VOID ami_sana2_tx_lazy_stop(AmiSana2If *iface);
#endif
VOID ami_sana2_tx_drain(AmiSana2If *iface);
UINT ami_sana2_tx_send(AmiSana2If *iface, NX_PACKET *packet, UWORD ether_type,
                       ULONG dst_msw, ULONG dst_lsw);

/* <proto/exec.h> is forced FIRST: the NDK's inline wait macros must expand
   (once, behind their guard) BEFORE ours are defined, or a TU that includes it
   later has ours silently replaced -- the NDK path is -isystem, so the
   redefinition never even warns. */
#ifdef AMINETXDUO_GREEN_REALM
#include <proto/exec.h>
BYTE ami_green_checked_waitio(struct IORequest *request);
struct Message *ami_green_checked_waitport(struct MsgPort *port);

#undef WaitIO
#define WaitIO(request) ami_green_checked_waitio(request)
#undef WaitPort
#define WaitPort(port) ami_green_checked_waitport(port)

#ifdef AMINETXDUO_RXPROBE
ULONG ami_green_checked_wait(ULONG sigmask);
#undef Wait
#define Wait(sigmask) ami_green_checked_wait(sigmask)
#endif
#endif

#endif /* AMINETXDUO_SANA2_INTERNAL_H */
