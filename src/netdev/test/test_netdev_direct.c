/*
 * anxnet.device's private SANA-II single-copy receive transaction, on host.
 *
 * The chip test proves bytes leave the FIFO once.  This side proves the same
 * frame claims only an eligible CMD_READ, carries the normal SANA-II metadata
 * and counters, and puts the request back untouched on every decline -- the
 * two-taker walk, RAW on the opener or the request, a filter hook, and the
 * stack refusing a destination.  There is no unclaim to test: a claim commits
 * once the drain begins (netdev_internal.h states why).
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <exec/tasks.h>
#include <proto/exec.h>

#include "netdev_internal.h"
#include "aminetxduo/anxs2ext.h"

static int failures;
static int replies;

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    if (got == want)
        return;
    printf("FAIL %s: got 0x%lx, want 0x%lx\n", what, got, want);
    failures++;
}

static void expect_ptr(const char *what, const void *got, const void *want)
{
    if (got == want)
        return;
    printf("FAIL %s: got %p, want %p\n", what, got, want);
    failures++;
}

static void expect_mem(const char *what, const void *got, const void *want,
                       size_t len)
{
    if (memcmp(got, want, len) == 0)
        return;
    printf("FAIL %s\n", what);
    failures++;
}

/* ------------------------------------------------------------- exec lists */

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

/*
 * IS THIS NODE STILL ON A LIST -- ASKED OF THE LISTS, NOT OF THE NODE.
 *
 * The stub above NULLs both links, which Exec's Remove() does NOT, and the
 * reply check below used to read `ln_Succ != NULL` as "still linked".  That
 * asserted a property of the stub: netdev_take() now unlinks through
 * nd_remove() (netdev_internal.h), which also clears the removed links,
 * as Exec does, and a check written that way calls a correct removal a
 * failure.
 *
 * What the test means is that the request is off the queue it was taken from,
 * so that is what is asked.
 */
static const struct List *watch_lists[4];
static unsigned          watch_n;

static int node_on_a_watched_list(const struct Node *n)
{
    unsigned i;

    for (i = 0; i < watch_n; i++)
    {
        const struct Node *p;

        for (p = watch_lists[i]->lh_Head; p != NULL && p->ln_Succ != NULL;
             p = p->ln_Succ)
        {
            if (p == n)
                return 1;
        }
    }

    return 0;
}

VOID Disable(VOID) {}
VOID Enable(VOID) {}

VOID ReplyMsg(struct Message *msg)
{
    replies++;
    if (node_on_a_watched_list(&msg->mn_Node))
    {
        printf("FAIL replied CMD_READ is still linked\n");
        failures++;
    }
    msg->mn_Node.ln_Type = NT_REPLYMSG;
}

VOID netdev_reply(struct IOSana2Req *io, LONG err, ULONG wire)
{
    io->ios2_Req.io_Error = (BYTE)err;
    io->ios2_WireError = wire;
    if ((io->ios2_Req.io_Flags & IOF_QUICK) == 0)
        ReplyMsg(&io->ios2_Req.io_Message);
}

/* The shell calls the opener's S2_CopyToBuff through a register shim; the
   host test's hook is a plain C function. */
BOOL netdev_copy_call(APTR fn, APTR to, APTR from, ULONG len)
{
    return ((BOOL (*)(APTR, APTR, ULONG))fn)(to, from, len);
}

/* --------------------------------------------------------------- fixture */

static NetdevUnit unit;
static NetdevOpener opener_a;
static NetdevOpener opener_b;
static struct IOSana2Req read_a;
static struct IOSana2Req read_b;
/* The link header lands in front of what RX_DIRECT answers, so the fixture
   has to have something in front of it to land in.  direct_area is the whole
   allocation; direct_buffer is the payload the hook returns. */
static UBYTE direct_area[NETDEV_HDR_LEN + NETDEV_MTU];
#define direct_buffer (direct_area + NETDEV_HDR_LEN)
static UBYTE data_cookie;

static ULONG direct_len;
static APTR direct_data;
static int direct_calls;
static int direct_accept;

static ULONG filled_len;
static ULONG filled_sum;
static APTR filled_data;
static UBYTE filled_summed;
static int filled_calls;

