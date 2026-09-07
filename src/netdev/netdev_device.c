/*
 * anxnet.device, layer 1 of 3: the AmigaOS device.
 *
 * The romtag lives here, so this file must come first on the link line, and
 * the "moveq #-1,d0 / rts" below must land at offset 0 of the first code hunk.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "aminetxduo/anxs2ext.h"
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
 * proto/expansion.h's inlines read this exact global and a -nostartfiles image
 * has no auto-open: without the definition here the link takes the toolchain's
 * zero stub and FindConfigDev() jumps through null inside the romtag init.
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
/* netdev_begin_io and netdev_abort_io are in netdev_io.c, declared in
   netdev_internal.h, because the host tier enters them. */

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

static VOID netdev_prof_segtag(NetdevDevice *base, BPTR seglist)
{
    NetdevProfSegTag *t = &base->nd_ProfSegTag;

    t->np_Magic   = NETDEV_PROF_SEGTAG_MAGIC;
    t->np_Size    = sizeof(*t);
    t->np_LibBase = (ULONG)base;
    t->np_SegList = (ULONG)seglist;
    t->np_Sum     = 0UL - (t->np_Magic + t->np_Size +
                           t->np_LibBase + t->np_SegList);
}

#if defined(NETDEV_TRACE) || defined(NETDEV_TIME)
/*
 * Raw serial, straight at the custom chips: a device's romtag init runs before
 * anything of ours is open, and this is the only channel needing no library.
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
   link.
 *
 * NOT UNDER NETDEV_TIME.  A line per COMMAND is a line per received frame, and
 * raw serial under this emulator is slow enough that the trace, not the
 * timing, is what made a NETDEV_TIME build run at 987 Kbit/s against 5.9
 * Mbit/s -- the guest took 76 frames a second instead of 480, and every span
 * the instrument reported was measured on a machine the instrument had
 * throttled.  The report itself prints once per 512 frames and is fine. */
#ifdef NETDEV_TRACE
VOID netdev_trace_cmd(UWORD c)
{
    nd_tracex("anx: cmd ", (ULONG)c);
}
#endif

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
 * Beam positions, because there is no timer at interrupt level and ReadEClock()
 * needs a base this code cannot hold.  VHPOSR is 227 colour clocks of 280 ns to
 * the line; the low eight bits of vpos wrap every 256 lines, which is 16 ms.
 */
/*
 * BEAM UNITS, NOT COLOUR CLOCKS, AND THE MULTIPLY IS WHY.
 *
 * This returned `vpos * 227 + hpos`, which is the true colour-clock count and
 * cost a MULU.L to get.  cpucal measures MULU.L on this rig at 43 cycles
 * against ADD.L's 2, and the emulator charges 76 ns a cycle -- so the multiply
 * alone was about 3 us, TWICE per measured span, and NETDEV_TIME's own
 * `t probe16` self-calibration read 4.9 us per nd_now().  That is what made
 * the instrument useless against events of tens of microseconds, and it was
 * blamed on the VHPOSR read: cpucal says the chip access is TWENTY-EIGHT
 * NANOSECONDS.  The multiply was the whole cost.
 *
 * A shift instead.  Each line then contributes 256 units of which 227 are
 * real, so a unit is 227/256 of a colour clock -- 0.248 us against 0.280 --
 * and every span is scaled the same way.  Spans stay comparable with each
 * other, which is all this instrument is for; an absolute figure has to be
 * multiplied by 227/256 first, and the report says so.
 */
/* The eight-bit wrap this clock used to rely on.  nd_now() now returns the
   full nine-bit vpos and nd_since() learns the field height, so nothing tests
   against a fixed range any more; kept out of the build rather than kept
   around to be believed. */
#define ND_UNIT_NUM     227UL           /* a unit is 227/256 colour clocks */
#define ND_UNIT_DEN     256UL

