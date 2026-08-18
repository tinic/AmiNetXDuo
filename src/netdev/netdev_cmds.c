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
#include "dp8390.h"

#include <exec/errors.h>
#include <exec/io.h>

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

static ULONG cmd_addr48(const UBYTE *a)
{
    /* The low 32 bits.  The top 16 are compared separately. */
    return ((ULONG)a[2] << 24) | ((ULONG)a[3] << 16) |
           ((ULONG)a[4] << 8)  | (ULONG)a[5];
}

static UWORD cmd_addr16(const UBYTE *a)
{
    return (UWORD)(((UWORD)a[0] << 8) | a[1]);
}

/* ------------------------------------------------------------- multicast -- */

/*
 * A range that would not fit the table becomes "accept every multicast".  The
 * test is on the range, so an add and the matching delete always take the same
 * branch and the accounting stays balanced.
 */
static BOOL mcast_range_wide(const UBYTE *lo, const UBYTE *hi, ULONG *count)
{
    ULONG lo32 = cmd_addr48(lo);
    ULONG hi32 = cmd_addr48(hi);

    if (cmd_addr16(lo) != cmd_addr16(hi) || hi32 < lo32)
    {
        *count = 0;
        return TRUE;
    }

    *count = hi32 - lo32 + 1;

    return (BOOL)(*count > NETDEV_MCAST_MAX);
}

/* ------------------------------------------------------------- statistics - */

static const char netdev_stat_mode[]  = "Data transfer mode";
static const char netdev_stat_ovw[]   = "Receive ring overruns";
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

#undef STAT

    hdr->RecordCountSupplied = n;
    netdev_reply(io, 0, 0);
}

/* ------------------------------------------------------------- the table -- */

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
    {
        extern VOID netdev_trace_cmd(UWORD c);
        netdev_trace_cmd(cmd);
    }
