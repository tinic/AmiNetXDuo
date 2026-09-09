/*
 * anxnet.device, layer 1 of 3: the SANA-II command table itself.
 *
 * WHY THIS FILE EXISTS.  netdev_cmds.c is 818 lines, it is the surface every
 * opener enters and every chip core reaches, and until now nothing compiled
 * it off-target.  test_netdev_beginio.c comes closest and deliberately
 * REPLACES it with a recorder (src/netdev/CMakeLists.txt), because that test
 * is about what BeginIO writes before dispatch.  So the twenty cases below
 * had exactly the coverage an emulated a2065 run gives them, which is CMD_READ,
 * CMD_WRITE, S2_CONFIGINTERFACE, S2_ONLINE and nothing else.  Every error path
 * in here -- and the error codes are the contract, they are the ones
 * Individual Computers' x-surf.device returns -- was unasserted.
 *
 * THE ONE WORTH READING FIRST is a_multicast_group_bit().  This driver accepts
 * an S2_ADDMULTICASTADDRESS whose bit 0 of the first octet is set; both IC
 * drivers test bit 7 instead.  IPv4 multicast is 01:00:5e:.. and IPv6 is
 * 33:33:.., and both have bit 7 CLEAR, so on those drivers every legitimate
 * join is refused, the hash filter stays empty and the card drops the
 * router's neighbour solicitation to 33:33:ff:xx:xx:xx.  On-link IPv6 still
 * works because a router advertisement comes back unicast; off-link does not,
 * because the return path never resolves.  That divergence is deliberate,
 * it is invisible in every IPv4 test, and a well-meaning "match the vendor"
 * edit would restore the bug.  It is asserted here in both directions.
 *
 * WHAT IS REAL AND WHAT IS STOOD IN FOR.  netdev_cmds.c and netdev_mcast.c
 * are compiled and linked for real -- the multicast table and its refcounts
 * are the thing several cases turn on.  Everything the dispatcher hands off
 * to is a recorder: netdev_reply, the transmit path, online/offline, the
 * event queue and the hardware filter.  Exec's list primitives are the same
 * eight test_netdev_event.c defines, including its rule that ReplyMsg() on a
 * node still linked into a list is a failure.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <proto/exec.h>

#include "netdev_internal.h"

/* Not in the SANA-II autodocs' command list, and netdev_cmds.c
   defines it for the same reason. */
#ifndef NSCMD_DEVICEQUERY
#define NSCMD_DEVICEQUERY   0x4000
#endif

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

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    checks++;
    if (got == want)
        return;

    printf("FAIL %s: got 0x%lx, want 0x%lx\n", what, got, want);
    failures++;
}

/* ---------------------------------------------------------------- exec --- */

static int disable_depth;
static int replies;

VOID Disable(VOID)
{
    disable_depth++;
}

VOID Enable(VOID)
{
    disable_depth--;
    if (disable_depth < 0)
    {
        printf("FAIL Enable() without a matching Disable()\n");
        failures++;
        disable_depth = 0;
    }
}

/* The one thing the driver must not do: complete somebody else's IORequest
   while it is still on the list it was taken from.  Remove() nulls ln_Succ,
   so a reply from a node still linked is caught here. */
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

/* ------------------------------------------------------- what it hands to - */

static LONG  last_err;
static ULONG last_wire;
static int   nreplies;

/* netdev_device.c's, three lines of it. */
VOID netdev_reply(struct IOSana2Req *io, LONG err, ULONG wire)
{
    nreplies++;
    last_err  = err;
    last_wire = wire;

    io->ios2_Req.io_Error = (BYTE)err;
    io->ios2_WireError    = wire;

    if ((io->ios2_Req.io_Flags & IOF_QUICK) != 0)
        return;

    ReplyMsg(&io->ios2_Req.io_Message);
}

static int                tx_calls;
static struct IOSana2Req *tx_last;

VOID netdev_tx_direct(NetdevUnit *unit, struct IOSana2Req *io)
{
    (VOID)unit;
    tx_calls++;
    tx_last = io;
}

static LONG online_answer;
static int  online_calls;
static int  offline_calls;
static ULONG offline_event;

LONG netdev_online(NetdevUnit *unit)
{
    online_calls++;
    if (online_answer == 0)
        unit->nu_Online = 1;
    return online_answer;
}

VOID netdev_offline(NetdevUnit *unit, ULONG event)
{
    offline_calls++;
    offline_event = event;
    unit->nu_Online = 0;
}

static int cancel_resume_calls;

VOID netdev_pcmcia_cancel_resume(const NetdevUnit *unit)
{
    (VOID)unit;
    cancel_resume_calls++;
}

static int rescan_calls;

VOID netdev_event_rescan(NetdevUnit *unit)
{
    (VOID)unit;
    rescan_calls++;
}

static int                event_wait_calls;
static struct IOSana2Req *event_wait_last;

VOID netdev_event_wait(NetdevUnit *unit, struct IOSana2Req *io)
{
    (VOID)unit;
    event_wait_calls++;
    event_wait_last = io;
}

static int drop_writes_calls;

VOID netdev_drop_writes(NetdevUnit *unit, NetdevOpener *op)
{
    (VOID)unit;
    (VOID)op;
    drop_writes_calls++;
}

static int rebuild_calls;

VOID netdev_rebuild_filter(NetdevUnit *unit)
{
    (VOID)unit;
    rebuild_calls++;
}

/* --------------------------------------------------------------- fixture - */

static NetdevUnit    unit;
static NetdevOpener  opener;
static NetdevDevice  device;
static NetdevNic    *nic;
static NetdevCard    card;

