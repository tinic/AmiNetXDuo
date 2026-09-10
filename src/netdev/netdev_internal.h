/*
 * anxnet.device, layer 1 of 3: what the SANA-II shell keeps.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_INTERNAL_H
#define AMINETXDUO_NETDEV_INTERNAL_H

#include <stddef.h>

#include <exec/types.h>
#include <exec/devices.h>
#include <exec/interrupts.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <dos/dos.h>          /* BPTR, for the expunge seglist */

#include "aminetxduo/anxnet.h"

/*
 * Exec's AddHead() and Remove() are a jsr through the library base and back
 * to do four stores.  Both of these run ONCE PER RECEIVED FRAME -- the
 * CMD_READ queueing in netdev_cmds.c and the take in netdev_direct.c, the
 * second at interrupt level -- and this device already hand-rolls NewList()
 * for the same reason, that a -nostartfiles image does not link amiga.lib.
 *
 * Identical semantics: this is what exec.library's own AddHead and Remove
 * do, and neither of them Disable()s -- serialising the list is the caller's
 * job here exactly as it is there, so nothing about the locking changes.
 */
static inline VOID nd_addhead(struct List *l, struct Node *n)
{
    n->ln_Succ           = l->lh_Head;
    n->ln_Pred           = (struct Node *)&l->lh_Head;
    l->lh_Head->ln_Pred  = n;
    l->lh_Head           = n;
}

/*
 * DELIBERATELY STRICTER THAN Exec's Remove(), which leaves the unlinked
 * node's pointers stale.  This device replies requests, and replying one
 * that is still on a list corrupts that list; clearing the links turns that
 * into a NULL an Enforcer hit will name.  src/netdev/test's ReplyMsg checks
 * exactly this ("replied CMD_READ is still linked") and its Remove() stub
 * clears them for the same reason -- the check caught this function the
 * first time it did not.  Two stores, still far short of the jsr it
 * replaces, and no caller here walks a list through a removed node.
 */
static inline VOID nd_remove(struct Node *n)
{
    n->ln_Pred->ln_Succ = n->ln_Succ;
    n->ln_Succ->ln_Pred = n->ln_Pred;
    n->ln_Succ          = NULL;
    n->ln_Pred          = NULL;
}
#include "netdev_nic.h"
#include "netdev_mcast.h"
#include "sana2_device.h"

#define NETDEV_MAX_UNITS    4
#define NETDEV_TRACK_MAX    16

struct NetdevUnit;

typedef struct NetdevTrack
{
    ULONG                       type;
    UWORD                       used;
    struct Sana2PacketTypeStats st;
} NetdevTrack;

/*
 * One per OpenDevice().  io_Unit points at the opener, so every request arrives
 * already attached to the opener that made it: the CMD_READ queue, the copy
 * hooks and the RAW flag are per opener, which is the whole of what SANA-II
 * means by an opener.
 */
typedef struct NetdevOpener
{
    struct MinNode      op_Node;
    struct Unit         op_Unit;
    struct NetdevUnit  *op_Hw;

    APTR                op_CopyTo;
    APTR                op_CopyFrom;
    APTR                op_Filter;
    APTR                op_RxDirect;    /* aminetxduo/anxs2ext.h, or NULL */
    BOOL                op_RxLinkHdr;   /* write the link header before dst */
    APTR                op_RxFilled;

    UBYTE               op_Raw;
    UBYTE               op_Promisc;
    UBYTE               op_Exclusive;
    UBYTE               op_Pad;

    struct List         op_Reads;
    struct List         op_Orphans;
    struct List         op_Events;

    NetdevTrack         op_Track[NETDEV_TRACK_MAX];
    UWORD               op_TrackHigh;   /* one past the highest used slot */
} NetdevOpener;

/* RAW is permitted on either the opener or one individual request.  Keep
   that SANA-II rule in one predicate: receive hand-over, direct receive and
   transmit must not quietly disagree about which framing the buffer uses. */
