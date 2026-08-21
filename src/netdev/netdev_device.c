/*
 * anxnet.device, layer 1 of 3: the AmigaOS device.
 *
 * The romtag lives here, so this file must come first on the link line.  The
 * "moveq #-1,d0 / rts" below must land at offset 0 of the first code hunk,
 * which makes the device file harmless when it is run as a program.
 *
 * One device, every NE2000/DP8390 board in the machine.  Unit N is the Nth
 * supported board in Zorro autoconfig order, which is slot order and is the
 * same on every boot of the same machine.  A unit of 100 or more, or an
 * S2_AnxCardType tag, pins the card type instead.  A pin that does not match
 * refuses the open rather than binding to whatever else was found.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "netdev_watchdog.h"
#include "netdev_macgen.h"
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
#include <utility/hooks.h>      /* S2_PacketFilter is a standard Hook */

#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>

struct ExecBase *SysBase;

/*
 * proto/expansion.h's inlines read this exact global, and a -nostartfiles
 * image has no auto-open to fill it in.  Without the definition here the link
 * still succeeds against the toolchain's stub, the stub is zero, and
 * FindConfigDev() jumps through a null pointer inside the romtag init.  On an
 * emulator that reads as the whole stack failing to start.
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
 * before anything of ours is open, and this is the only channel that needs no
 * library open.
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

/* nd_trace is static, so netdev_cmds.c reports the command number of every
   request through here.  Without it -DAMINETXDUO_NETDEV_TRACE=ON does not
   link. */
VOID netdev_trace_cmd(UWORD c)
{
    nd_tracex("anx: cmd ", (ULONG)c);
}

/* For the chip cores, which cannot see nd_tracex. */
VOID netdev_trace_val(const char *tag, ULONG v)
{
    nd_tracex(tag, v);
}
#else
#define nd_trace(s)         ((VOID)0)
#define nd_tracex(t, v)     ((VOID)0)
#endif

#ifdef NETDEV_TIME
/*
 * Where the time in an interrupt goes, in beam positions.  There is no timer
 * at interrupt level, and ReadEClock() needs a base this code cannot hold.
 * VHPOSR is a free-running counter one chip read away: 227 colour clocks of
 * 280 ns to the line.  The low eight bits of vpos wrap every 256 lines, which
 * is 16 ms and far longer than anything measured here.
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
static ULONG nd_t_pre;      /* type, group test, stats, before the walk      */
static ULONG nd_t_take;     /* netdev_take: the pending-read list walk       */
static ULONG nd_t_find;     /* netdev_track_find: the 16-entry scan          */
static ULONG nd_t_addr;     /* the two addresses and the request fields      */
static ULONG nd_t_reply;    /* netdev_reply: ReplyMsg at interrupt level     */
static ULONG nd_t_probe;    /* what 16 back-to-back probes cost, to subtract */
static ULONG nd_t_bld;      /* the opener's CopyFrom, framing a transmit     */
static ULONG nd_t_iss;      /* ops->tx: register setup and the port writes   */
static ULONG nd_t_rep;      /* netdev_reply on the transmit side             */
static ULONG nd_n_tx;
static ULONG nd_regs_isr;   /* scalar accesses inside the chip service */
static ULONG nd_regs_tx;    /* scalar accesses inside ops->tx          */
static ULONG nd_n_int;
static ULONG nd_n_frame;
static ULONG nd_n_hook;

ULONG netdev_time_copy;     /* dp8390.c adds its copy span here */
ULONG netdev_time_rdc;      /* ne2000.c counts its DMA-completion spins */
ULONG netdev_time_regs;     /* netdev_bus.h counts every scalar access    */
ULONG netdev_time_null;     /* interrupts where ISR read zero: not ours */
ULONG netdev_time_rx;       /* ISR passes with a receive bit set */
ULONG netdev_time_tx;       /* ISR passes with a transmit bit set */

static VOID nd_time_report(VOID)
{
    nd_t_copy += netdev_time_copy;
    netdev_time_copy = 0;
    nd_tracex("t frames ", nd_n_frame);
    nd_tracex("t up     ", nd_t_up);
    {
        ULONG tp = nd_now();
        UWORD k;

        for (k = 0; k < 16; k++)
            (VOID)nd_now();
        nd_t_probe = nd_since(tp);
    }
    nd_tracex("t hook   ", nd_t_hook);
    nd_tracex("t regs   ", netdev_time_regs);
    nd_tracex("t rgisr  ", nd_regs_isr);
    nd_tracex("t rgtx   ", nd_regs_tx);
    netdev_time_regs = nd_regs_isr = nd_regs_tx = 0;
    nd_tracex("t isr    ", nd_t_isr);
    nd_tracex("t copy   ", nd_t_copy);
    nd_tracex("t ntx    ", nd_n_tx);
    nd_tracex("t bld    ", nd_t_bld);
    nd_tracex("t iss    ", nd_t_iss);
    nd_tracex("t rep    ", nd_t_rep);
    nd_tracex("t probe16", nd_t_probe);
    netdev_time_rdc = netdev_time_null = 0;
    netdev_time_rx = netdev_time_tx = 0;
    nd_t_isr = nd_t_copy = nd_t_up = nd_t_tx = nd_t_hook = 0;
    nd_t_pre = nd_t_take = nd_t_find = nd_t_addr = nd_t_reply = 0;
    nd_t_bld = nd_t_iss = nd_t_rep = nd_n_tx = 0;
    nd_n_int = nd_n_frame = nd_n_hook = 0;
}
#endif

/*
 * A 6-byte Ethernet address, as a longword and a word.  Both ends are even:
 * the staging buffer is AllocMem'd, and ios2_DstAddr/ios2_SrcAddr sit at even
 * offsets in the request.  The source's second half is 2 mod 4, which costs a
 * 68020 one extra bus cycle and a 68000 nothing.  The byte loop this replaced
 * compiled to `move.b (a2)+,(a6)+ / cmpa.l / bne`, three instructions a byte,
 * twice per frame.  The profile priced the pair at 16% of the hand-over.
 */
static VOID nd_addr6(UBYTE *to, const UBYTE *from)
{
    *(ULONG *)(APTR)to        = *(const ULONG *)(const APTR)from;
    *(UWORD *)(APTR)(to + 4)  = *(const UWORD *)(const APTR)(from + 4);
}

