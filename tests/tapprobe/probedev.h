/*
 * tapprobe -- an instrumented synthetic SANA-II device.
 *
 * Derived from tests/tcpdrill/tapdev.c, which is the device our own stack is
 * tested against.  This copy exists to be opened by a FOREIGN stack --
 * Roadshow, AmiTCP -- and to record what that stack does with it:
 *
 *   * the buffer-management tag list handed to OpenDevice();
 *   * every CMD_READ arrival, with the request pointer and the queue depth;
 *   * every reply, so reply-to-repost latency and ordering are readable;
 *   * the E-Clock cost of the stack's own S2_CopyToBuff over a frame whose
 *     source alignment we choose.
 *
 * It is a measuring instrument, not a shipping component.  Nothing in src/
 * includes it and nothing here is on the shipped receive path.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TAPPROBE_PROBEDEV_H
#define TAPPROBE_PROBEDEV_H

#include <exec/types.h>

#define PROBE_DEVICE_NAME   "tcpdrill.device"

#define PROBE_FRAME_MAX     1600
#define PROBE_TX_SLOTS      160
#define PROBE_EVENTS        3000
#define PROBE_TAGS          32

/* Event kinds in the ring. */
#define PEV_OPEN        1       /* a=unit                                    */
#define PEV_READ_IN     2       /* a=request, b=depth after, type, aux=flags */
#define PEV_READ_REPLY  3       /* a=request, b=depth after removal          */
#define PEV_COPY        4       /* a=eclock ticks, b=len, aux=src align      */
#define PEV_WRITE       5       /* a=len, type                               */
#define PEV_NOREADER    6       /* type                                      */
#define PEV_CMD         7       /* a=command                                 */
#define PEV_ONLINE      8
#define PEV_OFFLINE     9
#define PEV_CLOSE       10
#define PEV_ABORT       11      /* a=request                                 */

typedef struct ProbeEvent
{
    UBYTE   kind;
    UBYTE   aux;
    UWORD   type;
    ULONG   t;                  /* E-Clock low word at the moment recorded  */
    ULONG   a;
    ULONG   b;
} ProbeEvent;

typedef struct ProbeStats
{
    ULONG   tx_frames;
    ULONG   tx_overrun;
    ULONG   rx_delivered;
    ULONG   rx_no_reader;
    ULONG   rx_copy_failed;
    ULONG   reads_now;          /* CMD_READs held right now                 */
    ULONG   reads_max;          /* the high-water mark                      */
    ULONG   reads_total;        /* CMD_READs ever posted                    */
    ULONG   raw_reads;          /* CMD_READs carrying SANA2IOF_RAW          */
    ULONG   online_count;
    ULONG   offline_count;
    ULONG   opens;
    ULONG   events_lost;        /* ring overflow                            */
} ProbeStats;

/*
 * `start_online` decides whether the device is live the moment it is opened.
 * Roadshow never issues S2_ONLINE to an Ethernet device -- it treats one as
 * running once it is configured -- so a device that starts offline stalls it.
 */
LONG probe_install(const UBYTE *mac, BOOL start_online);
VOID probe_remove(VOID);
BOOL probe_remove_safe(VOID);   /* FALSE, and does nothing, if still open   */

BOOL  probe_is_online(VOID);
ULONG probe_open_count(VOID);
VOID  probe_get_stats(ProbeStats *out);
ULONG probe_reads_for(UWORD ether_type);

/* The buffer-management tag list, as the stack passed it. */
ULONG probe_tags(ULONG **tag, ULONG **data);

/* Which hooks the stack actually supplied. */
APTR probe_hook_to(VOID);
APTR probe_hook_from(VOID);
APTR probe_hook_to16(VOID);
APTR probe_hook_from16(VOID);
APTR probe_hook_filter(VOID);
APTR probe_hook_dma_to(VOID);
APTR probe_hook_dma_from(VOID);

/*
 * Deliver one frame.  `frame` must already sit where the caller wants it:
 * the device hands &frame[14] to S2_CopyToBuff and nothing moves it.
 * Returns 0 on delivery, -1 when no CMD_READ of that EtherType was queued.
 *
 * probe_set_poison() makes the device overwrite the source bytes with 0xEE
 * the instant the hook returns, so a stack that reads the buffer a second
 * time corrupts its own packet and says so on the wire.
 */
LONG probe_rx_put(UBYTE *frame, ULONG len);
VOID probe_set_poison(BOOL on);

/*
 * Deliver through S2_DMACopyToBuff32 instead: ask the stack for a buffer it is
 * willing to have a bus master write into, and write there.  The alignment of
 * what it hands back is the stack saying what it wants, rather than us asking
 * what it will put up with.  *buf receives the pointer (NULL if it declined).
 */
LONG probe_rx_put_dma(UBYTE *frame, ULONG len, APTR *buf);

/* Collect one transmitted frame, rebuilt with its Ethernet header. */
ULONG probe_tx_get(UBYTE *buf, ULONG max);
ULONG probe_tx_pending(VOID);

/* The event ring, oldest first.  Returns how many were written to `out`. */
ULONG probe_events(ProbeEvent *out, ULONG max);

ULONG probe_eclock_rate(VOID);
ULONG probe_eclock_now(VOID);

#endif /* TAPPROBE_PROBEDEV_H */
