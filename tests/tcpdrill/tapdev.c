/*
 * tcpdrill, the synthetic SANA-II device.  See tapdev.h for why it exists.
 *
 * MakeLibrary() builds a six-entry Exec device vector, Open, Close, Expunge,
 * Null, BeginIO, AbortIO, and AddDevice() puts it where OpenDevice() looks
 * first.  Everything is in this program's own address space, so there is no
 * segment list, no __saveds and nothing resident: the device dies with the
 * process that installed it.
 *
 * OpenDevice() is handed a tag list carrying S2_CopyToBuff and S2_CopyFromBuff,
 * and a SANA-II device moves packet data only through them, the stack's
 * NX_PACKET is not something the device may touch directly.
 * src/sana2/sana2_copy.c is the other end of both, so calling them tests the
 * shipped receive and transmit paths rather than a bypass around them.
 *
 * They are m68k register-convention functions (a0 = to, a1 = from, d0 = len).
 * A `register ... __asm()` typedef miscompiles on this toolchain, GCC 15
 * loads the function pointer into a0 and then jumps through it, destroying the
 * first argument, so the call is written out as inline asm below.  The wrong
 * version compiles cleanly and corrupts every received frame, so it shows up
 * only under a disassembler.
 *
 * Raw framing (SANA2IOF_RAW) is answered S2ERR_NOT_SUPPORTED, so
 * ami_sana2_probe_raw() decides against it and the stack runs its cooked path.
 * That is the path a2065.device drives and the one every measurement in
 * docs/RESEARCH.md was taken through; exercising the raw path here would test
 * code the shipped configuration does not run.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tapdev.h"

#include <exec/types.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <utility/tagitem.h>

#include <proto/exec.h>

#include "sana2_device.h"

/* ------------------------------------------------------------ ether bits -- */

#define ETH_HDR         14
#define ETH_ADDR        6

/* ------------------------------------------------------------- the state -- */

typedef struct TapOpen
{
    struct Unit     unit;               /* so io_Unit is a real Unit       */
    APTR            copy_to;            /* S2_CopyToBuff                   */
    APTR            copy_from;          /* S2_CopyFromBuff                 */
} TapOpen;

typedef struct TapFrame
{
    ULONG           len;
    ULONG           stamp;
    UBYTE           data[TAP_FRAME_MAX];
} TapFrame;

typedef struct TapDevice
{
    struct Device   dd;
    TapOpen         open;
    struct List     reads;              /* queued CMD_READs                */
    struct List     events;             /* queued S2_ONEVENTs              */
    BOOL            online;
    BOOL            configured;
    UBYTE           mac[ETH_ADDR];
    TapStats        stats;
    TapFrame       *tx;                 /* TAP_TX_SLOTS frames             */
    UWORD           tx_head;            /* next to fill                    */
    UWORD           tx_tail;            /* next to collect                 */
    UWORD           tx_count;
} TapDevice;

static TapDevice       *tap_dev;
static struct Device   *tap_timer;
static struct IORequest tap_timer_req;
static struct MsgPort  *tap_timer_port;
static ULONG            tap_eclock_hz;

/* The device's own name and id, which must outlive every caller. */
static const char tap_name[] = TAP_DEVICE_NAME;
static const char tap_id[]   = TAP_DEVICE_NAME " 1.0 (tcpdrill)";

/* --------------------------------------------------------------- eclock --- */

/*
 * ReadEClock() by LVO rather than through <proto/timer.h>: the NDK spells
 * TimerBase as one type and this file holds another.  timer.device LVOs:
 * AddTime -42, SubTime -48, CmpTime -54, ReadEClock -60, GetSysTime -66.
 */
static ULONG tap_read_eclock(struct EClockVal *dest)
{
    register struct Device   *a6  __asm("a6") = tap_timer;
    register struct EClockVal *a0 __asm("a0") = dest;
    register ULONG            res __asm("d0");
    register LONG _clob_a0 __asm("a0");

    if (tap_timer == NULL)
    {
        dest->ev_hi = 0;
        dest->ev_lo = 0;
        return 0;
    }

    __asm __volatile ("jsr a6@(-60:W)"
                      : "=r" (res), "=r" (_clob_a0)
                      : "r" (a6), "r" (a0)
                      : "a1", "d1", "cc", "memory");
    return res;
}

ULONG tap_eclock_rate(VOID)
{
    return tap_eclock_hz;
}