static VOID nd_zero(UBYTE *p, ULONG n)
{
    /* The transmit pad is the hot caller and always lands on an even offset
       of nu_TxBuf.  Everything else here is init-time and does not care. */
    if ((((unsigned long)(APTR)p) & 1u) == 0)
    {
        while (n >= 2)
        {
            *(UWORD *)(APTR)p = 0;
            p += 2;
            n -= 2;
        }
    }
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
 * miscompiles here: GCC loads the pointer into a0 and jumps through it, which
 * destroys the first argument.  The call is written out instead, with the same
 * shape as tests/tcpdrill/tapdev.c.
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
 * A standard utility.library Hook, which S2_PacketFilter is and the two
 * buffer-management tags are not: a0 = the hook, a2 = the object, a1 = the
 * message, result in d0.  Written out for the same reason netdev_copy_call()
 * is: three address registers pinned by a convention no function-pointer
 * typedef can express.  h_Entry, not h_SubEntry, because the entry point is
 * the one the caller registered.
 */
BOOL netdev_hook_call(APTR hook, APTR object, APTR message)
{
    register APTR _a3 __asm("a3");
    register APTR _a0 __asm("a0") = hook;
    register APTR _a2 __asm("a2") = object;
    register APTR _a1 __asm("a1") = message;
    register LONG _d1 __asm("d1");
    register LONG res __asm("d0");

    if (hook == NULL)
        return TRUE;

    _a3 = (APTR)((struct Hook *)hook)->h_Entry;

    __asm __volatile ("jsr a3@"
                      : "=r" (res), "=r" (_a0), "=r" (_a1), "=r" (_a2),
                        "=r" (_d1)
                      : "r" (_a3), "1" (_a0), "2" (_a1), "3" (_a2)
                      : "cc", "memory");

    return (BOOL)(res != 0);
}

/*
 * There is deliberately no diagnostic printed from Open().  Exec calls a
 * device's Open vector under Forbid(), and dos.library's Write() can Wait,
 * which breaks Forbid inside the one call that must not break it.  A refused
 * open reports through io_Error: IOERR_OPENFAIL for a pin that names no board
 * in this machine, IOERR_UNITBUSY for a unit held exclusively.  The caller
 * explains it, because the caller is a shell command with a console and no
 * Forbid.
 */

/* ------------------------------------------------------- the RX callback -- */

/*
 * Bounded by the highest slot ever taken, not by the array.  The profile put
 * this at 26% of the hand-over, because it scanned all sixteen entries for
 * every opener on every frame, and usually nothing is tracked.  An opener that
 * tracks two types now scans two.
 */
static NetdevTrack *netdev_track_find(NetdevOpener *op, ULONG type)
{
    UWORD i;

    for (i = 0; i < op->op_TrackHigh; i++)
    {
        if (op->op_Track[i].used && op->op_Track[i].type == type)
            return &op->op_Track[i];
    }

    return NULL;
}

/*
 * The order is fixed.  The request is filled in first, then the filter hook is
 * asked, and only then is the packet copied out:
 *
 *   "The IOSana2Req structure should be set up to look (almost) exactly as it
 *    would if it was successfully returned for the current packet."
 *                                             copybuff.spec, PacketFilter
 *   "a pointer to a standard Hook to be called before S2_CopyToBuff is done."
 *                                             SANA-II standard.txt
 *
 * A rejected packet is not an error, and the request is not completed.  The
 * caller gets back its CMD_READ still queued, as Commodore's own slip.device
 * does it (main.c, the receive loop: on a FALSE return the IOSana2Req goes
 * back on the queue with AddHead).  The frame then goes to the next opener,
 * and to S2_READORPHAN if nobody else wanted it.
 *
 * RAW is an opener property in the IC drivers and a per-request flag in the
 * SANA-II autodocs.  Both are honoured, because a caller written against
 * either one expects the superset.
 */
static NetdevRxResult netdev_hand_over(NetdevOpener *op, struct IOSana2Req *io,
                                       const UBYTE *frame, UWORD len,
                                       ULONG type, UBYTE flags)
{
    ULONG        plen;
    const UBYTE *payload = netdev_payload(op, io, frame, len, &plen);

#ifdef NETDEV_TIME
    {
        ULONG ta = nd_now();
#endif
    nd_addr6(io->ios2_DstAddr, frame);
    nd_addr6(io->ios2_SrcAddr, frame + NETDEV_ADDR_LEN);
    io->ios2_PacketType = type;
    io->ios2_DataLength = plen;
    io->ios2_Req.io_Flags =
        (UBYTE)((io->ios2_Req.io_Flags & ~(SANA2IOF_BCAST | SANA2IOF_MCAST)) |
                flags);
#ifdef NETDEV_TIME
        nd_t_addr += nd_since(ta);
    }
#endif

    if (!netdev_filter_ok(op, io, payload))
        return NETDEV_RX_REJECTED;

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
            return NETDEV_RX_FAILED;
        }
    }
    if (0)
    {
#else
    if (!netdev_copy_call(op->op_CopyTo, io->ios2_Data, (APTR)payload, plen))
    {
#endif
        netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
        return NETDEV_RX_FAILED;
    }

#ifdef NETDEV_TIME
    {
        ULONG tr = nd_now();

        netdev_reply(io, 0, 0);
        nd_t_reply += nd_since(tr);
    }
#else
    netdev_reply(io, 0, 0);
#endif
    return NETDEV_RX_TAKEN;
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
 * a CMD_READ outstanding for this packet type gets a copy.  If none does, the
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
    /*
     * What to post once the frame is disposed of, rather than per opener: one
     * walk of the event lists at most, and none when the frame was delivered.
     * netdev_event() returns on a word test when nobody has an S2_ONEVENT
     * queued.
     */
    ULONG        events = 0;

    if (len < NETDEV_HDR_LEN)
    {
        /* Garbage on the wire.  slip.device's ReceivedGarbage() posts exactly
           this, BadData++ and S2EVENT_ERROR|S2EVENT_RX. */
        unit->nu_Stats.BadData++;
        netdev_event(unit, S2EVENT_ERROR | S2EVENT_RX);
        return;
    }

#ifdef NETDEV_TIME
    {
        ULONG tp = nd_now();
#endif
    /* The frame is even-aligned, so the type is one word and the broadcast
       test is one longword and one word rather than six byte reads. */
    type = *(const UWORD *)(const APTR)(frame + 12);

    if ((frame[0] & 1) != 0)
    {
        flags = (UBYTE)((*(const ULONG *)(const APTR)frame == 0xffffffffUL &&
                         *(const UWORD *)(const APTR)(frame + 4) == 0xffffu)
                        ? SANA2IOF_BCAST : SANA2IOF_MCAST);
    }

    unit->nu_Stats.PacketsReceived++;
#ifdef NETDEV_TIME
        nd_t_pre += nd_since(tp);
    }
#endif

    for (n = unit->nu_OpenerList.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        NetdevOpener      *op = (NetdevOpener *)n;
        struct IOSana2Req *io;
        NetdevTrack       *tr;
#ifdef NETDEV_TIME
        ULONG tf = nd_now();

        tr = netdev_track_find(op, type);
        nd_t_find += nd_since(tf);
        tf = nd_now();
        io = netdev_take(&op->op_Reads, type);
        nd_t_take += nd_since(tf);
#else
        tr = netdev_track_find(op, type);
        io = netdev_take(&op->op_Reads, type);
#endif
        if (io != NULL)
        {
            NetdevRxResult r = netdev_hand_over(op, io, frame, len, type,
                                                flags);

            if (r == NETDEV_RX_REJECTED)
            {
                /*
                 * The opener's filter hook said no.  Its CMD_READ was taken
                 * off the queue to be filled in, so it goes back at the head,
                 * where the opener does not lose its place.  The refusal is
                 * not counted as a drop, because the opener asked for it.
                 */
                AddHead(&op->op_Reads, &io->ios2_Req.io_Message.mn_Node);
            }
            else
            {
                taken = TRUE;
                if (tr != NULL)
                {
                    if (r == NETDEV_RX_TAKEN)
                    {
                        tr->st.PacketsReceived++;
                        tr->st.BytesReceived += len;
                    }
                    else
                    {
                        tr->st.PacketsDropped++;
                    }
                }
                /* The spec's own example of the qualifier rule: an error out
                   of a buffer management function during receive processing
                   is S2EVENT_ERROR, S2EVENT_RX and S2EVENT_BUFF together. */
                if (r == NETDEV_RX_FAILED)
                    events |= S2EVENT_ERROR | S2EVENT_RX | S2EVENT_BUFF;
            }
        }
        else if (tr != NULL)
        {
            tr->st.PacketsDropped++;
        }
    }

    if (!taken)
    {
        for (n = unit->nu_OpenerList.lh_Head; n->ln_Succ != NULL;
             n = n->ln_Succ)
        {
            NetdevOpener      *op = (NetdevOpener *)n;
            struct IOSana2Req *io = netdev_take(&op->op_Orphans, ~0UL);

            if (io != NULL)
            {
                NetdevRxResult r = netdev_hand_over(op, io, frame, len, type,
                                                    flags);

                if (r == NETDEV_RX_REJECTED)
                {
                    AddHead(&op->op_Orphans,
                            &io->ios2_Req.io_Message.mn_Node);
                    continue;   /* try the next opener's orphan reader */
                }
                if (r == NETDEV_RX_FAILED)
                    events |= S2EVENT_ERROR | S2EVENT_RX | S2EVENT_BUFF;
                taken = TRUE;
                break;
            }
        }
    }

    if (!taken)
    {
        /* Nobody wanted it.  slip.device's PacketDropped() and cnet.device's
           readpacket both post S2EVENT_ERROR|S2EVENT_RX here. */
        unit->nu_Stats.UnknownTypesReceived++;
        events |= S2EVENT_ERROR | S2EVENT_RX;
    }

    if (events != 0)
        netdev_event(unit, events);
}

