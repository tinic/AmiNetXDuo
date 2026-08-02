/*
 * tapprobe -- the instrumented SANA-II device.  See probedev.h.
 *
 * The device half is tests/tcpdrill/tapdev.c with an event ring bolted on and
 * the harness entry points widened.  The register-convention call to the
 * stack's copy hook is copied verbatim, including the reason it is written as
 * inline asm: a `register ... __asm()` typedef miscompiles here, GCC loads the
 * function pointer into a0 and destroys the first argument.
 *
 * SPDX-License-Identifier: MIT
 */

#include "probedev.h"

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
#include "sana2_r3_tags.h"

#define ETH_HDR         14
#define ETH_ADDR        6

typedef struct ProbeOpen
{
    struct Unit     unit;
    APTR            copy_to;
    APTR            copy_from;
} ProbeOpen;

typedef struct ProbeFrame
{
    ULONG           len;
    UWORD           type;
    UBYTE           data[PROBE_FRAME_MAX];
} ProbeFrame;

typedef struct ProbeDevice
{
    struct Device   dd;
    ProbeOpen       open;
    struct List     reads;
    struct List     events;
    BOOL            online;
    BOOL            configured;
    UBYTE           mac[ETH_ADDR];
    ProbeStats      stats;
    ProbeFrame     *tx;
    UWORD           tx_head;
    UWORD           tx_tail;
    UWORD           tx_count;
} ProbeDevice;

static ProbeDevice     *pd;
static struct Device   *pd_timer;
static struct IORequest pd_timer_req;
static struct MsgPort  *pd_timer_port;
static ULONG            pd_eclock_hz;
static BOOL             pd_poison;

/* Every hook the stack offered, whether or not this device calls it. */
static APTR             pd_to, pd_from, pd_to16, pd_from16, pd_filter;
static APTR             pd_to32, pd_from32, pd_dma_to, pd_dma_from;

static ULONG            pd_tag[PROBE_TAGS];
static ULONG            pd_tagdata[PROBE_TAGS];
static ULONG            pd_tagn;

static ProbeEvent       pd_ev[PROBE_EVENTS];
static ULONG            pd_ev_head;         /* next slot to write            */
static ULONG            pd_ev_count;        /* total ever recorded           */

static const char pd_name[] = PROBE_DEVICE_NAME;
static const char pd_id[]   = PROBE_DEVICE_NAME " 1.0 (tapprobe)";

/* --------------------------------------------------------------- eclock --- */