#endif

    switch (cmd)
    {
    case CMD_READ:
    case S2_READORPHAN:
    {
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

        Disable();
        queued = unit->nu_Online ? TRUE : FALSE;
        if (queued)
            AddTail(cmd == CMD_READ ? &op->op_Reads : &op->op_Orphans,
                    &io->ios2_Req.io_Message.mn_Node);
        Enable();

        if (!queued)
            netdev_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
        return;
    }

    case CMD_WRITE:
    case S2_MULTICAST:
    case S2_BROADCAST:
    {
        UBYTE bcast[NETDEV_ADDR_LEN];
        UWORD i;

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
            !((op->op_Raw || (io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0) &&
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
        return;
    }

    case S2_DEVICEQUERY:
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

        /*
         * Filled here and copied, rather than written through the caller's
         * pointer: SizeAvailable is the caller saying how much room there is,
         * and a caller built against an older Sana2DeviceQuery has less than
         * this structure.  Writing every field and then reporting a smaller
         * SizeSupplied overruns exactly the caller that was careful.
         */
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
        return;
    }

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
        netdev_reply(io, 0, 0);
        return;

    case CMD_FLUSH:
    {
        struct IOSana2Req *q;

        Disable();
        while ((q = (struct IOSana2Req *)RemHead(&op->op_Reads)) != NULL)
            netdev_reply(q, IOERR_ABORTED, 0);
        while ((q = (struct IOSana2Req *)RemHead(&op->op_Orphans)) != NULL)
            netdev_reply(q, IOERR_ABORTED, 0);
        while ((q = (struct IOSana2Req *)RemHead(&op->op_Events)) != NULL)
            netdev_reply(q, IOERR_ABORTED, 0);
        Enable();

        /* Recomputed rather than cleared: the other openers' waits are still
           on their own lists.  A stale bit only costs a walk that finds
           nothing, so the order here is not load-bearing. */
        netdev_event_rescan(unit);

        /* And the writes, which are queued on the unit rather than the
           opener.  A caller flushes so that teardown is safe, and one of its
           own requests still live in the driver is what the flush prevents. */
        netdev_drop_writes(unit, op);

        netdev_reply(io, 0, 0);
        return;
    }

    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESS:
    {
        BOOL add = (BOOL)(cmd == S2_ADDMULTICASTADDRESS);
        BOOL applied;

        /*
         * Bit 0 of the first octet is the Ethernet group bit.  A unicast
         * address is not a multicast group and is refused.
         */
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
        {
            applied = netdev_mcast_del(unit->nu_Mcast, io->ios2_SrcAddr);
        }
        if (applied)
            netdev_rebuild_filter(unit);
        Enable();

        if (applied)
            netdev_reply(io, 0, 0);
        else if (add)
            netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_MULTICAST_FULL);
        else
            netdev_reply(io, S2ERR_BAD_STATE, S2WERR_BAD_MULTICAST);
        return;
    }

    case S2_ADDMULTICASTADDRESSES:
    case S2_DELMULTICASTADDRESSES:
    {
        BOOL  add  = (BOOL)(cmd == S2_ADDMULTICASTADDRESSES);
        ULONG count;
        BOOL  wide;
        BOOL  applied = TRUE;

        if ((io->ios2_SrcAddr[0] & 1) == 0)
        {
            netdev_reply(io, S2ERR_BAD_ADDRESS, S2WERR_BAD_MULTICAST);
            return;
        }

        wide = mcast_range_wide(io->ios2_SrcAddr, io->ios2_DstAddr, &count);
        Disable();
        if (wide)
        {
            if (add)
            {
                /* Match the exact table's saturating references. */
                if (unit->nu_AllMulti != 0xffffu)
                    unit->nu_AllMulti++;
            }
            else if (unit->nu_AllMulti != 0)
                unit->nu_AllMulti--;
            else
                applied = FALSE;
        }
        else
        {
            applied = netdev_mcast_range_apply(unit->nu_Mcast,
                                               io->ios2_SrcAddr, count, add);
            if (!applied && add)
                unit->nu_McastFull++;
        }

        if (applied)
            netdev_rebuild_filter(unit);
        Enable();

        if (applied)
            netdev_reply(io, 0, 0);
        else if (add)
            netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_MULTICAST_FULL);
        else
            netdev_reply(io, S2ERR_BAD_STATE, S2WERR_BAD_MULTICAST);
        return;
    }

    case S2_TRACKTYPE:
    {
        UWORD i;
        LONG  free_slot = -1;

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
            {
                free_slot = i;
            }
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
        return;
    }

    case S2_UNTRACKTYPE:
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
        return;
    }

    case S2_GETTYPESTATS:
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
        return;
    }

    case S2_GETGLOBALSTATS:
        if (io->ios2_StatData == NULL)
        {
            netdev_reply(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
            return;
        }
        /* Filled from the chip core rather than kept twice: two counters for
           one event that disagree cost an hour in the field. */
        unit->nu_Stats.Overruns = unit->nu_Nic.overruns;
        unit->nu_Stats.BadData  = unit->nu_Nic.rx_errors;
        /*
         * LastStart stays zero, and that is a gap rather than a value.  It
         * wants a timeval, and timer.device is not open in this device.  It is
         * stated here so that the zero is not read as an interface that
         * started at the epoch.
         */
        cmd_bytes((UBYTE *)io->ios2_StatData, (const UBYTE *)&unit->nu_Stats,
                  sizeof(struct Sana2DeviceStats));
        netdev_reply(io, 0, 0);
        return;

    case S2_GETSPECIALSTATS:
        cmd_special_stats(unit, io);
        return;

    case S2_ONEVENT:
    {
        ULONG mask = io->ios2_WireError;
        ULONG now  = unit->nu_Online ? S2EVENT_ONLINE : S2EVENT_OFFLINE;

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
        if ((mask & ~(ULONG)(S2EVENT_ERROR | S2EVENT_TX | S2EVENT_RX |
                             S2EVENT_ONLINE | S2EVENT_OFFLINE |
                             S2EVENT_BUFF | S2EVENT_HARDWARE)) != 0)
        {
            netdev_reply(io, S2ERR_NOT_SUPPORTED, S2WERR_BAD_EVENT);
            return;
        }

        /* "Types ONLINE and OFFLINE return immediately if the device is
           already in the state to be waited for." */
        if ((mask & now) != 0)
        {
            netdev_reply(io, 0, mask & now);
            return;
        }

        netdev_event_wait(unit, io);
        return;
    }

    case NSCMD_DEVICEQUERY:
    {
        /*
         * The new-style query is asked with an IOStdReq, not an IOSana2Req,
         * and io_Actual lands where ios2_WireError does -- so netdev_reply()
         * would overwrite the answer.  This one replies by hand.
         */
        struct IOStdReq      *std = (struct IOStdReq *)io;
        struct NetdevNSQuery *q   = (struct NetdevNSQuery *)std->io_Data;

        if (q == NULL || std->io_Length < 16)
        {
            /* This is an IOStdReq, so ios2_WireError is io_Actual.  The SANA
               reply helper would turn this error into a bogus nonzero byte
               count in the caller's request. */
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

        std->io_Actual = sizeof(struct NetdevNSQuery);
        std->io_Error  = 0;
        if ((std->io_Flags & IOF_QUICK) == 0)
            ReplyMsg(&std->io_Message);
        return;
    }

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