/* ------------------------------------------------------------- transmit --- */

/*
 * Drain the pending CMD_WRITE queue into whatever transmit buffers the chip
 * has free.  Runs both from BeginIO under Disable() and from the interrupt
 * server after a transmit completes, so it never touches anything the ISR
 * does not already own.
 */
/*
 * Frame one CMD_WRITE into nu_TxBuf.  Returns the wire length, or 0 with the
 * request already answered.  No mask needed: everything here is host work,
 * and the opener's CopyFrom is 135 us of the 219 us a transmit spends in the
 * driver on an A3000.
 */
static UWORD netdev_tx_build(NetdevUnit *unit, struct IOSana2Req *io,
                             NetdevOpener *op)
{
    UBYTE *buf;
    ULONG  len = io->ios2_DataLength;
    UWORD  total;

    /* A core whose transmit buffer the CPU can address takes the frame
       directly.  Everything else is framed in the unit's own staging buffer
       and copied across by ops->tx. */
    buf = (unit->nu_Nic.tx_at != NULL) ? unit->nu_Nic.tx_at(&unit->nu_Nic)
                                       : NULL;
    if (buf == NULL)
        buf = (UBYTE *)unit->nu_TxBuf;
    unit->nu_TxAt = buf;

    if (op->op_Raw || (io->ios2_Req.io_Flags & SANA2IOF_RAW) != 0)
    {
        if (len < NETDEV_HDR_LEN || len > NETDEV_FRAME_MAX)
        {
            netdev_reply(io, S2ERR_MTU_EXCEEDED, S2WERR_GENERIC_ERROR);
            netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX);
            return 0;
        }
        if (!netdev_copy_call(op->op_CopyFrom, buf, io->ios2_Data, len))
        {
            netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
            netdev_event(unit,
                         S2EVENT_ERROR | S2EVENT_TX | S2EVENT_BUFF);
            return 0;
        }
        total = (UWORD)len;
    }
    else
    {
        /*
         * The RAW arm above bounds its length and this one did not, so a
         * CMD_WRITE with any ios2_DataLength copied that many bytes past
         * nu_TxBuf, and past the whole unit, because the buffer is its last
         * field.
         */
        if (len > NETDEV_MTU)
        {
            netdev_reply(io, S2ERR_MTU_EXCEEDED, S2WERR_GENERIC_ERROR);
            /* slip.device posts S2EVENT_TX for exactly this refusal and
               cnet.device posts it from its .toobig arm. */
            netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX);
            return 0;
        }

        /* Aligned moves, as on the receive side: nu_TxBuf is ULONG[] and both
           addresses sit at even offsets. */
        nd_addr6(buf, io->ios2_DstAddr);
        nd_addr6(buf + NETDEV_ADDR_LEN, unit->nu_Nic.mac);
        *(UWORD *)(APTR)(buf + 12) = (UWORD)io->ios2_PacketType;

        if (len != 0 &&
            !netdev_copy_call(op->op_CopyFrom, buf + NETDEV_HDR_LEN,
                              io->ios2_Data, len))
        {
            netdev_reply(io, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
            netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX | S2EVENT_BUFF);
            return 0;
        }
        total = (UWORD)(len + NETDEV_HDR_LEN);
    }

    /*
     * The chip pads a short frame to the Ethernet minimum out of whatever is
     * in the buffer, so the padding must be written.  Left out, the tail of
     * the last frame, up to 46 bytes of it, goes on the wire behind every ARP.
     */
    if (total < NETDEV_FRAME_MIN)
    {
        nd_zero(buf + total, (ULONG)(NETDEV_FRAME_MIN - total));
        total = NETDEV_FRAME_MIN;
    }

    return total;
}

/* The chip half, and the only half that needs the mask. */
static LONG netdev_tx_issue(NetdevUnit *unit, struct IOSana2Req *io,
                            NetdevOpener *op, UWORD total)
{
    NetdevTrack *tr;
    LONG         rc;

    rc = unit->nu_Nic.ops->tx(&unit->nu_Nic, unit->nu_TxAt, total);
    if (rc != 0)
        return rc;

    unit->nu_Stats.PacketsSent++;
    tr = netdev_track_find(op, io->ios2_PacketType);
    if (tr != NULL)
    {
        tr->st.PacketsSent++;
        tr->st.BytesSent += total;
    }

    return 0;
}

/*
 * The timing build's way in.  Wrappers rather than spans inside the two
 * functions, because the transmit rewrite already lost one set of spans that
 * were inlined into a body.  Wrappers cannot be dropped by editing the body.
 */
#ifdef NETDEV_TIME
static UWORD netdev_tx_timed_build(NetdevUnit *unit, struct IOSana2Req *io,
                                   NetdevOpener *op)
{
    ULONG t = nd_now();
    UWORD n = netdev_tx_build(unit, io, op);

    nd_t_bld += nd_since(t);
    nd_n_tx++;

    return n;
}

static LONG netdev_tx_timed_issue(NetdevUnit *unit, struct IOSana2Req *io,
                                  NetdevOpener *op, UWORD total)
{
    ULONG t  = nd_now();
    ULONG r0 = netdev_time_regs;
    LONG  r  = netdev_tx_issue(unit, io, op, total);

    nd_t_iss += nd_since(t);
    nd_regs_tx += netdev_time_regs - r0;

    return r;
}
#else
#define netdev_tx_timed_build(u, i, o)      netdev_tx_build((u), (i), (o))
#define netdev_tx_timed_issue(u, i, o, t)   netdev_tx_issue((u), (i), (o), (t))
#endif