static ULONG pd_read_eclock(struct EClockVal *dest)
{
    register struct Device    *a6 __asm("a6") = pd_timer;
    register struct EClockVal *a0 __asm("a0") = dest;
    register ULONG            res __asm("d0");
    register LONG _clob_a0 __asm("a0");

    if (pd_timer == NULL)
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

ULONG probe_eclock_rate(VOID) { return pd_eclock_hz; }

ULONG probe_eclock_now(VOID)
{
    struct EClockVal ev;
    (VOID)pd_read_eclock(&ev);
    return ev.ev_lo;
}

/* ------------------------------------------------------------ event ring -- */

/* Cheap enough for BeginIO: one E-Clock read and six stores, no allocation,
   no Forbid() of its own (every caller already holds one). */
static VOID pd_ev_put(UBYTE kind, UBYTE aux, UWORD type, ULONG a, ULONG b)
{
    ProbeEvent *e = &pd_ev[pd_ev_head];

    e->kind = kind;
    e->aux  = aux;
    e->type = type;
    e->t    = probe_eclock_now();
    e->a    = a;
    e->b    = b;

    pd_ev_head = (pd_ev_head + 1) % PROBE_EVENTS;
    pd_ev_count++;
}

ULONG probe_events(ProbeEvent *out, ULONG max)
{
    ULONG have = (pd_ev_count < PROBE_EVENTS) ? pd_ev_count : PROBE_EVENTS;
    ULONG start;
    ULONG i;

    if (out == NULL || max == 0)
        return 0;
    if (have > max)
        have = max;

    /* Oldest first, ending at the newest. */
    start = (pd_ev_head + PROBE_EVENTS - have) % PROBE_EVENTS;
    for (i = 0; i < have; i++)
        out[i] = pd_ev[(start + i) % PROBE_EVENTS];

    return have;
}

/* ------------------------------------------------------------ copy hooks -- */

static BOOL pd_copy_call(APTR fn, APTR to, APTR from, ULONG len)
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

/* One argument in a0, the buffer back in d0.  SANA-II R3's DMA offer. */
static APTR pd_dma_call(APTR fn, APTR arg)
{
    register APTR _a2 __asm("a2") = fn;
    register APTR _a0 __asm("a0") = arg;
    register APTR res __asm("d0");

    if (fn == NULL)
        return NULL;

    __asm __volatile ("jsr a2@"
                      : "=r" (res), "=r" (_a0)
                      : "r" (_a2), "1" (_a0)
                      : "a1", "d1", "cc", "memory");
    return res;
}

/* --------------------------------------------------------------- helpers -- */

static VOID pd_bytes(UBYTE *to, const UBYTE *from, ULONG n)
{
    while (n-- != 0)
        *to++ = *from++;
}

static VOID pd_fill(UBYTE *p, UBYTE v, ULONG n)
{
    while (n-- != 0)
        *p++ = v;
}

static VOID pd_take_tags(const struct TagItem *tags, ProbeOpen *open)
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

        if (pd_tagn < PROBE_TAGS)
        {
            pd_tag[pd_tagn]     = tag;
            pd_tagdata[pd_tagn] = tags->ti_Data;
            pd_tagn++;
        }

        if (tag == S2_CopyToBuff)
            pd_to = open->copy_to = (APTR)tags->ti_Data;
        else if (tag == S2_CopyFromBuff)
            pd_from = open->copy_from = (APTR)tags->ti_Data;
        else if (tag == S2_CopyToBuff16)
            pd_to16 = (APTR)tags->ti_Data;
        else if (tag == S2_CopyFromBuff16)
            pd_from16 = (APTR)tags->ti_Data;
        else if (tag == S2_PacketFilter)
            pd_filter = (APTR)tags->ti_Data;
        else if (tag == S2_CopyToBuff32)
            pd_to32 = (APTR)tags->ti_Data;
        else if (tag == S2_CopyFromBuff32)
            pd_from32 = (APTR)tags->ti_Data;
        else if (tag == S2_DMACopyToBuff32)
            pd_dma_to = (APTR)tags->ti_Data;
        else if (tag == S2_DMACopyFromBuff32)
            pd_dma_from = (APTR)tags->ti_Data;

        tags++;
    }

    /* A stack that offers only the 16-bit-safe pair still has to be driven. */
    if (open->copy_to == NULL)
        open->copy_to = pd_to16;
    if (open->copy_from == NULL)
        open->copy_from = pd_from16;
}

ULONG probe_tags(ULONG **tag, ULONG **data)
{
    if (tag  != NULL) *tag  = pd_tag;
    if (data != NULL) *data = pd_tagdata;
    return pd_tagn;
}

APTR probe_hook_to(VOID)     { return pd_to; }
APTR probe_hook_from(VOID)   { return pd_from; }
APTR probe_hook_to16(VOID)   { return pd_to16; }
APTR probe_hook_from16(VOID) { return pd_from16; }
APTR probe_hook_filter(VOID) { return pd_filter; }
APTR probe_hook_dma_to(VOID)   { return pd_dma_to; }
APTR probe_hook_dma_from(VOID) { return pd_dma_from; }

static VOID pd_complete(struct IOSana2Req *io, LONG err, ULONG wire)
{
    io->ios2_Req.io_Error = (BYTE)err;
    io->ios2_WireError    = wire;

    if ((io->ios2_Req.io_Flags & IOF_QUICK) != 0)
        return;

    ReplyMsg(&io->ios2_Req.io_Message);
}

/* ------------------------------------------------------ transmit capture -- */