static UBYTE *rx_direct(APTR data, ULONG len)
{
    direct_calls++;
    direct_data = data;
    direct_len = len;
    return direct_accept ? direct_buffer : NULL;
}

static VOID rx_filled(APTR data, ULONG len, ULONG sum, UBYTE summed)
{
    filled_calls++;
    filled_data = data;
    filled_len = len;
    filled_sum = sum;
    filled_summed = summed;
}

static void init_opener(NetdevOpener *op)
{
    memset(op, 0, sizeof(*op));
    op->op_Hw = &unit;
    op->op_RxDirect = (APTR)rx_direct;
    op->op_RxFilled = (APTR)rx_filled;
    NewList(&op->op_Reads);
    NewList(&op->op_Orphans);
    NewList(&op->op_Events);
}

static void reset_fixture(void)
{
    memset(&unit, 0, sizeof(unit));
    memset(&read_a, 0, sizeof(read_a));
    memset(&read_b, 0, sizeof(read_b));
    memset(direct_area, 0, sizeof(direct_area));
    NewList(&unit.nu_OpenerList);
    NewList(&unit.nu_Writes);

    watch_n = 0;
    watch_lists[watch_n++] = &opener_a.op_Reads;
    watch_lists[watch_n++] = &opener_a.op_Orphans;
    watch_lists[watch_n++] = &opener_b.op_Reads;
    watch_lists[watch_n++] = &opener_b.op_Orphans;
    init_opener(&opener_a);
    init_opener(&opener_b);
    AddTail(&unit.nu_OpenerList, (struct Node *)&opener_a.op_Node);
    AddTail(&unit.nu_OpenerList, (struct Node *)&opener_b.op_Node);

    direct_len = filled_len = filled_sum = 0;
    direct_data = filled_data = NULL;
    direct_calls = filled_calls = replies = 0;
    direct_accept = 1;
    filled_summed = 0;
}

static void queue_read(NetdevOpener *op, struct IOSana2Req *io, ULONG type)
{
    io->ios2_Req.io_Unit = &unit.nu_ExecUnit;
    io->ios2_BufferManagement = op;
    io->ios2_PacketType = type;
    io->ios2_Data = &data_cookie;
    AddTail(&op->op_Reads, &io->ios2_Req.io_Message.mn_Node);
}