/*
 * Drain the queue into whatever transmit buffers the chip has free.  Runs
 * from the interrupt server after a transmit completes, and from BeginIO for
 * anything that could not take the direct path.  The caller holds Disable().
 *
 * It stands off while a task-level build owns nu_TxBuf.  The builder pumps
 * again once its own frame is issued, so nothing is stranded.
 */
VOID netdev_tx_pump(NetdevUnit *unit)
{
    while (unit->nu_Nic.txb_inuse < unit->nu_Nic.txb_cnt)
    {
        struct IOSana2Req *io;
        NetdevOpener      *op;
        UWORD              total;
        LONG               rc;

        if (unit->nu_TxBuilding || IsListEmpty(&unit->nu_Writes))
            return;

        io = (struct IOSana2Req *)RemHead(&unit->nu_Writes);
        op = NETDEV_OPENER(io->ios2_Req.io_Unit);

        total = netdev_tx_timed_build(unit, io, op);
        if (total == 0)
            continue;

        rc = netdev_tx_timed_issue(unit, io, op, total);
        if (rc == DP8390_TX_BUSY)
        {
            AddHead(&unit->nu_Writes, &io->ios2_Req.io_Message.mn_Node);
            return;
        }
        if (rc != 0)
        {
            netdev_reply(io, S2ERR_TX_FAILURE, S2WERR_GENERIC_ERROR);
            netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX);
            continue;
        }

        netdev_reply(io, 0, 0);
    }
}

/*
 * From BeginIO at task level.  The pump runs the opener's CopyFrom under
 * Disable(), which the timing build prices at 135 us of the 219 us a transmit
 * spends in the driver.  That is 46% of an A3000's frame interval with
 * interrupts off, and on a write it holds up the acknowledgements.  So claim
 * nu_TxBuf under the mask, frame outside it, and take the mask again only for
 * the chip.
 */