static VOID pd_capture(ProbeDevice *dev, struct IOSana2Req *io)
{
    ProbeFrame *f;
    ULONG       len = io->ios2_DataLength;

    if (len + ETH_HDR > PROBE_FRAME_MAX || dev->tx_count >= PROBE_TX_SLOTS)
    {
        dev->stats.tx_overrun++;
        return;
    }

    f = &dev->tx[dev->tx_head];

    pd_bytes(&f->data[0], io->ios2_DstAddr, ETH_ADDR);
    pd_bytes(&f->data[6], dev->mac, ETH_ADDR);
    f->data[12] = (UBYTE)(io->ios2_PacketType >> 8);
    f->data[13] = (UBYTE)(io->ios2_PacketType);

    if (len != 0 &&
        !pd_copy_call(dev->open.copy_from, &f->data[ETH_HDR],
                      io->ios2_Data, len))
    {
        dev->stats.tx_overrun++;
        return;
    }

    f->len  = len + ETH_HDR;
    f->type = (UWORD)io->ios2_PacketType;

    dev->tx_head = (UWORD)((dev->tx_head + 1) % PROBE_TX_SLOTS);
    dev->tx_count++;
    dev->stats.tx_frames++;

    pd_ev_put(PEV_WRITE, 0, (UWORD)io->ios2_PacketType, len, 0);
}

/* ------------------------------------------------------------- device fns - */

static struct Device *pd_open(register struct Device     *dev __asm("a6"),
                              register struct IOSana2Req  *io __asm("a1"),
                              register ULONG             unit __asm("d0"),
                              register ULONG            flags __asm("d1"))
{
    ProbeDevice *d = (ProbeDevice *)dev;

    (VOID)flags;

    d->open.copy_to   = NULL;
    d->open.copy_from = NULL;
    pd_take_tags((const struct TagItem *)io->ios2_BufferManagement, &d->open);

    io->ios2_Req.io_Device = dev;
    io->ios2_Req.io_Unit   = &d->open.unit;
    io->ios2_Req.io_Error  = 0;

    dev->dd_Library.lib_OpenCnt++;
    dev->dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;

    d->stats.opens++;
    pd_ev_put(PEV_OPEN, 0, 0, unit, (ULONG)io->ios2_BufferManagement);

    return dev;
}

static BPTR pd_close(register struct Device     *dev __asm("a6"),
                     register struct IOSana2Req  *io __asm("a1"))
{
    io->ios2_Req.io_Device = (struct Device *)-1;
    io->ios2_Req.io_Unit   = (struct Unit *)-1;

    if (dev->dd_Library.lib_OpenCnt != 0)
        dev->dd_Library.lib_OpenCnt--;

    pd_ev_put(PEV_CLOSE, 0, 0, dev->dd_Library.lib_OpenCnt, 0);
    return (BPTR)0;
}

static BPTR pd_expunge(register struct Device *dev __asm("a6"))
{
    (VOID)dev;
    return (BPTR)0;
}

static ULONG pd_null(VOID) { return 0; }