/*
 * NINE BITS OF VPOS, NOT EIGHT, AND A FIELD SIZE THIS LEARNS FOR ITSELF.
 *
 * The eight-bit form fell back to zero TWICE -- once when vpos carried past
 * 255 and once at the end of the field -- and nd_since() told them apart BY
 * SIZE, on the stated assumption that "a span this instrument measures is
 * under a millisecond".  THAT ASSUMPTION IS FALSE FOR `isr`, and the counter
 * added in ddfb73b7 measured how false:
 *
 *     t maxisr 52,924 units   against a half-range of 32,768
 *
 * A span of 13.1 ms straddling the vpos carry has t0 - t1 UNDER the half-range
 * and was classified as an end-of-field and dropped.  The drops are therefore
 * the LONGEST samples, and the bias is not small:
 *
 *     dropisr 13 of 160 interrupts (8.1%)   dropup 12 of 513 frames (2.3%)
 *     isr 346,793 against up 890,887 -- SHORT BY 544,094 over 13 drops,
 *     which is 41,853 units each, the same order as maxisr.
 *
 * That is the whole of the isr < up contradiction, and it closes to the unit.
 *
 * VPOSR bit 0 is vpos bit 8, so the full line number is available for three
 * chip reads instead of one -- 28 ns each, measured, against the 4.9 us the
 * MULU this replaced used to cost.  VPOSR is read twice around VHPOSR because
 * the pair is not atomic: a line boundary between them would pair a new high
 * bit with an old low byte.  Disagreement is rare and costs one retry.
 *
 * With the full vpos there is only ONE discontinuity left, the end of the
 * field, so nd_since() no longer has to guess which one it saw.  The field
 * height in units is not a constant this file may assume -- PAL and NTSC
 * differ and an interlaced mode alternates -- so it is LEARNED: the largest
 * value ever returned, plus one, is the wrap, and it is correct from the first
 * field onwards.  Before that, a span is only dropped if the pre-calibration
 * guess is short, which the initial ND_FIELD_MIN prevents.
 */
#define ND_FIELD_MIN    (262UL * 256UL)     /* NTSC, the smaller of the two */

static ULONG nd_field_top;                  /* largest value seen, self-taught */

static ULONG nd_now(VOID)
{
    volatile UWORD *vposr  = (volatile UWORD *)0xdff004;
    volatile UWORD *vhposr = (volatile UWORD *)0xdff006;
    UWORD           hi0;
    UWORD           vh;
    UWORD           hi1;
    ULONG           v;

    do
    {
        hi0 = (UWORD)(*vposr & 1u);
        vh  = *vhposr;
        hi1 = (UWORD)(*vposr & 1u);
    }
    while (hi0 != hi1);

    v = (((ULONG)hi0 << 8) | (ULONG)(vh >> 8)) << 8 | (ULONG)(vh & 0xff);

    if (v > nd_field_top)
        nd_field_top = v;

    return v;
}

/*
 * A BACKWARDS STEP IS TWO DIFFERENT EVENTS AND THIS TREATED BOTH AS ONE.
 *
 * VHPOSR's high byte is the LOW EIGHT BITS of vpos, so the value this clock
 * returns falls back to zero twice, not once, and only one of the two is the
 * modular wrap the old line assumed:
 *
 *   vpos 255 -> 256   the low byte carries, t0 ~ 65,300 and t1 ~ 100.  A real
 *                     wrap of the eight-bit range, and adding it was right.
 *   vpos 312 -> 0     the END OF THE PAL FIELD, every 20 ms.  The low byte
 *                     goes 56 -> 0, a drop of about 14,336 -- and the old line
 *                     added 65,536 to it and returned about 51,200 units,
 *                     which is 12.7 MILLISECONDS of invented time charged to
 *                     whichever span happened to be open.
 *
 * THAT IS WHAT THE FIRST FULL REPORT MEASURED, and the sums say so without a
 * second run: `up` brackets the whole hand-over and `isr` brackets `up`, so
 * both must be at least as large as the parts inside them, and neither was.
 * One report of 523 frames read up 1,667,633 units against inner spans summing
 * to 2,982,931, and isr 878,956 -- LESS THAN THE CALLBACK IT CONTAINS.  The
 * excess is 1.3M units; 59 field boundaries in that 1.18 s window at ~51,200
 * units each is 3.0M spread over every span open at the time.  find at 468,574
 * units for a loop bounded by op_TrackHigh, which is one, was nine field
 * boundaries and almost nothing else.
 *
 * The two are told apart by size: a span this instrument measures is under a
 * millisecond, so a step back of more than half the range is the carry and
 * anything smaller is the field.  A field-straddling sample cannot be repaired
 * -- the clock does not say how many units the field was -- so it is DROPPED,
 * and nd_n_wrap counts the drops so a reader can see what the average is an
 * average of.  Dropping biases a span low by the few per cent of samples that
 * straddle; the old behaviour biased it high by three hundred.
 *
 * `iss` is why this went unnoticed: it runs under Disable() for ~85 us, so it
 * straddled a boundary about once in a report and its 352/348-unit steady
 * state was real.  Every longer span in the same report was not.
 */