VOID netdev_tx_direct(NetdevUnit *unit, struct IOSana2Req *io)
{
    NetdevOpener *op = NETDEV_OPENER(io->ios2_Req.io_Unit);
    UWORD         total;
    LONG          rc;

    Disable();
    /* The command-table check is outside this critical section.  If OFFLINE
       drained the queue between that check and here, adding the write now
       would strand it on a stopped unit with no completion left to pump it. */
    if (!unit->nu_Online || !unit->nu_Nic.running)
    {
        Enable();
        netdev_reply(io, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
        return;
    }

    if (unit->nu_TxBuilding || !IsListEmpty(&unit->nu_Writes) ||
        unit->nu_Nic.txb_inuse >= unit->nu_Nic.txb_cnt)
    {
        netdev_queue_tail(&unit->nu_Writes, io);
        netdev_tx_pump(unit);
        Enable();
        return;
    }
    unit->nu_TxBuilding = 1;
    Enable();

    total = netdev_tx_timed_build(unit, io, op);

    Disable();
    unit->nu_TxBuilding = 0;
    if (total == 0)
    {
        netdev_tx_pump(unit);       /* the build failed and answered io */
        Enable();
        return;
    }

    rc = netdev_tx_timed_issue(unit, io, op, total);
    if (rc == DP8390_TX_BUSY)
    {
        netdev_queue_head(&unit->nu_Writes, io);
        Enable();
        return;
    }
    netdev_tx_pump(unit);           /* anything queued while we built */
    Enable();

    if (rc != 0)
    {
        netdev_reply(io, S2ERR_TX_FAILURE, S2WERR_GENERIC_ERROR);
        netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX);
    }
    else
    {
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

/* ------------------------------------------------------ online / offline -- */

LONG netdev_online(NetdevUnit *unit)
{
    LONG rc;

    if (!netdev_pcmcia_available(unit))
    {
        netdev_event(unit, S2EVENT_ERROR | S2EVENT_HARDWARE);
        return -1;
    }

    Disable();
    rc = unit->nu_Nic.ops->init(&unit->nu_Nic);
    Enable();

    if (rc != 0)
    {
        /* The chip would not start.  cnet.device fires
           S2EVENT_ERROR|S2EVENT_HARDWARE from both init_card and init_nic for
           this, and it is the only condition in this driver that is squarely
           the hardware's. */
        netdev_event(unit, S2EVENT_ERROR | S2EVENT_HARDWARE);
        return rc;
    }

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
 * The unit outlives every opener, so anything an opener changed about it must
 * be undone when the last one goes.  Across a stack restart three things
 * matter:
 *   - an accept-all-multicast latch that never clears
 *   - a multicast table that fills with 32 stale groups and then refuses the
 *     new stack's joins with S2ERR_NO_RESOURCES, taking IPv6 solicited-node
 *     frames with it
 *   - a configured flag that makes the new stack's S2_CONFIGINTERFACE fail, so
 *     it silently runs on the old stack's address
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

static VOID netdev_set_offline(NetdevUnit *unit, ULONG event, BOOL stop)
{
    struct Node *n;

    Disable();
    /* The removal callback clears running before a task can close the unit.
       In that state a PCMCIA stop is an access to an empty socket. */
    if (stop && (!netdev_pcmcia_is_unit(unit) || unit->nu_Nic.running))
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

VOID netdev_offline(NetdevUnit *unit, ULONG event)
{
    netdev_set_offline(unit, event, TRUE);
}

VOID netdev_pcmcia_detached(NetdevUnit *unit, ULONG event)
{
    /* card.resource has already reset the socket control registers.  The
       controller is physically absent, so even a polite stop is a bus access
       to empty space. */
    netdev_set_offline(unit, event, FALSE);
}

/* ------------------------------------------------------ interrupt server -- */

ULONG netdev_interrupt(NetdevUnit *unit)
{
    /*
     * The chip's own ISR is the only test made.
     *
     * A board-level test came first.  X-Surf and X-Surf 100 put an
     * interrupt-status bit at board+0x40, and INT2 is shared, so a server must
     * not answer for another card.  It is not worth the risk.  NetBSD's
     * xsurf.c and xsh.c install dp8390_intr directly with no such test.  The
     * offset and bit are the one part of the card table that could not be
     * checked against NetBSD.
     *
     * A wrong offset on a row nobody has run leaves the chip's level-triggered
     * INT unacknowledged.  INT2 then stays down for the whole machine, or the
     * card never receives.  Reading ISR costs two bus cycles and cannot be
     * wrong.  dp8390_intr() returns FALSE when it reads zero, which is the
     * same answer the board bit gave.
     *
     * The chip's own error counters are read here and nowhere else.  A CRC
     * error, a framing error, a receiver overrun and a transmit that gave up
     * after sixteen collisions are known only to the core, and every core
     * records them.  A diff of the three counters across ops->intr() raises
     * S2EVENT_RX and S2EVENT_TX for the whole card family from one site,
     * rather than from ten cores.  slip.device's PacketOverrun() is the same
     * event from the same cause.
     *
     * The snapshot is gated on nu_EventMask, so a driver nobody is watching
     * pays one word read and a branch.
     */
    UWORD watched = unit->nu_EventMask;
    ULONG rx0 = 0;
    ULONG tx0 = 0;
    ULONG evt = 0;

    if (watched != 0)
    {
        rx0 = unit->nu_Nic.rx_errors + unit->nu_Nic.overruns;
        tx0 = unit->nu_Nic.tx_errors;
    }

#ifdef NETDEV_TIME
    {
        ULONG t0 = nd_now();
        ULONG r0 = netdev_time_regs;
        BOOL  mine;

        mine = unit->nu_Nic.ops->intr(&unit->nu_Nic);
        nd_t_isr += nd_since(t0);
        nd_regs_isr += netdev_time_regs - r0;
        nd_n_int++;
        if (!mine)
            return 0;

        t0 = nd_now();
        netdev_tx_pump(unit);
        nd_t_tx += nd_since(t0);

        if (nd_n_frame >= 512)
            nd_time_report();
    }
#else
    if (!unit->nu_Nic.ops->intr(&unit->nu_Nic))
        return 0;

    netdev_tx_pump(unit);
#endif

    if (watched != 0)
    {
        if (unit->nu_Nic.rx_errors + unit->nu_Nic.overruns != rx0)
            evt |= S2EVENT_ERROR | S2EVENT_RX;
        if (unit->nu_Nic.tx_errors != tx0)
            evt |= S2EVENT_ERROR | S2EVENT_TX;
        if (evt != 0)
            netdev_event(unit, evt);
    }

    return 1;
}

static ULONG netdev_server(register NetdevUnit *unit __asm("a1"))
{
    return netdev_interrupt(unit);
}

/*
 * NetBSD arms ifp->if_timer on every transmit and dp8390_watchdog() resets the
 * chip when it expires.  There is no callout here and no task, so the tick is
 * the vertical blank, the one clock a device gets for free.
 *
 * netdev_tx_pump() only runs while txb_inuse < txb_cnt, and only a core's own
 * re-init clears txb_inuse.  One lost or never-delivered transmit completion
 * therefore wedges the transmitter for good.  Every later CMD_WRITE queues on
 * nu_Writes and is never replied to, and the caller sits in WaitIO() forever.
 * Nothing else in the driver can notice, because that needs a clock the card
 * does not drive.
 *
 * A nonempty ring is not by itself a wedge.  At line rate a completion is
 * replaced before the next vertical blank and txb_inuse never reaches zero,
 * especially on a LANCE with four descriptors.  Each core therefore advances
 * tx_completed when hardware returns a frame, and only a full interval with no
 * such progress is a stall.
 *
 * The recovery goes through ops->reset rather than a named core.  This tick is
 * armed for every unit, so a call to dp8390_reset() here wrote DP8390 register
 * numbers into whatever chip the unit had.  On an A2065 or an Ariadne that is
 * a LANCE, and the result is an undefined chip, not a recovered one.
 *
 * Disable() rather than a software interrupt: the vertical blank is INT3 and
 * the card is INT2, so this can preempt the card's own server and must not
 * touch the chip alongside it.  The recovery is rare and costs a few
 * milliseconds once, against a transmitter that never comes back.
 */
static ULONG netdev_tick(register NetdevUnit *unit __asm("a1"))
{
    BOOL wedged = FALSE;

    Disable();

    if (netdev_tx_watchdog_tick(&unit->nu_TxStall, &unit->nu_TxProgress,
                               (BOOL)(unit->nu_Online &&
                                      (!netdev_pcmcia_is_unit(unit) ||
                                       unit->nu_Nic.running)),
                               unit->nu_Nic.txb_inuse,
                               unit->nu_Nic.tx_completed))
    {
        unit->nu_TxWedges++;
        unit->nu_Nic.tx_errors++;
        if (unit->nu_Nic.ops->reset != NULL)
            unit->nu_Nic.ops->reset(&unit->nu_Nic);
        netdev_tx_pump(unit);
        wedged = TRUE;
    }

    Enable();

    /*
     * A transmitter that had to be reset is both a transmit error and a
     * hardware error.  cnet.device posts S2EVENT_ERROR|S2EVENT_TX for a
     * transmit DMA timeout, and S2EVENT_ERROR|S2EVENT_HARDWARE for a chip that
     * had to be re-initialised.  Posted outside the Disable() above only for
     * tidiness, because netdev_event() takes its own and it nests.
     */
    if (wedged)
        netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX | S2EVENT_HARDWARE);

    return 0;
}

/* --------------------------------------------------- machine fingerprint -- */

/*
 * A card whose address PROM reads all-zero or all-ones needs an address from
 * somewhere.  The requirement on it is not uniqueness, which nothing here can
 * promise, but "the same on every boot of this machine, and different from the
 * next machine wherever the two machines differ".  What is reachable from a
 * device's probe, which runs with exec.library and expansion.library open and
 * nothing else:
 *
 *   Taken
 *     the caller's salt         the card row, its window and its autoconfig
 *                               serial: separates two units in one machine
 *     ExecBase identity         AttnFlags (CPU/FPU/MMU), the Kickstart version
 *                               and revision, and the E-clock (PAL vs NTSC)
 *     MaxLocMem                 chip RAM size, which is an A600/A1200 fact and
 *                               not a Zorro one
 *     the memory list           each region's attributes and its bounds, which
 *                               is where a trapdoor or PCMCIA-shadowing RAM
 *                               expansion shows up.  mh_Free is not read: it
 *                               moves with whatever is loaded.
 *     every ConfigDev           manufacturer, product and serial number, which
 *                               is per-board and is the input Commodore's own
 *                               convention uses for a MAC (lance.c:465)
 *     the card's CIS            MANFID identifies the model, VERS_1 carries
 *                               free-form strings that some cards use for a
 *                               lot or serial number, and FUNCE is read for
 *                               its LAN node ID before any of this runs
 *
 *   Rejected
 *     battclock / ReadEClock    not stable across boots, which is the one
 *                               property that matters
 *     AvailMem()                moves with what is loaded before the device is
 *                               opened, so the address would move with it
 *     the boot volume's root    the best per-install input there is, and it
 *     block creation date       needs dos.library and a mounted, readable boot
 *                               volume inside a device probe.  A SANA-II
 *                               provider is loaded by whichever stack wants
 *                               it, sometimes before a filesystem is up.  That
 *                               path must not gain a DOS dependency or a
 *                               bootstrap ordering rule.
 *     CIS MANFID alone          it is the card model.  Two people with the
 *                               same D-Link get the same value.
 *
 * The limit: two stock A1200s with the same RAM, the same Kickstart and the
 * same model of card derive the same address.  HARDWAREADDRESS in the
 * interface configuration is the fix, for that pair and for any other pair of
 * cards that collide.
 */
static VOID nd_fp_put(UBYTE *buf, UWORD max, UWORD *n, ULONG v, UBYTE bytes)
{
    UBYTE i;

    for (i = 0; i < bytes; i++)
    {
        if (*n >= max)
            return;
        buf[(*n)++] = (UBYTE)(v >> ((bytes - 1u - i) * 8u));
    }
}

UWORD netdev_mac_fingerprint(UBYTE *buf, UWORD max, ULONG salt)
{
    UWORD n = 0;
    UWORD i;

    nd_fp_put(buf, max, &n, salt, 4);

    if (SysBase != NULL)
    {
        struct MemHeader *mh;

        nd_fp_put(buf, max, &n, (ULONG)SysBase->AttnFlags, 2);
        nd_fp_put(buf, max, &n, (ULONG)SysBase->LibNode.lib_Version, 2);
        nd_fp_put(buf, max, &n, (ULONG)SysBase->LibNode.lib_Revision, 2);
        nd_fp_put(buf, max, &n, SysBase->ex_EClockFrequency, 4);
        nd_fp_put(buf, max, &n, (ULONG)SysBase->MaxLocMem, 4);

        /* Forbid(), not Disable(): the list is only rearranged by AddMemList
           and by a task, and this runs at probe time where a Disable() would
           be the heavier of the two for no gain. */
        Forbid();
        i = 0;
        for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
             mh->mh_Node.ln_Succ != NULL && i < 4; i++)
        {
            nd_fp_put(buf, max, &n, (ULONG)mh->mh_Attributes, 2);
            nd_fp_put(buf, max, &n, (ULONG)(APTR)mh->mh_Lower, 4);
            nd_fp_put(buf, max, &n, (ULONG)(APTR)mh->mh_Upper, 4);
            mh = (struct MemHeader *)mh->mh_Node.ln_Succ;
        }
        Permit();
    }

    if (ExpansionBase != NULL)
    {
        struct ConfigDev *cd = NULL;

        i = 0;
        while ((cd = FindConfigDev(cd, -1, -1)) != NULL && i < 4)
        {
            nd_fp_put(buf, max, &n, (ULONG)cd->cd_Rom.er_Manufacturer, 2);
            nd_fp_put(buf, max, &n, (ULONG)cd->cd_Rom.er_Product, 1);
            nd_fp_put(buf, max, &n, cd->cd_Rom.er_SerialNumber, 4);
            i++;
        }
    }

    n = (UWORD)(n + netdev_pcmcia_fingerprint(buf + n, (UWORD)(max - n)));

    nd_tracex("anx: fp bytes ", (ULONG)n);

    return n;
}

/* ----------------------------------------------------------------- probe -- */

/*
 * One matched board becomes one unit.  Shared by the two ways in: the
 * ConfigDev walk below, and the PCMCIA slot, which has no ConfigDev at all.
 */
static BOOL netdev_add_unit(NetdevDevice *dev, const NetdevCard *card,
                            APTR board, ULONG serial)
{
    NetdevUnit *unit;

    unit = &dev->nd_Units[dev->nd_UnitCount];
    nd_zero((UBYTE *)unit, sizeof(*unit));

    unit->nu_Nic.ops = netdev_nic_ops_for(card->chip);
    if (unit->nu_Nic.ops == NULL)
    {
        netdev_diag_note(ANXDIAG_NO_CORE, netdev_diag_card(card),
                         (ULONG)card->chip);
        return FALSE;       /* no core for this chip yet */
    }

    unit->nu_Dev    = dev;
    unit->nu_Nic.card   = card;
    unit->nu_Nic.board  = (volatile UBYTE *)board;
    unit->nu_Nic.serial = serial;
    unit->nu_Nic.rx     = netdev_rx;
    unit->nu_Nic.rx_arg = unit;

    netdev_bus_setup(&unit->nu_Nic.bus,
             (APTR)((UBYTE *)board + card->reg_off),
             card->stride,
             card->wide_off != 0
                 ? (APTR)((UBYTE *)board + card->wide_off)
                 : NULL);

    /* A register file split across two windows, which is Gayle's PCMCIA I/O
       and nothing else in the table. */
    if (card->odd_off != 0)
        netdev_bus_split(&unit->nu_Nic.bus,
                         (APTR)((UBYTE *)board + card->odd_off +
                                card->reg_off));

    /* A scattered register file replaces the stride entirely, and carries its
       own data-port address: entry 16 of the table. */
    if (card->regmap != NULL)
        netdev_bus_regmap(&unit->nu_Nic.bus, card->regmap,
                          (APTR)((UBYTE *)board + card->regmap[16]));

    netdev_diag_note(ANXDIAG_CHIP, netdev_diag_card(card), (ULONG)card->chip);

    nd_tracex("anx: board ", (ULONG)board);

    /*
     * A chip behind an ISA Plug and Play bridge decodes nothing until it is
     * configured, so on the one row that has one this runs before attach.
     * Attach then has a chip to find.  Every other row returns TRUE without
     * touching the board.
     *
     * A refusal is not passed on to attach.  There is no register file to
     * detect, so ne2000.c reads a floating bus and files the answer as a
     * command register that read $ff -- true, and three steps downstream of
     * the step that failed.  netdev_isapnp.c recorded which.
     */
    if (!netdev_isapnp_configure(card, board))
    {
        nd_trace("anx: isapnp failed\r\n");
        return FALSE;
    }
    if (unit->nu_Nic.ops->attach(&unit->nu_Nic) != 0)
    {
        nd_trace("anx: attach failed\r\n");
        /* The core leaves the reason in diag_why, and it is recorded here
           once, so a core has one field to set rather than a call to
           remember.  Zero is ANXDIAG_WHY_UNKNOWN, so a refusal is never
           silent. */
        netdev_diag_note(ANXDIAG_ATTACH_FAIL, netdev_diag_card(card),
                         (ULONG)unit->nu_Nic.diag_why);
        return FALSE;       /* the board did not answer as a DP8390 */
    }
    nd_tracex("anx: mac ", ((ULONG)unit->nu_Nic.factory[2] << 24) |
                   ((ULONG)unit->nu_Nic.factory[3] << 16) |
                   ((ULONG)unit->nu_Nic.factory[4] << 8) |
                   (ULONG)unit->nu_Nic.factory[5]);
    nd_tracex("anx: dmode ", (ULONG)unit->nu_Nic.bus.dmode);

    {
        UWORD ci = netdev_diag_card(card);

        netdev_diag_note(ANXDIAG_ATTACH_OK, ci, (ULONG)unit->nu_Nic.bus.dmode);
        /*
         * factory[], not mac[].  mac[] is the OPERATING address and nothing
         * has set it yet: it arrives with S2_CONFIGINTERFACE when an
         * interface is configured, which is long after a probe.  A tool that
         * only probes -- CheckNetDevice is the one that exists -- therefore
         * read six zeroes and printed them under a card that works, beside a
         * source line correctly naming where the address had been read from.
         * A diagnostic that reports a working card as having no address is
         * worse than one that reports nothing.
         *
         * factory[] is what the card itself answered, every driver fills it
         * at attach, and it is the thing the source line is about.
         */
        netdev_diag_note(ANXDIAG_MAC_HI, ci,
                         ((ULONG)unit->nu_Nic.factory[0] << 8) |
                         (ULONG)unit->nu_Nic.factory[1]);
        netdev_diag_note(ANXDIAG_MAC_LO, ci,
                         ((ULONG)unit->nu_Nic.factory[2] << 24) |
                         ((ULONG)unit->nu_Nic.factory[3] << 16) |
                         ((ULONG)unit->nu_Nic.factory[4] << 8) |
                         (ULONG)unit->nu_Nic.factory[5]);
        netdev_diag_note(ANXDIAG_MAC_SOURCE, ci,
                         (ULONG)unit->nu_Nic.mac_source);
        /* No ANXDIAG_GETODD here.  ne2000_detect() records it where the mode
           is decided, for the split-window cards where it can mean anything.
           A second record put the same sentence in the report twice. */
        netdev_diag_note(ANXDIAG_UNIT, ci, (ULONG)dev->nd_UnitCount);
    }

    nd_newlist(&unit->nu_OpenerList);
    nd_newlist(&unit->nu_Writes);
    unit->nu_Unit = dev->nd_UnitCount;

    unit->nu_Intr.is_Node.ln_Type = NT_INTERRUPT;
    unit->nu_Intr.is_Node.ln_Pri  = 10;
    unit->nu_Intr.is_Node.ln_Name = netdev_name;
    unit->nu_Intr.is_Data     = unit;
    unit->nu_Intr.is_Code     = (VOID (*)())netdev_server;

    unit->nu_Tick.is_Node.ln_Type = NT_INTERRUPT;
    unit->nu_Tick.is_Node.ln_Pri  = 0;
    unit->nu_Tick.is_Node.ln_Name = netdev_name;
    unit->nu_Tick.is_Data     = unit;
    unit->nu_Tick.is_Code     = (VOID (*)())netdev_tick;

    dev->nd_UnitCount++;

    return TRUE;
}

BOOL netdev_pcmcia_reattach(NetdevUnit *unit, const NetdevCard *card, APTR board)
{
    if (unit == NULL || card == NULL || card->bus != NETDEV_BUS_PCMCIA ||
        unit->nu_Nic.card != card)
        return FALSE;

    /* A replacement card starts with none of the measured bus state of the
       previous one.  In particular getodd must be probed again. */
    netdev_bus_setup(&unit->nu_Nic.bus,
                     (APTR)((UBYTE *)board + card->reg_off), card->stride,
                     card->wide_off != 0
                         ? (APTR)((UBYTE *)board + card->wide_off) : NULL);
    if (card->odd_off != 0)
        netdev_bus_split(&unit->nu_Nic.bus,
                         (APTR)((UBYTE *)board + card->odd_off + card->reg_off));

    unit->nu_Nic.board = (volatile UBYTE *)board;
    return (BOOL)(unit->nu_Nic.ops->attach(&unit->nu_Nic) == 0);
}

static VOID netdev_probe(NetdevDevice *dev)
{
    struct ConfigDev *cd = NULL;
    ULONG             boards = 0;

    if (dev->nd_ExpansionBase == NULL)
        return;

    /*
     * FindConfigDev(NULL, -1, -1) walks the board list in the order Expansion
     * built it, which is autoconfig order, which is slot order.  That makes
     * unit N the same board on the next boot.
     *
     * The ED core moved some unit numbers.  Hydra and the ASDG LAN Rover used
     * to be recognised and skipped, so on a machine holding one of them plus
     * an NE2000 board the NE2000 board was unit 0.  It is now unit 1 if the
     * ED board autoconfigures first.  The pinned numbers did not move, which
     * is why the two card rows were added before the core.  A pin is
     * (index + 1) * 100 + instance over netdev_cards[], and hydra has been
     * index 3 -> 400 and lanrover index 4 -> 500 since the table was written.
     * Anything that pinned a card by number or by S2_AnxCardType is
     * unaffected.
     *
     * Only bare positional units shift, and only on a machine that has one of
     * these two boards, where they previously did not work at all.
     */
    while ((cd = FindConfigDev(cd, -1, -1)) != NULL)
    {
        const NetdevCard *card = NULL;
        UWORD             i;

        boards++;

        for (i = 0; i < netdev_card_count; i++)
        {
            /* A PCMCIA row's manid/prodid are its CIS MANFID, not an
               autoconfig record, so it must never match a board here. */
            if (netdev_cards[i].bus != NETDEV_BUS_ZORRO)
                continue;

            if (cd->cd_Rom.er_Manufacturer == netdev_cards[i].manid &&
                cd->cd_Rom.er_Product == (UBYTE)netdev_cards[i].prodid)
            {
                card = &netdev_cards[i];
                break;
            }
        }

        if (card == NULL)
        {
            /* Recorded, because "boards on this bus, none of them known" is a
               different answer from "no boards", and only the first means a
               broken card. */
            netdev_diag_note(ANXDIAG_NOMATCH, ANXDIAG_NOCARD,
                             ((ULONG)cd->cd_Rom.er_Manufacturer << 16) |
                             (ULONG)cd->cd_Rom.er_Product);
            continue;
        }

        netdev_diag_note(ANXDIAG_ZORRO_FOUND, netdev_diag_card(card),
                         (ULONG)cd->cd_BoardAddr);

        /* A supported board past the table is dropped, and a silent drop is
           the worst case: the card is fitted, nothing opens it, and nothing
           says why.  Count it so S2_GETSPECIALSTATS can be asked.  Counted
           after the match, not before, because the count is boards this driver
           could have driven.  A RAM board walked past once the slots were full
           is not one of them. */
        if (dev->nd_UnitCount >= NETDEV_MAX_UNITS)
        {
            dev->nd_UnitsDropped++;
            netdev_diag_note(ANXDIAG_UNITS_FULL, netdev_diag_card(card),
                             (ULONG)NETDEV_MAX_UNITS);
            continue;
        }

        if (!netdev_add_unit(dev, card, (APTR)cd->cd_BoardAddr,
                             cd->cd_Rom.er_SerialNumber))
            continue;
    }

    netdev_diag_note(ANXDIAG_BOARDS, ANXDIAG_NOCARD, (ULONG)boards);

    /*
     * And the slot, after the boards.  Last so that a machine with both keeps
     * its Zorro unit numbers where they were: a PCMCIA card is the one card
     * that can be inserted between two boots.
     */
    {
        UWORD i;

        /* The fixed-address rows: nothing to claim and nothing to configure.
           The board is wherever the machine puts it, and attach() deciding
           the chip is not answering is the whole of the probe. */
        for (i = 0; i < netdev_card_count; i++)
        {
            const NetdevCard *card = &netdev_cards[i];
            APTR              base;

            if (card->bus != NETDEV_BUS_FIXED)
                continue;
            if (dev->nd_UnitCount >= NETDEV_MAX_UNITS)
            {
                dev->nd_UnitsDropped++;
                netdev_diag_note(ANXDIAG_UNITS_FULL, i,
                                 (ULONG)NETDEV_MAX_UNITS);
                break;
            }

            base = (APTR)(ULONG)card->base;
            netdev_diag_note(ANXDIAG_FIXED_TRY, i, (ULONG)base);

            nd_tracex("anx: fixed base ", (ULONG)base);
            (VOID)netdev_add_unit(dev, card, base, 0);
        }

        /*
         * And the slot, once, not once per PCMCIA row.
         *
         * There is one slot in the machine, and netdev_pcmcia.c holds one
         * handle for it.  A loop that claimed per row could only work while
         * exactly one row was PCMCIA, because a second claim overwrites the
         * handle card.resource still holds.  The card names itself in its CIS,
         * so the claim reads that and hands back the row.
         */
        if (dev->nd_UnitCount < NETDEV_MAX_UNITS)
        {
            const NetdevCard *card = NULL;
            APTR              base;

            ObtainSemaphore(&dev->nd_PcmciaLock);
            base = netdev_pcmcia_claim(dev, &card);

            if (base != NULL)
            {
                nd_tracex("anx: pcmcia base ", (ULONG)base);
                if (!netdev_add_unit(dev, card, base, 0))
                    netdev_pcmcia_release();
                else
                    netdev_pcmcia_bind(&dev->nd_Units[dev->nd_UnitCount - 1]);
            }
            ReleaseSemaphore(&dev->nd_PcmciaLock);
        }
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

/*
 * Device initialization cannot leave an IFAVAILABLE handle queued for an empty
 * slot, so a card inserted after the romtag probe needs one task-context retry.
 * A positional request can only mean the slot when it asks for the next unit:
 * PCMCIA is deliberately last in probe order.  A pinned request names the row
 * exactly and never falls through to the other PCMCIA core.
 */
static BOOL netdev_request_is_pcmcia(NetdevDevice *dev, ULONG unit,
                                     const char *pin_name,
                                     const NetdevCard **wanted)
{
    const NetdevCard *card = NULL;

    *wanted = NULL;
    if (pin_name != NULL)
    {
        card = netdev_card_by_name(pin_name);
        if (card == NULL || card->bus != NETDEV_BUS_PCMCIA)
            return FALSE;
        *wanted = card;
        return TRUE;
    }

    if (unit >= ANXNET_UNIT_PIN)
    {
        ULONG idx = (unit / ANXNET_UNIT_PIN) - 1;

        if (idx >= (ULONG)netdev_card_count)
            return FALSE;
        card = &netdev_cards[idx];
        if (card->bus != NETDEV_BUS_PCMCIA)
            return FALSE;
        *wanted = card;
        return TRUE;
    }

    return (BOOL)(unit == (ULONG)dev->nd_UnitCount);
}

static NetdevUnit *netdev_try_pcmcia_open(NetdevDevice *dev, ULONG unit,
                                          const char *pin_name,
                                          const char **why)
{
    const NetdevCard *wanted;
    const NetdevCard *card = NULL;
    NetdevUnit       *found;
    APTR              base;

    if (dev->nd_UnitCount >= NETDEV_MAX_UNITS ||
        !netdev_request_is_pcmcia(dev, unit, pin_name, &wanted))
        return NULL;

    ObtainSemaphore(&dev->nd_PcmciaLock);

    /* Another OpenDevice() may have completed the retry while this one waited. */
    found = netdev_find_unit(dev, unit, pin_name, why);
    if (found == NULL)
    {
        base = netdev_pcmcia_claim(dev, &card);
        if (base != NULL && (wanted == NULL || wanted == card))
        {
            if (netdev_add_unit(dev, card, base, 0))
            {
                netdev_pcmcia_bind(&dev->nd_Units[dev->nd_UnitCount - 1]);
                netdev_diag_counts(dev->nd_UnitCount, dev->nd_UnitsDropped);
            }
            else
            {
                netdev_pcmcia_release();
            }
        }
        else if (base != NULL)
        {
            /* The pin named the other chip family. */
            netdev_pcmcia_release();
        }
        found = netdev_find_unit(dev, unit, pin_name, why);
    }

    ReleaseSemaphore(&dev->nd_PcmciaLock);
    return found;
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
    InitSemaphore(&base->nd_PcmciaLock);

    base->nd_Device.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    base->nd_Device.dd_Library.lib_Node.ln_Name = netdev_name;
    base->nd_Device.dd_Library.lib_Flags    = LIBF_SUMUSED | LIBF_CHANGED;
    base->nd_Device.dd_Library.lib_Version  = NETDEV_VERSION;
    base->nd_Device.dd_Library.lib_Revision = NETDEV_REVISION;
    base->nd_Device.dd_Library.lib_IdString = (APTR)netdev_id;

    nd_trace("anx: init\r\n");

    /*
     * The record is armed before anything is tried and published after
     * everything has been, whether or not a single unit came up.  A machine
     * where none came up is the case this path exists for: there is no unit to
     * open and no other way to ask what happened.
     */
    netdev_diag_reset(&base->nd_Diag);
    netdev_diag_note(ANXDIAG_START, ANXDIAG_NOCARD, (ULONG)netdev_card_count);

    base->nd_ExpansionBase = OpenLibrary((CONST_STRPTR)"expansion.library", 36);
    ExpansionBase = (struct ExpansionBase *)base->nd_ExpansionBase;
    nd_tracex("anx: expansion ", (ULONG)base->nd_ExpansionBase);
    netdev_diag_note(ANXDIAG_EXPANSION, ANXDIAG_NOCARD,
                     (ULONG)base->nd_ExpansionBase);

    netdev_probe(base);
    nd_tracex("anx: units ", (ULONG)base->nd_UnitCount);

    netdev_diag_note(ANXDIAG_DONE, ANXDIAG_NOCARD, (ULONG)base->nd_UnitCount);
    netdev_diag_counts(base->nd_UnitCount, base->nd_UnitsDropped);
    netdev_diag_publish(&base->nd_Diag);

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
    if (hw != NULL && netdev_pcmcia_is_unit(hw) &&
        !netdev_pcmcia_available(hw))
    {
        why = "the PCMCIA card is not ready";
        hw = NULL;
    }
    else if (hw == NULL)
        hw = netdev_try_pcmcia_open(d, unit, pin, &why);
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
     * SANA2OPF_MINE is exclusive access, so a driver that takes the flag must
     * not share the unit.
     *
     * Tested, claimed and joined under one Disable().  Split apart, two opens
     * can race: both read nu_Openers == 0, both decide they can have the unit
     * exclusively, and both get it.  Opens come from processes, so the race is
     * rare and hard to reproduce.
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
        if (!netdev_pcmcia_is_unit(hw))
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

        /* The opener is off the unit's list by now, but nu_EventMask still
           carries whatever it was waiting for.  Left set, every dropped frame
           for the rest of the machine's uptime walks the event lists to find
           them empty. */
        netdev_event_rescan(hw);

        /*
         * And this opener's writes, which live on the unit and not on the
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
                if (!netdev_pcmcia_is_unit(hw))
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
            if (!netdev_pcmcia_is_unit(&d->nd_Units[i]))
                RemIntServer(INTB_PORTS, &d->nd_Units[i].nu_Intr);
            RemIntServer(INTB_VERTB, &d->nd_Units[i].nu_Tick);
            d->nd_Units[i].nu_IntrAdded = 0;
        }
        Disable();
        if (!netdev_pcmcia_is_unit(&d->nd_Units[i]) ||
            d->nd_Units[i].nu_Nic.running)
            d->nd_Units[i].nu_Nic.ops->stop(&d->nd_Units[i].nu_Nic);
        Enable();
    }

    /*
     * The PCMCIA slot goes back before the seglist does.  card.resource holds
     * the CardHandle passed to OwnCard(), and that handle and the removal
     * Interrupt hanging off it are statics in this driver's own BSS.  An
     * unload without a release leaves the resource with a node in freed
     * memory.  The next OwnCard() walks it, and the card is gone until the
     * machine is rebooted.
     */
    netdev_pcmcia_release();

    /*
     * The probe record leaves the semaphore list before the memory it lives in
     * is freed.  nd_Diag is inside this device base.  A name left published
     * hands the next FindSemaphore() a pointer into freed memory, which is the
     * same defect as the card.resource handle above.
     */
    netdev_diag_unpublish(&d->nd_Diag);

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
