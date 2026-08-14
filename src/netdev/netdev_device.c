/*
 * anxnet.device, layer 1 of 3: the AmigaOS device.
 *
 * The romtag lives here, so this file has to be FIRST on the link line: the
 * "moveq #-1,d0 / rts" below must land at offset 0 of the first code hunk,
 * which is what makes the device file harmless if someone runs it.
 *
 * One device, every NE2000/DP8390 board in the machine.  Unit N is the Nth
 * supported board in Zorro autoconfig order, which is slot order and is the
 * same on every boot of the same machine; a unit of 100 or more, or an
 * S2_AnxCardType tag, pins the card type instead, and a pin that does not
 * match refuses the open rather than binding to whatever else was found.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "dp8390.h"

#include "aminetxduo/version.h"

#include <exec/errors.h>
#include <exec/execbase.h>
#include <exec/initializers.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <libraries/configvars.h>
#include <hardware/intbits.h>

#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>

struct ExecBase *SysBase;

/*
 * proto/expansion.h's inlines read this exact global, and a -nostartfiles
 * image has no auto-open to fill it in: without the definition here the link
 * still succeeds against the toolchain's stub, the stub is zero, and
 * FindConfigDev() jumps through a null pointer inside the romtag init -- which
 * on an emulator reads as the whole stack failing to start.
 */
struct ExpansionBase *ExpansionBase;

asm("    .text                   \n"
    "    .globl _netdev_entry    \n"
    "_netdev_entry:              \n"
    "    moveq  #-1,%d0          \n"
    "    rts                     \n");

#define NETDEV_VERSION      1
#define NETDEV_REVISION     0

static char netdev_name[] = ANXNET_DEVICE_NAME;

static const char netdev_ver[] __attribute__((used)) =
    "$VER: " ANXNET_DEVICE_NAME " " AMINETXDUO_VERSION
    " (" AMINETXDUO_VERSION_DATE ") AmiNetXDuo " AMINETXDUO_VERSION_HASH
    AMINETXDUO_VERSION_CPU;

static char netdev_id[] =
    ANXNET_DEVICE_NAME " " AMINETXDUO_VERSION
    " (AmiNetXDuo, NE2000/DP8390 family)\r\n";

static struct Device *netdev_open(
    register struct Device     *dev   __asm("a6"),
    register struct IOSana2Req *io    __asm("a1"),
    register ULONG              unit  __asm("d0"),
    register ULONG              flags __asm("d1"));
static BPTR netdev_close(register struct Device     *dev __asm("a6"),
                         register struct IOSana2Req *io  __asm("a1"));
static BPTR netdev_expunge(register struct Device *dev __asm("a6"));
static ULONG netdev_null(VOID);
static VOID netdev_begin_io(register struct Device     *dev __asm("a6"),
                            register struct IOSana2Req *io  __asm("a1"));
static LONG netdev_abort_io(register struct Device     *dev __asm("a6"),
                            register struct IOSana2Req *io  __asm("a1"));

static NetdevDevice *netdev_init(
    register NetdevDevice   *base    __asm("d0"),
    register BPTR            seglist __asm("a0"),
    register struct ExecBase *sysbase __asm("a6"));

static const APTR netdev_vectors[] =
{
    (APTR)netdev_open,
    (APTR)netdev_close,
    (APTR)netdev_expunge,
    (APTR)netdev_null,
    (APTR)netdev_begin_io,
    (APTR)netdev_abort_io,
    (APTR)-1
};

static const APTR netdev_init_table[4] =
{
    (APTR)sizeof(NetdevDevice),
    (APTR)netdev_vectors,
    (APTR)NULL,
    (APTR)netdev_init
};

const struct Resident netdev_romtag =
{
    RTC_MATCHWORD,
    (struct Resident *)&netdev_romtag,
    (APTR)(&netdev_romtag + 1),
    RTF_AUTOINIT,
    NETDEV_VERSION,
    NT_DEVICE,
    0,
    netdev_name,
    netdev_id,
    (APTR)netdev_init_table
};

/* --------------------------------------------------------------- helpers -- */

#ifdef NETDEV_TRACE
/*
 * Raw serial, straight at the custom chips.  A device's romtag init runs
 * before anything of ours is open, and this is the only channel that needs
 * nothing to be.
 */
static VOID nd_trace(const char *s)
{
    volatile UWORD *serdat  = (volatile UWORD *)0xdff030;
    volatile UWORD *serdatr = (volatile UWORD *)0xdff018;

    while (*s != '\0')
    {
        ULONG guard = 200000;

        while ((*serdatr & 0x2000) == 0 && --guard != 0)
            ;
        *serdat = (UWORD)(0x100 | (UBYTE)*s++);
    }
}

static VOID nd_tracex(const char *tag, ULONG v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[12];
    int  i;

    nd_trace(tag);
    for (i = 0; i < 8; i++)
        buf[i] = hex[(v >> ((7 - i) * 4)) & 0xf];
    buf[8] = '\r';
    buf[9] = '\n';
    buf[10] = '\0';
    nd_trace(buf);
}

/* netdev_cmds.c's way in: nd_trace is static, so the command number of every
   request that reaches the table comes back through here.  Without it
   -DAMINETXDUO_NETDEV_TRACE=ON does not link at all. */
VOID netdev_trace_cmd(UWORD c)
{
    nd_tracex("anx: cmd ", (ULONG)c);
}
#else
#define nd_trace(s)         ((VOID)0)
#define nd_tracex(t, v)     ((VOID)0)
#endif