static inline BOOL netdev_io_is_raw(const NetdevOpener *op,
                                    const struct IOSana2Req *io)
{
    return (BOOL)(op->op_Raw ||
                  (io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0);
}

/*
 * EXEC'S LIST PRIMITIVES ARE ROM CALLS, AND TWO OF THEM RUN PER FRAME.
 *
 * <inline/exec.h> expands Remove() to `jsr a6@(-252:W)` -- a register setup,
 * a jump into Kickstart and an rts around three pointer stores.  On the
 * receive path that is paid twice for every frame: netdev_take() unlinks the
 * CMD_READ it matched, and netdev_queue_read() links the re-post back at the
 * head.  It was three until the batched reply was reverted for costing 0.55%
 * of receive and 1.11% of transmit; the tail helper stays because
 * netdev_queue_read()'s S2_READORPHAN arm uses it.
 *
 * These are the same three stores, written out.  The layout is Exec's and is
 * not being reinterpreted: nd_newlist() in netdev_device.c already builds
 * lh_Head/lh_Tail/lh_TailPred by hand for the same reason -- NewList() lives
 * in amiga.lib, which a -nostartfiles image does not link.
 *
 * COLD SITES KEEP THE ROM CALL.  Opening a unit, closing it, expunging the
 * device: those run once and are better left reading as the ordinary Exec
 * idiom.  Only what runs once a frame is written out here.
 */
static inline VOID nd_list_addtail(struct List *l, struct Node *n)
{
    struct Node *pred = l->lh_TailPred;

    n->ln_Succ     = (struct Node *)(APTR)&l->lh_Tail;
    n->ln_Pred     = pred;
    pred->ln_Succ  = n;
    l->lh_TailPred = n;
}

typedef struct NetdevUnit
{
    NetdevNic                   nu_Nic;
    struct NetdevDevice        *nu_Dev;
    UWORD                       nu_Unit;
    UWORD                       nu_Openers;

    UBYTE                       nu_Configured;
    UBYTE                       nu_Online;
    UBYTE                       nu_IntrAdded;
    UBYTE                       nu_Pad;

    struct List                 nu_OpenerList;
    struct List                 nu_Writes;      /* CMD_WRITE awaiting a slot */

    struct Interrupt            nu_Intr;      /* INT2, the card               */
    struct Interrupt            nu_Tick;      /* INT3 vertical blank, watchdog */
    UBYTE                       nu_TxBuilding; /* a task owns nu_TxBuf         */
    UBYTE                      *nu_TxAt;      /* where the frame was built    */
    UWORD                       nu_TxStall;   /* blanks with a transmit stuck  */
    UWORD                       nu_TxWedges;  /* how often it had to be reset  */
    ULONG                       nu_IntSeen;   /* claimed interrupts delivered  */
    ULONG                       nu_TickPolls; /* tick-serviced during silence  */
    UWORD                       nu_IntSilent; /* blanks since a claimed one    */
    ULONG                       nu_RxDirect;  /* completed direct RX fills      */
    UWORD                       nu_RxKickWait;/* blanks toward an RX re-roll   */
    UWORD                       nu_RxKicks;   /* deaf-boot resets performed    */
    volatile UBYTE              nu_InIsr;     /* interrupt server on the chip  */
    ULONG                       nu_TxProgress;/* last completion the tick saw  */

    NetdevMcast                 nu_Mcast[NETDEV_MCAST_MAX];
    ULONG                       nu_McastFull;   /* joins the table could not hold */
    UWORD                       nu_AllMulti;    /* ranges too wide to hash */
    UWORD                       nu_Promisc;     /* openers that asked for it */
    UBYTE                       nu_Exclusive;   /* SANA2OPF_MINE is held */
    UBYTE                       nu_Pad2;

    /*
     * The OR of every queued S2_ONEVENT mask on this unit, so that an
     * interrupt with an event to post can find out that nobody is listening in
     * one word.  Written under Disable(), read without one, and a word rather
     * than a long because a 68000 reads a long in two bus cycles and this is
     * read from interrupt context.  netdev_event.c owns it.
     */
    UWORD                       nu_EventMask;
    UWORD                       nu_Pad3;

    struct Sana2DeviceStats     nu_Stats;

    /* Frames are staged here on the way out, 4-aligned for the long window. */
    ULONG                       nu_TxBuf[(NETDEV_FRAME_MAX + 7) / 4];
} NetdevUnit;

/*
 * Let tools/profiler/Profile recover this device's LoadSeg hunks.  A device
 * base is private past struct Device, so the profiler cannot safely read
 * nd_SegList at a fixed offset.  Instead it scans the positive half for this
 * self-validating five-longword record, then verifies that the device's LVO
 * targets lie inside the reported hunks.  The convention is documented in
 * tools/profiler/prof.h; it is repeated here so the shipped driver does not
 * depend on a development tool's header.
 */
#define NETDEV_PROF_SEGTAG_MAGIC   0x50534731UL    /* 'PSG1' */

typedef struct NetdevProfSegTag
{
    ULONG   np_Magic;
    ULONG   np_Size;
    ULONG   np_LibBase;
    ULONG   np_SegList;
    ULONG   np_Sum;
} NetdevProfSegTag;

_Static_assert(sizeof(NetdevProfSegTag) == 5u * sizeof(ULONG),
               "the profiler segment tag is exactly five longwords");

typedef struct NetdevDevice
{
    struct Device       nd_Device;
    struct Library     *nd_ExpansionBase;
    BPTR                nd_SegList;
    NetdevProfSegTag    nd_ProfSegTag;
    UWORD               nd_UnitCount;
    /* Supported boards found past NETDEV_MAX_UNITS.  Non-zero means a fitted
       card has no unit and nothing else would have said so. */
    UWORD               nd_UnitsDropped;

    /*
     * What the probe did, published under a public semaphore for the life of
     * the device.  Here rather than in a static, because it must go when the
     * device base goes.  The reader finds it by name, and a record that
     * outlives its memory is the one way this can do harm.
     */
    AnxDiagMark         nd_Diag;

    /*
     * A card can disappear while an OpenDevice() retry or the insertion worker
     * is rebuilding its unit.  Those paths run in different tasks and both
     * change the one CardHandle and the one PCMCIA unit, so hardware access and
     * publication of the result are one semaphore-protected transaction.
     */
    struct SignalSemaphore nd_PcmciaLock;

    NetdevUnit          nd_Units[NETDEV_MAX_UNITS];
} NetdevDevice;

/*
 * io_Unit points at the embedded struct Unit, not at the opener, so a caller
 * or a tool that treats io_Unit as a struct Unit * is not being lied to.  The
 * opener is recovered from it here.
 */
#define NETDEV_OPENER(u) \
    ((NetdevOpener *)(void *)((UBYTE *)(u) - offsetof(NetdevOpener, op_Unit)))

/*
 * What one opener did with one received frame.  REJECTED is not a failure and
 * not a delivery: the opener's S2_PacketFilter hook said no, its CMD_READ is
 * still queued and untouched, and the frame goes on to whoever else wants it.
 */
typedef enum
{
    NETDEV_RX_TAKEN = 0,
    NETDEV_RX_REJECTED,
    NETDEV_RX_FAILED        /* the opener's CopyToBuff hook returned FALSE */
} NetdevRxResult;

/* netdev_device.c */
VOID netdev_reply(struct IOSana2Req *io, LONG err, ULONG wire);
BOOL netdev_copy_call(APTR fn, APTR to, APTR from, ULONG len);
/* A standard utility.library Hook: a0 = hook, a2 = object, a1 = message. */
BOOL netdev_hook_call(APTR hook, APTR object, APTR message);
VOID netdev_rebuild_filter(NetdevUnit *unit);
VOID netdev_tx_pump(NetdevUnit *unit);
/* netdev_direct.c: the private single-copy receive transaction, kept apart
   from the romtag so it can run as an ordinary host test.  There is no
   unclaim: neither supported port core has a recoverable error once its
   drain has begun, so a claim commits (netdev_nic.h states the contract). */
/*
 * THE LAST TWO PER-FRAME HELPERS THAT WERE STILL A CROSS-TU CALL.
 *
 * Both run once for every received frame from netdev_rx_body()
 * (netdev_device.c:724), and both lived in netdev_direct.c, so whether the
 * shipped image pays a jsr for them was a decision LTO made rather than one
 * the source stated -- the same gap 05b90c97 closed for netdev_payload() and
 * netdev_filter_ok(), and unanswerable the same way: anxnet.device links with
 * -flto and `nm` on a KEEP_SYMBOLS build returns 136 entries with every local
 * name collapsed, so tools/check-hot-calls.sh cannot count the sites.
 *
 * _netdev_take is 1.0% of the real-path profile and _netdev_track_find scans
 * op_TrackHigh entries, which is three.  Neither body is bigger than its own
 * call sequence.  The cold callers -- the two drain loops in netdev_close()
 * (netdev_device.c:1193) -- get a copy each and run once.
 */

/* Bounded by the highest slot ever taken, not by the array.  The profile put
   this at 26% of the hand-over when it scanned all sixteen entries for every
   opener on every frame, and usually nothing is tracked.  An opener that
   tracks two types now scans two. */
static inline NetdevTrack *netdev_track_find(NetdevOpener *op, ULONG type)
{
    UWORD i;

    for (i = 0; i < op->op_TrackHigh; i++)
    {
        if (op->op_Track[i].used && op->op_Track[i].type == type)
            return &op->op_Track[i];
    }

    return NULL;
}

static inline struct IOSana2Req *netdev_take(struct List *list, ULONG type)
{
    struct Node *n;

    for (n = list->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        struct IOSana2Req *io = (struct IOSana2Req *)n;

        /* (ULONG)-1, not ~0UL: this file also builds on the test host, where
           unsigned long is wider than ULONG and ~0UL could never match. */
        if (type == (ULONG)-1 || io->ios2_PacketType == type)
        {
            nd_remove(n);
            return io;
        }
    }

    return NULL;
}
UBYTE *netdev_rx_claim(APTR arg, const UBYTE *hdr, UWORD frame_len,
                       APTR *token);
VOID netdev_rx_claimed(APTR arg, APTR token, ULONG sum, UBYTE summed);

/* Every request that stops being quick and enters a list must be prepared the
   same way, whether it goes at the head or tail. */
VOID netdev_queue_tail(struct List *list, struct IOSana2Req *io);
VOID netdev_queue_head(struct List *list, struct IOSana2Req *io);

/* netdev_event.c */
VOID netdev_event(NetdevUnit *unit, ULONG mask);
VOID netdev_event_wait(NetdevUnit *unit, struct IOSana2Req *io);
VOID netdev_event_rescan(NetdevUnit *unit);

/*
 * BOTH OF THESE RUN ONCE A FRAME AND EACH HAS EXACTLY ONE CALL SITE, and both
 * lived in netdev_event.c while that site is in netdev_device.c -- so whether
 * the shipped image pays a jsr for them was a decision LTO made, not one this
 * source stated.  netdev_io_is_raw() directly above has always been inline for
 * the same reason.
 *
 * tools/check-hot-calls.sh exists because that distinction cost a rig run
 * once: a helper the profiler names may already be inlined, and the only way
 * to tell is to disassemble a shipping-shaped build.  IT CANNOT ANSWER FOR
 * THIS FILE -- anxnet.device links with -flto and its symbol table collapses
 * to 136 entries with every local name gone, so there is nothing to count.
 * Where a gate cannot assert the property, the source states it.
 *
 * The test tier still calls both by name (test_netdev_event.c): a static
 * inline in the header is callable from there exactly as the extern was.
 */

/* The frame from byte 0 for a RAW request, the payload past the 14-byte
   Ethernet header otherwise.  The filter sees the same data CopyToBuff would
   (copybuff.spec autodoc). */
static inline const UBYTE *netdev_payload(const NetdevOpener *op,
                                          const struct IOSana2Req *io,
                                          const UBYTE *frame, UWORD len,
                                          ULONG *plen)
{
    if (netdev_io_is_raw(op, io))
    {
        *plen = len;
        return frame;
    }

    *plen = (ULONG)(len - NETDEV_HDR_LEN);
    return frame + NETDEV_HDR_LEN;
}

/* TRUE when the packet can be handed over.  The hook itself runs at interrupt
   level, in the middle of the card's own service, and the autodoc requires it:
   "This function must be callable from interupts." */
static inline BOOL netdev_filter_ok(NetdevOpener *op, struct IOSana2Req *io,
                                    const UBYTE *data)
{
    if (op->op_Filter == NULL)
        return TRUE;

    return netdev_hook_call(op->op_Filter, io, (APTR)data);
}

/* netdev_pcmcia.c: the slot has no autoconfig record, so it is claimed
   rather than found.  NULL when there is no slot, nothing in it, or what is
   in it is not a LAN card. */
VOID netdev_trace_val(const char *tag, ULONG v);
/*
 * Claim the slot, identify what is in it from its CIS, and configure it for
 * the row that drives that card.  *card_out is that row, set only on success.
 * Called once, not once per PCMCIA row: there is one slot and one handle.
 */
APTR netdev_pcmcia_claim(NetdevDevice *dev, const NetdevCard **card_out);
/* The card's own CIS bytes, for the derived-address fingerprint.  0 when
   there is no slot or nothing was read from it. */
UWORD netdev_pcmcia_fingerprint(UBYTE *buf, UWORD max);
VOID netdev_pcmcia_release(VOID);
VOID netdev_pcmcia_bind(NetdevUnit *unit);
BOOL netdev_pcmcia_is_unit(const NetdevUnit *unit);
BOOL netdev_pcmcia_available(const NetdevUnit *unit);
/* An explicit S2_OFFLINE while the socket is empty overrides hot-plug resume. */
VOID netdev_pcmcia_cancel_resume(const NetdevUnit *unit);
/* The status-change callback uses the same core service as a Zorro INT2
   server, but card.resource owns the PCMCIA interrupt latch. */
ULONG netdev_interrupt(NetdevUnit *unit);
/* Card removal has no hardware left to stop.  Drain the software side only. */
VOID netdev_pcmcia_detached(NetdevUnit *unit, ULONG event);
/* Re-establish the bus and chip half of an existing hot-plugged unit. */
BOOL netdev_pcmcia_reattach(NetdevUnit *unit, const NetdevCard *card, APTR base);
VOID netdev_tx_direct(NetdevUnit *unit, struct IOSana2Req *io);
VOID netdev_drop_writes(NetdevUnit *unit, NetdevOpener *op);
LONG netdev_online(NetdevUnit *unit);
VOID netdev_offline(NetdevUnit *unit, ULONG event);

/* netdev_cmds.c */
VOID netdev_perform(NetdevOpener *op, struct IOSana2Req *io);
/* The two bulk commands, reachable without the generic dispatch. */
VOID netdev_write_cmd(NetdevOpener *op, struct IOSana2Req *io, UWORD cmd);
/* The CMD_READ / S2_READORPHAN half of it, reachable without the dispatch. */
VOID netdev_queue_read(NetdevOpener *op, struct IOSana2Req *io, UWORD cmd);
BOOL netdev_abort(NetdevOpener *op, struct IOSana2Req *io);

/* netdev_io.c.  Exec calls BeginIO and AbortIO with the device base in a6 and
   the request in a1; no host compiler can honour that, and the host tier
   enters both of them the way Exec does, so the annotation is m68k-only and
   the source is the one that ships.  See test/test_netdev_beginio.c. */
#ifdef __mc68000__
#define NETDEV_REG_A6   __asm("a6")
#define NETDEV_REG_A1   __asm("a1")
#else
#define NETDEV_REG_A6
#define NETDEV_REG_A1
#endif

VOID netdev_begin_io(register struct Device     *dev NETDEV_REG_A6,
                     register struct IOSana2Req *io  NETDEV_REG_A1);
LONG netdev_abort_io(register struct Device     *dev NETDEV_REG_A6,
                     register struct IOSana2Req *io  NETDEV_REG_A1);

#endif /* AMINETXDUO_NETDEV_INTERNAL_H */