static ULONG nd_n_wrap;

/*
 * AND ONE UNEXPLAINED THING IS LEFT, WHICH THIS IS HERE TO NAME.
 *
 * `isr` brackets ops->intr(), and lance_intr() -> le_rint() -> nic->rx() is
 * netdev_rx(), which is what `up` brackets.  isr therefore CONTAINS up and
 * cannot be smaller than it.  The first report taken with the repaired clock
 * says otherwise:
 *
 *     frames 532   nint 187   2.845 frames an interrupt
 *     up       901,366 units  1,694 a frame  ->  4,819 an interrupt
 *     isr      467,680 units  2,501 an interrupt
 *
 * A factor of 1.9 the wrong way.  Both callers of netdev_interrupt() are
 * bracketed (netdev_device.c:1298 the server, :1348 the vertical-blank poll),
 * the report itself runs outside the bracket, and the drop rule cannot explain
 * it: an isr span is ~6% of a PAL field, so ~11 of 187 samples should straddle,
 * not half of them.
 *
 * So the counters are split.  `t dropisr` and `t dropup` say how many samples
 * each of those two brackets actually lost, and `t maxisr` is the longest span
 * the isr bracket measured.  IT READ 52,924 AGAINST A HALF-RANGE OF 32,768,
 * which is the answer: an isr span is 13 ms, the size test could not tell a
 * carry from a field, and the samples it threw away were the longest ones.
 * nd_now() now returns nine bits of vpos and nd_since() learns the field
 * height, so there is one discontinuity and no guess.  These counters stay --
 * they now count spans REPAIRED across a field rather than lost, and
 * `t fldtop` says what height was learned.
 */
static ULONG nd_n_wrap_isr;
static ULONG nd_n_wrap_up;
static ULONG nd_n_wrap_pre;
static ULONG nd_n_wrap_take;
static ULONG nd_n_wrap_find;
static ULONG nd_n_wrap_addr;
static ULONG nd_n_wrap_reply;
static ULONG nd_n_wrap_hook;
static ULONG nd_t_isr_max;

static ULONG nd_since_at(ULONG t0, ULONG *drops)
{
    ULONG t1 = nd_now();
    ULONG wrap;

    if (t1 >= t0)
        return t1 - t0;

    /*
     * ONE DISCONTINUITY, SO NO GUESSING.  A backwards step is the end of the
     * field and nothing else, and the field's height is the largest value this
     * clock has returned -- learned within the first field, floored at NTSC's
     * so a span taken before that is not credited with a short one.
     */
    wrap = nd_field_top + 1UL;
    if (wrap < ND_FIELD_MIN)
        wrap = ND_FIELD_MIN;

    nd_n_wrap++;                            /* counted: it is a repaired span */
    if (drops != NULL)
        (*drops)++;

    return wrap + t1 - t0;
}

