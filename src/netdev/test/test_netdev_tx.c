/*
 * The write path and its pump, netdev_tx.c, against a fake ring.
 *
 * A BATCH WRITE (ANXD_S2F_TX_BATCH, aminetxduo/anxs2ext.h) is one CMD_WRITE
 * the device turns into several frames.  What can go wrong is ordering and
 * ownership: a ring that takes part of a batch must keep the request at the
 * head of the write queue and finish it before any later write, answer it
 * once, and an abort or close in the middle must answer it and keep nothing
 * of the opener's.  And the masked work: a pump pass runs with interrupts
 * off and must stop at NETDEV_PUMP_FRAMES whatever the ring has free, while
 * what it leaves still goes out, without the pump waking itself on a full
 * ring.  The fake core below is a ring of `ring_size` slots that records
 * every frame posted; the opener is a batch of numbered frames.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <proto/exec.h>

#include "netdev_internal.h"
#include "netdev_mcaf.h"
#include "dp8390.h"
#include "genet_ring.h"     /* the run's reclaim rule, as genet_tx_run */

static int failures;
static int checks;

static void expect(int ok, const char *what)
{
    checks++;
    if (ok)
        return;

    printf("FAIL %s\n", what);
    failures++;
}

/* ---------------------------------------------------------------- exec --- */

static int disable_depth;
static int forbid_depth;
static int forbids;
static int replies;
static int causes;

VOID Disable(VOID) { disable_depth++; }
VOID Enable(VOID)  { disable_depth--; }
VOID Forbid(VOID)  { forbid_depth++; forbids++; }
VOID Permit(VOID)  { forbid_depth--; }
VOID Cause(struct Interrupt *interrupt) { (VOID)interrupt; causes++; }

VOID ReplyMsg(struct Message *msg)
{
    replies++;
    if (msg->mn_Node.ln_Succ != NULL)
    {
        printf("FAIL ReplyMsg() on a node still linked into a list\n");
        failures++;
    }
    msg->mn_Node.ln_Type = NT_REPLYMSG;
}

