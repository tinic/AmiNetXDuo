/*
 * anxnet.device, layer 1 of 3: the SANA-II command table.
 *
 * The error codes here are the ones Individual Computers' x-surf.device and
 * x-surf-100.device return, so an application that was written against those
 * sees no difference -- with one deliberate exception, and it is the reason
 * this driver exists:
 *
 *   S2_ADDMULTICASTADDRESS accepts an address whose GROUP bit, bit 0 of the
 *   first octet, is set.  Both IC drivers test bit 7 instead.  IPv4 multicast
 *   is 01:00:5e:.. and IPv6 is 33:33:.., and both have bit 7 clear in the
 *   first octet, so every legitimate join is answered S2ERR_BAD_ADDRESS, the
 *   hash filter stays empty, and the router's neighbour solicitation to
 *   33:33:ff:xx:xx:xx is dropped by the card.  On-link IPv6 still works,
 *   because a router advertisement comes back unicast; off-link does not,
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

static VOID cmd_queue(struct List *list, struct IOSana2Req *io)
{
    io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;

    Disable();
    AddTail(list, &io->ios2_Req.io_Message.mn_Node);
    Enable();
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
    /* The low 32 bits; the top 16 are compared separately. */
    return ((ULONG)a[2] << 24) | ((ULONG)a[3] << 16) |
           ((ULONG)a[4] << 8)  | (ULONG)a[5];
}

static UWORD cmd_addr16(const UBYTE *a)
{
    return (UWORD)(((UWORD)a[0] << 8) | a[1]);
}

/* ------------------------------------------------------------- multicast -- */

static NetdevMcast *mcast_find(NetdevUnit *unit, const UBYTE *addr)
{
    UWORD i;

    for (i = 0; i < NETDEV_MCAST_MAX; i++)
    {
        NetdevMcast *m = &unit->nu_Mcast[i];
        UWORD        j;
        BOOL         same = TRUE;

        if (m->refs == 0)
            continue;
        for (j = 0; j < NETDEV_ADDR_LEN; j++)
        {
            if (m->addr[j] != addr[j])
            {
                same = FALSE;
                break;
            }
        }
        if (same)
            return m;
    }

    return NULL;
}

static BOOL mcast_add(NetdevUnit *unit, const UBYTE *addr)
{
    NetdevMcast *m = mcast_find(unit, addr);
    UWORD        i;

    if (m != NULL)
    {
        /* Saturate rather than wrap: a wrap to zero frees a row that callers
           still hold, and the group stops being received with nothing said. */
        if (m->refs != 0xffffu)
            m->refs++;
        return TRUE;
    }

    for (i = 0; i < NETDEV_MCAST_MAX; i++)
    {
        if (unit->nu_Mcast[i].refs == 0)
        {
            cmd_bytes(unit->nu_Mcast[i].addr, addr, NETDEV_ADDR_LEN);
            unit->nu_Mcast[i].refs = 1;
            return TRUE;
        }
    }

    unit->nu_McastFull++;
    return FALSE;
}

static BOOL mcast_del(NetdevUnit *unit, const UBYTE *addr)
{
    NetdevMcast *m = mcast_find(unit, addr);

    if (m == NULL)
        return FALSE;

    m->refs--;
    return TRUE;
}

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

static VOID mcast_range_apply(NetdevUnit *unit, const UBYTE *lo, ULONG count,
                              BOOL add)
{
    UBYTE addr[NETDEV_ADDR_LEN];
    ULONG i;

    cmd_bytes(addr, lo, NETDEV_ADDR_LEN);

    for (i = 0; i < count; i++)
    {
        if (add)
            (VOID)mcast_add(unit, addr);
        else
            (VOID)mcast_del(unit, addr);

        if (++addr[5] == 0 && ++addr[4] == 0 && ++addr[3] == 0)
            addr[2]++;
    }
}

/* ------------------------------------------------------------- statistics - */

