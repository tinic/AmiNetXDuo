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
 * CMD_READ is per packet type and the device has no buffers of its own: every
 * frame that arrives with no matching read outstanding is dropped. Each
 * outstanding read pins one NX_PACKET for its whole life, so a reader's depth
 * is its receive window in frames, and it is paid for out of the packet pool.
 *
 * The floors below are what a reader gets when nothing else can be afforded.
 * Four for IPv4 is measured: with tests/curl/run-curlverify.sh -p, sixteen
 * concurrent HTTP transfers through curl's multi interface lost six at depth
 * four, twenty-four lost seven, and forty lost fifteen. All failed as
 * `curl: (7) Could not connect` after about thirteen seconds, on connections
 * the host had already accepted -- the SYN went out, the peer answered, and
 * the SYN/ACK arrived in a burst with no read outstanding to catch it. At
 * depth eight, forty concurrent transfers lost none.
 *
 * What each reader gets ABOVE its floor is ami_sana2_rx_plan(), from the line
 * rate the device reports and what the pool can spare. Both numbers are read
 * there and the reasoning is there.
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
 * The receive window in frames, which is what a reader has to be deep enough
 * to catch.
 *
 * One in AMI_SANA2_RX_POOL_SHARE pool packets is exactly the share
 * src/bsdsocket/bsdsocket_internal.h's BSD_TCP_WINDOW_POOL_SHARE gives a
 * socket's TCP receive window, and that is not a coincidence to be tidied
 * away: the window is the bytes a peer is allowed to have in flight, and the
 * read queue is where those bytes land. The two constants have to move
 * together, and this comment is the only thing saying so -- sana2 is below
 * bsdsocket and cannot include its header.
 */
#ifndef AMI_SANA2_RX_POOL_SHARE
#define AMI_SANA2_RX_POOL_SHARE     8
#endif

/*
 * THERE IS DELIBERATELY NO MINIMUM HERE ABOVE AMI_SANA2_RX_DEPTH_IPV4, AND
 * RAISING ONE IS THE MISTAKE TO READ THIS BEFORE MAKING.
 *
 * Eight looked right. tests/curl/run-curlverify.sh -p loses fifteen SYN/ACKs
 * of forty at depth four and none at eight, so a want floored at eight was
 * tried, and it is wrong on the machine it was aimed at. Under a UDP flood the
 * memory-tight A1200 -- 2 MB chip, no Fast RAM, pool 47 packets -- caught this
 * many datagrams of 2552 offered at 6000 kbit/s, three runs each, arms
 * alternating direction:
 *
 *      plan 5/2/2, the pool's own number      168, 170, 186
 *      plan 7/2/2, the want floored at eight   28,  30,  39
 *      plan 5/2/4, the floor taken out again  166, 175, 184
 *
 * Non-overlapping, and the third row is the second row's binary with the floor
 * removed. Two extra reads out of forty-seven packets cost four fifths of what
 * the machine could catch.
 *
 * A PACKET IS WORTH MORE FREE THAN POSTED WHEN THE READ QUEUE AND THE SOCKET
 * RECEIVE QUEUE COME OUT OF ONE POOL. A deeper read queue hands the socket
 * more datagrams sooner, the socket queue overruns, and the reader cannot then
 * allocate a replacement to re-arm with -- so the queue that was made deeper
 * runs shallower in practice. That turnover is the whole reason this is a want
 * and not a floor.
 *
 * AND IT IS THE MEASURED ANSWER TO AROSTCP'S FLOOR OF SIXTEEN. Sixteen is a
 * third of this pool. Seven was already four fifths of the way down; sixteen
 * is far past where it turns over. The reference implementation is not wrong
 * for its own stack -- it is wrong for a stack whose reads and whose sockets
 * are drawing on the same packets -- and nothing in the code says so, which is
 * why this comment does.
 *
 * The curl measurement is not contradicted: it was taken on a machine whose
 * pool gives thirty-two anyway. Every machine with any Fast RAM at all is
 * already above eight from the pool alone, so the floor could only ever have
 * bitten where it did harm.
 */