#ifdef NETDEV_TIME
/*
 * Where the time in an interrupt actually goes, in beam positions.  There is
 * no timer at interrupt level and ReadEClock() needs a base this has no way to
 * hold, but VHPOSR is a free-running counter one chip read away: 227 colour
 * clocks of 280 ns to the line, and the low eight bits of vpos wrap every 256
 * lines, which is 16 ms and far longer than anything measured here.
 */
#define ND_TICKS_WRAP   (256UL * 227UL)

static ULONG nd_now(VOID)
{
    UWORD vh = *(volatile UWORD *)0xdff006;

    return (ULONG)((vh >> 8) & 0xff) * 227UL + (ULONG)(vh & 0xff);
}

static ULONG nd_since(ULONG t0)
{
    ULONG t1 = nd_now();

    return (t1 >= t0) ? (t1 - t0) : (ND_TICKS_WRAP + t1 - t0);
}

static ULONG nd_t_isr;      /* ops->intr(), the whole chip service */
static ULONG nd_t_copy;     /* the ring-to-rxbuf copy inside it */
static ULONG nd_t_up;       /* handing frames to the openers */
static ULONG nd_t_tx;       /* netdev_tx_pump() after the service */
static ULONG nd_t_hook;     /* the stack's CopyToBuff, inside the hand-over */
static ULONG nd_n_int;
static ULONG nd_n_frame;
static ULONG nd_n_hook;

ULONG netdev_time_copy;     /* dp8390.c adds its copy span here */
ULONG netdev_time_rdc;      /* ne2000.c counts its DMA-completion spins */
ULONG netdev_time_null;     /* interrupts where ISR read zero: not ours */
ULONG netdev_time_rx;       /* ISR passes with a receive bit set */
ULONG netdev_time_tx;       /* ISR passes with a transmit bit set */

static VOID nd_time_report(VOID)
{
    nd_t_copy += netdev_time_copy;
    netdev_time_copy = 0;
    nd_tracex("t frames ", nd_n_frame);
    nd_tracex("t ints   ", nd_n_int);
    nd_tracex("t isr    ", nd_t_isr);
    nd_tracex("t copy   ", nd_t_copy);
    nd_tracex("t up     ", nd_t_up);
    nd_tracex("t tx     ", nd_t_tx);
    nd_tracex("t hook   ", nd_t_hook);
    nd_tracex("t nhook  ", nd_n_hook);
    nd_tracex("t rdc    ", netdev_time_rdc);
    nd_tracex("t null   ", netdev_time_null);
    nd_tracex("t irx    ", netdev_time_rx);
    nd_tracex("t itx    ", netdev_time_tx);
    netdev_time_rdc = netdev_time_null = 0;
    netdev_time_rx = netdev_time_tx = 0;
    nd_t_isr = nd_t_copy = nd_t_up = nd_t_tx = nd_t_hook = 0;
    nd_n_int = nd_n_frame = nd_n_hook = 0;
}
#endif

static VOID nd_bytes(UBYTE *to, const UBYTE *from, ULONG n)
{
    while (n-- != 0)
        *to++ = *from++;
}

static VOID nd_zero(UBYTE *p, ULONG n)
{
    while (n-- != 0)
        *p++ = 0;
}

/* NewList() lives in amiga.lib, which a -nostartfiles image does not link. */
static VOID nd_newlist(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

VOID netdev_reply(struct IOSana2Req *io, LONG err, ULONG wire)
{
    io->ios2_Req.io_Error = (BYTE)err;
    io->ios2_WireError    = wire;

    if ((io->ios2_Req.io_Flags & IOF_QUICK) != 0)
        return;

    ReplyMsg(&io->ios2_Req.io_Message);
}

/*
 * The buffer-management hooks are m68k register-convention (a0 = to,
 * a1 = from, d0 = len).  A `register ... __asm()` function-pointer typedef
 * miscompiles here -- GCC loads the pointer into a0 and jumps through it,
 * destroying the first argument -- so the call is written out.  Same reason
 * and same shape as tests/tcpdrill/tapdev.c.
 */
BOOL netdev_copy_call(APTR fn, APTR to, APTR from, ULONG len)
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

    return (BOOL)(res != 0);
}

/*
 * There is deliberately no diagnostic printed from Open().  Exec calls a
 * device's Open vector under Forbid(), and dos.library's Write() can Wait --
 * breaking Forbid inside the one call that must not break it.  A refused open
 * says so through io_Error (IOERR_OPENFAIL for a pin that names no board in
 * this machine, IOERR_UNITBUSY for a unit somebody holds exclusively), and
 * explaining it belongs to the caller, which is a shell command with a
 * console and no Forbid.
 */

/* ------------------------------------------------------- the RX callback -- */

static NetdevTrack *netdev_track_find(NetdevOpener *op, ULONG type)
{
    UWORD i;

    for (i = 0; i < NETDEV_TRACK_MAX; i++)
    {
        if (op->op_Track[i].used && op->op_Track[i].type == type)
            return &op->op_Track[i];
    }

    return NULL;
}