static unsigned list_count(const struct List *l)
{
    const struct Node *n;
    unsigned count = 0;

    for (n = l->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        count++;
    return count;
}

static void make_header(UBYTE *hdr, ULONG type)
{
    static const UBYTE dst[6] = { 0x00, 0x80, 0x10, 0x11, 0x22, 0x33 };
    static const UBYTE src[6] = { 0x02, 0x60, 0x8c, 0x44, 0x55, 0x66 };
    UWORD wire_type = (UWORD)type;

    memset(hdr, 0, NETDEV_HDR_LEN);
    memcpy(hdr, dst, sizeof(dst));
    memcpy(hdr + 6, src, sizeof(src));
    memcpy(hdr + 12, &wire_type, sizeof(wire_type));
}

/* --------------------------------------------------------------- cases --- */

/*
 * THE LINK HEADER, WRITTEN BY THE DEVICE INSTEAD OF REBUILT BY THE OPENER.
 *
 * Both halves matter and the second is the one that keeps a third-party
 * driver honest: with the tag accepted the fourteen bytes in front of the
 * payload must BE the frame's header, and WITHOUT it they must be untouched,
 * because the opener is still going to synthesise them there and anything
 * this wrote would be overwritten -- or worse, trusted.
 */
static void test_link_header(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR  token = NULL;
    UBYTE *dst;

    reset_fixture();
    make_header(hdr, 0x0800);
    queue_read(&opener_a, &read_a, 0x0800);
    opener_a.op_RxLinkHdr = TRUE;

    dst = netdev_rx_claim(&unit, hdr, 60, &token, NULL);
    expect_ptr("link header: accepted", dst, direct_buffer);
    expect_mem("link header written in front of the payload",
               direct_buffer - NETDEV_HDR_LEN, hdr, NETDEV_HDR_LEN);

    /* And the request fields are still filled: SANA-II promises them whatever
       the private tag did, and an opener may read either. */
    expect_mem("destination address still filled", read_a.ios2_DstAddr, hdr, 6);
    expect_mem("source address still filled", read_a.ios2_SrcAddr, hdr + 6, 6);

    /* A driver that never answered the tag must not write there. */
    reset_fixture();
    make_header(hdr, 0x0800);
    queue_read(&opener_a, &read_a, 0x0800);
    opener_a.op_RxLinkHdr = FALSE;

    dst = netdev_rx_claim(&unit, hdr, 60, &token, NULL);
    expect_ptr("no tag: accepted", dst, direct_buffer);
    {
        UBYTE zero[NETDEV_HDR_LEN];
        memset(zero, 0, sizeof(zero));
        expect_mem("no tag: nothing written in front of the payload",
                   direct_buffer - NETDEV_HDR_LEN, zero, NETDEV_HDR_LEN);
    }
}


static void test_claim_complete(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR token = NULL;
    UBYTE *dst;
    UBYTE wanted = 0xff;

    reset_fixture();
    make_header(hdr, 0x0800);
    queue_read(&opener_a, &read_a, 0x0800);
    opener_a.op_ReadTypeCount = 1;
    opener_a.op_ReadTypes[0] = 0x0800;
    opener_a.op_TrackHigh = 1;
    opener_a.op_Track[0].used = 1;
    opener_a.op_Track[0].type = 0x0800;
    opener_a.op_RxFlags = ANXD_S2_RXF_VERIFIED;

    dst = netdev_rx_claim(&unit, hdr, 60, &token, &wanted);
    expect_ptr("accepted destination", dst, direct_buffer);
    expect_ptr("claim token", token, &read_a);
    expect_u32("one direct callback", direct_calls, 1);
    expect_ptr("direct ios2_Data", direct_data, &data_cookie);
    expect_u32("direct payload length", direct_len, 46);
    expect_u32("request removed while claimed", list_count(&opener_a.op_Reads), 0);
    expect_u32("request payload length", read_a.ios2_DataLength, 46);
    expect_mem("destination address", read_a.ios2_DstAddr, hdr, 6);
    expect_mem("source address", read_a.ios2_SrcAddr, hdr + 6, 6);
    expect_u32("claim returns negotiated receive flags", wanted,
               ANXD_S2_RXF_VERIFIED);

    netdev_rx_claimed(&unit, token, 0x12345678UL,
                      ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED);
    expect_u32("one filled callback", filled_calls, 1);
    expect_ptr("filled ios2_Data", filled_data, &data_cookie);
    expect_u32("filled payload length", filled_len, 46);
    expect_u32("filled checksum", filled_sum, 0x12345678UL);
    expect_u32("filled checksum and verdict", filled_summed,
               ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED);
    expect_u32("verified direct frame counted", unit.nu_Nic.rx_verified, 1);
    expect_u32("one reply", replies, 1);
    expect_u32("unit packets", unit.nu_Stats.PacketsReceived, 1);
    expect_u32("direct packets", unit.nu_RxDirect, 1);
    expect_u32("tracked packets", opener_a.op_Track[0].st.PacketsReceived, 1);
    expect_u32("tracked wire bytes", opener_a.op_Track[0].st.BytesReceived, 60);
}

static void test_raw_request_restored(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR token = NULL;

    /* RAW as the per-request flag, not the opener property: the claim only
       sees it after taking the request off its queue, so this is the decline
       that must put the very same request back at the head. */
    reset_fixture();
    make_header(hdr, 0x0806);
    queue_read(&opener_a, &read_a, 0x0806);
    read_a.ios2_Req.io_Flags |= SANA2IOF_RAW;
    expect_ptr("raw request declines", netdev_rx_claim(&unit, hdr, 64, &token, NULL),
               NULL);
    expect_u32("raw decline restores one reader",
               list_count(&opener_a.op_Reads), 1);
    expect_ptr("raw decline restores same reader", opener_a.op_Reads.lh_Head,
               &read_a.ios2_Req.io_Message.mn_Node);
    expect_u32("raw decline never asks the stack", direct_calls, 0);
    expect_u32("raw decline does not fill", filled_calls, 0);
    expect_u32("raw decline does not reply", replies, 0);
    expect_u32("raw decline does not count", unit.nu_Stats.PacketsReceived, 0);
}

static void test_declines_unsafe_claims(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR token = NULL;

    make_header(hdr, 0x86dd);

    reset_fixture();
    queue_read(&opener_a, &read_a, 0x86dd);
    queue_read(&opener_b, &read_b, 0x86dd);
    expect_ptr("two readers decline", netdev_rx_claim(&unit, hdr, 80, &token, NULL),
               NULL);
    expect_u32("first reader retained", list_count(&opener_a.op_Reads), 1);
    expect_u32("second reader retained", list_count(&opener_b.op_Reads), 1);
    expect_u32("no callback for two readers", direct_calls, 0);

    reset_fixture();
    queue_read(&opener_a, &read_a, 0x86dd);
    opener_a.op_Raw = 1;
    expect_ptr("raw reader declines", netdev_rx_claim(&unit, hdr, 80, &token, NULL),
               NULL);
    expect_u32("raw reader retained", list_count(&opener_a.op_Reads), 1);
    expect_u32("raw reader never asked", direct_calls, 0);

    reset_fixture();
    queue_read(&opener_a, &read_a, 0x86dd);
    opener_a.op_Filter = (APTR)&unit;
    expect_ptr("filtered reader declines",
               netdev_rx_claim(&unit, hdr, 80, &token, NULL), NULL);

    reset_fixture();
    queue_read(&opener_a, &read_a, 0x86dd);
    opener_a.op_RxFilled = NULL;
    expect_ptr("unpaired hooks decline",
               netdev_rx_claim(&unit, hdr, 80, &token, NULL), NULL);

    reset_fixture();
    queue_read(&opener_a, &read_a, 0x86dd);
    direct_accept = 0;
    expect_ptr("stack destination declines",
               netdev_rx_claim(&unit, hdr, 80, &token, NULL), NULL);
    expect_u32("declined destination restores reader",
               list_count(&opener_a.op_Reads), 1);
    expect_u32("declined destination asked once", direct_calls, 1);
}

static void test_other_type_does_not_block(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR token = NULL;

    reset_fixture();
    make_header(hdr, 0x0800);
    queue_read(&opener_a, &read_a, 0x0800);
    queue_read(&opener_b, &read_b, 0x0806);
    expect_ptr("different packet type does not block",
               netdev_rx_claim(&unit, hdr, 60, &token, NULL), direct_buffer);
    expect_ptr("right packet type claimed", token, &read_a);
    expect_u32("other reader retained", list_count(&opener_b.op_Reads), 1);
    netdev_rx_claimed(&unit, token, 0, 0);
}

static void test_behind_never_holds_a_second_opener(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR  token = NULL;

    reset_fixture();
    make_header(hdr, 0x0800);
    opener_a.op_ReadTypeCount = 1;
    opener_a.op_ReadTypes[0] = 0x0800;

    /* With just the direct opener, leaving the frame in a deep hardware ring
       is the lossless back-pressure contract. */
    Remove((struct Node *)&opener_b.op_Node);
    unit.nu_Openers = 1;
    expect_ptr("sole lagging opener may hold the ring",
               netdev_rx_claim(&unit, hdr, 60, &token, NULL), NULL);
    expect_ptr("sole lagging opener reports behind", token,
               NETDEV_CLAIM_BEHIND);

    /* A second opener makes that hold a unit-wide head-of-line stall.  Drop
       this unmatched frame and let the ring advance instead. */
    AddTail(&unit.nu_OpenerList, (struct Node *)&opener_b.op_Node);
    unit.nu_Openers = 2;
    token = NULL;
    expect_ptr("two openers do not hold the ring",
               netdev_rx_claim(&unit, hdr, 60, &token, NULL), NULL);
    expect_ptr("two openers do not report behind", token, NULL);
}

static void test_broadcast_metadata(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR token = NULL;

    reset_fixture();
    make_header(hdr, 0x0800);
    memset(hdr, 0xff, 6);
    queue_read(&opener_a, &read_a, 0x0800);
    read_a.ios2_Req.io_Flags = SANA2IOF_MCAST;
    expect_ptr("broadcast claim", netdev_rx_claim(&unit, hdr, 60, &token, NULL),
               direct_buffer);
    expect_u32("broadcast flag replaces multicast",
               read_a.ios2_Req.io_Flags & (SANA2IOF_BCAST | SANA2IOF_MCAST),
               SANA2IOF_BCAST);
    netdev_rx_claimed(&unit, token, 0, 0);
}

/* ---------------------------------------------------- ANXD_CMD_RX_BATCH -- */

/* Each cookie is its own slot with its own buffer, the way the stack's
   AmiRxSlot is; RxDirect returns the slot's payload area and RxFilled
   records what it was told.  A copy hook fills a slot from a staged frame. */
#define BATCH_SLOTS 4
typedef struct BatchSlot
{
    UBYTE area[NETDEV_HDR_LEN + NETDEV_MTU];
    ULONG filled_len;
    ULONG filled_sum;
    UBYTE filled_flags;
    int   filled_calls;
    int   armed;
} BatchSlot;

static BatchSlot batch_slot[BATCH_SLOTS];
static union
{
    AnxdS2RxBatch b;
    UBYTE         bytes[sizeof(AnxdS2RxBatch) + BATCH_SLOTS * sizeof(APTR)];
} batch_rec;
static struct IOSana2Req batch_req;
static int batch_direct_calls;
static int batch_copy_calls;

static UBYTE *batch_rx_direct(APTR data, ULONG len)
{
    BatchSlot *sl = (BatchSlot *)data;

    batch_direct_calls++;
    if (!sl->armed || len > NETDEV_MTU)
        return NULL;
    return sl->area + NETDEV_HDR_LEN;
}

static VOID batch_rx_filled(APTR data, ULONG len, ULONG sum, UBYTE flags)
{
    BatchSlot *sl = (BatchSlot *)data;

    sl->filled_calls++;
    sl->filled_len   = len;
    sl->filled_sum   = sum;
    sl->filled_flags = flags;
}

static BOOL batch_copy_to(APTR to, APTR from, ULONG len)
{
    BatchSlot *sl = (BatchSlot *)to;

    batch_copy_calls++;
    if (!sl->armed)
        return FALSE;
    memcpy(sl->area + NETDEV_HDR_LEN, from, len);
    return TRUE;
}

static void queue_batch(NetdevOpener *op, ULONG type, UWORD count)
{
    unsigned i;

    memset(&batch_req, 0, sizeof(batch_req));
    memset(&batch_rec, 0, sizeof(batch_rec));
    memset(batch_slot, 0, sizeof(batch_slot));
    for (i = 0; i < BATCH_SLOTS; i++)
    {
        batch_slot[i].armed = 1;
        batch_rec.b.Cookie[i] = &batch_slot[i];
    }
    batch_rec.b.Version = ANXD_S2_RX_BATCH_VERSION;
    batch_rec.b.Size = (UWORD)sizeof(batch_rec.bytes);
    batch_rec.b.Count = count;
    batch_req.ios2_Req.io_Command = ANXD_CMD_RX_BATCH;
    batch_req.ios2_Req.io_Unit = &unit.nu_ExecUnit;
    batch_req.ios2_BufferManagement = op;
    batch_req.ios2_PacketType = type;
    batch_req.ios2_Data = &batch_rec.b;
    op->op_RxDirect  = (APTR)batch_rx_direct;
    op->op_RxFilled  = (APTR)batch_rx_filled;
    op->op_CopyTo    = (APTR)batch_copy_to;
    op->op_RxLinkHdr = TRUE;
    AddHead(&op->op_Reads, &batch_req.ios2_Req.io_Message.mn_Node);
    netdev_note_read_type(op, type);   /* as netdev_queue_batch() does */
    batch_direct_calls = batch_copy_calls = 0;
}

/* A pass of three frames into a batch of four: every frame goes to the next
   cookie with its header in front, the request stays queued and unanswered
   until the pass ends, and then it is answered exactly once. */
static void test_batch_fills_in_order_and_replies_at_pass_end(void)
{
    UBYTE  hdr[NETDEV_HDR_LEN];
    APTR   token = NULL;
    UBYTE  wanted = 0xff;
    UBYTE *dst;
    unsigned i;

    reset_fixture();
    unit.nu_Openers = 1;
    make_header(hdr, 0x0800);
    queue_batch(&opener_a, 0x0800, BATCH_SLOTS);
    opener_a.op_RxFlags = ANXD_S2_RXF_VERIFIED;

    for (i = 0; i < 3; i++)
    {
        dst = netdev_rx_claim(&unit, hdr, 60 + i, &token, &wanted);
        expect_ptr("batch: claim lands in the next cookie", dst,
                   batch_slot[i].area + NETDEV_HDR_LEN);
        expect_ptr("batch: the token is the batch request", token, &batch_req);
        expect_u32("batch: wanted is the opener's flags", wanted,
                   ANXD_S2_RXF_VERIFIED);
        expect_mem("batch: link header in front of the payload",
                   batch_slot[i].area, hdr, NETDEV_HDR_LEN);
        netdev_rx_claimed(&unit, token, 0x1234 + i,
                          ANXD_S2_RXF_SUMMED | ANXD_S2_RXF_VERIFIED);
        expect_u32("batch: RxFilled once for the slot",
                   (unsigned long)batch_slot[i].filled_calls, 1);
        expect_u32("batch: RxFilled got the payload length",
                   batch_slot[i].filled_len, 60 + i - NETDEV_HDR_LEN);
        expect_u32("batch: RxFilled got the sum", batch_slot[i].filled_sum,
                   0x1234 + i);
        expect_u32("batch: Filled counts", batch_rec.b.Filled, i + 1);
        expect_u32("batch: no reply before the pass ends",
                   (unsigned long)replies, 0);
        expect_u32("batch: still queued", list_count(&opener_a.op_Reads), 1);
    }
    expect_u32("batch: one pending batch", unit.nu_BatchPending, 1);
    expect_u32("batch: verified frames counted", unit.nu_Nic.rx_verified, 3);
    expect_u32("batch: packets counted", unit.nu_Stats.PacketsReceived, 3);

    netdev_batch_flush(&unit);
    expect_u32("flush: one reply for the pass", (unsigned long)replies, 1);
    expect_u32("flush: the batch left the queue",
               list_count(&opener_a.op_Reads), 0);
    expect_u32("flush: io_Error 0", (unsigned long)(UBYTE)batch_req.ios2_Req.io_Error, 0);
    expect_u32("flush: Filled as delivered", batch_rec.b.Filled, 3);
    expect_u32("flush: nothing pending", unit.nu_BatchPending, 0);

    /* A second flush with nothing pending replies nothing. */
    netdev_batch_flush(&unit);
    expect_u32("flush: idempotent", (unsigned long)replies, 1);
}

/* A batch that fills up is answered at once, mid-pass. */
static void test_batch_full_replies_immediately(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR  token = NULL;
    unsigned i;

    reset_fixture();
    unit.nu_Openers = 1;
    make_header(hdr, 0x0800);
    queue_batch(&opener_a, 0x0800, 2);

    for (i = 0; i < 2; i++)
    {
        UBYTE *dst = netdev_rx_claim(&unit, hdr, 60, &token, NULL);
        expect_ptr("full: claimed", dst, batch_slot[i].area + NETDEV_HDR_LEN);
        netdev_rx_claimed(&unit, token, 0, ANXD_S2_RXF_SUMMED);
    }
    expect_u32("full: replied when Count was reached", (unsigned long)replies, 1);
    expect_u32("full: left the queue", list_count(&opener_a.op_Reads), 0);
    expect_u32("full: nothing pending", unit.nu_BatchPending, 0);

    /* With no read left, a further frame of the type is "the reader is
       behind" for a sole opener, as with CMD_READs. */
    token = NULL;
    expect_ptr("full: no destination without a read",
               netdev_rx_claim(&unit, hdr, 60, &token, NULL), NULL);
    expect_ptr("full: the core may hold the frame", token,
               NETDEV_CLAIM_BEHIND);
}

/* A cookie whose slot has no buffer declines the claim and the frame falls to
   the staging path, which fails the same way; the batch keeps its place. */
static void test_batch_declines_without_a_buffer(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR  token = NULL;

    reset_fixture();
    unit.nu_Openers = 1;
    make_header(hdr, 0x0800);
    queue_batch(&opener_a, 0x0800, BATCH_SLOTS);
    batch_slot[0].armed = 0;

    expect_ptr("unarmed: declined", netdev_rx_claim(&unit, hdr, 60, &token, NULL),
               NULL);
    expect_u32("unarmed: still queued", list_count(&opener_a.op_Reads), 1);
    expect_u32("unarmed: Filled untouched", batch_rec.b.Filled, 0);
    expect_u32("unarmed: staging fails too",
               (unsigned long)netdev_batch_stage(&unit, &opener_a, &batch_req,
                                                 hdr, 60),
               (unsigned long)NETDEV_RX_FAILED);
    expect_u32("unarmed: no reply", (unsigned long)replies, 0);
}

/* The staging copy: the frame lands in the next cookie through the copy
   hook with its header in front, RxFilled reports it without SUMMED, and the
   pass-end reply follows as for a direct fill. */
static void test_batch_stage_copies_with_header(void)
{
    UBYTE frame[NETDEV_HDR_LEN + 46];
    unsigned i;

    reset_fixture();
    unit.nu_Openers = 1;
    make_header(frame, 0x0800);
    for (i = 0; i < 46; i++)
        frame[NETDEV_HDR_LEN + i] = (UBYTE)(0x40 + i);
    queue_batch(&opener_a, 0x0800, BATCH_SLOTS);

    expect_u32("stage: taken",
               (unsigned long)netdev_batch_stage(&unit, &opener_a, &batch_req,
                                                 frame, sizeof(frame)),
               (unsigned long)NETDEV_RX_TAKEN);
    expect_u32("stage: copy hook used", (unsigned long)batch_copy_calls, 1);
    expect_mem("stage: header and payload in place", batch_slot[0].area, frame,
               sizeof(frame));
    expect_u32("stage: RxFilled reported the payload",
               batch_slot[0].filled_len, 46);
    expect_u32("stage: not SUMMED", (unsigned long)batch_slot[0].filled_flags, 0);
    expect_u32("stage: Filled", batch_rec.b.Filled, 1);
    expect_u32("stage: pending", unit.nu_BatchPending, 1);
    netdev_batch_flush(&unit);
    expect_u32("stage: one reply", (unsigned long)replies, 1);
}

/* Two openers reading the type: the batch is one taker, the CMD_READ another,
   so the direct path declines for both, exactly as two CMD_READs would. */
static void test_batch_counts_as_one_taker(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR  token = NULL;

    reset_fixture();
    unit.nu_Openers = 2;
    make_header(hdr, 0x0800);
    queue_batch(&opener_a, 0x0800, BATCH_SLOTS);
    queue_read(&opener_b, &read_b, 0x0800);

    expect_ptr("two takers: declined", netdev_rx_claim(&unit, hdr, 60, &token, NULL),
               NULL);
    expect_ptr("two takers: no hold", token, NULL);
    expect_u32("two takers: batch untouched", batch_rec.b.Filled, 0);
    expect_u32("two takers: both still queued",
               list_count(&opener_a.op_Reads) + list_count(&opener_b.op_Reads), 2);
}

int main(void)
{
    test_batch_fills_in_order_and_replies_at_pass_end();
    test_batch_full_replies_immediately();
    test_batch_declines_without_a_buffer();
    test_batch_stage_copies_with_header();
    test_batch_counts_as_one_taker();
    test_claim_complete();
    test_link_header();
    test_raw_request_restored();
    test_declines_unsafe_claims();
    test_other_type_does_not_block();
    test_behind_never_holds_a_second_opener();
    test_broadcast_metadata();

    if (failures != 0)
        printf("netdev direct: %d failure(s)\n", failures);
    else
        printf("netdev direct: all checks passed\n");
    return failures != 0;
}