static ULONG nd_since(ULONG t0)
{
    return nd_since_at(t0, NULL);
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
    /*
     * ONE OF THESE TWO IS TRUSTWORTHY AND THE OTHER IS NOT, AND A READER
     * COMPARING THEM AS EQUALS GETS THE WRONG ANSWER -- I nearly did.
     *
     * `iss` is ops->tx and runs under Disable(), so nothing preempts it.
     * Measured across three reports of one transfer it read 352, 348 and 575
     * beam units a transmit; the first two are the steady state and agree to
     * one per cent, and the third is the tail, where the guest sends 226 times
     * against 142 and the work per send is genuinely different.
     *
     * `bld` is the opener's CopyFrom and the framing, at TASK level with
     * interrupts ON, so it absorbs every interrupt that lands inside it.  The
     * same three reports read 928, 550 and 1071 units a transmit -- a factor
     * of two, on identical work.  IT IS NOT A COST, IT IS A COST PLUS
     * WHATEVER ELSE THE MACHINE DID.
     *
     * `iss` at 352 units is 87 us, which is 1,149 cycles at the 76 ns a cycle
     * cpucal measures on this rig.  That is ordinary instruction count for
     * lance_tx's twenty board writes, netdev_track_find and the stats -- NOT a
     * slow bus: the a2065 reads at 77.1 ns/B against Fast RAM's 76.96.
     */
    nd_tracex("t ntx    ", nd_n_tx);
    nd_tracex("t bldTASK", nd_t_bld);        /* preempted: NOT a cost */
    nd_tracex("t issDISA", nd_t_iss);        /* under Disable(): a cost */
    nd_tracex("t rep    ", nd_t_rep);
    /*
     * THE PER-FRAME IOREQUEST ROUND TRIP, WHICH THIS REPORT COLLECTED AND
     * NEVER SHOWED.  Five accumulators were summed on every frame and then
     * cleared unprinted, so the block they measure has only ever been
     * estimated -- by adding profile rows, which is how it got its current
     * "about eleven to twelve per cent of a receive run".
     *
     * That block is now the largest identified one after the copies, and the
     * change it points at -- a shared ring between this device and the reader,
     * SANA-II kept for third-party drivers -- is weeks of work.  Nobody should
     * start it on a number obtained by adding up shares from a sampling
     * profiler when the device already times the parts.
     *
     * `reply` is ReplyMsg at interrupt level, so it is under Disable() and
     * trustworthy the way `iss` is; `pre`, `take`, `find` and `addr` are all
     * inside the interrupt service too.  Divide by `frames`, not by `int`.
     */
    nd_tracex("t preISR ", nd_t_pre);
    nd_tracex("t takeISR", nd_t_take);
    nd_tracex("t findISR", nd_t_find);
    nd_tracex("t addrISR", nd_t_addr);
    nd_tracex("t replISR", nd_t_reply);
    nd_tracex("t txpump ", nd_t_tx);
    nd_tracex("t nint   ", nd_n_int);
    nd_tracex("t nhook  ", nd_n_hook);
    nd_tracex("t probe16", nd_t_probe);
    nd_tracex("t dropped", nd_n_wrap);
    /*
     * EVERY SPAN NOW SAYS HOW MANY OF ITS SAMPLES NEEDED A FIELD CORRECTION,
     * BECAUSE ONE CORRECTED SAMPLE CAN BE THE WHOLE SUM.
     *
     * With the clock repaired, a backwards step is unambiguously the end of a
     * field and `wrap + t1 - t0` is the TRUE elapsed time -- including any
     * higher-level interrupt that preempted the span.  That is right and it is
     * also brutal for a short one: `find` is about fifty units, a field is
     * 80,098, so a single preempted sample is sixteen hundred of them.  The
     * first report with the repair read find 271,901 against 24,468 before,
     * which is four corrections and not a change in the work.
     *
     * So the counts are printed beside the sums.  A span with `wrapfind 4` is
     * four fields of somebody else's time plus the real cost, and the reader
     * can subtract 4 x `fldtop`.  A span with zero is clean.
     *
     * `bldTASK` has always carried this caveat in words -- "preempted: NOT a
     * cost" -- and this is the same caveat as a number, for every row.
     *
     * AND THERE IS A FLOOR UNDER ALL OF IT: THIS CLOCK CANNOT RESOLVE TENS OF
     * UNITS.  `t probe16` prices sixteen back-to-back nd_now() calls at 400
     * beam units, which is 25 a call and 50 for the pair that brackets one
     * span -- the same order as `find` and `take` themselves ever were.  Three
     * runs of the identical code path measured find at 24,468 then 271,901
     * then 513,768, a factor of twenty-one, with only three to eight field
     * corrections between them to explain it.
     *
     * SO DO NOT QUOTE preISR, takeISR, findISR OR addrISR.  What this
     * instrument resolves is spans of hundreds to thousands of units -- `up`
     * at about two thousand a frame, `hook` at a thousand, `replISR` at a few
     * hundred, `isr` at several thousand an interrupt -- and those are stable
     * across runs.  The four short ones are below its own overhead, and the
     * honest reading of them is "smaller than the instrument", not a number.
     */
    nd_tracex("t dropisr", nd_n_wrap_isr);
    nd_tracex("t dropup ", nd_n_wrap_up);
    nd_tracex("t wrappre", nd_n_wrap_pre);
    nd_tracex("t wraptak", nd_n_wrap_take);
    nd_tracex("t wrapfnd", nd_n_wrap_find);
    nd_tracex("t wrapadr", nd_n_wrap_addr);
    nd_tracex("t wraprep", nd_n_wrap_reply);
    nd_tracex("t wraphok", nd_n_wrap_hook);
    nd_tracex("t maxisr ", nd_t_isr_max);
    nd_tracex("t fldtop ", nd_field_top);
    /* The scale, so a reader does not take a beam unit for a colour clock. */
    nd_tracex("t unitnum", ND_UNIT_NUM);
    nd_tracex("t unitden", ND_UNIT_DEN);
    netdev_time_rdc = netdev_time_null = 0;
    netdev_time_rx = netdev_time_tx = 0;
    nd_t_isr = nd_t_copy = nd_t_up = nd_t_tx = nd_t_hook = 0;
    nd_t_pre = nd_t_take = nd_t_find = nd_t_addr = nd_t_reply = 0;
    nd_t_bld = nd_t_iss = nd_t_rep = nd_n_tx = 0;
    nd_n_int = nd_n_frame = nd_n_hook = nd_n_wrap = 0;
    nd_n_wrap_isr = nd_n_wrap_up = nd_t_isr_max = 0;
    nd_n_wrap_pre = nd_n_wrap_take = nd_n_wrap_find = 0;
    nd_n_wrap_addr = nd_n_wrap_reply = nd_n_wrap_hook = 0;
}
#endif