static VOID pd_begin_io(register struct Device     *dev __asm("a6"),
                        register struct IOSana2Req  *io __asm("a1"))
{
    ProbeDevice *d   = (ProbeDevice *)dev;
    UWORD        cmd = io->ios2_Req.io_Command;

    io->ios2_Req.io_Error = 0;
    io->ios2_WireError    = 0;

    /* Everything but the two hot commands, which carry their own events. */
    if (cmd != CMD_READ && cmd != CMD_WRITE)
        pd_ev_put(PEV_CMD, io->ios2_Req.io_Flags, (UWORD)io->ios2_PacketType,
                  cmd, (ULONG)io);

    switch (cmd)
    {
    case CMD_READ:
        if (!d->online)
        {
            pd_complete(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
            return;
        }
        io->ios2_Req.io_Flags &= (UBYTE)~IOF_QUICK;
        io->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        Forbid();
        AddTail(&d->reads, &io->ios2_Req.io_Message.mn_Node);
        d->stats.reads_now++;
        d->stats.reads_total++;
        if (d->stats.reads_now > d->stats.reads_max)
            d->stats.reads_max = d->stats.reads_now;
        if ((io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0)
            d->stats.raw_reads++;
        pd_ev_put(PEV_READ_IN, io->ios2_Req.io_Flags,
                  (UWORD)io->ios2_PacketType, (ULONG)io, d->stats.reads_now);
        Permit();
        return;

    case CMD_WRITE:
    case S2_BROADCAST:
        if (!d->online)
        {
            pd_complete(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
            return;
        }
        if (cmd == S2_BROADCAST)
        {
            UWORD i;
            for (i = 0; i < ETH_ADDR; i++)
                io->ios2_DstAddr[i] = 0xFF;
        }
        Forbid();
        pd_capture(d, io);
        Permit();
        pd_complete(io, 0, 0);
        return;

    case CMD_FLUSH:
    {
        struct Node *n;

        Forbid();
        while ((n = RemHead(&d->reads)) != NULL)
        {
            struct IOSana2Req *r = (struct IOSana2Req *)n;

            d->stats.reads_now--;
            r->ios2_Req.io_Error = (BYTE)IOERR_ABORTED;
            ReplyMsg(&r->ios2_Req.io_Message);
        }
        Permit();
        pd_complete(io, 0, 0);
        return;
    }

    case S2_DEVICEQUERY:
    {
        struct Sana2DeviceQuery *q =
            (struct Sana2DeviceQuery *)io->ios2_StatData;

        if (q == NULL)
        {
            pd_complete(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
            return;
        }
        q->DevQueryFormat = 0;
        q->DeviceLevel    = 0;
        q->AddrFieldSize  = ETH_ADDR * 8;
        q->MTU            = 1500;
        q->BPS            = 10000000UL;
        q->HardwareType   = S2WireType_Ethernet;
        /* Never claim to have filled more than the caller offered. */
        q->SizeSupplied   = (q->SizeAvailable != 0 &&
                             q->SizeAvailable < (ULONG)sizeof(*q))
                            ? q->SizeAvailable : (ULONG)sizeof(*q);
        pd_complete(io, 0, 0);
        return;
    }

    case S2_GETSTATIONADDRESS:
        pd_bytes(io->ios2_SrcAddr, d->mac, ETH_ADDR);
        pd_bytes(io->ios2_DstAddr, d->mac, ETH_ADDR);
        pd_complete(io, 0, 0);
        return;

    case S2_CONFIGINTERFACE:
        if (d->configured)
        {
            pd_complete(io, S2ERR_BAD_STATE, S2WERR_IS_CONFIGURED);
            return;
        }
        pd_bytes(d->mac, io->ios2_SrcAddr, ETH_ADDR);
        d->configured = TRUE;
        pd_complete(io, 0, 0);
        return;

    case S2_ONLINE:
        /*
         * Idempotent on purpose.  A device that starts online -- which is what
         * an Ethernet card looks like once it is configured -- would otherwise
         * answer S2ERR_BAD_STATE to a stack that asks anyway, and that is not
         * the behaviour under study here.
         */
        if (d->online)
        {
            pd_complete(io, 0, 0);
            return;
        }
        d->online = TRUE;
        d->stats.online_count++;
        pd_ev_put(PEV_ONLINE, 0, 0, 0, 0);
        pd_complete(io, 0, 0);
        return;

    case S2_OFFLINE:
    {
        struct Node *n;

        Forbid();
        d->online = FALSE;
        while ((n = RemHead(&d->reads)) != NULL)
        {
            struct IOSana2Req *r = (struct IOSana2Req *)n;

            d->stats.reads_now--;
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
        pd_ev_put(PEV_OFFLINE, 0, 0, 0, 0);
        pd_complete(io, 0, 0);
        return;
    }

    case S2_GETGLOBALSTATS:
    {
        struct Sana2DeviceStats *s =
            (struct Sana2DeviceStats *)io->ios2_StatData;

        if (s == NULL)
        {
            pd_complete(io, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
            return;
        }
        pd_fill((UBYTE *)s, 0, (ULONG)sizeof(*s));
        s->PacketsReceived = d->stats.rx_delivered;
        s->PacketsSent     = d->stats.tx_frames;
        pd_complete(io, 0, 0);
        return;
    }

    case S2_ONEVENT:
    {
        ULONG want = io->ios2_WireError;

        pd_ev_put(PEV_CMD, 0x10, 0, want, d->online ? 1 : 0);

        if (((want & S2EVENT_ONLINE) != 0 && d->online) ||
            ((want & S2EVENT_OFFLINE) != 0 && !d->online))
        {
            io->ios2_WireError = want & (d->online ? S2EVENT_ONLINE
                                                   : S2EVENT_OFFLINE);
            pd_complete(io, 0, io->ios2_WireError);
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
        pd_complete(io, 0, 0);
        return;

    default:
        pd_ev_put(PEV_CMD, 0xFF, 0, cmd, 0);   /* refused: IOERR_NOCMD */
        pd_complete(io, IOERR_NOCMD, S2WERR_GENERIC_ERROR);
        return;
    }
}

static LONG pd_abort_io(register struct Device     *dev __asm("a6"),
                        register struct IOSana2Req  *io __asm("a1"))
{
    ProbeDevice *d = (ProbeDevice *)dev;
    struct Node *n;
    LONG         rc = -1;

    Forbid();
    for (n = d->reads.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        if (n == &io->ios2_Req.io_Message.mn_Node)
        {
            Remove(n);
            d->stats.reads_now--;
            io->ios2_Req.io_Error = (BYTE)IOERR_ABORTED;
            pd_ev_put(PEV_ABORT, 0, 0, (ULONG)io, d->stats.reads_now);
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

ULONG probe_tx_get(UBYTE *buf, ULONG max)
{
    ProbeDevice *d = pd;
    ProbeFrame  *f;
    ULONG        len;

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
    pd_bytes(buf, f->data, len);

    d->tx_tail = (UWORD)((d->tx_tail + 1) % PROBE_TX_SLOTS);
    d->tx_count--;
    Permit();

    return len;
}

ULONG probe_tx_pending(VOID)
{
    return (pd != NULL) ? pd->tx_count : 0;
}

VOID probe_set_poison(BOOL on) { pd_poison = on; }

LONG probe_rx_put(UBYTE *frame, ULONG len)
{
    ProbeDevice       *d    = pd;
    struct IOSana2Req *pick = NULL;
    struct IOSana2Req *raw  = NULL;
    struct Node       *n;
    UWORD              type;
    UBYTE             *src;
    ULONG              slen;
    ULONG              t0, t1;
    BOOL               ok;
    LONG               rc = 0;

    if (d == NULL || len < ETH_HDR)
        return -1;

    type = (UWORD)((((UWORD)frame[12]) << 8) | frame[13]);

    Forbid();
    for (n = d->reads.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        struct IOSana2Req *r = (struct IOSana2Req *)n;

        if (r->ios2_PacketType != (ULONG)type)
            continue;
        if ((r->ios2_Req.io_Flags & SANA2IOF_RAW) != 0)
        {
            if (raw == NULL)
                raw = r;
            continue;
        }
        pick = r;
        break;
    }

    /* A raw reader is served only when no cooked one wants the type. */
    if (pick == NULL && raw != NULL)
        pick = raw;

    if (pick == NULL)
    {
        d->stats.rx_no_reader++;
        pd_ev_put(PEV_NOREADER, 0, type, 0, 0);
        Permit();
        return -1;
    }

    Remove(&pick->ios2_Req.io_Message.mn_Node);
    d->stats.reads_now--;

    pd_bytes(pick->ios2_DstAddr, &frame[0], ETH_ADDR);
    pd_bytes(pick->ios2_SrcAddr, &frame[6], ETH_ADDR);
    pick->ios2_PacketType = (ULONG)type;
    pick->ios2_Req.io_Flags &= (UBYTE)~(SANA2IOF_BCAST | SANA2IOF_MCAST);
    if (frame[0] == 0xFF)
        pick->ios2_Req.io_Flags |= SANA2IOF_BCAST;
    else if ((frame[0] & 0x01) != 0)
        pick->ios2_Req.io_Flags |= SANA2IOF_MCAST;

    if ((pick->ios2_Req.io_Flags & SANA2IOF_RAW) != 0)
    {
        src  = &frame[0];
        slen = len;
    }
    else
    {
        src  = &frame[ETH_HDR];
        slen = len - ETH_HDR;
    }
    pick->ios2_DataLength = slen;

    t0 = probe_eclock_now();
    ok = pd_copy_call(d->open.copy_to, pick->ios2_Data, src, slen);
    t1 = probe_eclock_now();

    /*
     * SANA-II gives the stack the source buffer for the duration of the hook
     * and no longer.  Scribbling on it here turns a second read into visible
     * corruption on the wire instead of a coincidence.
     */
    if (pd_poison)
        pd_fill(src, 0xEE, slen);

    pd_ev_put(PEV_COPY, (UBYTE)((ULONG)src & 3), type,
              t1 - t0, slen);
    /* The destination the stack handed us, for its low bits. */
    pd_ev_put(PEV_CMD, 2, type, (ULONG)pick->ios2_Data, ok ? 1 : 0);

    if (ok)
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

    pd_ev_put(PEV_READ_REPLY, 0, type, (ULONG)pick, d->stats.reads_now);
    ReplyMsg(&pick->ios2_Req.io_Message);
    Permit();

    return rc;
}

LONG probe_rx_put_dma(UBYTE *frame, ULONG len, APTR *buf)
{
    ProbeDevice       *d    = pd;
    struct IOSana2Req *pick = NULL;
    struct Node       *n;
    UWORD              type;
    APTR               dma;

    if (buf != NULL)
        *buf = NULL;
    if (d == NULL || len < ETH_HDR || pd_dma_to == NULL)
        return -1;

    type = (UWORD)((((UWORD)frame[12]) << 8) | frame[13]);

    Forbid();
    for (n = d->reads.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        struct IOSana2Req *r = (struct IOSana2Req *)n;

        if (r->ios2_PacketType == (ULONG)type &&
            (r->ios2_Req.io_Flags & SANA2IOF_RAW) == 0)
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

    dma = pd_dma_call(pd_dma_to, pick->ios2_Data);
    if (buf != NULL)
        *buf = dma;
    pd_ev_put(PEV_CMD, 0x20, type, (ULONG)dma, 0);

    if (dma == NULL)
    {
        Permit();
        return -1;              /* declined; the read stays queued */
    }

    Remove(&pick->ios2_Req.io_Message.mn_Node);
    d->stats.reads_now--;

    pd_bytes(pick->ios2_DstAddr, &frame[0], ETH_ADDR);
    pd_bytes(pick->ios2_SrcAddr, &frame[6], ETH_ADDR);
    pick->ios2_PacketType = (ULONG)type;
    pick->ios2_DataLength = len - ETH_HDR;
    pick->ios2_Req.io_Flags &= (UBYTE)~(SANA2IOF_BCAST | SANA2IOF_MCAST);

    pd_bytes((UBYTE *)dma, &frame[ETH_HDR], len - ETH_HDR);

    pick->ios2_Req.io_Error = 0;
    d->stats.rx_delivered++;
    pd_ev_put(PEV_READ_REPLY, 1, type, (ULONG)pick, d->stats.reads_now);
    ReplyMsg(&pick->ios2_Req.io_Message);
    Permit();

    return 0;
}

ULONG probe_reads_for(UWORD ether_type)
{
    ProbeDevice *d = pd;
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

BOOL probe_is_online(VOID)
{
    return (pd != NULL && pd->online) ? TRUE : FALSE;
}

ULONG probe_open_count(VOID)
{
    return (pd != NULL) ? pd->dd.dd_Library.lib_OpenCnt : 0;
}

VOID probe_get_stats(ProbeStats *out)
{
    if (out == NULL)
        return;

    if (pd == NULL)
    {
        pd_fill((UBYTE *)out, 0, (ULONG)sizeof(*out));
        return;
    }

    Forbid();
    *out = pd->stats;
    out->events_lost = (pd_ev_count > PROBE_EVENTS)
                     ? (pd_ev_count - PROBE_EVENTS) : 0;
    Permit();
}

/* ------------------------------------------------------------- install ---- */

static const APTR pd_vectors[] =
{
    (APTR)pd_open,
    (APTR)pd_close,
    (APTR)pd_expunge,
    (APTR)pd_null,
    (APTR)pd_begin_io,
    (APTR)pd_abort_io,
    (APTR)-1
};

static VOID pd_timer_close(VOID)
{
    if (pd_timer != NULL)
    {
        CloseDevice(&pd_timer_req);
        pd_timer = NULL;
    }
    if (pd_timer_port != NULL)
    {
        DeleteMsgPort(pd_timer_port);
        pd_timer_port = NULL;
    }
}

LONG probe_install(const UBYTE *mac, BOOL start_online)
{
    ProbeDevice *d;

    if (pd != NULL)
        return 0;

    pd_timer_port = CreateMsgPort();
    if (pd_timer_port != NULL)
    {
        pd_timer_req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
        pd_timer_req.io_Message.mn_ReplyPort    = pd_timer_port;
        pd_timer_req.io_Message.mn_Length = (UWORD)sizeof(pd_timer_req);

        if (OpenDevice((STRPTR)"timer.device", UNIT_ECLOCK,
                       &pd_timer_req, 0) == 0)
        {
            struct EClockVal ev;

            pd_timer     = pd_timer_req.io_Device;
            pd_eclock_hz = pd_read_eclock(&ev);
        }
    }

    d = (ProbeDevice *)MakeLibrary((APTR)pd_vectors, NULL, NULL,
                                   (ULONG)sizeof(ProbeDevice), (BPTR)0);
    if (d == NULL)
    {
        pd_timer_close();
        return -1;
    }

    d->tx = (ProbeFrame *)AllocMem((ULONG)sizeof(ProbeFrame) * PROBE_TX_SLOTS,
                                   MEMF_PUBLIC | MEMF_CLEAR);
    if (d->tx == NULL)
    {
        FreeMem((APTR)((ULONG)d - d->dd.dd_Library.lib_NegSize),
                (ULONG)(d->dd.dd_Library.lib_NegSize +
                        d->dd.dd_Library.lib_PosSize));
        pd_timer_close();
        return -1;
    }

    d->reads.lh_Head     = (struct Node *)&d->reads.lh_Tail;
    d->reads.lh_Tail     = NULL;
    d->reads.lh_TailPred = (struct Node *)&d->reads.lh_Head;
    d->reads.lh_Type     = NT_MESSAGE;

    d->events.lh_Head     = (struct Node *)&d->events.lh_Tail;
    d->events.lh_Tail     = NULL;
    d->events.lh_TailPred = (struct Node *)&d->events.lh_Head;
    d->events.lh_Type     = NT_MESSAGE;

    pd_bytes(d->mac, mac, ETH_ADDR);
    d->online = start_online;

    d->dd.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    d->dd.dd_Library.lib_Node.ln_Name = (char *)&pd_name[0];
    d->dd.dd_Library.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    d->dd.dd_Library.lib_Version      = 1;
    d->dd.dd_Library.lib_Revision     = 0;
    d->dd.dd_Library.lib_IdString     = (char *)&pd_id[0];

    pd = d;
    AddDevice(&d->dd);

    return 0;
}

VOID probe_remove(VOID)
{
    ProbeDevice *d = pd;
    struct Node *n;

    if (d == NULL)
        return;

    Forbid();
    RemDevice(&d->dd);

    while ((n = RemHead(&d->reads)) != NULL)
    {
        struct IOSana2Req *r = (struct IOSana2Req *)n;

        d->stats.reads_now--;
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
        FreeMem(d->tx, (ULONG)sizeof(ProbeFrame) * PROBE_TX_SLOTS);

    FreeMem((APTR)((ULONG)d - d->dd.dd_Library.lib_NegSize),
            (ULONG)(d->dd.dd_Library.lib_NegSize +
                    d->dd.dd_Library.lib_PosSize));

    pd = NULL;
    pd_timer_close();
}

/*
 * Freeing a device a foreign stack still holds open removes the memory its
 * next BeginIO() jumps into.  The demo Roadshow cannot be unloaded at all, so
 * this refuses rather than guesses and the caller keeps the process alive.
 */
BOOL probe_remove_safe(VOID)
{
    if (pd == NULL)
        return TRUE;
    if (pd->dd.dd_Library.lib_OpenCnt != 0)
        return FALSE;

    probe_remove();
    return TRUE;
}
