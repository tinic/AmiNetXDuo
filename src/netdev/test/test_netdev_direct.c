/*
 * anxnet.device's private SANA-II single-copy receive transaction, on host.
 *
 * The chip test proves bytes leave the FIFO once.  This side proves the same
 * frame claims only an eligible CMD_READ, carries the normal SANA-II metadata
 * and counters, and returns the request to its queue if the chip cannot finish
 * the drain.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <proto/exec.h>

#include "netdev_internal.h"

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

VOID Disable(VOID) {}
VOID Enable(VOID) {}

VOID ReplyMsg(struct Message *msg)
{
    replies++;
    if (msg->mn_Node.ln_Succ != NULL)
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

/* --------------------------------------------------------------- fixture */

static NetdevUnit unit;
static NetdevOpener opener_a;
static NetdevOpener opener_b;
static struct IOSana2Req read_a;
static struct IOSana2Req read_b;
static UBYTE direct_buffer[NETDEV_MTU];
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
    memset(direct_buffer, 0, sizeof(direct_buffer));
    NewList(&unit.nu_OpenerList);
    NewList(&unit.nu_Writes);
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
    io->ios2_Req.io_Unit = &op->op_Unit;
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

static void test_claim_complete(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR token = NULL;
    UBYTE *dst;

    reset_fixture();
    make_header(hdr, 0x0800);
    queue_read(&opener_a, &read_a, 0x0800);
    opener_a.op_TrackHigh = 1;
    opener_a.op_Track[0].used = 1;
    opener_a.op_Track[0].type = 0x0800;

    dst = netdev_rx_claim(&unit, hdr, 60, &token);
    expect_ptr("accepted destination", dst, direct_buffer);
    expect_ptr("claim token", token, &read_a);
    expect_u32("one direct callback", direct_calls, 1);
    expect_ptr("direct ios2_Data", direct_data, &data_cookie);
    expect_u32("direct payload length", direct_len, 46);
    expect_u32("request removed while claimed", list_count(&opener_a.op_Reads), 0);
    expect_u32("request payload length", read_a.ios2_DataLength, 46);
    expect_mem("destination address", read_a.ios2_DstAddr, hdr, 6);
    expect_mem("source address", read_a.ios2_SrcAddr, hdr + 6, 6);

    netdev_rx_claimed(&unit, token, 0x12345678UL, 1);
    expect_u32("one filled callback", filled_calls, 1);
    expect_ptr("filled ios2_Data", filled_data, &data_cookie);
    expect_u32("filled payload length", filled_len, 46);
    expect_u32("filled checksum", filled_sum, 0x12345678UL);
    expect_u32("filled checksum valid", filled_summed, 1);
    expect_u32("one reply", replies, 1);
    expect_u32("unit packets", unit.nu_Stats.PacketsReceived, 1);
    expect_u32("direct packets", unit.nu_RxDirect, 1);
    expect_u32("tracked packets", opener_a.op_Track[0].st.PacketsReceived, 1);
    expect_u32("tracked wire bytes", opener_a.op_Track[0].st.BytesReceived, 60);
}

static void test_unclaim_restores_request(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR token = NULL;

    reset_fixture();
    make_header(hdr, 0x0806);
    queue_read(&opener_a, &read_a, 0x0806);
    expect_ptr("rollback claim", netdev_rx_claim(&unit, hdr, 64, &token),
               direct_buffer);
    netdev_rx_unclaim(&unit, token);
    expect_u32("rollback restores one reader", list_count(&opener_a.op_Reads), 1);
    expect_ptr("rollback restores same reader", opener_a.op_Reads.lh_Head,
               &read_a.ios2_Req.io_Message.mn_Node);
    expect_u32("rollback does not fill", filled_calls, 0);
    expect_u32("rollback does not reply", replies, 0);
    expect_u32("rollback does not count", unit.nu_Stats.PacketsReceived, 0);
}

static void test_declines_unsafe_claims(void)
{
    UBYTE hdr[NETDEV_HDR_LEN];
    APTR token = NULL;

    make_header(hdr, 0x86dd);

    reset_fixture();
    queue_read(&opener_a, &read_a, 0x86dd);
    queue_read(&opener_b, &read_b, 0x86dd);
    expect_ptr("two readers decline", netdev_rx_claim(&unit, hdr, 80, &token),
               NULL);
    expect_u32("first reader retained", list_count(&opener_a.op_Reads), 1);
    expect_u32("second reader retained", list_count(&opener_b.op_Reads), 1);
    expect_u32("no callback for two readers", direct_calls, 0);

    reset_fixture();
    queue_read(&opener_a, &read_a, 0x86dd);
    opener_a.op_Raw = 1;
    expect_ptr("raw reader declines", netdev_rx_claim(&unit, hdr, 80, &token),
               NULL);

    reset_fixture();
    queue_read(&opener_a, &read_a, 0x86dd);
    opener_a.op_Filter = (APTR)&unit;
    expect_ptr("filtered reader declines",
               netdev_rx_claim(&unit, hdr, 80, &token), NULL);

    reset_fixture();
    queue_read(&opener_a, &read_a, 0x86dd);
    opener_a.op_RxFilled = NULL;
    expect_ptr("unpaired hooks decline",
               netdev_rx_claim(&unit, hdr, 80, &token), NULL);

    reset_fixture();
    queue_read(&opener_a, &read_a, 0x86dd);
    direct_accept = 0;
    expect_ptr("stack destination declines",
               netdev_rx_claim(&unit, hdr, 80, &token), NULL);
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
               netdev_rx_claim(&unit, hdr, 60, &token), direct_buffer);
    expect_ptr("right packet type claimed", token, &read_a);
    expect_u32("other reader retained", list_count(&opener_b.op_Reads), 1);
    netdev_rx_unclaim(&unit, token);
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
    expect_ptr("broadcast claim", netdev_rx_claim(&unit, hdr, 60, &token),
               direct_buffer);
    expect_u32("broadcast flag replaces multicast",
               read_a.ios2_Req.io_Flags & (SANA2IOF_BCAST | SANA2IOF_MCAST),
               SANA2IOF_BCAST);
    netdev_rx_unclaim(&unit, token);
}

int main(void)
{
    test_claim_complete();
    test_unclaim_restores_request();
    test_declines_unsafe_claims();
    test_other_type_does_not_block();
    test_broadcast_metadata();

    if (failures != 0)
        printf("netdev direct: %d failure(s)\n", failures);
    else
        printf("netdev direct: all checks passed\n");
    return failures != 0;
}