ULONG tap_eclock_now(VOID)
{
    struct EClockVal ev;

    (VOID)tap_read_eclock(&ev);
    return ev.ev_lo;
}

/* ------------------------------------------------ the copy hooks, called --- */

static BOOL tap_copy_call(APTR fn, APTR to, APTR from, ULONG len)
{
    register APTR  _a2 __asm("a2") = fn;
    register APTR  _a0 __asm("a0") = to;
    register APTR  _a1 __asm("a1") = from;
    register ULONG _d0 __asm("d0") = len;
    register LONG  _d1 __asm("d1");
    register LONG  res __asm("d0");

    if (fn == NULL)
        return FALSE;

    __asm __volatile ("jsr a2@"
                      : "=r" (res), "=r" (_a0), "=r" (_a1), "=r" (_d1)
                      : "r" (_a2), "0" (_d0), "1" (_a0), "2" (_a1)
                      : "cc", "memory");

    return (res != 0) ? TRUE : FALSE;
}

/* --------------------------------------------------------------- helpers -- */

static VOID tap_bytes(UBYTE *to, const UBYTE *from, ULONG n)
{
    while (n-- != 0)
        *to++ = *from++;
}

static VOID tap_zero(UBYTE *p, ULONG n)
{
    while (n-- != 0)
        *p++ = 0;
}

/* Walk a flat tag list.  No utility.library: TAG_MORE is the only indirection
   a SANA-II buffer-management list is ever built with, and src/sana2 does not
   even use that. */
static VOID tap_take_tags(const struct TagItem *tags, TapOpen *open)
{
    while (tags != NULL)
    {
        ULONG tag = tags->ti_Tag;

        if (tag == TAG_DONE)
            break;
        if (tag == TAG_MORE)
        {
            tags = (const struct TagItem *)tags->ti_Data;
            continue;
        }
        if (tag == TAG_IGNORE)
        {
            tags++;
            continue;
        }
        if (tag == TAG_SKIP)
        {
            tags += 1 + (LONG)tags->ti_Data;
            continue;
        }

        if (tag == S2_CopyToBuff)
            open->copy_to = (APTR)tags->ti_Data;
        else if (tag == S2_CopyFromBuff)
            open->copy_from = (APTR)tags->ti_Data;

        tags++;
    }
}

static VOID tap_complete(struct IOSana2Req *io, LONG err, ULONG wire)
{
    io->ios2_Req.io_Error = (BYTE)err;
    io->ios2_WireError    = wire;

    if ((io->ios2_Req.io_Flags & IOF_QUICK) != 0)
        return;                          /* the caller reads it in place */

    ReplyMsg(&io->ios2_Req.io_Message);
}

/* ------------------------------------------------------ transmit capture -- */

/*
 * CMD_WRITE.  The stack hands over an IP-or-ARP payload with no link header
 * (cooked framing), so the 14 bytes are rebuilt here from the fields SANA-II
 * carries them in, the same reconstruction sana2_rx.c performs in the other
 * direction, so one pcap-shaped decoder reads both.
 */
static VOID tap_capture(TapDevice *dev, struct IOSana2Req *io)
{
    TapFrame *f;
    ULONG     len = io->ios2_DataLength;

    if (len + ETH_HDR > TAP_FRAME_MAX)
    {
        dev->stats.tx_overrun++;
        return;
    }

    if (dev->tx_count >= TAP_TX_SLOTS)
    {
        dev->stats.tx_overrun++;
        return;
    }

    f = &dev->tx[dev->tx_head];

    tap_bytes(&f->data[0], io->ios2_DstAddr, ETH_ADDR);
    tap_bytes(&f->data[6], dev->mac, ETH_ADDR);
    f->data[12] = (UBYTE)(io->ios2_PacketType >> 8);
    f->data[13] = (UBYTE)(io->ios2_PacketType);

    if (len != 0 &&
        !tap_copy_call(dev->open.copy_from, &f->data[ETH_HDR],
                       io->ios2_Data, len))
    {
        dev->stats.tx_overrun++;
        return;
    }

    f->len   = len + ETH_HDR;
    f->stamp = tap_eclock_now();

    dev->tx_head = (UWORD)((dev->tx_head + 1) % TAP_TX_SLOTS);
    dev->tx_count++;
    dev->stats.tx_frames++;
}

/* ------------------------------------------------------------- device fns - */