static BOOL netdev_hand_over(NetdevOpener *op, struct IOSana2Req *io,
                             const UBYTE *frame, UWORD len, ULONG type,
                             UBYTE flags)
{
    /* RAW is an opener property in the IC drivers and a per-request flag in
       the SANA-II autodocs.  Both are honoured; the superset is what a caller
       written against either one expects. */
    BOOL         raw     = (BOOL)(op->op_Raw ||
                                  (io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0);
    const UBYTE *payload = raw ? frame : frame + NETDEV_HDR_LEN;
    ULONG        plen    = raw ? len : (ULONG)(len - NETDEV_HDR_LEN);

    nd_bytes(io->ios2_DstAddr, frame, NETDEV_ADDR_LEN);
    nd_bytes(io->ios2_SrcAddr, frame + NETDEV_ADDR_LEN, NETDEV_ADDR_LEN);
    io->ios2_PacketType = type;
    io->ios2_DataLength = plen;
    io->ios2_Req.io_Flags =
        (UBYTE)((io->ios2_Req.io_Flags & ~(SANA2IOF_BCAST | SANA2IOF_MCAST)) |
                flags);

#ifdef NETDEV_TIME
    {
        ULONG th = nd_now();
        BOOL  ok = netdev_copy_call(op->op_CopyTo, io->ios2_Data,
                                    (APTR)payload, plen);

        nd_t_hook += nd_since(th);
        nd_n_hook++;
        if (!ok)
        {
            netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
            return FALSE;
        }
    }
    if (0)
    {
#else
    if (!netdev_copy_call(op->op_CopyTo, io->ios2_Data, (APTR)payload, plen))
    {
#endif
        netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
        return FALSE;
    }

    netdev_reply(io, 0, 0);
    return TRUE;
}

static struct IOSana2Req *netdev_take(struct List *list, ULONG type)
{
    struct Node *n;

    for (n = list->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        struct IOSana2Req *io = (struct IOSana2Req *)n;

        if (type == ~0UL || io->ios2_PacketType == type)
        {
            Remove(n);
            return io;
        }
    }

    return NULL;
}

/*
 * Called from the interrupt server with a whole frame.  Every opener that has
 * a CMD_READ outstanding for this packet type gets a copy; if none does, the
 * frame goes to an S2_READORPHAN, and only then is it dropped.
 */
#ifdef NETDEV_TIME
static VOID netdev_rx_body(APTR arg, const UBYTE *frame, UWORD len);

static VOID netdev_rx(APTR arg, const UBYTE *frame, UWORD len)
{
    ULONG t0 = nd_now();

    netdev_rx_body(arg, frame, len);
    nd_t_up += nd_since(t0);
    nd_n_frame++;
}

static VOID netdev_rx_body(APTR arg, const UBYTE *frame, UWORD len)
#else
static VOID netdev_rx(APTR arg, const UBYTE *frame, UWORD len)
#endif
{
    NetdevUnit  *unit = (NetdevUnit *)arg;
    struct Node *n;
    ULONG        type;
    UBYTE        flags = 0;
    BOOL         taken = FALSE;

    if (len < NETDEV_HDR_LEN)
    {
        unit->nu_Stats.BadData++;
        return;
    }

    type = ((ULONG)frame[12] << 8) | frame[13];

    if (frame[0] == 0xff)
    {
        UWORD i;
        UBYTE all = 0xff;

        for (i = 1; i < NETDEV_ADDR_LEN; i++)
            all &= frame[i];
        if (all == 0xff)
            flags = SANA2IOF_BCAST;
    }
    if (flags == 0 && (frame[0] & 1) != 0)
        flags = SANA2IOF_MCAST;

    unit->nu_Stats.PacketsReceived++;

    for (n = unit->nu_OpenerList.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        NetdevOpener      *op = (NetdevOpener *)n;
        struct IOSana2Req *io;
        NetdevTrack       *tr = netdev_track_find(op, type);

        io = netdev_take(&op->op_Reads, type);
        if (io != NULL)
        {
            BOOL ok = netdev_hand_over(op, io, frame, len, type, flags);

            taken = TRUE;
            if (tr != NULL)
            {
                if (ok)
                {
                    tr->st.PacketsReceived++;
                    tr->st.BytesReceived += len;
                }
                else
                {
                    tr->st.PacketsDropped++;
                }
            }
        }
        else if (tr != NULL)
        {
            tr->st.PacketsDropped++;
        }
    }

    if (taken)
        return;

    for (n = unit->nu_OpenerList.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        NetdevOpener      *op = (NetdevOpener *)n;
        struct IOSana2Req *io = netdev_take(&op->op_Orphans, ~0UL);

        if (io != NULL)
        {
            (VOID)netdev_hand_over(op, io, frame, len, type, flags);
            return;
        }
    }

    unit->nu_Stats.UnknownTypesReceived++;
}

/* ------------------------------------------------------------- transmit --- */

/*
 * Drain the pending CMD_WRITE queue into whatever transmit buffers the chip
 * has free.  Runs both from BeginIO under Disable() and from the interrupt
 * server after a transmit completes, so it never touches anything the ISR
 * does not already own.
 */
VOID netdev_tx_pump(NetdevUnit *unit)
{
    while (unit->nu_Nic.txb_inuse < unit->nu_Nic.txb_cnt)
    {
        struct IOSana2Req *io;
        NetdevOpener      *op;
        UBYTE             *buf = (UBYTE *)unit->nu_TxBuf;
        ULONG              len;
        UWORD              total;
        LONG               rc;
        NetdevTrack       *tr;

        if (IsListEmpty(&unit->nu_Writes))
            return;

        io = (struct IOSana2Req *)RemHead(&unit->nu_Writes);
        op = NETDEV_OPENER(io->ios2_Req.io_Unit);
        len = io->ios2_DataLength;

        if (op->op_Raw || (io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0)
        {
            if (len < NETDEV_HDR_LEN || len > NETDEV_FRAME_MAX)
            {
                netdev_reply(io, S2ERR_MTU_EXCEEDED, S2WERR_GENERIC_ERROR);
                continue;
            }
            if (!netdev_copy_call(op->op_CopyFrom, buf, io->ios2_Data, len))
            {
                netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
                continue;
            }
            total = (UWORD)len;
        }
        else
        {
            /*
             * The RAW arm above bounds its length and this one did not, so a
             * CMD_WRITE with any ios2_DataLength at all copied that many
             * bytes past nu_TxBuf -- past the whole unit, in fact, since the
             * buffer is its last field.  Found reading the transmit path for
             * the ED core, which is the write_buf on the other end of it.
             */
            if (len > NETDEV_MTU)
            {
                netdev_reply(io, S2ERR_MTU_EXCEEDED, S2WERR_GENERIC_ERROR);
                continue;
            }

            nd_bytes(buf, io->ios2_DstAddr, NETDEV_ADDR_LEN);
            nd_bytes(buf + NETDEV_ADDR_LEN, unit->nu_Nic.mac, NETDEV_ADDR_LEN);
            buf[12] = (UBYTE)(io->ios2_PacketType >> 8);
            buf[13] = (UBYTE)(io->ios2_PacketType);

            if (len != 0 &&
                !netdev_copy_call(op->op_CopyFrom, buf + NETDEV_HDR_LEN,
                                  io->ios2_Data, len))
            {
                netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
                continue;
            }
            total = (UWORD)(len + NETDEV_HDR_LEN);
        }

        /*
         * The chip pads a short frame to the Ethernet minimum out of whatever
         * is in the buffer, so the padding has to be written.  Left out, the
         * tail of the last frame -- up to 46 bytes of it -- goes on the wire
         * behind every ARP.
         */
        if (total < NETDEV_FRAME_MIN)
        {
            nd_zero(buf + total, (ULONG)(NETDEV_FRAME_MIN - total));
            total = NETDEV_FRAME_MIN;
        }

        rc = unit->nu_Nic.ops->tx(&unit->nu_Nic, buf, total);
        if (rc == DP8390_TX_BUSY)
        {
            AddHead(&unit->nu_Writes, &io->ios2_Req.io_Message.mn_Node);
            return;
        }
        if (rc != 0)
        {
            netdev_reply(io, S2ERR_TX_FAILURE, S2WERR_GENERIC_ERROR);
            continue;
        }

        unit->nu_Stats.PacketsSent++;
        tr = netdev_track_find(op, io->ios2_PacketType);
        if (tr != NULL)
        {
            tr->st.PacketsSent++;
            tr->st.BytesSent += total;
        }

        netdev_reply(io, 0, 0);
    }
}

/* ------------------------------------------------------------- the filter -- */

VOID netdev_rebuild_filter(NetdevUnit *unit)
{
    UBYTE mar[8];
    UWORD i;

    netdev_mar_clear(mar);

    if (unit->nu_Nic.promisc || unit->nu_AllMulti != 0)
    {
        netdev_mar_all(mar);
    }
    else
    {
        for (i = 0; i < NETDEV_MCAST_MAX; i++)
        {
            if (unit->nu_Mcast[i].refs != 0)
                netdev_mar_set(mar, unit->nu_Mcast[i].addr);
        }
    }

    for (i = 0; i < 8; i++)
        unit->nu_Nic.mar[i] = mar[i];

    Disable();
    unit->nu_Nic.ops->setfilter(&unit->nu_Nic);
    Enable();
}

/* ---------------------------------------------------------------- events -- */

VOID netdev_event(NetdevUnit *unit, ULONG mask)
{
    struct Node *n;

    Disable();
    for (n = unit->nu_OpenerList.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        NetdevOpener *op = (NetdevOpener *)n;
        struct Node  *e  = op->op_Events.lh_Head;

        while (e->ln_Succ != NULL)
        {
            struct IOSana2Req *io   = (struct IOSana2Req *)e;
            struct Node       *next = e->ln_Succ;

            if ((io->ios2_WireError & mask) != 0)
            {
                Remove(e);
                io->ios2_WireError = mask;
                netdev_reply(io, 0, mask);
            }
            e = next;
        }
    }
    Enable();
}

/* ------------------------------------------------------ online / offline -- */

LONG netdev_online(NetdevUnit *unit)
{
    LONG rc;

    Disable();
    rc = unit->nu_Nic.ops->init(&unit->nu_Nic);
    Enable();

    if (rc != 0)
        return rc;

    netdev_rebuild_filter(unit);

    unit->nu_Online = 1;
    unit->nu_Stats.Reconfigurations++;
    netdev_event(unit, S2EVENT_ONLINE);

    Disable();
    netdev_tx_pump(unit);
    Enable();

    return 0;
}

/*
 * Take one opener's CMD_WRITEs off the unit's queue.  Disable(), because the
 * interrupt server walks the same list, and because AddTail() on it is
 * Disable()d too -- that is the only arbitration this driver has.
 */
VOID netdev_drop_writes(NetdevUnit *unit, NetdevOpener *op)
{
    struct Node *n;

    Disable();
    n = unit->nu_Writes.lh_Head;
    while (n->ln_Succ != NULL)
    {
        struct IOSana2Req *io   = (struct IOSana2Req *)n;
        struct Node       *next = n->ln_Succ;

        if (NETDEV_OPENER(io->ios2_Req.io_Unit) == op)
        {
            Remove(n);
            netdev_reply(io, IOERR_ABORTED, 0);
        }
        n = next;
    }
    Enable();
}

/*
 * The unit outlives every opener, so anything an opener changed about it has
 * to be undone when the last one goes.  Across a stack restart the three that
 * matter are: an accept-all-multicast latch that never clears, a multicast
 * table that fills with 32 stale groups and then refuses the new stack's
 * joins with S2ERR_NO_RESOURCES -- taking IPv6 solicited-node frames with it
 * -- and a configured flag that makes the new stack's S2_CONFIGINTERFACE fail
 * so it silently runs on the old stack's address.
 */
static VOID netdev_release_unit(NetdevUnit *unit)
{
    UWORD i;

    for (i = 0; i < NETDEV_MCAST_MAX; i++)
    {
        unit->nu_Mcast[i].refs = 0;
        unit->nu_Mcast[i].addr[0] = 0;
    }
    unit->nu_AllMulti  = 0;
    unit->nu_Promisc   = 0;
    unit->nu_Exclusive = 0;
    unit->nu_Nic.promisc = FALSE;

    unit->nu_Configured = 0;
    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        unit->nu_Nic.mac[i] = unit->nu_Nic.factory[i];

    netdev_mar_clear(unit->nu_Nic.mar);
}

VOID netdev_offline(NetdevUnit *unit, ULONG event)
{
    struct Node *n;

    Disable();
    unit->nu_Nic.ops->stop(&unit->nu_Nic);
    unit->nu_Online = 0;

    /* Everything queued is answered now rather than left to time out. */
    for (n = unit->nu_OpenerList.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        NetdevOpener      *op = (NetdevOpener *)n;
        struct IOSana2Req *io;

        while ((io = netdev_take(&op->op_Reads, ~0UL)) != NULL)
            netdev_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
        while ((io = netdev_take(&op->op_Orphans, ~0UL)) != NULL)
            netdev_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
    }

    while (!IsListEmpty(&unit->nu_Writes))
    {
        struct IOSana2Req *io = (struct IOSana2Req *)RemHead(&unit->nu_Writes);

        netdev_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
    }
    Enable();

    if (event != 0)
        netdev_event(unit, event);
}

/* ------------------------------------------------------ interrupt server -- */

static ULONG netdev_server(register NetdevUnit *unit __asm("a1"))
{
    /*
     * THE CHIP'S OWN ISR IS THE QUESTION, and it is the only one asked.
     *
     * There was a board-level test here first -- X-Surf and X-Surf 100 put an
     * interrupt-status bit at board+0x40 -- on the theory that INT2 is shared
     * and a server should not answer for somebody else's card.  It is not
     * worth what it risks: NetBSD's xsurf.c and xsh.c install dp8390_intr
     * directly with no such test, the offset and bit are the one part of the
     * card table that could not be checked against NetBSD, and getting them
     * wrong on a row nobody has run means the chip's level-triggered INT is
     * never acknowledged.  That is INT2 held down for the whole machine, or a
     * card that simply never receives -- the silent whole-card failure the
     * table exists to prevent.
     *
     * Reading ISR costs two bus cycles and cannot be wrong: dp8390_intr()
     * returns FALSE when it reads zero, which is the same answer the board bit
     * was there to give.
     */
#ifdef NETDEV_TIME
    {
        ULONG t0 = nd_now();
        BOOL  mine;

        mine = unit->nu_Nic.ops->intr(&unit->nu_Nic);
        nd_t_isr += nd_since(t0);
        nd_n_int++;
        if (!mine)
            return 0;

        t0 = nd_now();
        netdev_tx_pump(unit);
        nd_t_tx += nd_since(t0);

        if (nd_n_frame >= 512)
            nd_time_report();

        return 1;
    }
#else
    if (!unit->nu_Nic.ops->intr(&unit->nu_Nic))
        return 0;

    netdev_tx_pump(unit);

    return 1;
#endif
}

/*
 * NetBSD arms ifp->if_timer on every transmit and dp8390_watchdog() resets the
 * chip when it expires.  There is no callout here and no task, so the tick is
 * the vertical blank -- the one clock a device can have for free.
 *
 * WHY IT IS NOT OPTIONAL.  netdev_tx_pump() only runs while
 * txb_inuse < txb_cnt, and only dp8390_init() clears txb_inuse.  A single lost
 * or never-delivered transmit completion therefore wedges the transmitter for
 * good: every later CMD_WRITE queues on nu_Writes and is never replied to, and
 * the caller sits in WaitIO() forever.  Nothing else in the driver can notice,
 * because noticing requires a clock the card does not drive.
 *
 * Disable() rather than a software interrupt: the vertical blank is INT3 and
 * the card is INT2, so this can preempt the card's own server and must not
 * touch the chip alongside it.  The recovery is rare and its cost is a few
 * milliseconds once, against a transmitter that never comes back.
 */
#define NETDEV_TX_STALL_BLANKS  120     /* ~2 s at 50 Hz, ~2 s at 60 Hz */

static ULONG netdev_tick(register NetdevUnit *unit __asm("a1"))
{
    Disable();

    if (!unit->nu_Online || unit->nu_Nic.txb_inuse == 0)
    {
        unit->nu_TxStall = 0;
    }
    else if (++unit->nu_TxStall >= NETDEV_TX_STALL_BLANKS)
    {
        unit->nu_TxStall = 0;
        unit->nu_TxWedges++;
        unit->nu_Nic.tx_errors++;
        dp8390_reset(&unit->nu_Nic);
        netdev_tx_pump(unit);
    }

    Enable();

    return 0;
}

/* ----------------------------------------------------------------- probe -- */

static VOID netdev_probe(NetdevDevice *dev)
{
    struct ConfigDev *cd = NULL;

    if (dev->nd_ExpansionBase == NULL)
        return;

    /*
     * FindConfigDev(NULL, -1, -1) walks the board list in the order Expansion
     * built it, which is autoconfig order, which is slot order.  That is what
     * makes unit N the same board on the next boot.
     *
     * WHAT MOVED WHEN THE ED CORE LANDED.  Hydra and the ASDG LAN Rover used
     * to be recognised and skipped, so on a machine holding one of them plus
     * an NE2000 board the NE2000 board was unit 0.  It is now unit 1 if the
     * ED board autoconfigures first.  The pinned numbers did not move and
     * that was the point of adding the two card rows before the core: a pin
     * is (index + 1) * 100 + instance over netdev_cards[], hydra has been
     * index 3 -> 400 and lanrover index 4 -> 500 since the table was written,
     * so anything that pinned a card by number or by S2_AnxCardType is
     * unaffected.  Only bare positional units shift, and only on a machine
     * that has one of these two boards -- where they previously did not work
     * at all.
     */
    while ((cd = FindConfigDev(cd, -1, -1)) != NULL)
    {
        const NetdevCard *card = NULL;
        NetdevUnit       *unit;
        UWORD             i;

        for (i = 0; i < netdev_card_count; i++)
        {
            if (cd->cd_Rom.er_Manufacturer == netdev_cards[i].manid &&
                cd->cd_Rom.er_Product == (UBYTE)netdev_cards[i].prodid)
            {
                card = &netdev_cards[i];
                break;
            }
        }

        if (card == NULL)
            continue;

        /* A supported board past the table is dropped, and a silent drop is
           the failure this driver exists to stop making: the card is fitted,
           nothing opens it, and nothing says why.  Count it so
           S2_GETSPECIALSTATS can be asked.  AFTER the match, not before: the
           count is boards this driver could have driven, and a RAM board
           walked past once the slots were full is not one of them. */
        if (dev->nd_UnitCount >= NETDEV_MAX_UNITS)
        {
            dev->nd_UnitsDropped++;
            continue;
        }

        unit = &dev->nd_Units[dev->nd_UnitCount];
        nd_zero((UBYTE *)unit, sizeof(*unit));

        unit->nu_Nic.ops = netdev_nic_ops_for(card->chip);
        if (unit->nu_Nic.ops == NULL)
            continue;               /* no core for this chip yet */

        unit->nu_Dev        = dev;
        unit->nu_Nic.card   = card;
        unit->nu_Nic.board  = (volatile UBYTE *)cd->cd_BoardAddr;
        unit->nu_Nic.rx     = netdev_rx;
        unit->nu_Nic.rx_arg = unit;

        netdev_bus_setup(&unit->nu_Nic.bus,
                         (APTR)((UBYTE *)cd->cd_BoardAddr + card->reg_off),
                         card->stride,
                         card->wide_off != 0
                             ? (APTR)((UBYTE *)cd->cd_BoardAddr + card->wide_off)
                             : NULL);

        nd_tracex("anx: board ", (ULONG)cd->cd_BoardAddr);
        if (unit->nu_Nic.ops->attach(&unit->nu_Nic) != 0)
        {
            nd_trace("anx: attach failed\r\n");
            continue;               /* the board did not answer as a DP8390 */
        }
        nd_tracex("anx: mac ", ((ULONG)unit->nu_Nic.factory[2] << 24) |
                               ((ULONG)unit->nu_Nic.factory[3] << 16) |
                               ((ULONG)unit->nu_Nic.factory[4] << 8) |
                               (ULONG)unit->nu_Nic.factory[5]);
        nd_tracex("anx: dmode ", (ULONG)unit->nu_Nic.bus.dmode);

        nd_newlist(&unit->nu_OpenerList);
        nd_newlist(&unit->nu_Writes);
        unit->nu_Unit = dev->nd_UnitCount;

        unit->nu_Intr.is_Node.ln_Type = NT_INTERRUPT;
        unit->nu_Intr.is_Node.ln_Pri  = 10;
        unit->nu_Intr.is_Node.ln_Name = netdev_name;
        unit->nu_Intr.is_Data         = unit;
        unit->nu_Intr.is_Code         = (VOID (*)())netdev_server;

        unit->nu_Tick.is_Node.ln_Type = NT_INTERRUPT;
        unit->nu_Tick.is_Node.ln_Pri  = 0;
        unit->nu_Tick.is_Node.ln_Name = netdev_name;
        unit->nu_Tick.is_Data         = unit;
        unit->nu_Tick.is_Code         = (VOID (*)())netdev_tick;

        dev->nd_UnitCount++;
    }
}

/* ------------------------------------------------------------ unit lookup - */

/*
 * A unit below ANXNET_UNIT_PIN is a position in the probe order.  At or above
 * it, the number names a card type: (index + 1) * 100 + instance.
 */
static NetdevUnit *netdev_find_unit(NetdevDevice *dev, ULONG unit,
                                    const char *pin_name, const char **why)
{
    const NetdevCard *want = NULL;
    UWORD             instance = 0;
    UWORD             seen = 0;
    UWORD             i;

    if (pin_name != NULL)
    {
        want = netdev_card_by_name(pin_name);
        if (want == NULL)
        {
            *why = "no such card type";
            return NULL;
        }
        instance = (UWORD)unit;
        if (unit >= ANXNET_UNIT_PIN)
            instance = (UWORD)(unit % ANXNET_UNIT_PIN);
    }
    else if (unit >= ANXNET_UNIT_PIN)
    {
        ULONG idx = (unit / ANXNET_UNIT_PIN) - 1;

        /* Range-checked as a ULONG: truncating first wraps a huge unit
           number onto a valid card index, which is the one thing a pin
           must never do. */
        if (idx >= (ULONG)netdev_card_count)
        {
            *why = "no such card type";
            return NULL;
        }
        want     = &netdev_cards[idx];
        instance = (UWORD)(unit % ANXNET_UNIT_PIN);
    }
    else
    {
        if (unit >= dev->nd_UnitCount)
        {
            *why = "no such board";
            return NULL;
        }
        return &dev->nd_Units[unit];
    }

    for (i = 0; i < dev->nd_UnitCount; i++)
    {
        if (dev->nd_Units[i].nu_Nic.card == want)
        {
            if (seen == instance)
                return &dev->nd_Units[i];
            seen++;
        }
    }

    *why = "the pinned card is not in this machine";
    return NULL;
}

/* ---------------------------------------------------------- the tag list -- */

static VOID netdev_take_tags(const struct TagItem *tags, NetdevOpener *op,
                             const char **pin)
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
            op->op_CopyTo = (APTR)tags->ti_Data;
        else if (tag == S2_CopyFromBuff)
            op->op_CopyFrom = (APTR)tags->ti_Data;
        else if (tag == S2_PacketFilter)
            op->op_Filter = (APTR)tags->ti_Data;
        else if (tag == S2_AnxCardType)
            *pin = (const char *)tags->ti_Data;

        tags++;
    }
}

/* ------------------------------------------------------------ device fns -- */

static NetdevDevice *netdev_init(
    register NetdevDevice    *base    __asm("d0"),
    register BPTR             seglist __asm("a0"),
    register struct ExecBase *sysbase __asm("a6"))
{
    SysBase = sysbase;

    base->nd_SegList   = seglist;
    base->nd_UnitCount = 0;

    base->nd_Device.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    base->nd_Device.dd_Library.lib_Node.ln_Name = netdev_name;
    base->nd_Device.dd_Library.lib_Flags    = LIBF_SUMUSED | LIBF_CHANGED;
    base->nd_Device.dd_Library.lib_Version  = NETDEV_VERSION;
    base->nd_Device.dd_Library.lib_Revision = NETDEV_REVISION;
    base->nd_Device.dd_Library.lib_IdString = (APTR)netdev_id;

    nd_trace("anx: init\r\n");
    base->nd_ExpansionBase = OpenLibrary((CONST_STRPTR)"expansion.library", 36);
    ExpansionBase = (struct ExpansionBase *)base->nd_ExpansionBase;
    nd_tracex("anx: expansion ", (ULONG)base->nd_ExpansionBase);
    netdev_probe(base);
    nd_tracex("anx: units ", (ULONG)base->nd_UnitCount);

    return base;
}

static struct Device *netdev_open(
    register struct Device     *dev   __asm("a6"),
    register struct IOSana2Req *io    __asm("a1"),
    register ULONG              unit  __asm("d0"),
    register ULONG              flags __asm("d1"))
{
    NetdevDevice *d    = (NetdevDevice *)dev;
    NetdevOpener *op;
    NetdevUnit   *hw;
    const char   *pin  = NULL;
    const char   *why  = "no such board";
    BOOL          first_opener;
    BOOL          first_promisc;

    io->ios2_Req.io_Error = 0;

    op = AllocMem(sizeof(NetdevOpener), MEMF_PUBLIC | MEMF_CLEAR);
    if (op == NULL)
    {
        /* A failed Open() poisons both fields, as the standard device
           skeleton does: a caller that uses the request anyway then faults
           on -1 instead of on whatever OpenDevice() left there. */
        io->ios2_Req.io_Device = (struct Device *)-1;
        io->ios2_Req.io_Unit   = (struct Unit *)-1;
        io->ios2_Req.io_Error  = IOERR_OPENFAIL;
        return NULL;
    }

    netdev_take_tags((const struct TagItem *)io->ios2_BufferManagement,
                     op, &pin);

    nd_tracex("anx: open unit ", unit);
    hw = netdev_find_unit(d, unit, pin, &why);
    if (hw == NULL)
    {
        (VOID)why;
        FreeMem(op, sizeof(NetdevOpener));
        io->ios2_Req.io_Device = (struct Device *)-1;
        io->ios2_Req.io_Unit   = (struct Unit *)-1;
        io->ios2_Req.io_Error  = IOERR_OPENFAIL;
        return NULL;
    }

    op->op_Hw        = hw;
    op->op_Raw       = (UBYTE)((io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0);
    op->op_Promisc   = (UBYTE)((flags & SANA2OPF_PROM) != 0);
    op->op_Exclusive = (UBYTE)((flags & SANA2OPF_MINE) != 0);
    op->op_Unit.unit_flags = 0;
    nd_newlist(&op->op_Reads);
    nd_newlist(&op->op_Orphans);
    nd_newlist(&op->op_Events);

    /*
     * SANA2OPF_MINE is exclusive access, and a driver that takes the flag and
     * then shares the unit anyway is worse than one that refuses it.
     *
     * Tested, claimed and joined under ONE Disable().  Split, two opens racing
     * both read nu_Openers == 0, both decide they may have the unit
     * exclusively, and both get it.  Rare -- opens come from processes -- and
     * exactly the kind of rare that is never reproduced afterwards.
     */
    Disable();
    if (hw->nu_Exclusive ||
        (op->op_Exclusive && hw->nu_Openers != 0))
    {
        Enable();
        FreeMem(op, sizeof(NetdevOpener));
        io->ios2_Req.io_Device = (struct Device *)-1;
        io->ios2_Req.io_Unit   = (struct Unit *)-1;
        io->ios2_Req.io_Error  = IOERR_UNITBUSY;
        return NULL;
    }
    if (op->op_Exclusive)
        hw->nu_Exclusive = 1;
    AddTail(&hw->nu_OpenerList, (struct Node *)&op->op_Node);
    /* Counted here and not after Enable(): the test above reads it, so an
       increment outside the bracket is the same race one line further down. */
    first_opener  = (BOOL)(hw->nu_Openers++ == 0);
    first_promisc = (BOOL)(op->op_Promisc && hw->nu_Promisc++ == 0);
    Enable();

    /* Both of these talk to the chip or to Exec and must not run Disable()d. */
    if (first_promisc)
    {
        hw->nu_Nic.promisc = TRUE;
        if (hw->nu_Online)
            netdev_rebuild_filter(hw);
    }

    if (first_opener && !hw->nu_IntrAdded)
    {
        AddIntServer(INTB_PORTS, &hw->nu_Intr);
        AddIntServer(INTB_VERTB, &hw->nu_Tick);
        hw->nu_IntrAdded = 1;
    }

    nd_trace("anx: open ok\r\n");
    io->ios2_Req.io_Device = dev;
    io->ios2_Req.io_Unit   = &op->op_Unit;
    io->ios2_Req.io_Error  = 0;

    dev->dd_Library.lib_OpenCnt++;
    dev->dd_Library.lib_Flags &= (UBYTE)~LIBF_DELEXP;

    return dev;
}

static BPTR netdev_close(register struct Device     *dev __asm("a6"),
                         register struct IOSana2Req *io  __asm("a1"))
{
    NetdevOpener *op = (io->ios2_Req.io_Unit != NULL &&
                        io->ios2_Req.io_Unit != (struct Unit *)-1)
                       ? NETDEV_OPENER(io->ios2_Req.io_Unit) : NULL;
    NetdevUnit   *hw;
    BPTR          seg = (BPTR)0;

    io->ios2_Req.io_Device = (struct Device *)-1;
    io->ios2_Req.io_Unit   = (struct Unit *)-1;

    if (op != NULL)
    {
        struct IOSana2Req *q;

        hw = op->op_Hw;

        Disable();
        Remove((struct Node *)&op->op_Node);
        Enable();

        while ((q = netdev_take(&op->op_Reads, ~0UL)) != NULL)
            netdev_reply(q, IOERR_ABORTED, 0);
        while ((q = netdev_take(&op->op_Orphans, ~0UL)) != NULL)
            netdev_reply(q, IOERR_ABORTED, 0);
        while ((q = netdev_take(&op->op_Events, ~0UL)) != NULL)
            netdev_reply(q, IOERR_ABORTED, 0);

        /*
         * And this opener's writes, which live on the UNIT and not on the
         * opener.  Left behind, the next transmit interrupt reads op_Raw and
         * op_CopyFrom out of the memory freed below, at interrupt level, and
         * the request is never replied to either.
         */
        netdev_drop_writes(hw, op);

        if (op->op_Exclusive)
            hw->nu_Exclusive = 0;

        if (op->op_Promisc && hw->nu_Promisc != 0 && --hw->nu_Promisc == 0)
        {
            hw->nu_Nic.promisc = FALSE;
            if (hw->nu_Online)
                netdev_rebuild_filter(hw);
        }

        if (hw->nu_Openers != 0 && --hw->nu_Openers == 0)
        {
            if (hw->nu_Online)
                netdev_offline(hw, S2EVENT_OFFLINE);
            netdev_release_unit(hw);
            if (hw->nu_IntrAdded)
            {
                RemIntServer(INTB_PORTS, &hw->nu_Intr);
                RemIntServer(INTB_VERTB, &hw->nu_Tick);
                hw->nu_IntrAdded = 0;
            }
        }

        FreeMem(op, sizeof(NetdevOpener));
    }

    if (dev->dd_Library.lib_OpenCnt != 0)
        dev->dd_Library.lib_OpenCnt--;

    if (dev->dd_Library.lib_OpenCnt == 0 &&
        (dev->dd_Library.lib_Flags & LIBF_DELEXP) != 0)
        seg = netdev_expunge(dev);

    return seg;
}

static BPTR netdev_expunge(register struct Device *dev __asm("a6"))
{
    NetdevDevice *d = (NetdevDevice *)dev;
    BPTR          seg;
    UWORD         i;

    if (dev->dd_Library.lib_OpenCnt != 0)
    {
        dev->dd_Library.lib_Flags |= LIBF_DELEXP;
        return (BPTR)0;
    }

    for (i = 0; i < d->nd_UnitCount; i++)
    {
        if (d->nd_Units[i].nu_IntrAdded)
        {
            RemIntServer(INTB_PORTS, &d->nd_Units[i].nu_Intr);
            RemIntServer(INTB_VERTB, &d->nd_Units[i].nu_Tick);
            d->nd_Units[i].nu_IntrAdded = 0;
        }
        Disable();
        d->nd_Units[i].nu_Nic.ops->stop(&d->nd_Units[i].nu_Nic);
        Enable();
    }

    if (d->nd_ExpansionBase != NULL)
    {
        CloseLibrary(d->nd_ExpansionBase);
        d->nd_ExpansionBase = NULL;
        ExpansionBase       = NULL;
    }

    seg = d->nd_SegList;

    Remove(&dev->dd_Library.lib_Node);
    FreeMem((UBYTE *)dev - dev->dd_Library.lib_NegSize,
            dev->dd_Library.lib_NegSize + dev->dd_Library.lib_PosSize);

    return seg;
}

static ULONG netdev_null(VOID)
{
    return 0;
}

static VOID netdev_begin_io(register struct Device     *dev __asm("a6"),
                            register struct IOSana2Req *io  __asm("a1"))
{
    NetdevOpener *op = (io->ios2_Req.io_Unit != NULL &&
                        io->ios2_Req.io_Unit != (struct Unit *)-1)
                       ? NETDEV_OPENER(io->ios2_Req.io_Unit) : NULL;

    (VOID)dev;

    io->ios2_Req.io_Error = 0;
    io->ios2_WireError    = 0;

    netdev_perform(op, io);
}

static LONG netdev_abort_io(register struct Device     *dev __asm("a6"),
                            register struct IOSana2Req *io  __asm("a1"))
{
    NetdevOpener *op = (io->ios2_Req.io_Unit != NULL &&
                        io->ios2_Req.io_Unit != (struct Unit *)-1)
                       ? NETDEV_OPENER(io->ios2_Req.io_Unit) : NULL;

    (VOID)dev;

    return netdev_abort(op, io) ? 0 : -1;
}