/*
 * The deepest the IPv6 reader is planned, however much the pool can spare.
 *
 * IPv4 is not capped this way and IPv6 is, because a packet pinned by a reader
 * nothing is arriving on is a packet the other reader's window and the
 * transmit path cannot have -- and a dual-stack machine is not receiving at
 * full rate on both protocols at once. One CPU, one wire, one pool.
 *
 * Both sides are measured, a2065 on an 8 MB A1200 (pool 367), the guest
 * pulling 2 MB over each protocol in the same boot, n=3 interleaved, medians
 * in kbit/s:
 *
 *      IPv6 depth       2      8     32
 *      IPv6 rate      386   1959   4013
 *      IPv4 beside   4660   4686   4686
 *
 * Two is a cliff and eight is five times off it. Thirty-two is twice as good
 * again and it is not what ships: the same tree at thirty-two read one to five
 * per cent below `main` on all nine cards of the streaming gate, one sign,
 * where eight does not -- and the twenty-four extra packets are eight per cent
 * of the biggest pool this stack ever gets and unaffordable on every smaller
 * one. Five times, for six packets.
 */
#ifndef AMI_SANA2_RX_WANT_IPV6
#define AMI_SANA2_RX_WANT_IPV6      8
#endif

/*
 * One in this many pool packets can be PINNED by the readers, all of them
 * together and not one each.
 *
 * A quarter, and the floors override it: a machine whose pool cannot spare
 * even the floors still gets them, because a reader below its floor cannot
 * absorb the smallest burst there is and the alternative to spending the
 * packets is a link that drops SYN/ACKs.
 *
 * Twice the window share above, so a machine can hold a window's worth of
 * reads on one protocol and still have the same again for the other.
 */
#ifndef AMI_SANA2_RX_BUDGET_SHARE
#define AMI_SANA2_RX_BUDGET_SHARE   4
#endif

#ifndef AMI_SANA2_RX_MAX_DEPTH
#define AMI_SANA2_RX_MAX_DEPTH      32
#endif

/*
 * The line rate assumed for a device that does not report one.
 *
 * S2_DEVICEQUERY answers into a zeroed block, so a device that does not fill
 * BPS in, and one that supplies a short block that stops before it, both
 * arrive here as 0. Ten megabits is what every Ethernet board in
 * src/netdev/netdev_cards.c but the x-surf-100 reports, so a device that
 * cannot say lands where the common case is rather than at either extreme.
 *
 * A nonsense rate needs no separate case: the ladder's bottom step covers
 * every wire slower than the reader, so a device answering 1 is capped where a
 * serial line is capped, which is the floor, which is where it belongs.
 */
#ifndef AMI_SANA2_BPS_DEFAULT
#define AMI_SANA2_BPS_DEFAULT       10000000UL
#endif

/*
 * How long a reader waits for the device to give its queued CMD_READs back
 * once it has asked. 25 x 2 ticks is one second: generous for a driver that
 * honours AbortIO(), and bounded for one that does not.
 */
#ifndef AMI_SANA2_RX_REAP_TRIES
#define AMI_SANA2_RX_REAP_TRIES     25
#endif

#ifndef AMI_SANA2_TX_SLOTS
#define AMI_SANA2_TX_SLOTS          8
#endif

#include "../thread_priorities.h"
#ifndef AMI_SANA2_RX_STACK_SIZE
#define AMI_SANA2_RX_STACK_SIZE     4096
#endif

/* Ticks to spin on a full TX ring before dropping the frame. */
#ifndef AMI_SANA2_TX_WAIT_TICKS
#define AMI_SANA2_TX_WAIT_TICKS     4
#endif

/*
 * Probe for raw-frame support at open time (see ami_sana2_probe_raw).
 *
 * Off, and off here rather than only in the build: the probe's WaitIO() has no
 * deadline, and a2065.device 2.16 does not answer the AbortIO() before it, so
 * ami_sana2_open() never returns. CMakeLists.txt gives the same answer. This
 * is what a build that does not go through it gets.
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
 * How deep each read queue goes on this machine, on this wire.
 *
 * Three numbers rather than one, because the pool pays for all three at once
 * and the ARP reader is not the same kind of consumer as the other two. Filled
 * by ami_sana2_rx_plan(); see it for what decides each.
 */
typedef struct AmiRxDepths
{
    UWORD   ipv4;
    UWORD   arp;
    UWORD   ipv6;       /* 0 when the plan was asked for a single stack */
} AmiRxDepths;