static struct Device *tap_open(register struct Device    *dev __asm("a6"),
                               register struct IOSana2Req *io __asm("a1"),
                               register ULONG            unit __asm("d0"),
                               register ULONG           flags __asm("d1"))
{
    TapDevice *d = (TapDevice *)dev;

    (VOID)unit;
    (VOID)flags;

    d->open.copy_to   = NULL;
    d->open.copy_from = NULL;
    tap_take_tags((const struct TagItem *)io->ios2_BufferManagement, &d->open);

    io->ios2_Req.io_Device = dev;
    io->ios2_Req.io_Unit   = &d->open.unit;
    io->ios2_Req.io_Error  = 0;

    dev->dd_Library.lib_OpenCnt++;
    dev->dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;

    return dev;
}

static BPTR tap_close(register struct Device     *dev __asm("a6"),
                      register struct IOSana2Req  *io __asm("a1"))
{
    io->ios2_Req.io_Device = (struct Device *)-1;
    io->ios2_Req.io_Unit   = (struct Unit *)-1;

    if (dev->dd_Library.lib_OpenCnt != 0)
        dev->dd_Library.lib_OpenCnt--;

    return (BPTR)0;
}

static BPTR tap_expunge(register struct Device *dev __asm("a6"))
{
    (VOID)dev;
    return (BPTR)0;                      /* never expunged; tap_remove() does it */
}

static ULONG tap_null(VOID)
{
    return 0;
}

