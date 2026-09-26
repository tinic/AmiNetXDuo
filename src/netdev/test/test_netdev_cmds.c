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
#include <stddef.h>
#include <string.h>

#include <proto/exec.h>

#include "netdev_internal.h"
#include "aminetxduo/anxs2ext.h"

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

static int forbid_depth;

VOID Forbid(VOID)
{
    forbid_depth++;
}

VOID Permit(VOID)
{
    forbid_depth--;
    if (forbid_depth < 0)
    {
        printf("FAIL Permit() without a matching Forbid()\n");
        failures++;
        forbid_depth = 0;
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

/* ANXD_CMD_RX_POLL runs the chip service when a core left frames behind; this
   harness has no core, so it counts the call and reports nothing found. */
static int interrupt_calls;

ULONG netdev_interrupt(NetdevUnit *unit)
{
    interrupt_calls++;
    unit->nu_Nic.rx_behind = 0;
    return 0;
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
    /* Private-command tests model an opener which completed negotiation.
       z_unnegotiated_extensions_are_inert() clears this explicitly. */
    opener.op_Extensions = ANXD_S2F_ALL;

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
    io->ios2_Req.io_Unit    = &unit.nu_ExecUnit;
    io->ios2_BufferManagement = &opener;
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

                snprintf(what, sizeof(what),
                         "S2_DEVICEQUERY wrote at offset %lu of %lu"
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
    std.io_Message.mn_Length = sizeof(std);   /* as CreateIORequest() sets it */
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
    std.io_Message.mn_Length = sizeof(std);   /* as CreateIORequest() sets it */
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
    std.io_Message.mn_Length = sizeof(std);   /* as CreateIORequest() sets it */
    std.io_Command = NSCMD_DEVICEQUERY;
    std.io_Data    = NULL;
    std.io_Length  = 64;
    netdev_perform(&opener, (struct IOSana2Req *)&std);
    expect_u32("a new-style query with no buffer", (unsigned long)(UBYTE)std.io_Error,
               (unsigned long)(UBYTE)IOERR_BADLENGTH);

    /* IOF_QUICK set means the caller is not waiting on a reply port. */
    reset();
    memset(&std, 0, sizeof(std));
    std.io_Message.mn_Length = sizeof(std);   /* as CreateIORequest() sets it */
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
/*
 * ISSUE #39, THE FORM mcastfilter 1.20 USES.  It sends NSCMD_DEVICEQUERY in
 * the IOSana2Req it opened (CreateIORequest(port, 88)), buffer in ios2_Data,
 * size in ios2_DataLength -- and io_Data/io_Length, which that request's
 * ios2_SrcAddr/ios2_PacketType occupy, stay zero.  The handler read only the
 * IOStdReq form and answered IOERR_BADLENGTH: "not a NewStyle device".
 *
 * The rules pinned here: the IOStdReq form wins whenever it names a buffer of
 * at least 16 bytes; otherwise, and only in a request whose mn_Length says it
 * is a full IOSana2Req, the SANA-II form is used; a short or length-less
 * request never has its SANA-II fields read.  Every buffer carries a canary so
 * a write outside the 16 answered bytes, or into the losing form, shows up.
 */
typedef struct
{
    ULONG  DevQueryFormat;
    ULONG  SizeAvailable;
    UWORD  DeviceType;
    UWORD  DeviceSubType;
    UWORD *SupportedCommands;
    UBYTE  canary[16];
} NsdAnswer;

/* What the handler writes: 16 bytes on m68k, wider on a 64-bit host. */
#define NSD_SIZE ((ULONG)offsetof(NsdAnswer, canary))

static void nsd_answer_init(NsdAnswer *a)
{
    memset(a, 0x5a, sizeof(*a));
}

static int nsd_answer_untouched(const NsdAnswer *a)
{
    NsdAnswer ref;

    nsd_answer_init(&ref);
    return memcmp(a, &ref, sizeof(ref)) == 0;
}

static int nsd_canary_intact(const NsdAnswer *a)
{
    int i;

    for (i = 0; i < (int)sizeof(a->canary); i++)
        if (a->canary[i] != 0x5a)
            return 0;
    return 1;
}

/* A full request as mcastfilter builds it, before any form is chosen. */
static void nsd_sana(struct IOSana2Req *io, UWORD mn_length)
{
    memset(io, 0, sizeof(*io));
    io->ios2_Req.io_Message.mn_Length = mn_length;
    io->ios2_Req.io_Command           = NSCMD_DEVICEQUERY;
}

/* A request built by hand at its real size, with the bytes after it
   belonging to someone else: a canaried tail holding what an IOSana2Req's
   ios2_Data/ios2_DataLength would read, pointed at a decoy. */
typedef struct
{
    struct IOStdReq std;
    UBYTE           tail[sizeof(struct IOSana2Req) + 32 - sizeof(struct IOStdReq)];
} NsdFrame;

typedef struct
{
    struct IORequest req;
    UBYTE            tail[sizeof(struct IOSana2Req) + 32 - sizeof(struct IORequest)];
} NsdBaseFrame;

static NsdAnswer nsd_decoy;

static void nsd_frame(NsdFrame *f)
{
    struct IOSana2Req *wide = (struct IOSana2Req *)f;

    memset(f, 0xa5, sizeof(*f));
    wide->ios2_Data             = &nsd_decoy;    /* in the tail */
    wide->ios2_DataLength       = NSD_SIZE + 8;
    wide->ios2_WireError        = 0xa5a5a5a5UL;
    memset(&f->std.io_Message.mn_Node, 0, sizeof(f->std.io_Message.mn_Node));
    f->std.io_Message.mn_Length = 0;             /* built by hand, unset */
    f->std.io_Command           = NSCMD_DEVICEQUERY;
    f->std.io_Flags             = 0;
    f->std.io_Error             = 0;
    f->std.io_Actual            = 0;
    f->std.io_Data              = NULL;
    f->std.io_Length            = 0;
}

static int nsd_frame_tail_intact(const NsdFrame *f)
{
    NsdFrame ref;

    nsd_frame(&ref);
    return memcmp(f->tail, ref.tail, sizeof(ref.tail)) == 0;
}

static void nsd_base_frame(NsdBaseFrame *f)
{
    memset(f, 0xa5, sizeof(*f));
    memset(&f->req.io_Message.mn_Node, 0, sizeof(f->req.io_Message.mn_Node));
    f->req.io_Message.mn_Length = (UWORD)sizeof(struct IORequest);
    f->req.io_Command           = NSCMD_DEVICEQUERY;
    f->req.io_Flags             = 0;
    f->req.io_Error             = 0;
}

/* From where an IOStdReq's io_Actual would be: on m68k that is the first
   byte past the IORequest, on a 64-bit host it is inside its padding. */
static int nsd_base_frame_tail_intact(const NsdBaseFrame *f)
{
    NsdBaseFrame ref;
    size_t       from = offsetof(struct IOStdReq, io_Actual);

    nsd_base_frame(&ref);
    return memcmp((const UBYTE *)f + from, (const UBYTE *)&ref + from,
                  sizeof(ref) - from) == 0;
}

static void i3_nsquery_short_frames(void)
{
    NsdFrame     f;
    NsdBaseFrame b;
    NsdAnswer    a;

    /* A real 48-byte IOStdReq with mn_Length 0 (#64): refused, neither form
       read, and not one byte past its end written. */
    reset();
    nsd_frame(&f);
    nsd_answer_init(&nsd_decoy);
    nsd_answer_init(&a);
    f.std.io_Data   = &a;
    f.std.io_Length = NSD_SIZE;
    netdev_nsd_query((struct IOSana2Req *)&f);
    expect_u32("a zero-length 48-byte IOStdReq query is refused",
               (unsigned long)(UBYTE)f.std.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_answer_untouched(&a), "  and io_Data is not written");
    expect(nsd_answer_untouched(&nsd_decoy),
           "  and ios2_Data past the IOStdReq is not read as a buffer");
    expect(nsd_frame_tail_intact(&f),
           "  and not one byte past the 48-byte IOStdReq was written");
    expect(replies == 1, "  and it was replied to");

    /* A real 32-byte IORequest stating its size: refused through the base
       fields only; io_Actual, at offset 32, is the tail's. */
    reset();
    nsd_base_frame(&b);
    netdev_nsd_query((struct IOSana2Req *)&b);
    expect_u32("a bare IORequest query is refused",
               (unsigned long)(UBYTE)b.req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_base_frame_tail_intact(&b),
           "  and not one byte from io_Actual on was written");
    expect(replies == 1, "  and it was replied to");
}

static void i2_nsquery_both_forms(void)
{
    struct IOSana2Req io;
    NsdAnswer         a;
    NsdAnswer         b;
    struct IOStdReq  *std = (struct IOStdReq *)&io;

    /* 1. The SANA-II form, full request: answered into ios2_Data. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOSana2Req));
    nsd_answer_init(&a);
    io.ios2_Data       = &a;
    io.ios2_DataLength = NSD_SIZE;
    io.ios2_WireError  = 0xdeadbeefUL;
    netdev_perform(&opener, &io);
    expect_u32("the SANA-II form is answered", (unsigned long)(UBYTE)io.ios2_Req.io_Error, 0);
    expect_u32("  with SizeAvailable 16", (unsigned long)a.SizeAvailable, NSD_SIZE);
    expect_u32("  and DeviceType SANA-II", a.DeviceType, 7);
    expect(a.SupportedCommands != NULL, "  and the command list mcastfilter looks for");
    expect_u32("  ios2_DataLength carries the byte count", (unsigned long)io.ios2_DataLength, NSD_SIZE);
    expect_u32("  and a success has no wire error", (unsigned long)io.ios2_WireError, 0);
    expect(nsd_canary_intact(&a), "  and nothing past the 16 bytes was written");
    expect(replies == 1, "  and it was replied to");

    /* 2. A full request naming both: the SANA-II form wins, because in a
          full request io_Data/io_Length are ios2_SrcAddr/ios2_PacketType. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOSana2Req));
    nsd_answer_init(&a);
    nsd_answer_init(&b);
    std->io_Data       = &a;
    std->io_Length     = NSD_SIZE;
    io.ios2_Data       = &b;
    io.ios2_DataLength = NSD_SIZE;
    netdev_nsd_query(&io);
    expect_u32("in a full request naming both, the SANA-II form is used",
               (unsigned long)b.SizeAvailable, NSD_SIZE);
    expect(nsd_answer_untouched(&a), "  and the IOStdReq-shaped buffer is not written");
    expect(nsd_canary_intact(&b), "  within its 16 bytes");

    /* 3. With a one-short IOStdReq buffer as well, still the SANA-II form. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOSana2Req));
    nsd_answer_init(&a);
    nsd_answer_init(&b);
    std->io_Data       = &a;
    std->io_Length     = NSD_SIZE - 1;
    io.ios2_Data       = &b;
    io.ios2_DataLength = NSD_SIZE;
    netdev_nsd_query(&io);
    expect(nsd_answer_untouched(&a), "a one-short IOStdReq-shaped buffer is never written");
    expect_u32("  the valid SANA-II buffer answers instead", (unsigned long)b.SizeAvailable, NSD_SIZE);

    /* 4. An undersized SANA-II buffer is refused, and not written. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOSana2Req));
    nsd_answer_init(&b);
    io.ios2_Data       = &b;
    io.ios2_DataLength = NSD_SIZE - 1;
    netdev_nsd_query(&io);
    expect_u32("a one-short SANA-II buffer is refused",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_answer_untouched(&b), "  and not written");
    expect(replies == 1, "  and the refusal is replied to");

    /* 5. mn_Length 0 is refused by the query (#64), neither form read.
          (a) An unsized FULL request, reused: ios2_Data names a real buffer,
          and io_Data/io_Length are the stale ios2_SrcAddr/ios2_PacketType. */
    reset();
    nsd_sana(&io, 0);
    nsd_answer_init(&a);
    nsd_answer_init(&b);
    io.ios2_Data       = &b;
    io.ios2_DataLength = NSD_SIZE;
    std->io_Data       = &a;              /* the stale ios2_SrcAddr[0..3] */
    std->io_Length     = 0x0800;          /* the stale ios2_PacketType */
    netdev_nsd_query(&io);
    expect_u32("a zero-length full request is refused",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_answer_untouched(&a), "  and nothing is written at the MAC-derived address");
    expect(nsd_answer_untouched(&b), "  nor through ios2_Data");

    /* (b) A hand-built 48-byte IOStdReq with no length: io_Data is real and
          what lies past offset 48 (ios2_DataLength/ios2_Data) is someone
          else's memory, here a decoy buffer. */
    reset();
    nsd_sana(&io, 0);
    nsd_answer_init(&a);
    nsd_answer_init(&b);
    std->io_Data       = &a;
    std->io_Length     = NSD_SIZE;
    io.ios2_Data       = &b;              /* past the 48 bytes */
    io.ios2_DataLength = NSD_SIZE;
    netdev_nsd_query(&io);
    expect_u32("a zero-length 48-byte IOStdReq is refused",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_answer_untouched(&a), "  and io_Data is not written");
    expect(nsd_answer_untouched(&b), "  and nothing past its end is read as a buffer");

    /* (c) A stated length below an IOStdReq: io_Data/io_Length are past it. */
    reset();
    nsd_sana(&io, (UWORD)(sizeof(struct IOStdReq) - 2));
    nsd_answer_init(&a);
    std->io_Data   = &a;
    std->io_Length = NSD_SIZE;
    netdev_nsd_query(&io);
    expect_u32("a request shorter than an IOStdReq is refused",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_answer_untouched(&a), "  and io_Data is not written");
    expect(replies == 1, "  and the refusal is replied to");

    /* (d) A bare IORequest (32 bytes): io_Actual would be past its end. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IORequest));
    std->io_Actual = 0xdeadbeefUL;
    netdev_nsd_query(&io);
    expect_u32("a bare IORequest is refused",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect_u32("  and io_Actual, past its end, is not written",
               (unsigned long)std->io_Actual, 0xdeadbeefUL);

    /* 6. A short request (a 48-byte IOStdReq): same, and it says so itself. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOStdReq));
    nsd_answer_init(&b);
    io.ios2_Data       = &b;
    io.ios2_DataLength = NSD_SIZE;
    netdev_nsd_query(&io);
    expect_u32("a short request with no IOStdReq buffer is refused",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_answer_untouched(&b), "  and the bytes past its end are not read as a buffer");

    /* 7. The IOStdReq form in a short request still works. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOStdReq));
    nsd_answer_init(&a);
    std->io_Data   = &a;
    std->io_Length = NSD_SIZE;
    netdev_nsd_query(&io);
    expect_u32("a short request's IOStdReq form is answered",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, 0);
    expect_u32("  io_Actual is the byte count", (unsigned long)std->io_Actual, NSD_SIZE);
    expect(nsd_canary_intact(&a), "  within its 16 bytes");

    /*
     * 8. THE REUSED REQUEST (codex review of #41).  A full request after a
     *    CMD_READ: ios2_Data names the caller's buffer, and io_Data/io_Length
     *    are a stale source MAC and EtherType 0x0800.  The answer must go to
     *    ios2_Data; the MAC-derived "buffer" (a decoy here) must not be
     *    written.  On m68k the aliasing is exact (the driver asserts it);
     *    the host lays the fields out differently, so this pins the rule.
     */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOSana2Req));
    nsd_answer_init(&a);
    nsd_answer_init(&b);
    std->io_Data       = &a;                 /* the stale "source address" */
    std->io_Length     = 0x0800;             /* the stale packet type */
    io.ios2_Data       = &b;
    io.ios2_DataLength = 1514;
    netdev_nsd_query(&io);
    expect_u32("a reused request answers into ios2_Data",
               (unsigned long)b.SizeAvailable, NSD_SIZE);
    expect(nsd_answer_untouched(&a), "  and never the MAC-derived address");

    /* 9. The same stale request with ios2_Data cleared: nothing believable is
          left, so it is refused -- EtherType 0x0800 is no NewStyle length. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOSana2Req));
    nsd_answer_init(&a);
    std->io_Data       = &a;
    std->io_Length     = 0x0800;
    netdev_nsd_query(&io);
    expect_u32("a stale EtherType is not a NewStyle length in a full request",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_answer_untouched(&a), "  and the MAC-derived address is not written");

    /* 10. A full request's io_Data is never read, even a fresh cast one: its
           size says SANA-II, and a caller wanting the NewStyle form from an
           88-byte allocation sets mn_Length to an IOStdReq's. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOSana2Req));
    nsd_answer_init(&a);
    std->io_Data   = &a;
    std->io_Length = NSD_SIZE;
    netdev_nsd_query(&io);
    expect_u32("a full request's io_Data is never used",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_answer_untouched(&a), "  and not written");

    /* ...and the same 88-byte allocation saying it is an IOStdReq works. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOStdReq));
    nsd_answer_init(&a);
    std->io_Data   = &a;
    std->io_Length = NSD_SIZE;
    netdev_nsd_query(&io);
    expect_u32("the same allocation with mn_Length of an IOStdReq is answered",
               (unsigned long)a.SizeAvailable, NSD_SIZE);

    /* 11. An odd io_Data in a full request is not believed either. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOSana2Req));
    nsd_answer_init(&a);
    std->io_Data   = (UBYTE *)&a + 1;
    std->io_Length = NSD_SIZE;
    netdev_nsd_query(&io);
    expect_u32("an odd io_Data in a full request is refused",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_answer_untouched(&a), "  and not written");

    /*
     * 12. THE HEURISTIC'S HOLE (codex review of 55545211).  A reused request
     *     with no ios2_Data, an 802.3-length packet type of 64 and an even
     *     "MAC" decoy passed the old length/alignment heuristic.  A full
     *     request's io_Data is now never read at all: refused, nothing
     *     written.
     */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOSana2Req));
    nsd_answer_init(&a);
    io.ios2_DstAddr[0] = 0x02; io.ios2_DstAddr[1] = 0x41;
    io.ios2_DstAddr[5] = 0x07;
    std->io_Data   = &a;                     /* even, and plausible */
    std->io_Length = 64;
    netdev_nsd_query(&io);
    expect_u32("a reused request with PacketType 64 and an even MAC decoy is refused",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_answer_untouched(&a), "  and the MAC-derived address is not written");

    /* 13. Both destinations unusable: a too-short ios2_Data and a reused
           io_Data.  Refused, and neither is written. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOSana2Req));
    nsd_answer_init(&a);
    nsd_answer_init(&b);
    io.ios2_Data       = &b;
    io.ios2_DataLength = NSD_SIZE - 1;
    std->io_Data       = &a;
    std->io_Length     = 64;
    netdev_nsd_query(&io);
    expect_u32("with both destinations unusable the query is refused",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_answer_untouched(&a) && nsd_answer_untouched(&b),
           "  and neither buffer is written");

    /* 14. Any SANA-II history counts: statistics, then a cast query. */
    reset();
    nsd_sana(&io, (UWORD)sizeof(struct IOSana2Req));
    nsd_answer_init(&a);
    io.ios2_StatData = &b;
    std->io_Data     = &a;
    std->io_Length   = NSD_SIZE;
    netdev_nsd_query(&io);
    expect_u32("a request that carried S2 statistics is not believed either",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)IOERR_BADLENGTH);
    expect(nsd_answer_untouched(&a), "  and not written");
}