static void reset(void)
{
    memset(&unit, 0, sizeof(unit));
    memset(&opener, 0, sizeof(opener));
    memset(&device, 0, sizeof(device));
    memset(&card, 0, sizeof(card));

    card.name = "test";
    card.bps  = 10000000UL;

    nic = &unit.nu_Nic;
    nic->card = &card;

    unit.nu_Dev = &device;
    NewList(&unit.nu_Writes);

    opener.op_Hw = &unit;
    NewList(&opener.op_Reads);
    NewList(&opener.op_Orphans);
    NewList(&opener.op_Events);

    /* A frame can only be handed up or taken down through the opener's hooks,
       so a non-NULL pair is what "this opener is usable" means. */
    opener.op_CopyTo   = (APTR)&opener;
    opener.op_CopyFrom = (APTR)&opener;

    unit.nu_Online     = 1;
    unit.nu_Configured = 1;

    online_answer = 0;

    disable_depth = 0;
    replies = nreplies = 0;
    tx_calls = online_calls = offline_calls = 0;
    cancel_resume_calls = rescan_calls = event_wait_calls = 0;
    drop_writes_calls = rebuild_calls = 0;
    last_err = 0x7f;
    last_wire = 0xffffffffUL;
}

/* A request with io_Flags clear, so every reply goes through ReplyMsg() and
   the still-linked check above has something to check. */
static void req(struct IOSana2Req *io, UWORD cmd)
{
    memset(io, 0, sizeof(*io));
    io->ios2_Req.io_Command = cmd;
    io->ios2_Req.io_Unit    = (struct Unit *)&opener.op_Unit;
}

static void balanced(const char *what)
{
    expect(disable_depth == 0, what);
}

/* ============================================================ multicast === */

/*
 * THE DELIBERATE DIVERGENCE.  Bit 0 of the first octet is the Ethernet group
 * bit and is what decides a multicast address.  Bit 7 is the U/L bit and
 * decides nothing of the kind.  Both real multicast prefixes have bit 7 clear.
 */
static void a_multicast_group_bit(void)
{
    static const UBYTE ipv4_mc[6]  = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    static const UBYTE ipv6_mc[6]  = { 0x33, 0x33, 0xff, 0x12, 0x34, 0x56 };
    static const UBYTE unicast[6]  = { 0x02, 0x41, 0x4d, 0x49, 0x00, 0x01 };
    static const UBYTE bit7_only[6] = { 0x80, 0x00, 0x00, 0x00, 0x00, 0x01 };
    struct IOSana2Req  io;

    /* IPv4 multicast: bit 0 set, bit 7 clear.  Accepted. */
    reset();
    req(&io, S2_ADDMULTICASTADDRESS);
    memcpy(io.ios2_SrcAddr, ipv4_mc, 6);
    netdev_perform(&opener, &io);
    expect_u32("01:00:5e join accepted", (unsigned long)(UBYTE)last_err, 0);
    expect(rebuild_calls == 1, "01:00:5e join reprogrammed the filter");
    balanced("01:00:5e join left Disable balanced");

    /*
     * IPv6 solicited-node: 33:33:ff:.. -- bit 0 set, bit 7 CLEAR.  This is
     * the join both IC drivers refuse, and refusing it is what breaks
     * off-link IPv6: the neighbour solicitation for the return path is
     * dropped by the card and nothing ever resolves.
     */
    reset();
    req(&io, S2_ADDMULTICASTADDRESS);
    memcpy(io.ios2_SrcAddr, ipv6_mc, 6);
    netdev_perform(&opener, &io);
    expect_u32("33:33:ff join accepted", (unsigned long)(UBYTE)last_err, 0);
    expect(rebuild_calls == 1, "33:33:ff join reprogrammed the filter");

    /* A unicast address is not a group and is refused, group bit clear. */
    reset();
    req(&io, S2_ADDMULTICASTADDRESS);
    memcpy(io.ios2_SrcAddr, unicast, 6);
    netdev_perform(&opener, &io);
    expect_u32("a unicast address is refused", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ADDRESS);
    expect_u32("and refused as a bad multicast", last_wire,
               (unsigned long)S2WERR_BAD_MULTICAST);
    expect(rebuild_calls == 0, "a refused join did not touch the filter");

    /*
     * Bit 7 set and bit 0 clear.  A driver testing the wrong bit takes this
     * one and refuses the two above, so it is the case that tells the two
     * implementations apart in a single request.
     */
    reset();
    req(&io, S2_ADDMULTICASTADDRESS);
    memcpy(io.ios2_SrcAddr, bit7_only, 6);
    netdev_perform(&opener, &io);
    expect_u32("bit 7 alone is not a group address", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ADDRESS);
}

/* A leave of an address never joined is a state error, not a bad address. */
static void b_multicast_refcounts(void)
{
    static const UBYTE mc[6] = { 0x01, 0x00, 0x5e, 0x01, 0x02, 0x03 };
    struct IOSana2Req  io;

    reset();

    req(&io, S2_DELMULTICASTADDRESS);
    memcpy(io.ios2_SrcAddr, mc, 6);
    netdev_perform(&opener, &io);
    expect_u32("leaving a group never joined", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_STATE);
    expect_u32("names the multicast", last_wire,
               (unsigned long)S2WERR_BAD_MULTICAST);

    /* Join, then leave, then leave again. */
    req(&io, S2_ADDMULTICASTADDRESS);
    memcpy(io.ios2_SrcAddr, mc, 6);
    netdev_perform(&opener, &io);
    expect_u32("join", (unsigned long)(UBYTE)last_err, 0);

    req(&io, S2_DELMULTICASTADDRESS);
    memcpy(io.ios2_SrcAddr, mc, 6);
    netdev_perform(&opener, &io);
    expect_u32("leave", (unsigned long)(UBYTE)last_err, 0);

    req(&io, S2_DELMULTICASTADDRESS);
    memcpy(io.ios2_SrcAddr, mc, 6);
    netdev_perform(&opener, &io);
    expect_u32("leaving twice", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_STATE);
    balanced("the join/leave sequence left Disable balanced");
}

/* The table is finite, and a refused join is counted where the special
   statistics can report it. */