static VOID tap_begin_io(register struct Device     *dev __asm("a6"),
                         register struct IOSana2Req  *io __asm("a1"))
{
    TapDevice *d   = (TapDevice *)dev;
    UWORD      cmd = io->ios2_Req.io_Command;

    io->ios2_Req.io_Error = 0;
    io->ios2_WireError    = 0;

    switch (cmd)
    {
    case CMD_READ:
        /*
         * Raw reads are refused on purpose, see the file header.  The
         * refusal has to happen here, in BeginIO, because that is the only
         * signal ami_sana2_probe_raw() reads.
         */
        if ((io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0)
        {
            tap_complete(io, S2ERR_NOT_SUPPORTED, S2WERR_GENERIC_ERROR);
            return;
        }
        if (!d->online)
        {
            tap_complete(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
            return;
        }
        io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
        io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        Forbid();
        AddTail(&d->reads, &io->ios2_Req.io_Message.mn_Node);
        d->stats.reads_queued++;
        Permit();
        return;

    case CMD_WRITE:
    case S2_BROADCAST:
        if (!d->online)
        {
            tap_complete(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
            return;
        }
        if (cmd == S2_BROADCAST)
        {
            UWORD i;
            for (i = 0; i < ETH_ADDR; i++)
                io->ios2_DstAddr[i] = 0xFF;
        }
        Forbid();
        tap_capture(d, io);
        Permit();
        tap_complete(io, 0, 0);
        return;

    case CMD_FLUSH:
    {
        struct Node *n;

        Forbid();
        while ((n = RemHead(&d->reads)) != NULL)
        {
            struct IOSana2Req *r = (struct IOSana2Req *)n;

            d->stats.reads_queued--;
            r->ios2_Req.io_Error = (BYTE)IOERR_ABORTED;
            ReplyMsg(&r->ios2_Req.io_Message);
        }
        Permit();
        tap_complete(io, 0, 0);
        return;
    }

    case S2_DEVICEQUERY:
    {
        struct Sana2DeviceQuery *q =
            (struct Sana2DeviceQuery *)io->ios2_StatData;

        if (q == NULL)
        {
            tap_complete(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
            return;
        }
        q->SizeSupplied   = (ULONG)sizeof(*q);
        q->DevQueryFormat = 0;
        q->DeviceLevel    = 0;
        q->AddrFieldSize  = ETH_ADDR * 8;
        q->MTU            = 1500;
        q->BPS            = 10000000UL;
        q->HardwareType   = S2WireType_Ethernet;
        tap_complete(io, 0, 0);
        return;
    }

    case S2_GETSTATIONADDRESS:
        tap_bytes(io->ios2_SrcAddr, d->mac, ETH_ADDR);
        tap_bytes(io->ios2_DstAddr, d->mac, ETH_ADDR);
        tap_complete(io, 0, 0);
        return;

    case S2_CONFIGINTERFACE:
        if (d->configured)
        {
            tap_complete(io, S2ERR_BAD_STATE, S2WERR_IS_CONFIGURED);
            return;
        }
        tap_bytes(d->mac, io->ios2_SrcAddr, ETH_ADDR);
        d->configured = TRUE;
        tap_complete(io, 0, 0);
        return;

    case S2_ONLINE:
        if (d->online)
        {
            tap_complete(io, S2ERR_BAD_STATE, S2WERR_UNIT_ONLINE);
            return;
        }
        d->online = TRUE;
        d->stats.online_count++;
        tap_complete(io, 0, 0);
        return;

    case S2_OFFLINE:
    {
        struct Node *n;

        /*
         * S2_OFFLINE returns every queued CMD_READ with S2ERR_OUTOFSERVICE.
         * src/sana2/sana2_rx.c depends on exactly this: it is the only
         * mechanism that frees the readers on a device which ignores
         * AbortIO(), as a2065.device 2.16 does.  Implemented here so the
         * shutdown path under test is the one that ships.
         */
        Forbid();
        d->online = FALSE;
        while ((n = RemHead(&d->reads)) != NULL)
        {
            struct IOSana2Req *r = (struct IOSana2Req *)n;

            d->stats.reads_queued--;
            r->ios2_Req.io_Error = (BYTE)S2ERR_OUTOFSERVICE;
            r->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            ReplyMsg(&r->ios2_Req.io_Message);
        }
        while ((n = RemHead(&d->events)) != NULL)
        {
            struct IOSana2Req *r = (struct IOSana2Req *)n;

            r->ios2_Req.io_Error = 0;
            r->ios2_WireError    = S2EVENT_OFFLINE;
            ReplyMsg(&r->ios2_Req.io_Message);
        }
        Permit();
        d->stats.offline_count++;
        tap_complete(io, 0, 0);
        return;
    }

    case S2_GETGLOBALSTATS:
    {
        struct Sana2DeviceStats *s =
            (struct Sana2DeviceStats *)io->ios2_StatData;

        if (s == NULL)
        {
            tap_complete(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
            return;
        }
        tap_zero((UBYTE *)s, (ULONG)sizeof(*s));
        s->PacketsReceived = d->stats.rx_delivered;
        s->PacketsSent     = d->stats.tx_frames;
        tap_complete(io, 0, 0);
        return;
    }

    /*
     * S2_ONEVENT.  A request that names an event which has already happened
     * completes at once; anything else is held until it does, which for a
     * device with no wire means forever.  Returning IOERR_NOCMD instead spins
     * a stack that polls its link state, and completing an ONLINE request that
     * has not happened yet tells one it has a link when it has not.  Held
     * requests are returned by S2_OFFLINE and by tap_remove().
     *
     * Our stack never issues this; Roadshow does, and the device has to be
     * openable by both.
     */
    case S2_ONEVENT:
    {
        ULONG want = io->ios2_WireError;

        if (((want & S2EVENT_ONLINE) != 0 && d->online) ||
            ((want & S2EVENT_OFFLINE) != 0 && !d->online))
        {
            io->ios2_WireError = want & (d->online ? S2EVENT_ONLINE
                                                   : S2EVENT_OFFLINE);
            tap_complete(io, 0, io->ios2_WireError);
            return;
        }

        io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
        io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        Forbid();
        AddTail(&d->events, &io->ios2_Req.io_Message.mn_Node);
        Permit();
        return;
    }

    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESS:
    case S2_TRACKTYPE:
    case S2_UNTRACKTYPE:
    case CMD_RESET:
    case CMD_UPDATE:
    case CMD_CLEAR:
    case CMD_START:
    case CMD_STOP:
        tap_complete(io, 0, 0);
        return;

    default:
        tap_complete(io, IOERR_NOCMD, S2WERR_GENERIC_ERROR);
        return;
    }
}

static LONG tap_abort_io(register struct Device     *dev __asm("a6"),
                         register struct IOSana2Req  *io __asm("a1"))
{
    TapDevice   *d = (TapDevice *)dev;
    struct Node *n;
    LONG         rc = -1;

    Forbid();
    for (n = d->reads.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        if (n == &io->ios2_Req.io_Message.mn_Node)
        {
            Remove(n);
            d->stats.reads_queued--;
            io->ios2_Req.io_Error = (BYTE)IOERR_ABORTED;
            ReplyMsg(&io->ios2_Req.io_Message);
            rc = 0;
            break;
        }
    }
    if (rc != 0)
    {
        for (n = d->events.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        {
            if (n == &io->ios2_Req.io_Message.mn_Node)
            {
                Remove(n);
                io->ios2_Req.io_Error = (BYTE)IOERR_ABORTED;
                ReplyMsg(&io->ios2_Req.io_Message);
                rc = 0;
                break;
            }
        }
    }
    Permit();

    return rc;
}

/* ----------------------------------------------------------- the harness -- */

ULONG tap_tx_get(UBYTE *buf, ULONG max, ULONG *stamp)
{
    TapDevice *d = tap_dev;
    TapFrame  *f;
    ULONG      len;

    if (d == NULL)
        return 0;

    Forbid();
    if (d->tx_count == 0)
    {
        Permit();
        return 0;
    }

    f   = &d->tx[d->tx_tail];
    len = f->len;
    if (len > max)
        len = max;
    tap_bytes(buf, f->data, len);
    if (stamp != NULL)
        *stamp = f->stamp;

    d->tx_tail = (UWORD)((d->tx_tail + 1) % TAP_TX_SLOTS);
    d->tx_count--;
    Permit();

    return len;
}

LONG tap_rx_put(const UBYTE *frame, ULONG len)
{
    TapDevice         *d = tap_dev;
    struct IOSana2Req *pick = NULL;
    struct Node       *n;
    UWORD              type;
    LONG               rc  = 0;

    if (d == NULL || len < ETH_HDR)
        return -1;

    type = (UWORD)((((UWORD)frame[12]) << 8) | frame[13]);

    Forbid();
    for (n = d->reads.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        struct IOSana2Req *r = (struct IOSana2Req *)n;

        if (r->ios2_PacketType == (ULONG)type)
        {
            pick = r;
            break;
        }
    }

    if (pick == NULL)
    {
        d->stats.rx_no_reader++;
        Permit();
        return -1;
    }

    Remove(&pick->ios2_Req.io_Message.mn_Node);
    d->stats.reads_queued--;

    tap_bytes(pick->ios2_DstAddr, &frame[0], ETH_ADDR);
    tap_bytes(pick->ios2_SrcAddr, &frame[6], ETH_ADDR);
    pick->ios2_PacketType = (ULONG)type;
    pick->ios2_DataLength = len - ETH_HDR;
    pick->ios2_Req.io_Flags &= (UBYTE)~(SANA2IOF_BCAST | SANA2IOF_MCAST);
    if (frame[0] == 0xFF)
        pick->ios2_Req.io_Flags |= SANA2IOF_BCAST;
    else if ((frame[0] & 0x01) != 0)
        pick->ios2_Req.io_Flags |= SANA2IOF_MCAST;

    if (tap_copy_call(d->open.copy_to, pick->ios2_Data,
                      (APTR)&frame[ETH_HDR], len - ETH_HDR))
    {
        pick->ios2_Req.io_Error = 0;
        d->stats.rx_delivered++;
    }
    else
    {
        pick->ios2_Req.io_Error = (BYTE)S2ERR_NO_RESOURCES;
        pick->ios2_WireError    = S2WERR_BUFF_ERROR;
        d->stats.rx_copy_failed++;
        rc = -1;
    }

    ReplyMsg(&pick->ios2_Req.io_Message);
    Permit();

    return rc;
}

ULONG tap_reads_for(UWORD ether_type)
{
    TapDevice   *d = tap_dev;
    struct Node *n;
    ULONG        count = 0;

    if (d == NULL)
        return 0;

    Forbid();
    for (n = d->reads.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        struct IOSana2Req *r = (struct IOSana2Req *)n;

        if (ether_type == 0 || r->ios2_PacketType == (ULONG)ether_type)
            count++;
    }
    Permit();

    return count;
}

BOOL tap_is_online(VOID)
{
    return (tap_dev != NULL && tap_dev->online) ? TRUE : FALSE;
}

VOID tap_get_stats(TapStats *out)
{
    if (out == NULL)
        return;

    if (tap_dev == NULL)
    {
        tap_zero((UBYTE *)out, (ULONG)sizeof(*out));
        return;
    }

    Forbid();
    *out = tap_dev->stats;
    Permit();
}

/* ------------------------------------------------------------- install ---- */

static const APTR tap_vectors[] =
{
    (APTR)tap_open,
    (APTR)tap_close,
    (APTR)tap_expunge,
    (APTR)tap_null,
    (APTR)tap_begin_io,
    (APTR)tap_abort_io,
    (APTR)-1
};

/*
 * The timer half of the teardown, on its own because tap_install() acquires it
 * before tap_dev is set and tap_remove() gates on tap_dev: both of the failure
 * returns below used to leave the MsgPort and the open timer.device behind.
 */
static VOID tap_timer_close(VOID)
{
    if (tap_timer != NULL)
    {
        CloseDevice(&tap_timer_req);
        tap_timer = NULL;
    }
    if (tap_timer_port != NULL)
    {
        DeleteMsgPort(tap_timer_port);
        tap_timer_port = NULL;
    }
}

LONG tap_install(const UBYTE *mac)
{
    TapDevice *d;

    if (tap_dev != NULL)
        return 0;

    /* The E-Clock is the only monotonic reading available from every context
       here, and it is what timestamps the transmit ring. */
    tap_timer_port = CreateMsgPort();
    if (tap_timer_port != NULL)
    {
        tap_timer_req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        tap_timer_req.io_Message.mn_ReplyPort    = tap_timer_port;
        tap_timer_req.io_Message.mn_Length = (UWORD)sizeof(tap_timer_req);

        if (OpenDevice((STRPTR)"timer.device", UNIT_ECLOCK,
                       &tap_timer_req, 0) == 0)
        {
            struct EClockVal ev;

            tap_timer     = tap_timer_req.io_Device;
            tap_eclock_hz = tap_read_eclock(&ev);
        }
    }

    d = (TapDevice *)MakeLibrary((APTR)tap_vectors, NULL, NULL,
                                 (ULONG)sizeof(TapDevice), (BPTR)0);
    if (d == NULL)
    {
        tap_timer_close();
        return -1;
    }

    d->tx = (TapFrame *)AllocMem((ULONG)sizeof(TapFrame) * TAP_TX_SLOTS,
                                 MEMF_PUBLIC | MEMF_CLEAR);
    if (d->tx == NULL)
    {
        FreeMem((APTR)((ULONG)d - d->dd.dd_Library.lib_NegSize),
                (ULONG)(d->dd.dd_Library.lib_NegSize +
                        d->dd.dd_Library.lib_PosSize));
        tap_timer_close();
        return -1;
    }

    /* NewList() lives in amiga.lib, which nothing else here links.  Four
       stores are cheaper than the dependency. */
    d->reads.lh_Head     = (struct Node *)&d->reads.lh_Tail;
    d->reads.lh_Tail     = NULL;
    d->reads.lh_TailPred = (struct Node *)&d->reads.lh_Head;
    d->reads.lh_Type     = NT_MESSAGE;

    d->events.lh_Head     = (struct Node *)&d->events.lh_Tail;
    d->events.lh_Tail     = NULL;
    d->events.lh_TailPred = (struct Node *)&d->events.lh_Head;
    d->events.lh_Type     = NT_MESSAGE;

    tap_bytes(d->mac, mac, ETH_ADDR);

    d->dd.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    d->dd.dd_Library.lib_Node.ln_Name = (char *)&tap_name[0];
    d->dd.dd_Library.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    d->dd.dd_Library.lib_Version      = 1;
    d->dd.dd_Library.lib_Revision     = 0;
    d->dd.dd_Library.lib_IdString     = (char *)&tap_id[0];

    tap_dev = d;
    AddDevice(&d->dd);

    return 0;
}

VOID tap_remove(VOID)
{
    TapDevice   *d = tap_dev;
    struct Node *n;

    if (d == NULL)
        return;

    Forbid();
    RemDevice(&d->dd);

    /*
     * Anything still queued belongs to a caller that is about to have the
     * memory holding this device freed underneath it.  Our stack closes
     * cleanly before we get here; a foreign stack that keeps the interface up
     * does not, and an I/O request that is never replied hangs its caller.
     */
    while ((n = RemHead(&d->reads)) != NULL)
    {
        struct IOSana2Req *r = (struct IOSana2Req *)n;

        d->stats.reads_queued--;
        r->ios2_Req.io_Error = (BYTE)IOERR_ABORTED;
        ReplyMsg(&r->ios2_Req.io_Message);
    }
    while ((n = RemHead(&d->events)) != NULL)
    {
        struct IOSana2Req *r = (struct IOSana2Req *)n;

        r->ios2_Req.io_Error = (BYTE)IOERR_ABORTED;
        ReplyMsg(&r->ios2_Req.io_Message);
    }
    Permit();

    if (d->tx != NULL)
        FreeMem(d->tx, (ULONG)sizeof(TapFrame) * TAP_TX_SLOTS);

    FreeMem((APTR)((ULONG)d - d->dd.dd_Library.lib_NegSize),
            (ULONG)(d->dd.dd_Library.lib_NegSize +
                    d->dd.dd_Library.lib_PosSize));

    tap_dev = NULL;

    tap_timer_close();
}