VOID NewList(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

VOID AddTail(struct List *l, struct Node *n)
{
    n->ln_Succ              = (struct Node *)&l->lh_Tail;
    n->ln_Pred              = l->lh_TailPred;
    l->lh_TailPred->ln_Succ = n;
    l->lh_TailPred          = n;
}

VOID AddHead(struct List *l, struct Node *n)
{
    n->ln_Succ          = l->lh_Head;
    n->ln_Pred          = (struct Node *)&l->lh_Head;
    l->lh_Head->ln_Pred = n;
    l->lh_Head          = n;
}

VOID Remove(struct Node *n)
{
    n->ln_Pred->ln_Succ = n->ln_Succ;
    n->ln_Succ->ln_Pred = n->ln_Pred;
    n->ln_Succ = NULL;
    n->ln_Pred = NULL;
}

struct Node *RemHead(struct List *l)
{
    struct Node *n = l->lh_Head;

    if (n->ln_Succ == NULL)
        return NULL;
    Remove(n);
    return n;
}

/* --------------------------------------------------- the shell's others --- */

static int events;

VOID netdev_event(NetdevUnit *unit, ULONG mask)
{
    (VOID)unit;
    (VOID)mask;
    events++;
}

/* netdev_device.c's chip service, as the real one runs it: the core, then
   the pump when the core had work (`intr_mine`). */
static int intr_mine;
static int intr_calls;

ULONG netdev_interrupt(NetdevUnit *unit)
{
    intr_calls++;
    if (!intr_mine)
        return 0;
    netdev_tx_pump(unit);
    return 1;
}

/* netdev_hooks.c is m68k register glue: a C call stands in for it. */
BOOL netdev_copy_call(APTR fn, APTR to, APTR from, ULONG len)
{
    return ((BOOL (*)(APTR, APTR, ULONG))fn)(to, from, len);
}

/* ------------------------------------------------------- the fake ring --- */

#define RING_MAX    64
#define LOG_MAX     256
#define FRAME_MAX   256

/* Every frame the core posted: which write, which frame of it, the bytes. */
typedef struct
{
    ULONG   write;          /* the request's tag (ios2_PacketType)          */
    UWORD   index;          /* frame of a batch, 0xffff for an ordinary one */
    UWORD   len;
    UBYTE   bytes[FRAME_MAX];
} Posted;

static Posted  posted[LOG_MAX];
static int     nposted;
static UWORD   ring_size;
static UWORD   ring_held;   /* posted and not yet kicked                    */
static UWORD   ring_busy;   /* kicked, and the chip not done with them      */
static int     kicks;
static int     reclaims;
static int     flushes;
static int     run_calls;
static int     run_masked;  /* tx_run calls made with Disable() held        */
static int     max_masked_frames;
static int     masked_frames;   /* frames posted inside the current Disable */
static int     order[16];   /* 'f' flush, 'r' reclaim, in call order        */
static int     norder;
static UBYTE   slots[RING_MAX][2048];
static UBYTE   tx_buf[2048];
static int     flushed_before_reclaim;

static NetdevUnit   unit;
static NetdevOpener opener;

/* The EtherType the shell stored, as the shell stored it. */
static ULONG frame_tag(const UBYTE *frame)
{
    UWORD t;

    memcpy(&t, frame + 12, sizeof(t));
    return (ULONG)t;
}

static VOID note_masked(UWORD n)
{
    if (disable_depth > 0)
    {
        masked_frames += n;
        if (masked_frames > max_masked_frames)
            max_masked_frames = masked_frames;
    }
}

static UBYTE *t_tx_at(NetdevNic *nic)
{
    if (nic->txb_inuse >= ring_size)
        return NULL;
    return tx_buf;
}

static LONG t_tx(NetdevNic *nic, const UBYTE *frame, UWORD len)
{
    Posted *p;

    if (nic->txb_inuse >= ring_size)
        return DP8390_TX_BUSY;
    if (nposted < LOG_MAX)
    {
        p = &posted[nposted++];
        p->write = frame_tag(frame);
        p->index = 0xffff;
        p->len   = len;
        memcpy(p->bytes, frame, len < FRAME_MAX ? len : FRAME_MAX);
    }
    nic->txb_inuse++;
    ring_busy++;
    note_masked(1);
    kicks++;
    return 0;
}

static VOID t_flush(NetdevNic *nic)
{
    (VOID)nic;
    flushes++;
    if (norder < 16)
        order[norder++] = 'f';
    if (ring_held != 0)
    {
        kicks++;
        ring_held = 0;
    }
}

/* The chip retires what it has finished; held frames stay. */
static BOOL t_reclaim(NetdevNic *nic)
{
    UWORD before = nic->txb_inuse;

    reclaims++;
    if (norder < 16)
        order[norder++] = 'r';
    if (flushes != 0)
        flushed_before_reclaim = 1;
    nic->txb_inuse = (UWORD)(ring_held + ring_busy);
    return (BOOL)(nic->txb_inuse != before);
}

static UWORD t_run(NetdevNic *nic, UWORD first, UWORD count,
                   UWORD (*fill)(APTR arg, UWORD index, UBYTE *buf),
                   APTR arg, UBYTE *failed)
{
    UWORD room;
    UWORD done = 0;

    run_calls++;
    if (disable_depth > 0)
        run_masked++;
    *failed = 0;
    /* What genet_tx_run does before it counts its room. */
    if (genet_tx_run_reclaims(nic->txb_inuse, ring_size, count))
    {
        t_flush(nic);
        (VOID)t_reclaim(nic);
    }
    room = (UWORD)(ring_size - nic->txb_inuse);
    if (count > room)
        count = room;
    while (done < count)
    {
        UBYTE *buf = slots[nic->txb_inuse % RING_MAX];
        UWORD  len = fill(arg, (UWORD)(first + done), buf);
        Posted *p;

        if (len == 0)
        {
            *failed = 1;
            break;
        }
        if (nposted < LOG_MAX)
        {
            p = &posted[nposted++];
            p->write = frame_tag(buf);
            p->index = (UWORD)(first + done);
            p->len   = len;
            memcpy(p->bytes, buf, len < FRAME_MAX ? len : FRAME_MAX);
        }
        nic->txb_inuse++;
        ring_busy++;
        done++;
    }
    note_masked(done);
    if (done != 0)
        kicks++;            /* one kick for the run */
    return done;
}

static VOID t_stop(NetdevNic *nic) { (VOID)nic; }
static VOID t_reset(NetdevNic *nic) { (VOID)nic; }

static const struct NetdevNicOps t_ops = {
    .tx    = t_tx,
    .stop  = t_stop,
    .reset = t_reset,
};

/* ----------------------------------------------------- the fake opener --- */

/*
 * Each request's ios2_Data is one of these.  A batch's frame i is SegLen (or
 * LastLen) bytes of (tag, i, i, i...); `fail_at` refuses one frame's copy.
 */
typedef struct
{
    UWORD   count;          /* 0: an ordinary write                         */
    UWORD   seg_len;
    UWORD   last_len;
    UWORD   tag;
    int     fail_at;        /* -1: never                                    */
    int     answer_zero;    /* TxBatch answers 0                            */
    ULONG   posted;         /* the last TxPosted count                      */
    int     posts;
    int     copies[32];     /* TxChild calls per frame                      */
} Write;

static ULONG child_calls;

static UBYTE t_txflags(APTR data)
{
    const Write *w = (const Write *)data;

    return (UBYTE)((w->count != 0) ? ANXD_S2_TXF_BATCH : 0);
}

static ULONG t_txbatch(APTR data, AnxdS2TxBatchInfo *info)
{
    const Write *w = (const Write *)data;

    info->Count    = w->count;
    info->SegLen   = w->seg_len;
    info->LastLen  = w->last_len;
    info->Reserved = 0;
    return w->answer_zero ? 0 : w->count;
}

static VOID t_txposted(APTR data, ULONG count)
{
    Write *w = (Write *)data;

    w->posted = count;
    w->posts++;
}

static ULONG t_txchild(APTR data, ULONG index, UBYTE *dst, ULONG len)
{
    Write *w = (Write *)data;
    ULONG  i;

    child_calls++;
    if (index >= w->count || (int)index == w->fail_at)
        return 0;
    if (len != ((index + 1 == w->count) ? w->last_len : w->seg_len))
        return 0;
    w->copies[index]++;
    dst[0] = (UBYTE)w->tag;
    for (i = 1; i < len; i++)
        dst[i] = (UBYTE)index;
    return 1;
}

static BOOL t_copyfrom(APTR to, APTR from, ULONG len)
{
    const Write *w = (const Write *)from;

    memset(to, 0xC0 | (w->tag & 0x0f), len);
    return TRUE;
}

/* --------------------------------------------------------- the fixture --- */

#define REQS 12
static struct IOSana2Req req[REQS];
static Write             wr[REQS];

static void fixture(UWORD ring)
{
    memset(&unit, 0, sizeof(unit));
    memset(&opener, 0, sizeof(opener));
    memset(req, 0, sizeof(req));
    memset(wr, 0, sizeof(wr));
    memset(posted, 0, sizeof(posted));
    nposted = 0;
    ring_size = ring;
    ring_held = 0;
    ring_busy = 0;
    kicks = reclaims = flushes = run_calls = run_masked = 0;
    max_masked_frames = masked_frames = 0;
    norder = 0;
    flushed_before_reclaim = 0;
    disable_depth = forbid_depth = forbids = replies = causes = events = 0;
    child_calls = 0;
    intr_mine = intr_calls = 0;

    NewList(&unit.nu_Writes);
    NewList(&unit.nu_OpenerList);
    unit.nu_Online           = 1;
    unit.nu_Nic.running      = 1;
    unit.nu_Nic.ops          = &t_ops;
    unit.nu_Nic.txb_cnt      = ring;
    unit.nu_Nic.tx_at        = t_tx_at;
    unit.nu_Nic.tx_reclaim   = t_reclaim;
    unit.nu_Nic.tx_flush     = t_flush;
    unit.nu_Nic.tx_run       = t_run;
    unit.nu_Nic.tx_task_lock = 1;
    unit.nu_Nic.tx_short_build = 1;
    memcpy(unit.nu_Nic.mac, "\x02\x00\x00\x00\x00\x01", 6);

    opener.op_Hw         = &unit;
    opener.op_CopyFrom   = (APTR)t_copyfrom;
    opener.op_TxFlags    = (APTR)t_txflags;
    opener.op_TxBatch    = (APTR)t_txbatch;
    opener.op_TxChild    = (APTR)t_txchild;
    opener.op_TxPosted   = (APTR)t_txposted;
    opener.op_Extensions = ANXD_S2F_TX_BATCH | ANXD_S2F_TX_QUICK;
}

/* Request n: a batch of `count` frames (0: an ordinary 100-byte write). */
static struct IOSana2Req *mk(int n, UWORD count)
{
    struct IOSana2Req *io = &req[n];
    Write             *w  = &wr[n];

    w->count    = count;
    w->seg_len  = 100;
    w->last_len = 60;
    w->tag      = (UWORD)(0x40 + n);
    w->fail_at  = -1;

    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    io->ios2_Req.io_Command = CMD_WRITE;
    io->ios2_Req.io_Flags   = IOF_QUICK;
    io->ios2_PacketType     = (ULONG)w->tag;
    io->ios2_DataLength     = 100;
    io->ios2_Data           = w;
    io->ios2_BufferManagement = &opener;
    memcpy(io->ios2_DstAddr, "\x02\x00\x00\x00\x00\x02", 6);
    return io;
}

/* The chip finishes everything kicked. */
static void drain_ring(void)
{
    ring_busy = 0;
    unit.nu_Nic.txb_inuse = ring_held;
}

/* One masked section that pumps once, as the write path's own runs it. */
static void pump_pass(void)
{
    masked_frames = 0;
    Disable();
    netdev_tx_section(&unit);
    netdev_tx_pump(&unit);
    Enable();
}

/* The software interrupt's section: the service and the owed pass. */
static void soft_pass(void)
{
    masked_frames = 0;
    max_masked_frames = 0;
    Disable();
    (VOID)netdev_tx_service(&unit);
    Enable();
}

/* Frames posted for write n, in posting order, were exactly 0..count-1. */
static int frames_in_order(int n, int from, int to)
{
    int   i;
    UWORD next = 0;

    for (i = from; i < to; i++)
    {
        if (posted[i].write != (ULONG)wr[n].tag)
            return 0;
        if (posted[i].index != next)
            return 0;
        next++;
    }
    return next == wr[n].count;
}

/* ---------------------------------------------------------------- tests --- */

static void test_one_batch(void)
{
    struct IOSana2Req *io;
    int                i, ok = 1;

    printf("netdev tx: one batch write, N frames, one Forbid, one answer\n");
    fixture(64);
    io = mk(0, 5);
    netdev_tx_direct(&unit, io);

    expect(nposted == 5 && frames_in_order(0, 0, 5), "five frames, in order");
    for (i = 0; i < 5; i++)
    {
        const Posted *p = &posted[i];
        UWORD want = (UWORD)(14 + (i == 4 ? 60 : 100));

        if (p->len != want || memcmp(p->bytes, io->ios2_DstAddr, 6) != 0 ||
            memcmp(p->bytes + 6, unit.nu_Nic.mac, 6) != 0 ||
            p->bytes[14] != wr[0].tag || p->bytes[15] != (UBYTE)i ||
            p->bytes[want - 1] != (UBYTE)i || wr[0].copies[i] != 1)
            ok = 0;
    }
    expect(ok, "each its own length, link header and payload, copied once");
    expect(forbids == 1, "one Forbid() for the batch");
    expect(run_calls == 1 && kicks == 1, "one run, one kick");
    expect(run_masked == 0, "the copies run under Forbid(), not Disable()");
    expect(replies == 0 && io->ios2_Req.io_Error == 0 &&
           (io->ios2_Req.io_Flags & IOF_QUICK) != 0,
           "answered once, quick, no error");
    expect(wr[0].posted == 5 && wr[0].posts == 1,
           "the committed prefix reported once: five");
    expect(unit.nu_Stats.PacketsSent == 5, "five frames counted, not one");
    expect(unit.nu_TxBatchIo == NULL && unit.nu_TxBatches == 1,
           "and no batch left in hand");
}

/* Three slots free for a five-frame batch, a later write behind it, then
   the ring empties: the rest of the batch, then the later write. */
static void test_partial_then_resume(void)
{
    struct IOSana2Req *io, *later;

    printf("netdev tx: a ring that takes part of a batch pauses it at the head\n");
    fixture(8);
    unit.nu_Nic.txb_inuse = 5;
    ring_busy = 5;              /* kicked, the chip not done with them */
    io = mk(0, 5);
    netdev_tx_direct(&unit, io);

    expect(nposted == 3 && posted[2].index == 2, "three frames now");
    expect(wr[0].posted == 3, "and three reported committed");
    expect(replies == 0 && (io->ios2_Req.io_Flags & IOF_QUICK) == 0,
           "not answered: queued, IOF_QUICK cleared");
    expect(unit.nu_Writes.lh_Head == &io->ios2_Req.io_Message.mn_Node &&
           unit.nu_TxBatchIo == io && unit.nu_TxBatchNext == 3,
           "at the head of the queue, its cursor at frame 3");

    later = mk(1, 0);
    netdev_tx_direct(&unit, later);
    expect(unit.nu_Writes.lh_Head == &io->ios2_Req.io_Message.mn_Node &&
           unit.nu_Writes.lh_TailPred == &later->ios2_Req.io_Message.mn_Node,
           "a later write queues behind it");
    expect(nposted == 3, "and nothing passes it");

    drain_ring();
    pump_pass();
    expect(nposted == 6 && posted[3].index == 3 && posted[4].index == 4 &&
           posted[5].index == 0xffff && posted[5].write == wr[1].tag,
           "the pump: frames 3 and 4, then the later write");
    expect(frames_in_order(0, 0, 5) && wr[0].copies[0] == 1 &&
           wr[0].copies[3] == 1 && wr[0].copies[4] == 1,
           "every frame once, none twice");
    expect(replies == 2 && io->ios2_Req.io_Error == 0 &&
           io->ios2_Req.io_Message.mn_Node.ln_Type == NT_REPLYMSG,
           "the batch answered once, after its last frame, and the write");
    expect(wr[0].posted == 5 && wr[0].posts == 2,
           "the prefix reported per run: 3, then 5");
    expect(unit.nu_TxBatchIo == NULL && IsListEmpty(&unit.nu_Writes),
           "nothing left");
    expect(unit.nu_Stats.PacketsSent == 6, "six frames counted");
}

/* A full ring: the batch queues with nothing copied, and pumping a full
   ring neither copies, nor spins, nor wakes itself. */
static void test_ring_full(void)
{
    struct IOSana2Req *io, *later;
    int                i;

    printf("netdev tx: a full ring takes nothing and wakes nobody\n");
    fixture(4);
    unit.nu_Nic.txb_inuse = 4;
    ring_busy = 4;
    io = mk(0, 3);
    netdev_tx_direct(&unit, io);
    later = mk(1, 0);
    netdev_tx_direct(&unit, later);

    expect(nposted == 0 && child_calls == 0, "k = 0: no frame, no copy");
    expect(unit.nu_Writes.lh_Head == &io->ios2_Req.io_Message.mn_Node,
           "the batch waits first");
    for (i = 0; i < 10; i++)
        pump_pass();
    expect(nposted == 0 && causes == 0 && unit.nu_PumpOwed == 0,
           "ten passes on a full ring: nothing posted, no self-signal");
    netdev_tx_section(&unit);
    expect(!netdev_tx_tick(&unit), "and the blank's backstop stays out");

    drain_ring();
    netdev_tx_section(&unit);
    expect(netdev_tx_tick(&unit) && nposted == 4 &&
           frames_in_order(0, 0, 3) && posted[3].write == wr[1].tag,
           "the ring free: the blank sends the batch, then the write");
    expect(replies == 2 && causes == 0, "two answers, still no self-signal");
}

/* Run frames the core is holding are kicked before a full ring is
   reclaimed: held, they are never retired. */
static void test_flush_before_reclaim(void)
{
    struct IOSana2Req *io;

    printf("netdev tx: held frames are kicked before the ring is called full\n");
    fixture(4);
    unit.nu_Nic.txb_inuse = 4;
    ring_held = 4;              /* a run's frames, unkicked */
    io = mk(0, 2);
    netdev_tx_direct(&unit, io);
    expect(norder >= 2 && order[0] == 'f' && order[1] == 'r',
           "flush, then reclaim");
    expect(nposted == 2 && replies == 0 &&
           (io->ios2_Req.io_Flags & IOF_QUICK) != 0,
           "and the batch goes out at once");
}

/* A ring out of phase with the batches: 49 of 64 in use, all of them sent
   by the chip but not yet retired, and a 16-frame batch.  The run asks
   first and takes all sixteen; it does not pause with one left. */
static void test_out_of_phase(void)
{
    struct IOSana2Req *io;

    printf("netdev tx: a batch larger than the free slots reclaims first\n");
    fixture(64);
    unit.nu_Nic.txb_inuse = 49;
    ring_busy = 0;              /* sent; only the retirement is owed */
    io = mk(0, 16);
    netdev_tx_direct(&unit, io);
    expect(nposted == 16 && frames_in_order(0, 0, 16) && reclaims >= 1,
           "all sixteen frames, after one reclaim");
    expect(unit.nu_TxBatchIo == NULL && unit.nu_TxBatchPauses == 0 &&
           (io->ios2_Req.io_Flags & IOF_QUICK) != 0,
           "never paused: answered quick");
}

/* 64 slots free and three 16-frame batches waiting: no pass copies more
   than NETDEV_PUMP_FRAMES, each cut owes one pass, and the owed passes
   finish the job in order. */
static void test_pump_budget(void)
{
    int n, passes = 0, worst = 0;

    printf("netdev tx: a pump pass copies at most %d frames\n",
           NETDEV_PUMP_FRAMES);
    fixture(64);
    for (n = 0; n < 3; n++)
        netdev_queue_tail(&unit.nu_Writes, mk(n, 16));

    pump_pass();
    expect(nposted == NETDEV_PUMP_FRAMES && max_masked_frames ==
           NETDEV_PUMP_FRAMES, "the first pass stops at the budget");
    expect(causes == 1 && unit.nu_PumpOwed == 1, "and owes one more");
    worst = max_masked_frames;

    while (unit.nu_PumpOwed && passes < 20)
    {
        soft_pass();
        if (max_masked_frames > worst)
            worst = max_masked_frames;
        passes++;
    }
    expect(worst <= NETDEV_PUMP_FRAMES, "no pass past the budget");
    expect(nposted == 48 && frames_in_order(0, 0, 16) &&
           frames_in_order(1, 16, 32) && frames_in_order(2, 32, 48),
           "all three batches, in order, every frame once");
    expect(passes == 5 && causes == 5,
           "six passes, five owed: one signal per cut, none after the last");
    expect(replies == 3, "three answers");
}

/* Leftovers after a cut, on a quiet wire with the owed pass lost: the
   blank's backstop sends them within one tick, without a new write or a
   received frame. */
static void test_tick_backstop(void)
{
    printf("netdev tx: the blank sends what a cut pass left\n");
    fixture(64);
    netdev_queue_tail(&unit.nu_Writes, mk(0, 12));
    pump_pass();
    expect(nposted == NETDEV_PUMP_FRAMES && !IsListEmpty(&unit.nu_Writes),
           "cut at the budget, the batch left at the head");
    unit.nu_PumpOwed = 0;       /* the software interrupt never came */
    drain_ring();
    expect(unit.nu_Nic.txb_inuse == 0, "an empty ring, nothing to retire");
    Disable();
    netdev_tx_section(&unit);
    netdev_tx_blank(&unit);
    Enable();
    expect(nposted > NETDEV_PUMP_FRAMES, "the blank pumps");
    expect(nposted == 12 && frames_in_order(0, 0, 12) && replies == 1,
           "and the batch is out and answered");
}

/*
 * One budget per masked section, not per pump: the software interrupt's
 * service pumps and then runs the owed pass, the blank pumps and may run
 * the chip's service as well, and ANXD_CMD_RX_POLL runs the service.  None
 * of them copies more than NETDEV_PUMP_FRAMES in all.
 */
static void test_section_budget(void)
{
    int n;

    printf("netdev tx: every masked section copies at most %d frames\n",
           NETDEV_PUMP_FRAMES);

    /* The soft interrupt: the chip had work, and a pass was owed. */
    fixture(64);
    for (n = 0; n < 3; n++)
        netdev_queue_tail(&unit.nu_Writes, mk(n, 16));
    intr_mine = 1;
    unit.nu_PumpOwed = 1;
    soft_pass();
    expect(intr_calls == 1 && nposted == NETDEV_PUMP_FRAMES &&
           max_masked_frames == NETDEV_PUMP_FRAMES,
           "soft: the service and the owed pass share eight");
    expect(unit.nu_PumpOwed == 1 && causes == 1,
           "and the rest is owed to the next section");

    /* The blank: its pump, then the chip's service for a silent line. */
    fixture(64);
    for (n = 0; n < 3; n++)
        netdev_queue_tail(&unit.nu_Writes, mk(n, 16));
    intr_mine = 1;
    masked_frames = max_masked_frames = 0;
    Disable();
    netdev_tx_section(&unit);
    netdev_tx_blank(&unit);
    (VOID)netdev_interrupt(&unit);
    Enable();
    expect(nposted == NETDEV_PUMP_FRAMES &&
           max_masked_frames == NETDEV_PUMP_FRAMES,
           "blank: its pump and the service share eight");

    /* RX_POLL is the service under its own Disable(). */
    fixture(64);
    for (n = 0; n < 3; n++)
        netdev_queue_tail(&unit.nu_Writes, mk(n, 16));
    intr_mine = 1;
    soft_pass();
    expect(nposted == NETDEV_PUMP_FRAMES, "poll: eight");
}

/* Writes that fail cost the budget too: twelve malformed batches are not
   twelve refusals in one masked pass. */
static void test_failures_spend_budget(void)
{
    int n;

    printf("netdev tx: refused writes are bounded per pass\n");
    fixture(64);
    for (n = 0; n < REQS; n++)
    {
        netdev_queue_tail(&unit.nu_Writes, mk(n, 3));
        wr[n].last_len = 20;            /* under the minimum: refused */
    }
    pump_pass();
    expect(nposted == 0 && events == NETDEV_PUMP_FRAMES &&
           replies == NETDEV_PUMP_FRAMES,
           "eight refused in the pass, the rest left");
    expect(causes == 1 && !IsListEmpty(&unit.nu_Writes),
           "and one pass owed for them");

    fixture(64);
    for (n = 0; n < REQS; n++)
    {
        netdev_queue_tail(&unit.nu_Writes, mk(n, 3));
        wr[n].fail_at = 0;              /* the first copy fails */
    }
    pump_pass();
    expect(nposted == 0 && replies == NETDEV_PUMP_FRAMES &&
           child_calls == NETDEV_PUMP_FRAMES,
           "eight copy failures in the pass, not twelve");
}

/* CloseDevice() with a batch paused at the head: answered aborted, once,
   and the pump never comes back to it. */
static void test_close_mid_batch(void)
{
    struct IOSana2Req *io;

    printf("netdev tx: a batch closed part way\n");
    fixture(4);
    unit.nu_Nic.txb_inuse = 2;
    ring_busy = 2;
    io = mk(0, 5);
    netdev_tx_direct(&unit, io);
    expect(nposted == 2 && unit.nu_TxBatchIo == io, "two out, paused");

    netdev_drop_writes(&unit, &opener);
    expect(replies == 1 && io->ios2_Req.io_Error == IOERR_ABORTED,
           "answered IOERR_ABORTED, once");
    expect(unit.nu_TxBatchIo == NULL && IsListEmpty(&unit.nu_Writes),
           "the device holds nothing of it");
    drain_ring();
    pump_pass();
    expect(nposted == 2 && replies == 1 && wr[0].copies[2] == 0,
           "and nothing more is copied from it");
}

/* A frame the opener cannot copy ends the batch with an error: the frames
   before it are out, the request answered once. */
static void test_copy_fails(void)
{
    struct IOSana2Req *io;

    printf("netdev tx: a batch whose copy fails part way\n");
    fixture(64);
    io = mk(0, 5);
    wr[0].fail_at = 2;
    netdev_tx_direct(&unit, io);
    expect(nposted == 2 && io->ios2_Req.io_Error == S2ERR_NO_RESOURCES &&
           io->ios2_WireError == S2WERR_BUFF_ERROR && events == 1,
           "two out, answered S2ERR_NO_RESOURCES");
    expect(unit.nu_TxBatchIo == NULL && unit.nu_Stats.PacketsSent == 2,
           "nothing held, two frames counted");
}

/* A shape outside the contract is refused before anything is copied. */
static void test_bad_shape(void)
{
    struct IOSana2Req *io;

    printf("netdev tx: a batch shape outside the contract\n");
    fixture(64);
    io = mk(0, (UWORD)(ANXD_S2_TX_BATCH_MAX + 1));
    netdev_tx_direct(&unit, io);
    expect(nposted == 0 && child_calls == 0 &&
           io->ios2_Req.io_Error == S2ERR_BAD_ARGUMENT,
           "seventeen frames: refused, nothing copied");
    /* A flagged write whose TxBatch answers 0 is malformed, not an
       ordinary write: nothing of it goes out (anxs2ext.h). */
    fixture(64);
    io = mk(0, 3);
    wr[0].answer_zero = 1;
    netdev_tx_direct(&unit, io);
    expect(nposted == 0 && child_calls == 0 &&
           io->ios2_Req.io_Error == S2ERR_BAD_ARGUMENT,
           "TxBatch answering 0: refused, nothing sent");
    fixture(64);
    io = mk(0, 3);
    wr[0].last_len = 20;
    netdev_tx_direct(&unit, io);
    expect(nposted == 0 && io->ios2_Req.io_Error == S2ERR_BAD_ARGUMENT,
           "a frame under the Ethernet minimum: refused");
}

/* An ordinary write, from an opener with batches or without: one frame,
   built by S2_CopyFromBuff, as before. */
static void test_ordinary_unchanged(void)
{
    struct IOSana2Req *io;
    int                arm;

    printf("netdev tx: an ordinary write is the one frame it was\n");
    for (arm = 0; arm < 2; arm++)
    {
        fixture(64);
        if (arm)
        {
            opener.op_Extensions = ANXD_S2F_TX_QUICK;
            opener.op_TxBatch = opener.op_TxChild = NULL;
        }
        io = mk(0, 0);
        netdev_tx_direct(&unit, io);
        expect(nposted == 1 && posted[0].len == 114 &&
               posted[0].index == 0xffff && posted[0].bytes[14] == 0xC0 &&
               run_calls == 0 && replies == 0 && io->ios2_Req.io_Error == 0,
               arm ? "no batches agreed: one frame by CopyFromBuff"
                   : "batches agreed: one frame by CopyFromBuff");
    }

    /* And a batch-flagged write from an opener that was not granted the
       bit is an ordinary write of ios2_DataLength. */
    fixture(64);
    opener.op_Extensions = ANXD_S2F_TX_QUICK | ANXD_S2F_TX_CSUM_TCP;
    io = mk(0, 4);
    netdev_tx_direct(&unit, io);
    expect(nposted == 1 && run_calls == 0 && child_calls == 0,
           "no TX_BATCH granted: the flag means nothing");
}

/* The ordinary pump pass is under the same budget. */
static void test_ordinary_budget(void)
{
    int n;

    printf("netdev tx: ordinary writes are under the pump budget too\n");
    fixture(64);
    for (n = 0; n < REQS; n++)
        netdev_queue_tail(&unit.nu_Writes, mk(n, 0));
    pump_pass();
    expect(nposted == NETDEV_PUMP_FRAMES && causes == 1,
           "eight of twelve, one more pass owed");
}

int main(void)
{
    test_one_batch();
    test_partial_then_resume();
    test_ring_full();
    test_flush_before_reclaim();
    test_out_of_phase();
    test_pump_budget();
    test_tick_backstop();
    test_section_budget();
    test_failures_spend_budget();
    test_close_mid_batch();
    test_copy_fails();
    test_bad_shape();
    test_ordinary_unchanged();
    test_ordinary_budget();

    printf("%d checks, %d failures, %s\n", checks, failures,
           failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