static void c_multicast_table_fills(void)
{
    struct IOSana2Req io;
    UWORD             i;
    int               refused = 0;

    reset();

    for (i = 0; i < (UWORD)(NETDEV_MCAST_MAX + 4); i++)
    {
        req(&io, S2_ADDMULTICASTADDRESS);
        io.ios2_SrcAddr[0] = 0x01;
        io.ios2_SrcAddr[1] = 0x00;
        io.ios2_SrcAddr[2] = 0x5e;
        io.ios2_SrcAddr[3] = (UBYTE)(i >> 8);
        io.ios2_SrcAddr[4] = (UBYTE)i;
        io.ios2_SrcAddr[5] = 0x01;
        netdev_perform(&opener, &io);

        if (last_err != 0)
        {
            refused++;
            expect_u32("a full table refuses with NO_RESOURCES",
                       (unsigned long)(UBYTE)last_err,
                       (unsigned long)(UBYTE)S2ERR_NO_RESOURCES);
            expect_u32("and says the table is full", last_wire,
                       (unsigned long)S2WERR_MULTICAST_FULL);
        }
    }

    expect(refused == 4, "exactly the joins past NETDEV_MCAST_MAX are refused");
    expect_u32("and every one is counted for the statistics",
               unit.nu_McastFull, 4);
}

/* ============================================================== reads ==== */

/*
 * A CMD_READ goes to the HEAD of op_Reads and an S2_READORPHAN to the TAIL of
 * op_Orphans.  The head is not a detail: this shim keeps three readers on one
 * opener (IPv4, ARP, IPv6), and with AddTail the two idle ones settle
 * permanently in front, so every arriving frame walks past them.
 */
static void d_read_queue_ends(void)
{
    struct IOSana2Req a, b, orphan1, orphan2;

    reset();

    req(&a, CMD_READ);
    req(&b, CMD_READ);
    netdev_perform(&opener, &a);
    netdev_perform(&opener, &b);

    expect(opener.op_Reads.lh_Head == &b.ios2_Req.io_Message.mn_Node,
           "the second CMD_READ is at the head");
    expect(b.ios2_Req.io_Message.mn_Node.ln_Succ ==
           &a.ios2_Req.io_Message.mn_Node,
           "and the first is behind it");
    expect(nreplies == 0, "a queued read is not replied to");

    req(&orphan1, S2_READORPHAN);
    req(&orphan2, S2_READORPHAN);
    netdev_perform(&opener, &orphan1);
    netdev_perform(&opener, &orphan2);

    expect(opener.op_Orphans.lh_Head ==
           &orphan1.ios2_Req.io_Message.mn_Node,
           "the first orphan read is at the head");
    expect(opener.op_Orphans.lh_TailPred ==
           &orphan2.ios2_Req.io_Message.mn_Node,
           "and the second is at the tail");

    /* Queueing clears IOF_QUICK and stamps the node, or Exec never replies. */
    expect((a.ios2_Req.io_Flags & IOF_QUICK) == 0,
           "a queued read has IOF_QUICK cleared");
    expect_u32("and is marked NT_MESSAGE",
               a.ios2_Req.io_Message.mn_Node.ln_Type, NT_MESSAGE);
    balanced("queueing reads left Disable balanced");
}

/* An opener with no receive hook cannot be handed a frame, so its reads are
   refused rather than queued to wait for a delivery that cannot happen. */
static void e_read_needs_a_hook(void)
{
    struct IOSana2Req io;

    reset();
    opener.op_CopyTo = NULL;

    req(&io, CMD_READ);
    netdev_perform(&opener, &io);

    expect_u32("a read with no copy hook", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ARGUMENT);
    expect_u32("names the null pointer", last_wire,
               (unsigned long)S2WERR_NULL_POINTER);
    expect(opener.op_Reads.lh_Head->ln_Succ == NULL,
           "and nothing was queued");
}

/* An offline unit refuses reads rather than holding them: a read queued on an
   offline unit is one nothing will ever complete. */
static void f_offline_refuses_reads(void)
{
    struct IOSana2Req io;

    reset();
    unit.nu_Online = 0;

    req(&io, CMD_READ);
    netdev_perform(&opener, &io);

    expect_u32("a read on an offline unit", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_OUTOFSERVICE);
    expect_u32("names the offline unit", last_wire,
               (unsigned long)S2WERR_UNIT_OFFLINE);
    expect(opener.op_Reads.lh_Head->ln_Succ == NULL,
           "and it was not left on the list");
    balanced("the offline refusal left Disable balanced");
}

/* ============================================================== writes === */