static const char netdev_stat_mode[]  = "Data transfer mode";
static const char netdev_stat_ovw[]   = "Receive ring overruns";
static const char netdev_stat_rst[]   = "Chip resets";
static const char netdev_stat_mc[]    = "Multicast addresses";
static const char netdev_stat_mcful[] = "Multicast joins refused";
static const char netdev_stat_coll[]  = "Collisions";

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

        cmd_queue(&unit->nu_Writes, io);
        Disable();
        netdev_tx_pump(unit);
        Enable();
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

        /* And the writes, which are queued on the unit rather than the
           opener.  A caller flushes so that teardown is safe, and one of its
           own requests still live in the driver is exactly what it flushed
           to prevent. */
        netdev_drop_writes(unit, op);

        netdev_reply(io, 0, 0);
        return;
    }

    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESS:
        /*
         * THE FIX.  Bit 0 of the first octet is the Ethernet group bit; a
         * unicast address is not a multicast group and is refused.
         */
        if ((io->ios2_SrcAddr[0] & 1) == 0)
        {
            netdev_reply(io, S2ERR_BAD_ADDRESS, S2WERR_BAD_MULTICAST);
            return;
        }

        if (cmd == S2_ADDMULTICASTADDRESS)
        {
            if (!mcast_add(unit, io->ios2_SrcAddr))
            {
                netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_MULTICAST_FULL);
                return;
            }
        }
        else if (!mcast_del(unit, io->ios2_SrcAddr))
        {
            netdev_reply(io, S2ERR_BAD_STATE, S2WERR_BAD_MULTICAST);
            return;
        }

        netdev_rebuild_filter(unit);
        netdev_reply(io, 0, 0);
        return;

    case S2_ADDMULTICASTADDRESSES:
    case S2_DELMULTICASTADDRESSES:
    {
        BOOL  add  = (BOOL)(cmd == S2_ADDMULTICASTADDRESSES);
        ULONG count;
        BOOL  wide;

        if ((io->ios2_SrcAddr[0] & 1) == 0)
        {
            netdev_reply(io, S2ERR_BAD_ADDRESS, S2WERR_BAD_MULTICAST);
            return;
        }

        wide = mcast_range_wide(io->ios2_SrcAddr, io->ios2_DstAddr, &count);
        if (wide)
        {
            if (add)
                unit->nu_AllMulti++;
            else if (unit->nu_AllMulti != 0)
                unit->nu_AllMulti--;
        }
        else
        {
            mcast_range_apply(unit, io->ios2_SrcAddr, count, add);
        }

        netdev_rebuild_filter(unit);
        netdev_reply(io, 0, 0);
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
         * LastStart stays zero, and that is a gap rather than a value: it
         * wants a timeval, timer.device is not open in this device, and a
         * driver that opens one to fill in a statistic nobody has asked for
         * is the wrong trade.  Declared here so it is not read as "the
         * interface started at the epoch".
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

        if ((mask & ~(ULONG)(S2EVENT_ERROR | S2EVENT_TX | S2EVENT_RX |
                             S2EVENT_ONLINE | S2EVENT_OFFLINE |
                             S2EVENT_BUFF | S2EVENT_HARDWARE |
                             S2EVENT_SOFTWARE)) != 0)
        {
            netdev_reply(io, S2ERR_NOT_SUPPORTED, S2WERR_BAD_EVENT);
            return;
        }

        if ((mask & now) != 0)
        {
            io->ios2_WireError = mask & now;
            netdev_reply(io, 0, mask & now);
            return;
        }

        cmd_queue(&op->op_Events, io);
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
            netdev_reply(io, IOERR_BADLENGTH, S2WERR_NULL_POINTER);
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

    if (op == NULL)
        return FALSE;

    unit = op->op_Hw;

    Disable();
    found = cmd_dequeue(&op->op_Reads, io);
    if (!found)
        found = cmd_dequeue(&op->op_Orphans, io);
    if (!found)
        found = cmd_dequeue(&op->op_Events, io);
    if (!found)
        found = cmd_dequeue(&unit->nu_Writes, io);
    Enable();

    if (!found)
        return FALSE;

    netdev_reply(io, IOERR_ABORTED, 0);

    return TRUE;
}
