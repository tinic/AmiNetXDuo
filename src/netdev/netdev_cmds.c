/*
 * anxnet.device, layer 1 of 3: the SANA-II command table.
 *
 * The error codes here are the ones Individual Computers' x-surf.device and
 * x-surf-100.device return, so an application written against those sees no
 * difference.  There is one deliberate exception:
 *
 *   S2_ADDMULTICASTADDRESS accepts an address whose group bit, bit 0 of the
 *   first octet, is set.  Both IC drivers test bit 7 instead.  IPv4 multicast
 *   is 01:00:5e:.. and IPv6 is 33:33:.., and both have bit 7 clear in the
 *   first octet, so every legitimate join is answered S2ERR_BAD_ADDRESS, the
 *   hash filter stays empty, and the router's neighbour solicitation to
 *   33:33:ff:xx:xx:xx is dropped by the card.  On-link IPv6 still works,
 *   because a router advertisement comes back unicast.  Off-link does not,
 *   because the return path never resolves.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "aminetxduo/anxs2ext.h"
#include "dp8390.h"

#include <exec/errors.h>
#include <exec/io.h>
#include <stddef.h>

#include <proto/exec.h>

/* Not in the SANA-II autodocs' command list, but universally implemented. */
#define S2_ADDMULTICASTADDRESSES    0xC000
#define S2_DELMULTICASTADDRESSES    0xC001

#ifndef NSCMD_DEVICEQUERY
#define NSCMD_DEVICEQUERY           0x4000
#endif
#ifndef NSDEVTYPE_SANA2
#define NSDEVTYPE_SANA2             7
#endif

struct NetdevNSQuery
{
    ULONG   DevQueryFormat;
    ULONG   SizeAvailable;
    UWORD   DeviceType;
    UWORD   DeviceSubType;
    UWORD  *SupportedCommands;
};

static UWORD netdev_supported[] =
{
    CMD_READ, CMD_WRITE, CMD_FLUSH,
    S2_DEVICEQUERY, S2_GETSTATIONADDRESS, S2_CONFIGINTERFACE,
    S2_ADDMULTICASTADDRESS, S2_DELMULTICASTADDRESS,
    S2_MULTICAST, S2_BROADCAST,
    S2_TRACKTYPE, S2_UNTRACKTYPE, S2_GETTYPESTATS,
    S2_GETSPECIALSTATS, S2_GETGLOBALSTATS,
    S2_ONEVENT, S2_READORPHAN, S2_ONLINE, S2_OFFLINE,
    S2_ADDMULTICASTADDRESSES, S2_DELMULTICASTADDRESSES,
    NSCMD_DEVICEQUERY,
    ANXD_CMD_RX_POLL, ANXD_CMD_RX_CAPACITY, ANXD_CMD_RX_BATCH,
    ANXD_CMD_TX_FLUSH,
    0
};

/* ---------------------------------------------------------------- helpers - */

static VOID cmd_bytes(UBYTE *to, const UBYTE *from, ULONG n)
{
    while (n-- != 0)
        *to++ = *from++;
}

static VOID cmd_zero(UBYTE *p, ULONG n)
{
    while (n-- != 0)
        *p++ = 0;
}

/* Ethernet station addresses are individual and non-zero.  A locally
   administered address is valid; multicast/broadcast and the all-zero
   placeholder are not addresses a SANA-II unit can be configured with. */
static BOOL cmd_station_address_valid(const UBYTE *address)
{
    UWORD i;
    UBYTE any = 0;

    if ((address[0] & 1U) != 0)
        return FALSE;

    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        any |= address[i];

    return (BOOL)(any != 0);
}

static BOOL cmd_dequeue(struct List *list, struct IOSana2Req *io)
{
    struct Node *n;

    for (n = list->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        if ((struct IOSana2Req *)n == io)
        {
            Remove(n);
            return TRUE;
        }
    }

    return FALSE;
}

/* ------------------------------------------------------------- statistics - */

static const char netdev_stat_mode[]  = "Data transfer mode";
static const char netdev_stat_ovw[]   = "Receive ring overruns";
static const char netdev_stat_txur[]  = "Transmit FIFO underruns";
static const char netdev_stat_rst[]   = "Chip resets";
static const char netdev_stat_mc[]    = "Multicast addresses";
static const char netdev_stat_mcful[] = "Multicast joins refused";
static const char netdev_stat_coll[]  = "Collisions";
static const char netdev_stat_wedge[] = "Transmitter watchdog resets";
static const char netdev_stat_drop[]  = "Supported boards with no unit";
static const char netdev_stat_grp[]   = "ROM address group bit cleared";
static const char netdev_stat_cis[]   = "Address taken from the card's CIS";
static const char netdev_stat_derv[]  = "Address derived, PROM was blank";
static const char netdev_stat_godd[]  = "Odd registers read as words";
static const char netdev_stat_drx[]   = "Direct receive fills";
static const char netdev_stat_tick[]  = "Vertical-blank interrupt polls";
static const char netdev_stat_kick[]  = "PCMCIA deaf-receiver resets";
static const char netdev_stat_txerr[] = "Transmit errors";
static const char netdev_stat_ints[]  = "Interrupts claimed";
static const char netdev_stat_poll[]  = "Opener polls";
static const char netdev_stat_pollh[] = "Opener polls that found frames held";
static const char netdev_stat_ver[]   = "Direct receive frames verified";