/*
 * A 6-byte Ethernet address as a longword and a word.  Both ends must be even:
 * the staging buffer is AllocMem'd and ios2_DstAddr/ios2_SrcAddr sit at even
 * offsets in the request.
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
 * The buffer-management hooks are m68k register-convention (a0 = to, a1 = from,
 * d0 = len).  A `register ... __asm()` function-pointer typedef miscompiles
 * here: GCC loads the pointer into a0, destroying the first argument.
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
 * A standard utility.library Hook: a0 = the hook, a2 = the object, a1 = the
 * message, result in d0.  Written out because no function-pointer typedef can
 * express three pinned address registers.  h_Entry, not h_SubEntry.
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
 * Nothing may be printed from Open(): exec calls a device's Open vector under
 * Forbid(), and dos.library's Write() can Wait.  A refused open reports through
 * io_Error and the caller explains it.
 */

/* ------------------------------------------------------- the RX callback -- */

/*
 * The order is fixed: fill the request in, ask the filter hook, and only then
 * copy the packet out (copybuff.spec, PacketFilter).  A rejected packet is not
 * an error and the request is not completed: the CMD_READ goes back queued.
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
        nd_t_addr += nd_since_at(ta, &nd_n_wrap_addr);
    }
#endif

    if (!netdev_filter_ok(op, io, payload))
        return NETDEV_RX_REJECTED;

#ifdef NETDEV_TIME
    {
        ULONG th = nd_now();
        BOOL  ok = netdev_copy_call(op->op_CopyTo, io->ios2_Data,
                                    (APTR)payload, plen);

        nd_t_hook += nd_since_at(th, &nd_n_wrap_hook);
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
        nd_t_reply += nd_since_at(tr, &nd_n_wrap_reply);
    }
#else
    netdev_reply(io, 0, 0);
#endif
    return NETDEV_RX_TAKEN;
}

/*
 * Called from the interrupt server with a whole frame.  Every opener with a
 * matching CMD_READ gets a copy; failing that S2_READORPHAN, then a drop.
 */
#ifdef NETDEV_TIME
static VOID netdev_rx_body(APTR arg, const UBYTE *frame, UWORD len);