static void g_write_paths(void)
{
    struct IOSana2Req io;
    UWORD             i;

    /* An ordinary write reaches the transmit path. */
    reset();
    req(&io, CMD_WRITE);
    io.ios2_DataLength = NETDEV_MTU;
    netdev_perform(&opener, &io);
    expect(tx_calls == 1 && tx_last == &io, "a write reaches the transmitter");

    /* One byte past the MTU, and not raw. */
    reset();
    req(&io, CMD_WRITE);
    io.ios2_DataLength = NETDEV_MTU + 1;
    netdev_perform(&opener, &io);
    expect_u32("a write past the MTU", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_MTU_EXCEEDED);
    expect(tx_calls == 0, "and it did not reach the transmitter");

    /* The same length from a raw opener, which sends its own header. */
    reset();
    opener.op_Raw = 1;
    req(&io, CMD_WRITE);
    io.ios2_DataLength = NETDEV_FRAME_MAX;
    netdev_perform(&opener, &io);
    expect(tx_calls == 1, "a raw write up to the frame maximum is sent");

    reset();
    opener.op_Raw = 1;
    req(&io, CMD_WRITE);
    io.ios2_DataLength = NETDEV_FRAME_MAX + 1;
    netdev_perform(&opener, &io);
    expect_u32("even a raw write stops at the frame maximum",
               (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_MTU_EXCEEDED);

    /* S2_BROADCAST fills the destination itself. */
    reset();
    req(&io, S2_BROADCAST);
    memset(io.ios2_DstAddr, 0x11, NETDEV_ADDR_LEN);
    io.ios2_DataLength = 64;
    netdev_perform(&opener, &io);
    expect(tx_calls == 1, "a broadcast is sent");
    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        expect(io.ios2_DstAddr[i] == 0xff,
               "and its destination was overwritten with the broadcast");

    /* S2_MULTICAST to a unicast destination is refused. */
    reset();
    req(&io, S2_MULTICAST);
    io.ios2_DstAddr[0] = 0x02;
    io.ios2_DataLength = 64;
    netdev_perform(&opener, &io);
    expect_u32("a multicast send to a unicast address",
               (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ADDRESS);
    expect(tx_calls == 0, "and it did not reach the transmitter");

    /* A write with no transmit hook, and a write to an offline unit. */
    reset();
    opener.op_CopyFrom = NULL;
    req(&io, CMD_WRITE);
    netdev_perform(&opener, &io);
    expect_u32("a write with no copy hook", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ARGUMENT);

    reset();
    unit.nu_Online = 0;
    req(&io, CMD_WRITE);
    netdev_perform(&opener, &io);
    expect_u32("a write on an offline unit", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_OUTOFSERVICE);
}

/* =========================================================== the queries = */

/*
 * SizeAvailable is the caller saying how much room it has.  A caller built
 * against an older Sana2DeviceQuery has less than this structure, and writing
 * every field and then reporting a smaller SizeSupplied overruns exactly the
 * caller that was careful.
 */
static void h_devicequery_honours_sizeavailable(void)
{
    /*
     * The buffer is the FULL structure plus a canary run, and the check is
     * that nothing at or past offset SizeAvailable was touched -- not that
     * nothing past the structure was.  An earlier draft put the canary after
     * the structure, where a driver writing sizeof() instead of the caller's
     * SizeAvailable lands harmlessly inside the box and the test passes: the
     * overrun this case exists for is a caller SHORTER than the structure,
     * so the boundary has to be the caller's number.
     */
    static UBYTE box[sizeof(struct Sana2DeviceQuery) + 32];
    struct Sana2DeviceQuery *q = (struct Sana2DeviceQuery *)(APTR)box;
    struct IOSana2Req        io;
    ULONG                    want;
    ULONG                    i;

    for (want = 4; want <= sizeof(struct Sana2DeviceQuery); want += 4)
    {
        reset();
        memset(box, 0x5a, sizeof(box));
        q->SizeAvailable = want;

        req(&io, S2_DEVICEQUERY);
        io.ios2_StatData = q;
        netdev_perform(&opener, &io);

        expect_u32("the query is answered", (unsigned long)(UBYTE)last_err, 0);

        for (i = want; i < (ULONG)sizeof(box); i++)
        {
            if (box[i] != 0x5a)
            {
                char what[80];

                sprintf(what, "S2_DEVICEQUERY wrote at offset %lu of %lu"
                              " the caller offered",
                        (unsigned long)i, (unsigned long)want);
                expect(0, what);
                break;
            }
        }
        checks++;

        if (want >= sizeof(struct Sana2DeviceQuery))
        {
            expect_u32("a full-sized query reports the line rate",
                       q->BPS, card.bps);
            expect_u32("and the MTU", q->MTU, NETDEV_MTU);
            expect_u32("and a 48-bit address field", q->AddrFieldSize, 48);
            expect_u32("and says how much it supplied", q->SizeSupplied,
                       (unsigned long)sizeof(struct Sana2DeviceQuery));
            expect_u32("and names Ethernet", q->HardwareType,
                       (unsigned long)S2WireType_Ethernet);
        }
    }

    /* And a NULL StatData is refused rather than written through. */
    reset();
    req(&io, S2_DEVICEQUERY);
    io.ios2_StatData = NULL;
    netdev_perform(&opener, &io);
    expect_u32("a query with no buffer", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ARGUMENT);
    expect_u32("names the null pointer", last_wire,
               (unsigned long)S2WERR_NULL_POINTER);
}

/*
 * The new-style query arrives in an IOStdReq, and on the TARGET io_Actual
 * lands exactly where ios2_WireError does -- both at offset 32, which
 * netdev_cmds.c now carries a _Static_assert for under __mc68000__.
 * netdev_reply() would therefore write a wire error into the caller's byte
 * count, so this case replies by hand.
 *
 * WHAT THIS TEST CAN AND CANNOT SEE.  The host shim is not that layout: its
 * struct IORequest is 8-byte aligned because it holds host pointers, so
 * ios2_WireError sits at 72 while IOStdReq's inline io_Actual sits at 68.
 * The alias is therefore NOT reproducible here and this test does not claim
 * to reproduce it.  What it pins is the behaviour the alias would break --
 * io_Actual carries the size, io_Error carries the verdict, and ReplyMsg()
 * happens only when IOF_QUICK is clear -- which is the contract on both
 * layouts.  The assert in the driver covers the layout half.
 */
static void i_nsquery_replies_by_hand(void)
{
    struct IOStdReq   std;
    struct IOSana2Req io;
    struct
    {
        ULONG  DevQueryFormat;
        ULONG  SizeAvailable;
        UWORD  DeviceType;
        UWORD  DeviceSubType;
        UWORD *SupportedCommands;
    } answer;

    reset();
    memset(&std, 0, sizeof(std));
    memset(&answer, 0x5a, sizeof(answer));
    std.io_Command = NSCMD_DEVICEQUERY;
    std.io_Data    = &answer;
    std.io_Length  = sizeof(answer);

    netdev_perform(&opener, (struct IOSana2Req *)&std);

    expect_u32("the new-style query reports its own size",
               (unsigned long)std.io_Actual, (unsigned long)sizeof(answer));
    expect_u32("with no error", (unsigned long)(UBYTE)std.io_Error, 0);
    expect_u32("and names SANA-II", answer.DeviceType, 7);
    expect(answer.SupportedCommands != NULL,
           "and hands back the supported-command list");
    expect(nreplies == 0, "and did not go through the SANA reply helper");
    expect(replies == 1, "but was replied to");

    /* Too short: io_Actual must be ZERO, not a byte count, because the caller
       reads that field as one. */
    reset();
    memset(&std, 0, sizeof(std));
    std.io_Command = NSCMD_DEVICEQUERY;
    std.io_Data    = &answer;
    std.io_Length  = 15;
    netdev_perform(&opener, (struct IOSana2Req *)&std);
    expect_u32("a short new-style query", (unsigned long)(UBYTE)std.io_Error,
               (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect_u32("reports no bytes at all", (unsigned long)std.io_Actual, 0);

    /* A NULL buffer is the same answer. */
    reset();
    memset(&std, 0, sizeof(std));
    std.io_Command = NSCMD_DEVICEQUERY;
    std.io_Data    = NULL;
    std.io_Length  = 64;
    netdev_perform(&opener, (struct IOSana2Req *)&std);
    expect_u32("a new-style query with no buffer", (unsigned long)(UBYTE)std.io_Error,
               (unsigned long)(UBYTE)IOERR_BADLENGTH);

    /* IOF_QUICK set means the caller is not waiting on a reply port. */
    reset();
    memset(&std, 0, sizeof(std));
    std.io_Command = NSCMD_DEVICEQUERY;
    std.io_Data    = &answer;
    std.io_Length  = sizeof(answer);
    std.io_Flags   = IOF_QUICK;
    netdev_perform(&opener, (struct IOSana2Req *)&std);
    expect(replies == 0, "a quick new-style query is not replied to");
    expect_u32("but is still answered", (unsigned long)std.io_Actual,
               (unsigned long)sizeof(answer));

    (VOID)io;
}

/*
 * TWO COPIES OF THE SAME FACT.  netdev_supported[] is what the driver
 * ADVERTISES it can do; the switch in netdev_perform() is what it actually
 * does.  Drift is silent in both directions -- an advertised command the
 * dispatcher answers IOERR_NOCMD is a caller told to use something that does
 * not work, and a working command left out of the list is one a careful
 * caller will never try.
 */
static void j_the_advertised_list_is_the_real_one(void)
{
    struct IOStdReq   std;
    UWORD            *list;
    UWORD             n;

    reset();
    memset(&std, 0, sizeof(std));
    {
        struct
        {
            ULONG  DevQueryFormat;
            ULONG  SizeAvailable;
            UWORD  DeviceType;
            UWORD  DeviceSubType;
            UWORD *SupportedCommands;
        } answer;

        memset(&answer, 0, sizeof(answer));
        std.io_Command = NSCMD_DEVICEQUERY;
        std.io_Data    = &answer;
        std.io_Length  = sizeof(answer);
        netdev_perform(&opener, (struct IOSana2Req *)&std);
        list = answer.SupportedCommands;
    }

    expect(list != NULL, "the supported-command list is published");
    if (list == NULL)
        return;

    for (n = 0; list[n] != 0; n++)
    {
        struct IOSana2Req io;
        char              what[64];

        /* NSCMD_DEVICEQUERY needs an IOStdReq of its own and is answered
           above; every other advertised command is asked here. */
        if (list[n] == (UWORD)NSCMD_DEVICEQUERY)
            continue;

        reset();
        req(&io, list[n]);

        /* Enough of a request for the command to reach its own code rather
           than an argument check: a NULL buffer answers BAD_ARGUMENT, which
           would pass an "is not NOCMD" test for the wrong reason. */
        io.ios2_SrcAddr[0] = 0x01;
        io.ios2_DstAddr[0] = 0x01;
        io.ios2_DataLength = 64;
        io.ios2_WireError  = S2EVENT_ERROR;
        io.ios2_StatData   = NULL;

        netdev_perform(&opener, &io);

        sprintf(what, "advertised command 0x%04x is not refused as unknown",
                (unsigned)list[n]);
        expect(!(last_err == (LONG)IOERR_NOCMD &&
                 last_wire == (ULONG)S2WERR_GENERIC_ERROR), what);
    }

    expect(n == 22, "the advertised list is the length this test read it at");
}

/* Anything else is what both IC drivers answer, and what a caller probes
   with. */
static void k_unknown_commands(void)
{
    /*
     * The standard Exec commands this device does not implement, the gaps in
     * the SANA-II range, and three numbers far from either.  NOT 0x0f, which
     * is S2_DELMULTICASTADDRESS -- an earlier draft of this list used it and
     * the dispatcher was right to answer S2ERR_BAD_ADDRESS.
     */
    static const UWORD unknown[] = {
        CMD_INVALID, CMD_RESET, CMD_UPDATE, CMD_CLEAR, CMD_STOP, CMD_START,
        12, 13, 27, 0x2000, 0x7fff, 0xC002, 0xffff
    };
    struct IOSana2Req  io;
    UWORD              i;

    for (i = 0; i < (UWORD)(sizeof(unknown) / sizeof(unknown[0])); i++)
    {
        reset();
        req(&io, unknown[i]);
        netdev_perform(&opener, &io);
        expect_u32("an unknown command is refused",
                   (unsigned long)(UBYTE)last_err, (unsigned long)(UBYTE)IOERR_NOCMD);
        expect_u32("generically", last_wire,
                   (unsigned long)S2WERR_GENERIC_ERROR);
    }
}

/* ============================================================== events === */

/*
 * S2EVENT_SOFTWARE is refused and used to be accepted: this driver has no
 * condition that raises it, so an accepted request waits forever with
 * io_Error zero and the caller cannot see why.  A mask of zero names no
 * condition at all and would queue for a post that can never share a bit.
 */
static void l_onevent_masks(void)
{
    static const ULONG good[] = {
        S2EVENT_ERROR, S2EVENT_TX, S2EVENT_RX, S2EVENT_ONLINE,
        S2EVENT_OFFLINE, S2EVENT_BUFF, S2EVENT_HARDWARE,
        S2EVENT_ONLINE | S2EVENT_OFFLINE,
        S2EVENT_ERROR | S2EVENT_RX | S2EVENT_TX
    };
    static const ULONG bad[] = {
        0UL, S2EVENT_SOFTWARE, S2EVENT_SOFTWARE | S2EVENT_ONLINE,
        0xffffffffUL, 0x80000000UL
    };
    struct IOSana2Req io;
    UWORD             i;

    for (i = 0; i < (UWORD)(sizeof(good) / sizeof(good[0])); i++)
    {
        reset();
        req(&io, S2_ONEVENT);
        io.ios2_WireError = good[i];
        netdev_perform(&opener, &io);
        expect(event_wait_calls == 1 && event_wait_last == &io,
               "a supported event mask reaches the event queue");
        expect(nreplies == 0, "and is not answered immediately");
    }

    for (i = 0; i < (UWORD)(sizeof(bad) / sizeof(bad[0])); i++)
    {
        reset();
        req(&io, S2_ONEVENT);
        io.ios2_WireError = bad[i];
        netdev_perform(&opener, &io);
        expect_u32("an unsupported event mask", (unsigned long)(UBYTE)last_err,
                   (unsigned long)(UBYTE)S2ERR_NOT_SUPPORTED);
        expect_u32("names the bad event", last_wire,
                   (unsigned long)S2WERR_BAD_EVENT);
        expect(event_wait_calls == 0, "and never reached the event queue");
    }
}

/* ============================================================== tracking = */

static void m_track_types(void)
{
    struct IOSana2Req io;
    UWORD             i;

    reset();

    req(&io, S2_TRACKTYPE);
    io.ios2_PacketType = 0x0800;
    netdev_perform(&opener, &io);
    expect_u32("a type is tracked", (unsigned long)(UBYTE)last_err, 0);
    expect_u32("and the high-water mark moved", opener.op_TrackHigh, 1);

    /* The same type twice. */
    req(&io, S2_TRACKTYPE);
    io.ios2_PacketType = 0x0800;
    netdev_perform(&opener, &io);
    expect_u32("tracking a type twice", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_STATE);
    expect_u32("says it is already tracked", last_wire,
               (unsigned long)S2WERR_ALREADY_TRACKED);

    /* Statistics for a tracked type, and for one that is not. */
    {
        struct Sana2PacketTypeStats st;

        opener.op_Track[0].st.PacketsReceived = 1234;

        req(&io, S2_GETTYPESTATS);
        io.ios2_PacketType = 0x0800;
        io.ios2_StatData   = &st;
        memset(&st, 0, sizeof(st));
        netdev_perform(&opener, &io);
        expect_u32("statistics for a tracked type", (unsigned long)(UBYTE)last_err, 0);
        expect_u32("carry its counters", st.PacketsReceived, 1234);

        req(&io, S2_GETTYPESTATS);
        io.ios2_PacketType = 0x86dd;
        io.ios2_StatData   = &st;
        netdev_perform(&opener, &io);
        expect_u32("statistics for an untracked type",
                   (unsigned long)(UBYTE)last_err,
                   (unsigned long)(UBYTE)S2ERR_BAD_STATE);
        expect_u32("say it is not tracked", last_wire,
                   (unsigned long)S2WERR_NOT_TRACKED);

        req(&io, S2_GETTYPESTATS);
        io.ios2_PacketType = 0x0800;
        io.ios2_StatData   = NULL;
        netdev_perform(&opener, &io);
        expect_u32("statistics with no buffer", (unsigned long)(UBYTE)last_err,
                   (unsigned long)(UBYTE)S2ERR_BAD_ARGUMENT);
    }

    /* Fill the table, then one more. */
    for (i = 1; i < (UWORD)NETDEV_TRACK_MAX; i++)
    {
        req(&io, S2_TRACKTYPE);
        io.ios2_PacketType = (UWORD)(0x1000 + i);
        netdev_perform(&opener, &io);
        expect_u32("the table takes it", (unsigned long)(UBYTE)last_err, 0);
    }
    expect_u32("and is full to its high-water mark", opener.op_TrackHigh,
               NETDEV_TRACK_MAX);

    req(&io, S2_TRACKTYPE);
    io.ios2_PacketType = 0x9999;
    netdev_perform(&opener, &io);
    expect_u32("one past the table", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_NO_RESOURCES);

    /*
     * The high-water mark is what the interrupt server walks on every frame,
     * so it has to come back down when the top slots are released -- but only
     * as far as the highest slot still in use.
     */
    req(&io, S2_UNTRACKTYPE);
    io.ios2_PacketType = (UWORD)(0x1000 + NETDEV_TRACK_MAX - 1);
    netdev_perform(&opener, &io);
    expect_u32("untracking the top slot", (unsigned long)(UBYTE)last_err, 0);
    expect_u32("lowers the high-water mark", opener.op_TrackHigh,
               (unsigned long)(NETDEV_TRACK_MAX - 1));

    req(&io, S2_UNTRACKTYPE);
    io.ios2_PacketType = 0x0800;
    netdev_perform(&opener, &io);
    expect_u32("untracking slot 0", (unsigned long)(UBYTE)last_err, 0);
    expect_u32("leaves the mark where the highest live slot is",
               opener.op_TrackHigh, (unsigned long)(NETDEV_TRACK_MAX - 1));

    req(&io, S2_UNTRACKTYPE);
    io.ios2_PacketType = 0x0800;
    netdev_perform(&opener, &io);
    expect_u32("untracking it again", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_STATE);
    expect_u32("says it was not tracked", last_wire,
               (unsigned long)S2WERR_NOT_TRACKED);
    balanced("the tracking sequence left Disable balanced");
}

/* =========================================================== statistics == */

/* RecordCountMax is the caller's array length.  Supplying more records than
   that writes past it. */
static void n_special_stats_respect_the_caller(void)
{
    struct
    {
        struct Sana2SpecialStatHeader hdr;
        struct Sana2SpecialStatRecord rec[24];
        UBYTE                         canary[32];
    } box;
    struct IOSana2Req io;
    ULONG             max;
    ULONG             full = 0;
    UWORD             i;

    for (max = 0; max <= 20; max++)
    {
        reset();
        memset(&box, 0x5a, sizeof(box));
        box.hdr.RecordCountMax      = max;
        box.hdr.RecordCountSupplied = 0xffffffffUL;

        req(&io, S2_GETSPECIALSTATS);
        io.ios2_StatData = &box.hdr;
        netdev_perform(&opener, &io);

        expect_u32("the statistics are answered", (unsigned long)(UBYTE)last_err, 0);
        expect(box.hdr.RecordCountSupplied <= max,
               "never more records than the caller offered room for");

        for (i = 0; i < (UWORD)sizeof(box.canary); i++)
            expect(box.canary[i] == 0x5a,
                   "S2_GETSPECIALSTATS wrote past the caller's array");

        /* Types are the record's index, so a caller that learned the table
           by index still finds what it learned. */
        for (i = 0; i < (UWORD)box.hdr.RecordCountSupplied; i++)
        {
            expect_u32("record Type is its index", box.rec[i].Type, i);
            expect(box.rec[i].String != NULL, "and it is named");
        }

        if (box.hdr.RecordCountSupplied > full)
            full = box.hdr.RecordCountSupplied;
    }

    expect(full == 16, "the table is the sixteen records this test read");

    reset();
    req(&io, S2_GETSPECIALSTATS);
    io.ios2_StatData = NULL;
    netdev_perform(&opener, &io);
    expect_u32("statistics with no header", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ARGUMENT);
    expect_u32("names the null pointer", last_wire,
               (unsigned long)S2WERR_NULL_POINTER);
}

/* The global counters are filled from the chip core rather than kept twice:
   two counters for one event that disagree cost an hour in the field. */
static void o_global_stats_come_from_the_core(void)
{
    struct Sana2DeviceStats st;
    struct IOSana2Req       io;

    reset();
    nic->overruns  = 77;
    nic->rx_errors = 99;

    req(&io, S2_GETGLOBALSTATS);
    io.ios2_StatData = &st;
    memset(&st, 0xa5, sizeof(st));
    netdev_perform(&opener, &io);

    expect_u32("the global statistics are answered",
               (unsigned long)(UBYTE)last_err, 0);
    expect_u32("overruns come from the core", st.Overruns, 77);
    expect_u32("and bad data does too", st.BadData, 99);

    reset();
    req(&io, S2_GETGLOBALSTATS);
    io.ios2_StatData = NULL;
    netdev_perform(&opener, &io);
    expect_u32("global statistics with no buffer", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ARGUMENT);
}

/* ============================================================ interface == */

static void p_configure_and_online(void)
{
    static const UBYTE want[6] = { 0x02, 0x41, 0x4d, 0x49, 0x00, 0x07 };
    static const UBYTE other[6] = { 0x02, 0x41, 0x4d, 0x49, 0x00, 0x08 };
    struct IOSana2Req  io;

    /* A first configure takes the caller's address and brings the unit up. */
    reset();
    unit.nu_Configured = 0;
    unit.nu_Online     = 0;

    req(&io, S2_CONFIGINTERFACE);
    memcpy(io.ios2_SrcAddr, want, 6);
    netdev_perform(&opener, &io);
    expect_u32("the interface is configured", (unsigned long)(UBYTE)last_err, 0);
    expect(memcmp(nic->mac, want, 6) == 0, "with the address it was given");
    expect(online_calls == 1, "and brought online");

    /*
     * A second configure is refused -- and the caller is told which address
     * is actually in force, rather than being refused and left believing its
     * own was taken.
     */
    req(&io, S2_CONFIGINTERFACE);
    memcpy(io.ios2_SrcAddr, other, 6);
    netdev_perform(&opener, &io);
    expect_u32("configuring twice", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_STATE);
    expect_u32("says it is already configured", last_wire,
               (unsigned long)S2WERR_IS_CONFIGURED);
    expect(memcmp(io.ios2_SrcAddr, want, 6) == 0,
           "and hands back the address in force, not the one refused");
    expect(memcmp(nic->mac, want, 6) == 0, "which is unchanged");

    /* A configure whose bring-up fails leaves the unit unconfigured, or the
       address is taken and the interface can never be configured again. */
    reset();
    unit.nu_Configured = 0;
    unit.nu_Online     = 0;
    online_answer      = -1;

    req(&io, S2_CONFIGINTERFACE);
    memcpy(io.ios2_SrcAddr, want, 6);
    netdev_perform(&opener, &io);
    expect_u32("a configure whose bring-up failed",
               (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_OUTOFSERVICE);
    expect_u32("leaves the unit unconfigured", unit.nu_Configured, 0);

    /* S2_ONLINE before any configure. */
    reset();
    unit.nu_Configured = 0;
    unit.nu_Online     = 0;
    req(&io, S2_ONLINE);
    netdev_perform(&opener, &io);
    expect_u32("online before configure", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_STATE);
    expect_u32("says it is not configured", last_wire,
               (unsigned long)S2WERR_NOT_CONFIGURED);

    /* S2_ONLINE on a unit already online does not touch the hardware. */
    reset();
    req(&io, S2_ONLINE);
    netdev_perform(&opener, &io);
    expect_u32("online when already online", (unsigned long)(UBYTE)last_err, 0);
    expect(online_calls == 0, "does not bring the card up again");

    /* S2_OFFLINE posts the event; a second one cancels a pending resume
       instead, because there is nothing left to take down. */
    reset();
    req(&io, S2_OFFLINE);
    netdev_perform(&opener, &io);
    expect_u32("offline is answered", (unsigned long)(UBYTE)last_err, 0);
    expect(offline_calls == 1, "and took the unit down");
    expect_u32("posting the offline event", offline_event,
               (unsigned long)S2EVENT_OFFLINE);

    req(&io, S2_OFFLINE);
    netdev_perform(&opener, &io);
    expect(offline_calls == 1, "a second offline does not take it down again");
    expect(cancel_resume_calls == 1, "but cancels a pending resume");

    /* S2_GETSTATIONADDRESS hands back the address in force and the factory
       one, and clears the whole SANA-II address field rather than the six
       bytes it fills. */
    reset();
    memcpy(nic->mac, want, 6);
    memcpy(nic->factory, other, 6);

    req(&io, S2_GETSTATIONADDRESS);
    memset(io.ios2_SrcAddr, 0x5a, SANA2_MAX_ADDR_BYTES);
    memset(io.ios2_DstAddr, 0x5a, SANA2_MAX_ADDR_BYTES);
    netdev_perform(&opener, &io);
    expect_u32("the station address is answered", (unsigned long)(UBYTE)last_err, 0);
    expect(memcmp(io.ios2_SrcAddr, want, 6) == 0,
           "the address in force is the source");
    expect(memcmp(io.ios2_DstAddr, other, 6) == 0,
           "and the factory address the destination");
    {
        UWORD i;
        for (i = 6; i < (UWORD)SANA2_MAX_ADDR_BYTES; i++)
            expect(io.ios2_SrcAddr[i] == 0 && io.ios2_DstAddr[i] == 0,
                   "and the rest of the address field is cleared");
    }
}

/* ============================================================ flush ====== */

/*
 * A caller flushes so that teardown is safe.  Everything of that opener's must
 * come back -- reads, orphans and event waits -- and so must its writes, which
 * are queued on the UNIT and not on the opener: one of its own requests still
 * live in the driver is exactly what the flush is preventing.
 */
static void q_flush_returns_everything(void)
{
    struct IOSana2Req rd, orph, ev, io;

    reset();

    req(&rd, CMD_READ);
    netdev_perform(&opener, &rd);
    req(&orph, S2_READORPHAN);
    netdev_perform(&opener, &orph);

    /* An event wait is linked by netdev_event.c, which is stood in for here,
       so the request is put on the list the way that file would. */
    req(&ev, S2_ONEVENT);
    ev.ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    AddTail(&opener.op_Events, &ev.ios2_Req.io_Message.mn_Node);

    nreplies = 0;
    req(&io, CMD_FLUSH);
    netdev_perform(&opener, &io);

    expect_u32("the flush is answered", (unsigned long)(UBYTE)last_err, 0);
    expect_u32("the read came back aborted",
               (unsigned long)(UBYTE)rd.ios2_Req.io_Error,
               (unsigned long)(UBYTE)IOERR_ABORTED);
    expect_u32("the orphan read came back aborted",
               (unsigned long)(UBYTE)orph.ios2_Req.io_Error,
               (unsigned long)(UBYTE)IOERR_ABORTED);
    expect_u32("the event wait came back aborted",
               (unsigned long)(UBYTE)ev.ios2_Req.io_Error,
               (unsigned long)(UBYTE)IOERR_ABORTED);
    expect(opener.op_Reads.lh_Head->ln_Succ == NULL &&
           opener.op_Orphans.lh_Head->ln_Succ == NULL &&
           opener.op_Events.lh_Head->ln_Succ == NULL,
           "and all three lists are empty");
    expect(drop_writes_calls == 1, "the opener's writes were dropped too");
    expect(rescan_calls == 1, "and the event mask was recomputed");
    balanced("the flush left Disable balanced");
}

/* ============================================================ abort ====== */

/*
 * AbortIO has to find the request on whichever of the four lists it is on, and
 * only an aborted S2_ONEVENT drags a rescan behind it: an aborted CMD_READ is
 * routine and must not walk every opener.
 */
static void r_abort_finds_each_list(void)
{
    struct IOSana2Req rd, orph, ev, wr, stranger;
    BOOL              found;

    reset();

    req(&rd, CMD_READ);
    netdev_perform(&opener, &rd);
    rescan_calls = 0;

    found = netdev_abort(&opener, &rd);
    expect(found == TRUE, "an aborted read is found");
    expect_u32("and comes back aborted", (unsigned long)(UBYTE)rd.ios2_Req.io_Error,
               (unsigned long)(UBYTE)IOERR_ABORTED);
    expect(rescan_calls == 0, "without a rescan of every opener");

    req(&orph, S2_READORPHAN);
    netdev_perform(&opener, &orph);
    expect(netdev_abort(&opener, &orph) == TRUE,
           "an aborted orphan read is found");

    req(&ev, S2_ONEVENT);
    AddTail(&opener.op_Events, &ev.ios2_Req.io_Message.mn_Node);
    rescan_calls = 0;
    expect(netdev_abort(&opener, &ev) == TRUE, "an aborted event is found");
    expect(rescan_calls == 1, "and it alone triggers the rescan");

    req(&wr, CMD_WRITE);
    AddTail(&unit.nu_Writes, &wr.ios2_Req.io_Message.mn_Node);
    expect(netdev_abort(&opener, &wr) == TRUE,
           "an aborted write is found on the unit's list");

    /* A request on no list of ours is not ours to complete. */
    req(&stranger, CMD_READ);
    nreplies = 0;
    expect(netdev_abort(&opener, &stranger) == FALSE,
           "a request on none of the lists is refused");
    expect(nreplies == 0, "and is not replied to");

    expect(netdev_abort(NULL, &stranger) == FALSE,
           "and an abort with no opener is refused");
    balanced("the aborts left Disable balanced");
}

/* A dispatch with no opener at all is the detached-request case, and answers
   the error Exec's own devices answer. */
static void s_no_opener(void)
{
    struct IOSana2Req io;

    reset();
    req(&io, CMD_READ);
    netdev_perform(NULL, &io);
    expect_u32("a dispatch with no opener", (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)IOERR_BADADDRESS);
    expect_u32("generically", last_wire,
               (unsigned long)S2WERR_GENERIC_ERROR);
}

int main(void)
{
    a_multicast_group_bit();
    b_multicast_refcounts();
    c_multicast_table_fills();
    d_read_queue_ends();
    e_read_needs_a_hook();
    f_offline_refuses_reads();
    g_write_paths();
    h_devicequery_honours_sizeavailable();
    i_nsquery_replies_by_hand();
    j_the_advertised_list_is_the_real_one();
    k_unknown_commands();
    l_onevent_masks();
    m_track_types();
    n_special_stats_respect_the_caller();
    o_global_stats_come_from_the_core();
    p_configure_and_online();
    q_flush_returns_everything();
    r_abort_finds_each_list();
    s_no_opener();

    if (failures != 0)
    {
        printf("netdev_cmds: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("netdev_cmds: %d checks ok\n", checks);

    return 0;
}