static VOID cmd_special_stats(NetdevUnit *unit, struct IOSana2Req *io)
{
    struct Sana2SpecialStatHeader *hdr =
        (struct Sana2SpecialStatHeader *)io->ios2_StatData;
    struct Sana2SpecialStatRecord *rec;
    ULONG max;
    ULONG n = 0;
    UWORD i;
    ULONG mc = 0;

    if (hdr == NULL)
    {
        netdev_reply(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        return;
    }

    rec = (struct Sana2SpecialStatRecord *)(hdr + 1);
    max = hdr->RecordCountMax;

    for (i = 0; i < NETDEV_MCAST_MAX; i++)
    {
        if (unit->nu_Mcast[i].refs != 0)
            mc++;
    }

#define STAT(str, val)                              \
    do {                                            \
        if (n < max) {                              \
            rec[n].Type   = n;                      \
            rec[n].Count  = (ULONG)(val);           \
            rec[n].String = (char *)(str);          \
            n++;                                    \
        }                                           \
    } while (0)

    /*
     * The transfer mode is here because it is the one thing about this driver
     * that cannot be inferred from the outside: an X-Surf 100 whose 32-bit
     * window failed its readback runs at half speed and works perfectly, and
     * without this the only symptom is a number in a benchmark.
     */
    STAT(netdev_stat_mode,  unit->nu_Nic.bus.dmode);
    STAT(netdev_stat_ovw,   unit->nu_Nic.overruns);
    STAT(netdev_stat_txur,  unit->nu_Nic.tx_underruns);
    STAT(netdev_stat_rst,   unit->nu_Nic.resets);
    STAT(netdev_stat_mc,    mc);
    STAT(netdev_stat_mcful, unit->nu_McastFull);
    STAT(netdev_stat_coll,  unit->nu_Nic.collisions);

    /*
     * Both of these were counted and read by nothing.  The increments then
     * read as a reported number that nobody could see.  A transmitter that
     * wedges twice a minute and is quietly reset looks the same as one that
     * never wedges.
     */
    STAT(netdev_stat_wedge, unit->nu_TxWedges);
    STAT(netdev_stat_drop,  unit->nu_Dev->nd_UnitsDropped);

    /*
     * The three ways the address in PAR0..5 can differ from what the card's
     * PROM said.  Each is 0 or 1.  They are here rather than only in the trace
     * build, because a machine that invented its own hardware address must be
     * able to report it without a serial cable.
     */
    STAT(netdev_stat_grp,   unit->nu_Nic.mac_group_fix);
    STAT(netdev_stat_cis,   unit->nu_Nic.mac_from_cis);
    STAT(netdev_stat_derv,  unit->nu_Nic.mac_derived);

    /*
     * And whether the probe put this card on cnet16's word-read path.  Same
     * reason as the transfer mode at record 0: it cannot be inferred from
     * outside, and it is the difference between a card that works and one that
     * receives nothing.  The probe replaces two binaries, so nobody has to
     * work out which binary is running.
     */
    STAT(netdev_stat_godd,  unit->nu_Nic.bus.getodd);

    /* The performance result is meaningful only if the private hook was
       actually negotiated and used.  Appended so every existing record keeps
       its numeric Type for callers which learned the older table by index. */
    STAT(netdev_stat_drx,   unit->nu_RxDirect);

    /* These are recovery, not traffic, counters.  A nonzero value is the
       evidence that the card stopped interrupting or booted deaf, and must
       survive the SANA-II boundary to be useful in a field report. */
    STAT(netdev_stat_tick,  unit->nu_TickPolls);
    STAT(netdev_stat_kick,  unit->nu_RxKicks);
    /* The chip's own transmit failures (jabber, underrun, excessive
       collisions on el3; the TSR error bits on dp8390), appended so every
       older record keeps its index.  Frames the wire never saw. */
    STAT(netdev_stat_txerr, unit->nu_Nic.tx_errors);
    /* ANXD_CMD_RX_POLL: how often the opener asked, and how often a core
       had frames waiting for it (aminetxduo/anxs2ext.h). */
    STAT(netdev_stat_poll,  unit->nu_RxPolls);
    STAT(netdev_stat_pollh, unit->nu_RxPollsHeld);

    /* Whatever the core itself counts, after everything above. */
    if (unit->nu_Nic.core_stat_names != NULL)
    {
        for (i = 0; i < NETDEV_CORE_STATS &&
                    unit->nu_Nic.core_stat_names[i] != NULL; i++)
            STAT(unit->nu_Nic.core_stat_names[i], unit->nu_Nic.core_stat[i]);
    }

    /* Last so every previously published numeric Type, including a core's
       private records, stays where it was. */
    STAT(netdev_stat_ver, unit->nu_Nic.rx_verified);
    /* Interrupts the server claimed: the counter the vertical-blank poll is
       the inverse of.  Without it "the card interrupts" and "the card is
       being polled and nobody noticed" read the same in a field report --
       an A3000 with an X-Surf 100 received 2.8 Mbit/s on the poll for
       exactly that reason. */
    STAT(netdev_stat_ints, unit->nu_IntSeen);

#undef STAT

    hdr->RecordCountSupplied = n;
    netdev_reply(io, 0, 0);
}

/*
 * Queue a CMD_READ or an S2_READORPHAN.
 *
 * Split out so netdev_begin_io() can reach it without the generic dispatch.
 * CMD_READ is most of what this device is ever asked to do -- one per received
 * frame, from ami_sana2_rx_post_slot() -- and the generic path pays a 40-byte
 * frame, a movem of five registers and a jump-table dispatch to reach three
 * stores and an AddHead.
 *
 * _netdev_perform is 2.1% of the real-path profile and that is a share to
 * TRUST, unlike the thin static helpers this campaign was recently burned by:
 * it is non-static, sits in its own translation unit and carries twenty cases,
 * so no inliner folds it away.  The saving also lands inside the reader's
 * nx_ip_protection hold, where a calibrated burn measured a cycle costing
 * about 1.53 times what it costs outside (86641b7c).
 */
VOID netdev_queue_read(NetdevOpener *op, struct IOSana2Req *io, UWORD cmd)
{
    NetdevUnit *unit = op->op_Hw;
    BOOL queued;

    if (op->op_CopyTo == NULL)
    {
        netdev_reply(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        return;
    }

    /*
     * One Disable() over the test and the queueing.  Split, a concurrent
     * S2_OFFLINE drains the list between them and the read lands on an
     * offline unit with nothing left to answer it.
     */
    io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;

    /*
     * A CMD_READ GOES TO THE HEAD, AN ORPHAN TO THE TAIL.
     *
     * netdev_take() walks op_Reads matching ios2_PacketType, and this
     * shim keeps three readers on one opener -- IPv4, ARP and IPv6
     * (sana2_rx.c:1400).  During a bulk IPv4 transfer the ARP and IPv6
     * reads are never satisfied, so with AddTail they settle permanently
     * at the head and every arriving frame walks past all four of them
     * before it matches: the steady state is [ARP, ARP, IPv6, IPv6,
     * IPv4...] once the first few frames have cycled their reads to the
     * back.  _netdev_take is 1.2% of the wire profile.
     *
     * Outstanding reads of one type are interchangeable -- each is an
     * empty buffer waiting to be filled, and SANA-II promises nothing
     * about which one a frame lands in -- so handing back the most
     * recently freed one is as correct as handing back the oldest, and it
     * puts the type that is actually receiving at the front.  The idle
     * ARP and IPv6 reads sink behind it and stay there.
     *
     * Worth 1.0% of receive.  Clean build per arm, md5 of anxnet.device
     * printed before a round ran (13542d48 tail, dcdac8e5 head), six
     * rounds alternating which arm went first, tcp-rx medians:
     *
     *     arm       first        second       overall
     *     AddTail   5,486,559    5,545,202    5,515,880
     *     AddHead   5,559,536    5,580,368    5,569,952
     *
     * Ahead in both positions, +1.3% and +0.6%; transmit -0.1%.  READ THAT
     * AS SMALL AND POSITIVE, NOT AS 1.0% EXACTLY -- the within-arm spread
     * between positions is about 1% here, the same size as the effect.  It
     * clears the bar this tree uses, ahead in both positions on clean
     * builds, and no more than that.
     *
     * THIS QUEUE IS SHARED BY EVERY BOARD, so it was checked on a second
     * one.  ne2000_pcmcia (dp8390, not a2065's lance), same two clean
     * builds, SIX rounds alternated, tcp-rx:
     *
     *     arm      first        second       median
     *     before   4,231,006    4,001,262    4,018,809
     *     after    4,071,943    4,074,700    4,071,943
     *
     * READ THAT AS "NO REGRESSION ON A SECOND DRIVER" AND NOT AS A SECOND
     * CONFIRMATION OF THE GAIN.  The +1.3% in those medians is the control
     * moving, not this change: the control's own two positions differ by
     * 5.7% while the after arm sits at 4.07M in both, tight to 0.07%.  Six
     * rounds cannot call 1% against a control that noisy, and a four-round
     * run before it put the control's outlier in the OTHER position.
     *
     * What it does rule out is the thing worth ruling out -- that
     * reordering a queue every board shares costs a board whose driver
     * takes a different path into it.  The after arm is never below the
     * control's range.  Transmit there is -0.7%, inside the same noise.
     *
     * An earlier run called this inconclusive because its arms disagreed
     * by position.  Those arms were built in two reused worktrees, one
     * holding a stale library (d5323947): the disagreement WAS the
     * artefact.  AMI_SANA2_RX_RUN_MAX read +2.8% and then -4.0% the same
     * bad way and is +1.5% measured clean.
     */
    Disable();
    queued = unit->nu_Online ? TRUE : FALSE;
    if (queued)
    {
        /* Once a frame, inside a Disable(): see netdev_internal.h. */
        if (cmd == CMD_READ)
        {
            /* Newest first, as always -- but behind any batch at the head:
               a batch is the opener's preferred read of its type, and its
               plain CMD_READs are the pool that takes over while both
               batches are with the reader.  At most two batches lead. */
            struct Node *after = NULL;
            struct Node *n     = op->op_Reads.lh_Head;

            while (n->ln_Succ != NULL &&
                   netdev_is_batch((const struct IOSana2Req *)n))
            {
                after = n;
                n     = n->ln_Succ;
            }
            if (after == NULL)
                nd_addhead(&op->op_Reads, &io->ios2_Req.io_Message.mn_Node);
            else
                nd_insert_after(&io->ios2_Req.io_Message.mn_Node, after);
            netdev_note_read_type(op, io->ios2_PacketType);
        }
        else
            nd_list_addtail(&op->op_Orphans,
                            &io->ios2_Req.io_Message.mn_Node);
    }
    Enable();

    if (!queued)
        netdev_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
    return;
}

/*
 * ANXD_CMD_RX_BATCH (aminetxduo/anxs2ext.h).  Queued at the head of the
 * opener's reads like a CMD_READ, so the claim and the staging hand-over
 * find it where they look for a read of its type.  Refused for an opener
 * the batch cannot serve: one without the direct pair or the link header
 * (the batch carries no per-frame addresses), a raw opener (the direct
 * destination starts after the header) or one with a filter hook (which
 * wants a request to judge by).  The batch record itself: at least one
 * cookie, and Filled starts at zero whatever the opener left there.
 */
VOID netdev_queue_batch(NetdevOpener *op, struct IOSana2Req *io)
{
    NetdevUnit    *unit = op->op_Hw;
    AnxdS2RxBatch *b    = (AnxdS2RxBatch *)io->ios2_Data;
    BOOL           queued;

    if ((op->op_Extensions & ANXD_S2F_RX_BATCH) == 0)
    {
        netdev_reply(io, S2ERR_NOT_SUPPORTED, S2WERR_GENERIC_ERROR);
        return;
    }
    if (b == NULL || op->op_CopyTo == NULL)
    {
        netdev_reply(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        return;
    }
    if (b->Version != ANXD_S2_RX_BATCH_VERSION ||
        b->Size < (UWORD)sizeof(*b) ||
        b->Count == 0 || b->Count > ANXD_S2_RX_BATCH_MAX ||
        (ULONG)b->Size < (ULONG)ANXD_S2_RX_BATCH_SIZE(b->Count))
    {
        netdev_reply(io, S2ERR_BAD_ARGUMENT, S2WERR_GENERIC_ERROR);
        return;
    }
    if (!unit->nu_Nic.rx_batches ||
        op->op_RxDirect == NULL || op->op_RxFilled == NULL ||
        !op->op_RxLinkHdr || op->op_Raw || op->op_Filter != NULL ||
        (io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0)
    {
        netdev_reply(io, S2ERR_NOT_SUPPORTED, S2WERR_GENERIC_ERROR);
        return;
    }

    b->Filled = 0;
    io->ios2_DataLength = 0;
    io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;

    Disable();
    queued = unit->nu_Online ? TRUE : FALSE;
    if (queued)
    {
        nd_addhead(&op->op_Reads, &io->ios2_Req.io_Message.mn_Node);
        netdev_note_read_type(op, io->ios2_PacketType);
    }
    Enable();

    if (!queued)
        netdev_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
}

/* ------------------------------------------------------------- the table -- */

/*
 * Send a CMD_WRITE, an S2_MULTICAST or an S2_BROADCAST.
 *
 * Split out for the same reason netdev_queue_read() was, and on the same
 * evidence.  netdev_begin_io() already sends CMD_READ straight here rather
 * than through netdev_perform()'s twenty-case jump table, 40-byte frame and
 * movem of five registers; CMD_WRITE is the OTHER command this device is asked
 * for in bulk, and during a RECEIVE it is the acknowledgement path -- roughly
 * one send for every two frames taken -- which is what reopens the window the
 * far end is filling.  _netdev_perform still carries 1.8% of the real-path
 * profile with CMD_READ already bypassing it, and an inbound-only transfer has
 * nothing else going through it at that rate.
 *
 * netdev_perform() keeps its own case, so src/netdev/test, which enters at
 * both, sees no behaviour change; the body moved and nothing in it did.
 */
VOID netdev_write_cmd(NetdevOpener *op, struct IOSana2Req *io, UWORD cmd)
{
    NetdevUnit *unit = op->op_Hw;
    UBYTE       bcast[NETDEV_ADDR_LEN];
    UWORD       i;

    if (!unit->nu_Online)
    {
        netdev_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
        return;
    }
    if (op->op_CopyFrom == NULL)
    {
        netdev_reply(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        return;
    }
    if (io->ios2_DataLength > NETDEV_MTU &&
        !(netdev_io_is_raw(op, io) &&
          io->ios2_DataLength <= NETDEV_FRAME_MAX))
    {
        netdev_reply(io, S2ERR_MTU_EXCEEDED, S2WERR_GENERIC_ERROR);
        return;
    }
    if (cmd == S2_BROADCAST)
    {
        for (i = 0; i < NETDEV_ADDR_LEN; i++)
            bcast[i] = 0xff;
        cmd_bytes(io->ios2_DstAddr, bcast, NETDEV_ADDR_LEN);
    }
    else if (cmd == S2_MULTICAST && (io->ios2_DstAddr[0] & 1) == 0)
    {
        netdev_reply(io, S2ERR_BAD_ADDRESS, S2WERR_BAD_MULTICAST);
        return;
    }

    netdev_tx_direct(unit, io);
}

static VOID cmd_device_query(NetdevUnit *unit, struct IOSana2Req *io)
{
    struct Sana2DeviceQuery *q =
        (struct Sana2DeviceQuery *)io->ios2_StatData;
    struct Sana2DeviceQuery answer;
    ULONG want;

    if (q == NULL)
    {
        netdev_reply(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        return;
    }

    want = sizeof(struct Sana2DeviceQuery);
    if (q->SizeAvailable < want)
        want = q->SizeAvailable;

    /* SizeAvailable is the caller saying how much room there is.  Fill a
       local answer and copy only that much, so an older structure cannot be
       overrun before SizeSupplied reports its smaller size. */
    answer.SizeAvailable  = q->SizeAvailable;
    answer.SizeSupplied   = want;
    answer.DevQueryFormat = 0;
    answer.DeviceLevel    = 0;
    answer.AddrFieldSize  = 48;
    answer.MTU            = NETDEV_MTU;
    answer.BPS            = unit->nu_Nic.card->bps;
    answer.HardwareType   = S2WireType_Ethernet;

    cmd_bytes((UBYTE *)q, (const UBYTE *)&answer, want);
    netdev_reply(io, 0, 0);
}

static VOID cmd_flush(NetdevOpener *op, struct IOSana2Req *io)
{
    NetdevUnit *unit = op->op_Hw;
    struct IOSana2Req *queued;

    Disable();
    while ((queued = (struct IOSana2Req *)RemHead(&op->op_Reads)) != NULL)
        netdev_reply(queued, IOERR_ABORTED, 0);
    while ((queued = (struct IOSana2Req *)RemHead(&op->op_Orphans)) != NULL)
        netdev_reply(queued, IOERR_ABORTED, 0);
    while ((queued = (struct IOSana2Req *)RemHead(&op->op_Events)) != NULL)
        netdev_reply(queued, IOERR_ABORTED, 0);
    Enable();

    /* Other openers may still have waits of their own. */
    netdev_event_rescan(unit);

    /* Writes are queued on the unit, not the opener. */
    netdev_drop_writes(unit, op);
    netdev_reply(io, 0, 0);
}

static VOID cmd_multicast_address(NetdevUnit *unit,
                                  struct IOSana2Req *io, BOOL add)
{
    BOOL applied;

    if ((io->ios2_SrcAddr[0] & 1) == 0)
    {
        netdev_reply(io, S2ERR_BAD_ADDRESS, S2WERR_BAD_MULTICAST);
        return;
    }

    /* BeginIO is callable from unrelated tasks.  Keep the exact table and
       the hash programmed from it in one serialized transaction. */
    Disable();
    if (add)
    {
        applied = netdev_mcast_add(unit->nu_Mcast, io->ios2_SrcAddr);
        if (!applied)
            unit->nu_McastFull++;
    }
    else
        applied = netdev_mcast_del(unit->nu_Mcast, io->ios2_SrcAddr);
    if (applied)
        netdev_rebuild_filter(unit);
    Enable();

    if (applied)
        netdev_reply(io, 0, 0);
    else if (add)
        netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_MULTICAST_FULL);
    else
        netdev_reply(io, S2ERR_BAD_STATE, S2WERR_BAD_MULTICAST);
}

static VOID cmd_multicast_range(NetdevUnit *unit,
                                struct IOSana2Req *io, BOOL add)
{
    ULONG count;
    BOOL wide;
    BOOL applied = TRUE;

    if ((io->ios2_SrcAddr[0] & 1) == 0)
    {
        netdev_reply(io, S2ERR_BAD_ADDRESS, S2WERR_BAD_MULTICAST);
        return;
    }

    wide = netdev_mcast_range_wide(io->ios2_SrcAddr, io->ios2_DstAddr,
                                   &count);
    Disable();
    if (wide)
    {
        if (add)
        {
            if (unit->nu_AllMulti == 0xffffu)
                applied = FALSE;
            else
                unit->nu_AllMulti++;
        }
        else if (unit->nu_AllMulti != 0)
            unit->nu_AllMulti--;
        else
            applied = FALSE;
    }
    else
        applied = netdev_mcast_range_apply(unit->nu_Mcast,
                                           io->ios2_SrcAddr, count, add);

    if (!applied && add)
        unit->nu_McastFull++;
    if (applied)
        netdev_rebuild_filter(unit);
    Enable();

    if (applied)
        netdev_reply(io, 0, 0);
    else if (add)
        netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_MULTICAST_FULL);
    else
        netdev_reply(io, S2ERR_BAD_STATE, S2WERR_BAD_MULTICAST);
}

static VOID cmd_track_type(NetdevOpener *op, struct IOSana2Req *io)
{
    UWORD i;
    LONG free_slot = -1;

    for (i = 0; i < NETDEV_TRACK_MAX; i++)
    {
        if (op->op_Track[i].used)
        {
            if (op->op_Track[i].type == io->ios2_PacketType)
            {
                netdev_reply(io, S2ERR_BAD_STATE, S2WERR_ALREADY_TRACKED);
                return;
            }
        }
        else if (free_slot < 0)
            free_slot = i;
    }

    if (free_slot < 0)
    {
        netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_GENERIC_ERROR);
        return;
    }

    /* The interrupt server walks this array on every frame. */
    Disable();
    cmd_zero((UBYTE *)&op->op_Track[free_slot],
             sizeof(op->op_Track[free_slot]));
    op->op_Track[free_slot].type = io->ios2_PacketType;
    op->op_Track[free_slot].used = 1;
    if ((UWORD)(free_slot + 1) > op->op_TrackHigh)
        op->op_TrackHigh = (UWORD)(free_slot + 1);
    Enable();
    netdev_reply(io, 0, 0);
}

static VOID cmd_untrack_type(NetdevOpener *op, struct IOSana2Req *io)
{
    UWORD i;

    for (i = 0; i < NETDEV_TRACK_MAX; i++)
    {
        if (op->op_Track[i].used &&
            op->op_Track[i].type == io->ios2_PacketType)
        {
            Disable();
            op->op_Track[i].used = 0;
            while (op->op_TrackHigh != 0 &&
                   !op->op_Track[op->op_TrackHigh - 1].used)
                op->op_TrackHigh--;
            Enable();
            netdev_reply(io, 0, 0);
            return;
        }
    }

    netdev_reply(io, S2ERR_BAD_STATE, S2WERR_NOT_TRACKED);
}

static VOID cmd_type_stats(NetdevOpener *op, struct IOSana2Req *io)
{
    UWORD i;

    if (io->ios2_StatData == NULL)
    {
        netdev_reply(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        return;
    }

    for (i = 0; i < NETDEV_TRACK_MAX; i++)
    {
        if (op->op_Track[i].used &&
            op->op_Track[i].type == io->ios2_PacketType)
        {
            cmd_bytes((UBYTE *)io->ios2_StatData,
                      (const UBYTE *)&op->op_Track[i].st,
                      sizeof(struct Sana2PacketTypeStats));
            netdev_reply(io, 0, 0);
            return;
        }
    }

    netdev_reply(io, S2ERR_BAD_STATE, S2WERR_NOT_TRACKED);
}

static VOID cmd_global_stats(NetdevUnit *unit, struct IOSana2Req *io)
{
    if (io->ios2_StatData == NULL)
    {
        netdev_reply(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        return;
    }

    /* These chip counters have one owner instead of a shadow that can
       disagree.  LastStart remains zero because this device does not open
       timer.device to manufacture a timeval. */
    unit->nu_Stats.Overruns = unit->nu_Nic.overruns;
    unit->nu_Stats.BadData  = unit->nu_Nic.rx_errors;
    cmd_bytes((UBYTE *)io->ios2_StatData, (const UBYTE *)&unit->nu_Stats,
              sizeof(struct Sana2DeviceStats));
    netdev_reply(io, 0, 0);
}

/* The largest io_Length believed as a NewStyle buffer in a full request:
   far above any real one, far below the smallest EtherType (0x600). */
#define NETDEV_NSQ_PLAUSIBLE    256UL

VOID netdev_nsd_query(struct IOSana2Req *io)
{
    struct IOStdReq      *std  = (struct IOStdReq *)io;
    struct NetdevNSQuery *q    = NULL;
    BOOL                  sana = FALSE;

    /* io_Actual aliases ios2_WireError on m68k, so the SANA-II reply helper
       cannot answer this IOStdReq without destroying the byte count. */
#ifdef __mc68000__
    _Static_assert(offsetof(struct IOStdReq, io_Actual) ==
                   offsetof(struct IOSana2Req, ios2_WireError),
                   "NSCMD_DEVICEQUERY io_Actual alias changed");
    /* THE ALIASES THE PRECEDENCE BELOW EXISTS FOR, pinned where they are
       true: in a full request io_Length is ios2_PacketType and io_Data is
       the first four bytes of ios2_SrcAddr.  mcastfilter writes +72/+76. */
    _Static_assert(offsetof(struct IOStdReq, io_Length) ==
                   offsetof(struct IOSana2Req, ios2_PacketType) &&
                   offsetof(struct IOStdReq, io_Data) ==
                   offsetof(struct IOSana2Req, ios2_SrcAddr),
                   "NSCMD_DEVICEQUERY io_Data/io_Length alias changed");
    _Static_assert(offsetof(struct IOSana2Req, ios2_DataLength) == 72 &&
                   offsetof(struct IOSana2Req, ios2_Data) == 76 &&
                   sizeof(struct IOStdReq) == 48 &&
                   sizeof(struct IOSana2Req) == 88,
                   "SANA-II request layout changed");
    /* The NewStyle answer is 16 bytes on the target; the host's 24 is only
       the host's.  mcastfilter asks for exactly 16. */
    _Static_assert(sizeof(struct NetdevNSQuery) == 16,
                   "NSCMD_DEVICEQUERY answer is not 16 bytes on m68k");
#endif

    /*
     * TWO FORMS, AND WHICH ONE A REQUEST IS DECIDES BY ITS SIZE.
     *
     * A request that SAYS it is a full IOSana2Req (mn_Length) is asked in the
     * SANA-II form first -- ios2_Data/ios2_DataLength, which is what
     * mcastfilter 1.20 sends.  In such a request io_Data/io_Length are not
     * fields at all: they are ios2_SrcAddr[0..3] and ios2_PacketType, and a
     * reused request carries a stale source address and a packet type of
     * 0x800 there.  Taking those first wrote the answer to an address made of
     * MAC bytes.  The worst the SANA-II form can do is write the caller's own
     * data buffer.  Only when it names no usable buffer is io_Data consulted,
     * which is how a fresh request cast to an IOStdReq still works.
     *
     * Anything else -- a short request, or mn_Length 0 which could be a
     * hand-built 48-byte IOStdReq -- is an IOStdReq, and offsets 72 and 76
     * are past its end: the IOStdReq form only.
     *
     * AND IN A FULL REQUEST THE IOStdReq FORM MUST LOOK LIKE ONE.  With
     * ios2_Data NULL a stale source address would still reach it.  What
     * gives a stale request away is io_Length: there it is the packet type,
     * an EtherType of 0x600 or more (0x0800, 0x0806, 0x86DD), where a real
     * NewStyle buffer is tens of bytes.  So it must be at most
     * NETDEV_NSQ_PLAUSIBLE, and word-aligned for the ULONG fields.
     *
     * The minimum is the size written, not a literal 16: that is the m68k
     * layout, and the host's is wider.
     */
    if (NETDEV_IO_IS_FULL(io) && io->ios2_Data != NULL &&
        io->ios2_DataLength >= sizeof(struct NetdevNSQuery))
    {
        q    = (struct NetdevNSQuery *)io->ios2_Data;
        sana = TRUE;
    }
    else if (std->io_Data != NULL &&
             std->io_Length >= sizeof(struct NetdevNSQuery) &&
             (!NETDEV_IO_IS_FULL(io) ||
              (std->io_Length <= NETDEV_NSQ_PLAUSIBLE &&
               ((size_t)std->io_Data & 1U) == 0)))
    {
        q = (struct NetdevNSQuery *)std->io_Data;
    }
    else
    {
        std->io_Actual = 0;
        std->io_Error  = IOERR_BADLENGTH;
        if ((std->io_Flags & IOF_QUICK) == 0)
            ReplyMsg(&std->io_Message);
        return;
    }

    q->DevQueryFormat    = 0;
    q->SizeAvailable     = sizeof(struct NetdevNSQuery);
    q->DeviceType        = NSDEVTYPE_SANA2;
    q->DeviceSubType     = 0;
    q->SupportedCommands = netdev_supported;

    /* The byte count goes where the form keeps it.  In the SANA-II form the
       aliased word is ios2_WireError, and a successful command has none. */
    if (sana)
    {
        io->ios2_DataLength = sizeof(struct NetdevNSQuery);
        io->ios2_WireError  = 0;
    }
    else
        std->io_Actual = sizeof(struct NetdevNSQuery);
    std->io_Error = 0;
    if ((std->io_Flags & IOF_QUICK) == 0)
        ReplyMsg(&std->io_Message);
}

VOID netdev_perform(NetdevOpener *op, struct IOSana2Req *io)
{
    NetdevUnit *unit;
    UWORD       cmd = io->ios2_Req.io_Command;

    if (op == NULL)
    {
        netdev_reply(io, IOERR_BADADDRESS, S2WERR_GENERIC_ERROR);
        return;
    }

    unit = op->op_Hw;

#ifdef NETDEV_TRACE
    netdev_trace_cmd(cmd);
#endif

    switch (cmd)
    {
    case CMD_READ:
    case S2_READORPHAN:
        netdev_queue_read(op, io, cmd);
        return;

    case ANXD_CMD_RX_BATCH:
        netdev_queue_batch(op, io);
        return;

    case ANXD_CMD_TX_FLUSH:
        /* Start what a run of ANXD_S2_TXF_MORE writes left unstarted.  The
           core's kick is a register write; Forbid() keeps it clear of a
           task mid-transmit under the tx task lock, and an interrupt's
           own kick writes the same index.  Quick, nothing to wait for. */
        if ((op->op_Extensions & ANXD_S2F_TX_MORE) == 0 ||
            unit->nu_Nic.tx_flush == NULL)
        {
            netdev_reply(io, S2ERR_NOT_SUPPORTED, S2WERR_GENERIC_ERROR);
            return;
        }
        if (unit->nu_Online)
        {
            Forbid();
            unit->nu_Nic.tx_flush(&unit->nu_Nic);
            Permit();
        }
        netdev_reply(io, 0, 0);
        return;

    case ANXD_CMD_RX_CAPACITY:
        /* What the card holds from the wire with nobody draining it
           (aminetxduo/anxs2ext.h): the core said at attach. */
        if ((op->op_Extensions & ANXD_S2F_RX_CAPACITY) == 0)
        {
            netdev_reply(io, S2ERR_NOT_SUPPORTED, S2WERR_GENERIC_ERROR);
            return;
        }
        io->ios2_DataLength = unit->nu_Nic.rx_capacity;
        netdev_reply(io, 0, 0);
        return;

    case ANXD_CMD_RX_POLL:
        /*
         * The opener has re-posted its reads and is about to sleep: if a
         * core left frames in its ring for want of one
         * (NETDEV_CLAIM_BEHIND), deliver them now rather than at the next
         * interrupt or blank.  The same masked context the server and the
         * blank give the core; nu_InIsr keeps the three apart.  Quick, and
         * answered with nothing: the frames arrive as the CMD_READs.
         */
        /* A poll only means something to a core that holds frames for a
           late read (NetdevNic rx_holds); every other unit says so once and
           the opener stops asking -- on a 68030 a device call per drain is
           measurable, and there would be nothing at the end of it. */
        if ((op->op_Extensions & ANXD_S2F_RX_POLL) == 0 ||
            !unit->nu_Nic.rx_holds)
        {
            netdev_reply(io, S2ERR_NOT_SUPPORTED, S2WERR_GENERIC_ERROR);
            return;
        }
        unit->nu_RxPolls++;
        if (unit->nu_Nic.rx_behind && unit->nu_Online)
        {
            unit->nu_RxPollsHeld++;
            Disable();
            if (unit->nu_InIsr == 0)
            {
                unit->nu_InIsr = 1;
                (VOID)netdev_interrupt(unit);
                unit->nu_InIsr = 0;
            }
            Enable();
        }
        netdev_reply(io, 0, 0);
        return;

    case CMD_WRITE:
    case S2_MULTICAST:
    case S2_BROADCAST:
        netdev_write_cmd(op, io, cmd);
        return;

    case S2_DEVICEQUERY:
        cmd_device_query(unit, io);
        return;

    case S2_GETSTATIONADDRESS:
        cmd_zero(io->ios2_SrcAddr, SANA2_MAX_ADDR_BYTES);
        cmd_zero(io->ios2_DstAddr, SANA2_MAX_ADDR_BYTES);
        cmd_bytes(io->ios2_SrcAddr, unit->nu_Nic.mac, NETDEV_ADDR_LEN);
        cmd_bytes(io->ios2_DstAddr, unit->nu_Nic.factory, NETDEV_ADDR_LEN);
        netdev_reply(io, 0, 0);
        return;

    case S2_CONFIGINTERFACE:
        if (unit->nu_Configured)
        {
            /* The caller learns which address is actually in force, rather
               than being refused and left believing its own was taken. */
            cmd_bytes(io->ios2_SrcAddr, unit->nu_Nic.mac, NETDEV_ADDR_LEN);
            netdev_reply(io, S2ERR_BAD_STATE, S2WERR_IS_CONFIGURED);
            return;
        }
        if (!cmd_station_address_valid(io->ios2_SrcAddr))
        {
            netdev_reply(io, S2ERR_BAD_ADDRESS, S2WERR_SRC_ADDRESS);
            return;
        }
        cmd_bytes(unit->nu_Nic.mac, io->ios2_SrcAddr, NETDEV_ADDR_LEN);
        unit->nu_Configured = 1;
        if (netdev_online(unit) != 0)
        {
            unit->nu_Configured = 0;
            netdev_reply(io, S2ERR_OUTOFSERVICE, S2WERR_GENERIC_ERROR);
            return;
        }
        cmd_bytes(io->ios2_SrcAddr, unit->nu_Nic.mac, NETDEV_ADDR_LEN);
        netdev_reply(io, 0, 0);
        return;

    case S2_ONLINE:
        if (!unit->nu_Configured)
        {
            netdev_reply(io, S2ERR_BAD_STATE, S2WERR_NOT_CONFIGURED);
            return;
        }
        if (!unit->nu_Online && netdev_online(unit) != 0)
        {
            netdev_reply(io, S2ERR_OUTOFSERVICE, S2WERR_GENERIC_ERROR);
            return;
        }
        netdev_reply(io, 0, 0);
        return;

    case S2_OFFLINE:
        if (unit->nu_Online)
            netdev_offline(unit, S2EVENT_OFFLINE);
        else
            netdev_pcmcia_cancel_resume(unit);
        netdev_reply(io, 0, 0);
        return;

    case CMD_FLUSH:
        cmd_flush(op, io);
        return;

    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESS:
        cmd_multicast_address(unit, io,
                              (BOOL)(cmd == S2_ADDMULTICASTADDRESS));
        return;

    case S2_ADDMULTICASTADDRESSES:
    case S2_DELMULTICASTADDRESSES:
        cmd_multicast_range(unit, io,
                            (BOOL)(cmd == S2_ADDMULTICASTADDRESSES));
        return;

    case S2_TRACKTYPE:
        cmd_track_type(op, io);
        return;

    case S2_UNTRACKTYPE:
        cmd_untrack_type(op, io);
        return;

    case S2_GETTYPESTATS:
        cmd_type_stats(op, io);
        return;

    case S2_GETGLOBALSTATS:
        cmd_global_stats(unit, io);
        return;

    case S2_GETSPECIALSTATS:
        cmd_special_stats(unit, io);
        return;

    case S2_ONEVENT:
    {
        ULONG mask = io->ios2_WireError;

        /*
         * S2EVENT_SOFTWARE is refused, and used to be accepted.  This driver
         * has no condition that raises it, so an accepted request waits
         * forever with io_Error zero and the caller cannot see why.  The spec
         * gives the answer: "If this device driver does not understand the
         * specified event condition(s) then the command returns immediately
         * with io_Error set to S2ERR_NOT_SUPPORTED and ios2_WireError
         * S2WERR_BAD_EVENT".  cnet.device's accepted set is the seven below,
         * so no stack that works with it asks for the eighth.
         */
        /* Zero names no condition and would otherwise queue forever: no
           future post can share a bit with it. */
        if (mask == 0 ||
            (mask & ~(ULONG)(S2EVENT_ERROR | S2EVENT_TX | S2EVENT_RX |
                             S2EVENT_ONLINE | S2EVENT_OFFLINE |
                             S2EVENT_BUFF | S2EVENT_HARDWARE)) != 0)
        {
            netdev_reply(io, S2ERR_NOT_SUPPORTED, S2WERR_BAD_EVENT);
            return;
        }

        /* netdev_event_wait() checks the current state and links a waiter in
           one transaction, so a transition cannot land between them. */
        netdev_event_wait(unit, io);
        return;
    }

    case NSCMD_DEVICEQUERY:
        netdev_nsd_query(io);
        return;

    default:
        /* What both IC drivers answer, and what a caller probes with. */
        netdev_reply(io, IOERR_NOCMD, S2WERR_GENERIC_ERROR);
        return;
    }
}

BOOL netdev_abort(NetdevOpener *op, struct IOSana2Req *io)
{
    NetdevUnit *unit;
    BOOL        found;
    BOOL        was_event = FALSE;

    if (op == NULL)
        return FALSE;

    unit = op->op_Hw;

    Disable();
    found = cmd_dequeue(&op->op_Reads, io);
    if (!found)
        found = cmd_dequeue(&op->op_Orphans, io);
    if (!found)
        was_event = found = cmd_dequeue(&op->op_Events, io);
    if (!found)
        found = cmd_dequeue(&unit->nu_Writes, io);
    Enable();

    if (!found)
        return FALSE;

    /* Only when an S2_ONEVENT was the thing aborted: an aborted CMD_READ is
       routine and must not drag a walk of every opener behind it. */
    if (was_event)
        netdev_event_rescan(unit);

    netdev_reply(io, IOERR_ABORTED, 0);

    return TRUE;
}