/*
 * `bps` is what S2_DEVICEQUERY reported, 0 when the device did not say.
 * `pool_total` is nx_packet_pool_total, 0 when there is no pool yet.
 * `dual_stack` says whether an IPv6 reader will be started at all.
 *
 * Extern, and taking scalars rather than the interface, so the arithmetic runs
 * under tests/sana2/host with no device and no pool behind it.
 */
VOID ami_sana2_rx_plan(ULONG bps, ULONG pool_total, BOOL dual_stack,
                       AmiRxDepths *out);

/* ---------------------------------------------------------- receive probe */

/*
 * Off by default, in the shape AMINETXDUO_NXCENSUS uses: two ReadEClock()
 * calls per drain, and the totals go to the serial log when the readers stop.
 *
 *   cmake -B build/rxprobe -DAMINETXDUO_RXPROBE=ON ...
 *
 * It answers two questions the existing counters cannot. The first is how many
 * CMD_READs the device still held each time the reader woke, which is the
 * receive window in frames, live rather than configured. A frame that arrives
 * with none outstanding is dropped by the device and counted nowhere. The
 * second is whether the TCP sequence space is already holed when the frame
 * reaches NetX Duo, which separates a frame lost below this line from one
 * NetX Duo dropped after it.
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
 * `next` is the sequence that wire order produces, so a segment starting
 * beyond it is a hole that already existed when the frame reached this line.
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
 * header lands on a 4-byte boundary. NetX Duo's NX_PHYSICAL_HEADER is 16 for
 * the same reason, and the 68020 pays for misaligned longword reads.
 */
#define AMI_SANA2_RX_PAD            2

#define AMI_ETHERTYPE_RARP          0x8035

/* --------------------------------------------------------------- RX slots */

struct AmiSana2Rx;

/*
 * One outstanding CMD_READ. `req` must stay first: completed requests come
 * back as struct Message * from GetMsg() and are cast straight back to the
 * slot.
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
     * already doing.  Here rather than in NX_PACKET because this struct
     * belongs to the shim and its lifetime is the slot's. It is written by the
     * copy hook and read by the drain, one request apart, and re-armed before
     * the next. Growing NX_PACKET to carry the same value hung the stack.
     */
    ULONG               sum;
    BOOL                summed;
#endif
    BOOL                posted;
} AmiRxSlot;

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
     * TX reaping duty. Exactly one reader carries it (see
     * ami_sana2_tx_reap_bind()), and only that reader has a nonzero reap_mask,
     * the signal the TX reply port raises on completion.
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
} AmiTxSlot;

/* ------------------------------------------------------------- interface */

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
    struct TagItem      buffer_tags[8];

    /* Hardware facts from S2_DEVICEQUERY / S2_GETSTATIONADDRESS. */
    UCHAR               mac[AMI_ETH_ADDR_SIZE];
    ULONG               mtu;
    ULONG               bps;
    ULONG               hw_type;
    UWORD               addr_bits;
    UWORD               addr_bytes;     /* 6 for Ethernet, 0 if addressless */

    BOOL                online;
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
/*
 * `slot` is the request the frame arrived on, or NULL when there is none.  It
 * carries the sum the copy hook computed, so the check below does not walk the
 * payload a second time.
 */
VOID ami_sana2_rx_deliver(AmiSana2If *iface, NX_PACKET *packet,
                          const AmiRxSlot *slot);

/* sana2_tx.c */
VOID ami_sana2_tx_init(AmiSana2If *iface);
VOID ami_sana2_tx_reap(AmiSana2If *iface);
VOID ami_sana2_tx_reap_bind(AmiSana2If *iface, struct Task *task, BYTE sigbit);
VOID ami_sana2_tx_reap_unbind(AmiSana2If *iface);
VOID ami_sana2_tx_defer(AmiSana2If *iface);
VOID ami_sana2_tx_drain(AmiSana2If *iface);
UINT ami_sana2_tx_send(AmiSana2If *iface, NX_PACKET *packet, UWORD ether_type,
                       ULONG dst_msw, ULONG dst_lsw);

#endif /* AMINETXDUO_SANA2_INTERNAL_H */