static void j_the_advertised_list_is_the_real_one(void)
{
    struct IOStdReq   std;
    UWORD            *list;
    UWORD             n;

    reset();
    memset(&std, 0, sizeof(std));
    std.io_Message.mn_Length = sizeof(std);   /* as CreateIORequest() sets it */
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

        snprintf(what, sizeof(what),
                 "advertised command 0x%04x is not refused as unknown",
                 (unsigned)list[n]);
        expect(!(last_err == (LONG)IOERR_NOCMD &&
                 last_wire == (ULONG)S2WERR_GENERIC_ERROR), what);
    }

    expect(n == 26, "the advertised list is the length this test read it at");
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

    expect(full == 20, "the table is the twenty records this test read");

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
    static const UBYTE multicast[6] = { 0x01, 0x00, 0x5e, 0x01, 0x02, 0x03 };
    static const UBYTE zero[6] = { 0, 0, 0, 0, 0, 0 };
    struct IOSana2Req  io;

    /* A station address must be an individual, non-zero Ethernet address.
       Rejecting it must not configure or start the unit. */
    reset();
    unit.nu_Configured = 0;
    unit.nu_Online     = 0;

    req(&io, S2_CONFIGINTERFACE);
    memcpy(io.ios2_SrcAddr, multicast, 6);
    netdev_perform(&opener, &io);
    expect_u32("a multicast station address",
               (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ADDRESS);
    expect_u32("names the source address", last_wire,
               (unsigned long)S2WERR_SRC_ADDRESS);
    expect(unit.nu_Configured == 0 && online_calls == 0,
           "and leaves the interface untouched");

    req(&io, S2_CONFIGINTERFACE);
    memcpy(io.ios2_SrcAddr, zero, 6);
    netdev_perform(&opener, &io);
    expect_u32("an all-zero station address",
               (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ADDRESS);
    expect(unit.nu_Configured == 0 && online_calls == 0,
           "and also leaves the interface untouched");

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

/* ANXD_CMD_RX_POLL: runs the chip service only when a core is holding frames,
   answers quick either way, and is in the supported-command list. */
static void t_rx_poll(void)
{
    struct IOSana2Req io;
    UWORD            *cmds;
    int               listed = 0;

    /* A unit that holds nothing for a late read declines polls once. */
    reset();
    req(&io, ANXD_CMD_RX_POLL);
    io.ios2_Req.io_Flags = IOF_QUICK;
    unit.nu_Nic.rx_holds = 0;
    interrupt_calls = 0;
    netdev_perform(&opener, &io);
    expect_u32("a core without a holding ring declines the poll",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, (unsigned long)(UBYTE)S2ERR_NOT_SUPPORTED);
    expect(interrupt_calls == 0, "and runs nothing");

    reset();
    unit.nu_Nic.rx_holds = 1;
    req(&io, ANXD_CMD_RX_POLL);
    io.ios2_Req.io_Flags = IOF_QUICK;
    interrupt_calls = 0;
    netdev_perform(&opener, &io);
    expect_u32("a poll with nothing held", (unsigned long)(UBYTE)io.ios2_Req.io_Error, 0);
    expect(interrupt_calls == 0, "does not run the chip service");
    expect((io.ios2_Req.io_Flags & IOF_QUICK) != 0, "and stays quick");

    reset();
    unit.nu_Nic.rx_holds = 1;
    req(&io, ANXD_CMD_RX_POLL);
    io.ios2_Req.io_Flags = IOF_QUICK;
    unit.nu_Nic.rx_behind = 1;
    interrupt_calls = 0;
    netdev_perform(&opener, &io);
    expect(interrupt_calls == 1, "a poll with frames held runs the chip service once");
    expect(unit.nu_Nic.rx_behind == 0, "and the service cleared the flag");
    expect_u32("with no error", (unsigned long)(UBYTE)io.ios2_Req.io_Error, 0);

    reset();
    unit.nu_Nic.rx_holds = 1;
    req(&io, ANXD_CMD_RX_POLL);
    unit.nu_Nic.rx_behind = 1;
    unit.nu_Online        = 0;
    interrupt_calls = 0;
    netdev_perform(&opener, &io);
    expect(interrupt_calls == 0, "an offline unit is not serviced by a poll");

    /* Also the type note: a CMD_READ remembers its packet type so a claim
       can tell a reader that is behind from a type nobody reads. */
    reset();
    opener.op_ReadTypeCount = 0;
    opener.op_ReadTypeLast  = 0;
    req(&io, CMD_READ);
    io.ios2_PacketType = 0x0800;
    netdev_perform(&opener, &io);
    expect(netdev_reads_type(&opener, 0x0800), "a posted read notes its type");
    expect(!netdev_reads_type(&opener, 0x86DD), "and no other");

    /* The list, through the query that hands it out. */
    {
        struct IOStdReq   std;
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
        std.io_Message.mn_Length = sizeof(std);   /* as CreateIORequest() sets it */
        memset(&answer, 0, sizeof(answer));
        std.io_Command = NSCMD_DEVICEQUERY;
        std.io_Data    = &answer;
        std.io_Length  = sizeof(answer);
        netdev_perform(&opener, (struct IOSana2Req *)&std);
        for (cmds = answer.SupportedCommands; cmds != NULL && *cmds != 0; cmds++)
            if (*cmds == ANXD_CMD_RX_POLL)
                listed = 1;
    }
    expect(listed, "ANXD_CMD_RX_POLL is in the supported-command list");
}

/* ANXD_CMD_RX_CAPACITY answers what the core said at attach, quick, and is
   advertised. */
static void v_rx_capacity(void)
{
    struct IOSana2Req io;
    int               listed = 0;

    reset();
    unit.nu_Nic.rx_capacity = 52UL * 256UL;
    req(&io, ANXD_CMD_RX_CAPACITY);
    io.ios2_DataLength = 0;
    netdev_perform(&opener, &io);
    expect(last_err == 0, "ANXD_CMD_RX_CAPACITY is answered");
    expect(io.ios2_DataLength == 52UL * 256UL,
           "ANXD_CMD_RX_CAPACITY reports the core's ring bytes");

    unit.nu_Nic.rx_capacity = 0;
    req(&io, ANXD_CMD_RX_CAPACITY);
    io.ios2_DataLength = 1;
    netdev_perform(&opener, &io);
    expect(io.ios2_DataLength == 0,
           "a core that cannot say answers 0");

    {
        struct IOStdReq   std;
        struct
        {
            ULONG  DevQueryFormat;
            ULONG  SizeAvailable;
            UWORD  DeviceType;
            UWORD  DeviceSubType;
            UWORD *SupportedCommands;
        } answer;
        const UWORD *cmds;

        reset();
        memset(&std, 0, sizeof(std));
        std.io_Message.mn_Length = sizeof(std);   /* as CreateIORequest() sets it */
        memset(&answer, 0, sizeof(answer));
        std.io_Command = NSCMD_DEVICEQUERY;
        std.io_Data    = &answer;
        std.io_Length  = sizeof(answer);
        netdev_perform(&opener, (struct IOSana2Req *)&std);
        for (cmds = answer.SupportedCommands; cmds != NULL && *cmds != 0; cmds++)
            if (*cmds == ANXD_CMD_RX_CAPACITY)
                listed = 1;
    }
    expect(listed, "ANXD_CMD_RX_CAPACITY is in the supported-command list");
}

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

/* NSD reserves $4000-$7fff and $c000-$ffff for the OS team.  Keep the
   extension in a third-party block even if its individual numbers move. */
static void w_private_commands_are_in_an_nsd_vendor_block(void)
{
    expect((ANXD_CMD_RX_POLL & 0xc000U) == 0x8000U,
           "ANXD_CMD_RX_POLL is in an NSD third-party block");
    expect((ANXD_CMD_RX_CAPACITY & 0xc000U) == 0x8000U,
           "ANXD_CMD_RX_CAPACITY is in an NSD third-party block");
    expect((ANXD_CMD_RX_BATCH & 0xc000U) == 0x8000U,
           "ANXD_CMD_RX_BATCH is in an NSD third-party block");
    expect((ANXD_CMD_TX_FLUSH & 0xc000U) == 0x8000U,
           "ANXD_CMD_TX_FLUSH is in an NSD third-party block");
}

/* ANXD_CMD_TX_FLUSH: the core's flush under Forbid(), quick, once the unit
   is online; S2ERR_NOT_SUPPORTED from a core that cannot hold a start; not
   run on an offline unit; in the supported-command list. */
static int  tx_flush_calls;
static int  tx_flush_forbid_depth;
static VOID y_tx_flush_core(NetdevNic *nic)
{
    (void)nic;
    tx_flush_calls++;
    tx_flush_forbid_depth = forbid_depth;
}

static void y_tx_flush(void)
{
    struct IOSana2Req io;
    UWORD            *cmds;
    int               listed = 0;

    reset();
    unit.nu_Nic.tx_flush = NULL;
    req(&io, ANXD_CMD_TX_FLUSH);
    io.ios2_Req.io_Flags = IOF_QUICK;
    netdev_perform(&opener, &io);
    expect_u32("a core that cannot hold a start declines the flush",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error,
               (unsigned long)(UBYTE)S2ERR_NOT_SUPPORTED);

    reset();
    unit.nu_Nic.tx_flush = y_tx_flush_core;
    tx_flush_calls = 0;
    req(&io, ANXD_CMD_TX_FLUSH);
    io.ios2_Req.io_Flags = IOF_QUICK;
    netdev_perform(&opener, &io);
    expect(tx_flush_calls == 1, "a flush runs the core's flush once");
    expect(tx_flush_forbid_depth == 1, "under Forbid()");
    expect(forbid_depth == 0, "and Permit()s again");
    expect_u32("with no error", (unsigned long)(UBYTE)io.ios2_Req.io_Error, 0);
    expect((io.ios2_Req.io_Flags & IOF_QUICK) != 0, "and stays quick");

    reset();
    unit.nu_Nic.tx_flush = y_tx_flush_core;
    unit.nu_Online       = 0;
    tx_flush_calls = 0;
    req(&io, ANXD_CMD_TX_FLUSH);
    netdev_perform(&opener, &io);
    expect(tx_flush_calls == 0, "an offline unit's core is not flushed");
    expect_u32("and the command still answers 0",
               (unsigned long)(UBYTE)io.ios2_Req.io_Error, 0);

    reset();
    {
        struct IOStdReq std;
        struct
        {
            ULONG  DevQueryFormat;
            ULONG  SizeAvailable;
            UWORD  DeviceType;
            UWORD  DeviceSubType;
            UWORD *SupportedCommands;
        } answer;

        memset(&std, 0, sizeof(std));
        std.io_Message.mn_Length = sizeof(std);   /* as CreateIORequest() sets it */
        memset(&answer, 0, sizeof(answer));
        std.io_Command = NSCMD_DEVICEQUERY;
        std.io_Data    = &answer;
        std.io_Length  = sizeof(answer);
        netdev_perform(&opener, (struct IOSana2Req *)&std);
        for (cmds = answer.SupportedCommands; cmds != NULL && *cmds != 0; cmds++)
            if (*cmds == ANXD_CMD_TX_FLUSH)
                listed = 1;
    }
    expect(listed, "ANXD_CMD_TX_FLUSH is in the supported-command list");
}

/* Command numbers in NSD's third-party range are not globally owned.  A
   different opener can use the same number for something unrelated, so the
   driver must not act on ours until that opener negotiated the matching bit. */
static void z_unnegotiated_extensions_are_inert(void)
{
    static const UWORD commands[] = {
        ANXD_CMD_RX_POLL, ANXD_CMD_RX_CAPACITY,
        ANXD_CMD_RX_BATCH, ANXD_CMD_TX_FLUSH
    };
    struct IOSana2Req io;
    UWORD i;

    for (i = 0; i < (UWORD)(sizeof(commands) / sizeof(commands[0])); i++)
    {
        reset();
        opener.op_Extensions = 0;
        unit.nu_Nic.rx_holds = 1;
        unit.nu_Nic.rx_batches = 1;
        unit.nu_Nic.rx_capacity = 8192;
        unit.nu_Nic.tx_flush = y_tx_flush_core;
        tx_flush_calls = 0;
        interrupt_calls = 0;

        req(&io, commands[i]);
        io.ios2_DataLength = 0x12345678UL;
        netdev_perform(&opener, &io);

        expect_u32("an unnegotiated private command is refused",
                   (unsigned long)(UBYTE)last_err,
                   (unsigned long)(UBYTE)S2ERR_NOT_SUPPORTED);
        expect(tx_flush_calls == 0 && interrupt_calls == 0,
               "and has no hardware-side effect");
        if (commands[i] == ANXD_CMD_RX_CAPACITY)
            expect_u32("and does not disclose a capacity result",
                       (unsigned long)io.ios2_DataLength, 0x12345678UL);
    }
}

static UBYTE *x_direct(APTR data, ULONG len) { (void)data; (void)len; return NULL; }
static VOID   x_filled(APTR data, ULONG len, ULONG sum, UBYTE flags)
{ (void)data; (void)len; (void)sum; (void)flags; }

/* ANXD_CMD_RX_BATCH: queued at the head of the reads with Filled cleared and
   its type noted, so the claim finds it and a later frame of the type with no
   read left counts as "the reader is behind"; refused for an opener the
   batch cannot serve, for an empty record, and when the unit is offline;
   answered by CMD_FLUSH like a read; in the supported-command list. */
static void x_rx_batch(void)
{
    struct IOSana2Req io;
    union
    {
        AnxdS2RxBatch b;
        UBYTE         bytes[sizeof(AnxdS2RxBatch) + 2 * sizeof(APTR)];
    } rec;
    APTR   cookie_a = &io;
    APTR   cookie_b = &rec;
    UWORD *cmds;
    int    listed = 0;

    reset();
    unit.nu_Nic.rx_batches = 1;
    opener.op_RxDirect  = (APTR)x_direct;
    opener.op_RxFilled  = (APTR)x_filled;
    opener.op_RxLinkHdr = TRUE;
    memset(&rec, 0, sizeof(rec));
    rec.b.Version   = ANXD_S2_RX_BATCH_VERSION;
    rec.b.Size      = (UWORD)sizeof(rec.bytes);
    rec.b.Count     = 2;
    rec.b.Filled    = 7;                 /* whatever the opener left there */
    rec.b.Cookie[0] = cookie_a;
    rec.b.Cookie[1] = cookie_b;
    req(&io, ANXD_CMD_RX_BATCH);
    io.ios2_Req.io_Flags   = IOF_QUICK;
    io.ios2_PacketType     = 0x0800;
    io.ios2_Data           = &rec.b;
    netdev_perform(&opener, &io);
    expect(opener.op_Reads.lh_Head == &io.ios2_Req.io_Message.mn_Node,
           "a batch is queued at the head of the reads");
    expect(replies == 0, "and not answered");
    expect((io.ios2_Req.io_Flags & IOF_QUICK) == 0, "and is never quick");
    expect(rec.b.Filled == 0, "with Filled cleared");
    expect(netdev_reads_type(&opener, 0x0800), "and its type noted");
    expect(netdev_is_batch(&io), "netdev_is_batch() knows it by command");
    {
        struct IOSana2Req rd1, rd2;

        req(&rd1, CMD_READ);
        rd1.ios2_PacketType = 0x0800;
        rd1.ios2_Data       = &rd1;
        netdev_perform(&opener, &rd1);
        req(&rd2, CMD_READ);
        rd2.ios2_PacketType = 0x0800;
        rd2.ios2_Data       = &rd2;
        netdev_perform(&opener, &rd2);
        expect(opener.op_Reads.lh_Head == &io.ios2_Req.io_Message.mn_Node,
               "a CMD_READ posted later leaves the batch at the head");
        expect(opener.op_Reads.lh_Head->ln_Succ == &rd2.ios2_Req.io_Message.mn_Node &&
               rd2.ios2_Req.io_Message.mn_Node.ln_Succ == &rd1.ios2_Req.io_Message.mn_Node,
               "and the reads keep newest-first order behind it");
    }

    reset();
    unit.nu_Nic.rx_batches = 1;
    req(&io, ANXD_CMD_RX_BATCH);
    io.ios2_PacketType = 0x0800;
    io.ios2_Data       = &rec.b;
    rec.b.Count        = 2;
    netdev_perform(&opener, &io);
    expect_u32("an opener without the direct pair is refused",
               (unsigned long)(UBYTE)last_err, (unsigned long)(UBYTE)S2ERR_NOT_SUPPORTED);
    expect(opener.op_Reads.lh_Head->ln_Succ == NULL, "and nothing is queued");

    reset();
    unit.nu_Nic.rx_batches = 1;
    opener.op_RxDirect  = (APTR)x_direct;
    opener.op_RxFilled  = (APTR)x_filled;
    opener.op_RxLinkHdr = FALSE;
    req(&io, ANXD_CMD_RX_BATCH);
    io.ios2_PacketType = 0x0800;
    io.ios2_Data       = &rec.b;
    netdev_perform(&opener, &io);
    expect_u32("without the link header it is refused",
               (unsigned long)(UBYTE)last_err, (unsigned long)(UBYTE)S2ERR_NOT_SUPPORTED);

    reset();
    unit.nu_Nic.rx_batches = 1;
    opener.op_RxDirect  = (APTR)x_direct;
    opener.op_RxFilled  = (APTR)x_filled;
    opener.op_RxLinkHdr = TRUE;
    opener.op_Raw       = 1;
    req(&io, ANXD_CMD_RX_BATCH);
    io.ios2_PacketType = 0x0800;
    io.ios2_Data       = &rec.b;
    netdev_perform(&opener, &io);
    expect_u32("a raw opener is refused",
               (unsigned long)(UBYTE)last_err, (unsigned long)(UBYTE)S2ERR_NOT_SUPPORTED);

    reset();
    unit.nu_Nic.rx_batches = 1;
    opener.op_RxDirect  = (APTR)x_direct;
    opener.op_RxFilled  = (APTR)x_filled;
    opener.op_RxLinkHdr = TRUE;
    req(&io, ANXD_CMD_RX_BATCH);
    io.ios2_PacketType = 0x0800;
    io.ios2_Data       = &rec.b;
    rec.b.Count        = 0;
    netdev_perform(&opener, &io);
    expect_u32("an empty record is a bad argument",
               (unsigned long)(UBYTE)last_err, (unsigned long)(UBYTE)S2ERR_BAD_ARGUMENT);

    reset();
    unit.nu_Nic.rx_batches = 1;
    opener.op_RxDirect  = (APTR)x_direct;
    opener.op_RxFilled  = (APTR)x_filled;
    opener.op_RxLinkHdr = TRUE;
    req(&io, ANXD_CMD_RX_BATCH);
    io.ios2_PacketType = 0x0800;
    io.ios2_Data       = &rec.b;
    rec.b.Count        = ANXD_S2_RX_BATCH_MAX + 1;
    netdev_perform(&opener, &io);
    expect_u32("an oversized record is a bad argument",
               (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ARGUMENT);
    rec.b.Count = 2;

    reset();
    unit.nu_Nic.rx_batches = 1;
    opener.op_RxDirect  = (APTR)x_direct;
    opener.op_RxFilled  = (APTR)x_filled;
    opener.op_RxLinkHdr = TRUE;
    req(&io, ANXD_CMD_RX_BATCH);
    io.ios2_PacketType = 0x0800;
    io.ios2_Data       = &rec.b;
    rec.b.Size         = (UWORD)(ANXD_S2_RX_BATCH_SIZE(2) - 1);
    netdev_perform(&opener, &io);
    expect_u32("a short batch allocation is a bad argument",
               (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ARGUMENT);
    rec.b.Size = (UWORD)sizeof(rec.bytes);

    reset();
    unit.nu_Nic.rx_batches = 1;
    opener.op_RxDirect  = (APTR)x_direct;
    opener.op_RxFilled  = (APTR)x_filled;
    opener.op_RxLinkHdr = TRUE;
    req(&io, ANXD_CMD_RX_BATCH);
    io.ios2_PacketType = 0x0800;
    io.ios2_Data       = &rec.b;
    rec.b.Version      = 0;
    netdev_perform(&opener, &io);
    expect_u32("an unknown batch version is a bad argument",
               (unsigned long)(UBYTE)last_err,
               (unsigned long)(UBYTE)S2ERR_BAD_ARGUMENT);
    rec.b.Version = ANXD_S2_RX_BATCH_VERSION;

    reset();
    unit.nu_Nic.rx_batches = 1;
    opener.op_RxDirect  = (APTR)x_direct;
    opener.op_RxFilled  = (APTR)x_filled;
    opener.op_RxLinkHdr = TRUE;
    req(&io, ANXD_CMD_RX_BATCH);
    io.ios2_PacketType = 0x0800;
    io.ios2_Data       = NULL;
    netdev_perform(&opener, &io);
    expect_u32("no record is a null pointer",
               (unsigned long)(UBYTE)last_err, (unsigned long)(UBYTE)S2ERR_BAD_ARGUMENT);
    expect_u32("said so", last_wire, (unsigned long)S2WERR_NULL_POINTER);

    reset();
    unit.nu_Nic.rx_batches = 1;
    opener.op_RxDirect  = (APTR)x_direct;
    opener.op_RxFilled  = (APTR)x_filled;
    opener.op_RxLinkHdr = TRUE;
    unit.nu_Online      = 0;
    req(&io, ANXD_CMD_RX_BATCH);
    io.ios2_PacketType = 0x0800;
    io.ios2_Data       = &rec.b;
    netdev_perform(&opener, &io);
    expect_u32("an offline unit refuses a batch as it refuses a read",
               (unsigned long)(UBYTE)last_err, (unsigned long)(UBYTE)S2ERR_OUTOFSERVICE);

    reset();
    opener.op_RxDirect  = (APTR)x_direct;
    opener.op_RxFilled  = (APTR)x_filled;
    opener.op_RxLinkHdr = TRUE;
    req(&io, ANXD_CMD_RX_BATCH);
    io.ios2_PacketType = 0x0800;
    io.ios2_Data       = &rec.b;
    netdev_perform(&opener, &io);
    expect_u32("a core that delivers one frame per pass refuses batches",
               (unsigned long)(UBYTE)last_err, (unsigned long)(UBYTE)S2ERR_NOT_SUPPORTED);

    reset();
    unit.nu_Nic.rx_batches = 1;
    {
        struct IOStdReq std;
        struct
        {
            ULONG  DevQueryFormat;
            ULONG  SizeAvailable;
            UWORD  DeviceType;
            UWORD  DeviceSubType;
            UWORD *SupportedCommands;
        } answer;

        reset();
    unit.nu_Nic.rx_batches = 1;
        memset(&std, 0, sizeof(std));
        std.io_Message.mn_Length = sizeof(std);   /* as CreateIORequest() sets it */
        memset(&answer, 0, sizeof(answer));
        std.io_Command = NSCMD_DEVICEQUERY;
        std.io_Data    = &answer;
        std.io_Length  = sizeof(answer);
        netdev_perform(&opener, (struct IOSana2Req *)&std);
        for (cmds = answer.SupportedCommands; cmds != NULL && *cmds != 0; cmds++)
            if (*cmds == ANXD_CMD_RX_BATCH)
                listed = 1;
    }
    expect(listed, "ANXD_CMD_RX_BATCH is in the supported-command list");
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
    i2_nsquery_both_forms();
    i3_nsquery_short_frames();
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
    t_rx_poll();
    v_rx_capacity();
    w_private_commands_are_in_an_nsd_vendor_block();
    x_rx_batch();
    y_tx_flush();
    z_unnegotiated_extensions_are_inert();

    if (failures != 0)
    {
        printf("netdev_cmds: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("netdev_cmds: %d checks ok\n", checks);

    return 0;
}