static VOID netdev_rx(APTR arg, const UBYTE *frame, UWORD len)
{
    ULONG t0 = nd_now();

    netdev_rx_body(arg, frame, len);
    nd_t_up += nd_since_at(t0, &nd_n_wrap_up);
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
        nd_t_pre += nd_since_at(tp, &nd_n_wrap_pre);
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
        nd_t_find += nd_since_at(tf, &nd_n_wrap_find);
        tf = nd_now();
        io = netdev_take(&op->op_Reads, type);
        nd_t_take += nd_since_at(tf, &nd_n_wrap_take);
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
                 * off the queue to be filled in, so it goes back at the head.
                 * Not counted as a drop: the opener asked for it.
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
 * Frame one CMD_WRITE into nu_TxBuf.  Returns the wire length, or 0 with the
 * request already answered.
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

    if (netdev_io_is_raw(op, io))
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
         * nu_TxBuf is the unit's last field, so an unbounded ios2_DataLength
         * copies past the buffer and past the whole unit.
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
     * The chip pads a short frame to the Ethernet minimum out of whatever is in
     * the buffer, so the padding must be written: left out, up to 46 bytes of
     * the last frame go on the wire behind every ARP.
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
 * Drain the queue into whatever transmit buffers the chip has free.  The caller
 * holds Disable().  Stands off while a task-level build owns nu_TxBuf; that
 * builder pumps again once its own frame is issued, so nothing is stranded.
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
 * From BeginIO at task level.  Claim nu_TxBuf under the mask, frame outside it,
 * take the mask again only for the chip: the opener's CopyFrom is 135 us of the
 * 219 us a transmit spends here and must not run with interrupts off.
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
           S2EVENT_ERROR|S2EVENT_HARDWARE from init_card and init_nic for this. */
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
 * interrupt server walks the same list and AddTail() on it is Disable()d too --
 * that is the only arbitration this driver has.
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
 * be undone when the last one goes: the accept-all-multicast latch, the 32-slot
 * multicast table, and the configured flag.
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
     * Only the chip's own ISR is tested.  Reading it costs two bus cycles and
     * cannot be wrong, where a board-level status bit on an unverified row can.
     * The error-counter snapshot below is gated on nu_EventMask.
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
        {
            ULONG span = nd_since_at(t0, &nd_n_wrap_isr);

            nd_t_isr += span;
            if (span > nd_t_isr_max)
                nd_t_isr_max = span;
        }
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
    ULONG mine;

    unit->nu_InIsr = 1;
    mine = netdev_interrupt(unit);
    unit->nu_InIsr = 0;

    if (mine != 0)
    {
        unit->nu_IntSeen++;
        unit->nu_IntSilent = 0;
    }

    return mine;
}

/*
 * The vertical blank is the only clock a device gets for free.  Recovery goes
 * through ops->reset and never a named core, since this tick is armed for every
 * unit.  Disable(): INT3 can preempt the card's own INT2 server.
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

    /*
     * Gayle's card interrupt is a LATCHED status change, and a lost edge on a
     * level source is deafness forever, so the blank services the chip after
     * ten blanks with no claimed interrupt.  nu_InIsr excludes the server.
     */
    if (unit->nu_IntSilent < 0xffffu)
        unit->nu_IntSilent++;

    if (unit->nu_Online && unit->nu_IntSilent >= 10u && unit->nu_InIsr == 0 &&
        (!netdev_pcmcia_is_unit(unit) || unit->nu_Nic.running))
    {
        unit->nu_TickPolls++;
        (VOID)netdev_interrupt(unit);
    }

    /*
     * On an A1200 whose slot never sees CC_RESET the 3c589 wakes in a state
     * that decides whether the receiver hears the wire and is reachable through
     * no register; re-initialising rolls again.  The first frame ends this.
     */
    if (unit->nu_Online && netdev_pcmcia_is_unit(unit) &&
        unit->nu_Nic.running && unit->nu_Nic.rx_packets == 0 &&
        unit->nu_Nic.tx_packets >= 4)
    {
        if (++unit->nu_RxKickWait >= 100u)
        {
            unit->nu_RxKickWait = 0;
            unit->nu_RxKicks++;
            if (unit->nu_Nic.ops->reset != NULL)
                unit->nu_Nic.ops->reset(&unit->nu_Nic);
            netdev_tx_pump(unit);
        }
    }
    else
    {
        unit->nu_RxKickWait = 0;
    }

    Enable();

    /*
     * A transmitter that had to be reset is both a transmit error and a
     * hardware error.  netdev_event() takes its own Disable() and it nests.
     */
    if (wedged)
        netdev_event(unit, S2EVENT_ERROR | S2EVENT_TX | S2EVENT_HARDWARE);

    return 0;
}

/* --------------------------------------------------- machine fingerprint -- */

/*
 * A card whose address PROM reads all-zero or all-ones needs a derived address.
 * The requirement is not uniqueness but "the same on every boot of this machine,
 * and different from the next machine wherever the two machines differ".
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
 * One matched board becomes one unit.  Shared by the two ways in: the ConfigDev
 * walk below, and the PCMCIA slot, which has no ConfigDev at all.
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
    unit->nu_Nic.rx_claim   = netdev_rx_claim;
    unit->nu_Nic.rx_claimed = netdev_rx_claimed;

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
     * configured, so this runs before attach.  A refusal is not passed on to
     * attach, which would read a floating bus and file the wrong reason.
     */
    if (!netdev_isapnp_configure(card, board))
    {
        nd_trace("anx: isapnp failed\r\n");
        return FALSE;
    }
    if (unit->nu_Nic.ops->attach(&unit->nu_Nic) != 0)
    {
        nd_trace("anx: attach failed\r\n");
        /* The core leaves the reason in diag_why and it is recorded here once.
           Zero is ANXDIAG_WHY_UNKNOWN, so a refusal is never silent. */
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
         * factory[], not mac[]: mac[] is the OPERATING address and arrives with
         * S2_CONFIGINTERFACE, long after a probe.  factory[] is what the card
         * itself answered, and every driver fills it at attach.
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
     * built it, which is autoconfig order and so slot order: unit N is the same
     * board on the next boot.  A pin is (index + 1) * 100 + instance.
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

        /* A supported board past the end of the table is dropped; count it so
           S2_GETSPECIALSTATS can be asked.  Counted after the match, because
           the count is boards this driver could have driven. */
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
     * The slot last, so a machine with both keeps its Zorro unit numbers where
     * they were: a PCMCIA card is the one card that can be inserted between two
     * boots.
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
         * The slot once, not once per PCMCIA row.  There is one slot and
         * netdev_pcmcia.c holds one handle for it, so a second claim would
         * overwrite the handle card.resource still holds.
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
 * PCMCIA is deliberately last in probe order.
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
        else if (tag == ANXD_S2_RX_DIRECT)
            op->op_RxDirect = (APTR)tags->ti_Data;
        else if (tag == ANXD_S2_RX_LINK_HDR)
        {
            /* Answering IS the acceptance: the opener reads this back to
               decide whether it still has to synthesise the header. */
            if (tags->ti_Data != 0)
            {
                op->op_RxLinkHdr        = TRUE;
                *(BOOL *)tags->ti_Data  = TRUE;
            }
        }
        else if (tag == ANXD_S2_RX_FILLED)
            op->op_RxFilled = (APTR)tags->ti_Data;
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
    netdev_prof_segtag(base, seglist);
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
     * Armed before anything is tried and published after everything has been: a
     * machine where no unit came up has no unit to open and no other way to be
     * asked what happened.
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
     * SANA2OPF_MINE is exclusive access.  Tested, claimed and joined under one
     * Disable(): split apart, two opens both read nu_Openers == 0 and both take
     * the unit exclusively.
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
           carries what it was waiting for.  Left set, every dropped frame for
           the rest of uptime walks the event lists to find them empty. */
        netdev_event_rescan(hw);

        /*
         * This opener's writes live on the unit and not on the opener.  Left
         * behind, the next transmit interrupt reads op_Raw and op_CopyFrom out
         * of the memory freed below, at interrupt level.
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
     * The PCMCIA slot goes back before the seglist does: the CardHandle and the
     * removal Interrupt hanging off it are statics in this driver's own BSS, so
     * an unload without a release leaves card.resource a node in freed memory.
     */
    netdev_pcmcia_release();

    /*
     * nd_Diag is inside this device base, so the record must leave the semaphore
     * list before the memory it lives in is freed.
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

