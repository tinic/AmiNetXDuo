/*
 * anxnet.device: the Broadcom GENET v5, the Ethernet MAC inside the Raspberry
 * Pi 4 and CM4, driven from a 68k through Emu68 on a PiStorm32.
 *
 * A bus master with its descriptor rings INSIDE its register window: the
 * 256 receive and 256 transmit descriptors are memory-mapped registers at
 * 0x2000 and 0x4000, and only the frame buffers live in RAM.  Emu68 maps fast
 * RAM one-to-one onto the Pi's physical memory, which the MAC's DMA reaches
 * directly, so a buffer's 68k address is its DMA address.
 *
 * Register sequences follow NetBSD's sys/dev/ic/bcmgenet.c (Jared McNeill,
 * BSD-2-Clause), which is the same reference genetreg.h carries.  The PHY's
 * RGMII delay setup is the BCM54213PE's, the part on every Pi 4 and CM4.
 *
 * WHAT COSTS ON THIS MACHINE, measured on Emu68 1.1 (cachebench, 2026-09-15):
 * a copy of a frame is 0.4 us, a register access 0.1 us, and a cache
 * operation of ANY length 42.5 us, because Emu68 implements every one as a
 * whole-cache clean.  So this core copies freely and does exactly one cache
 * operation per burst: one clean before a batch of transmits is handed to the
 * chip, one invalidate before a batch of received frames is read.  A transmit
 * that finds the chip busy is written to the ring and kicked together with
 * everything else that queued behind it when the completion interrupt lands.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_nic.h"
#include "netdev_cards.h"
#include "netdev_mcast.h"
#include "netdev_dtree.h"
#include "genetreg.h"
#include "dp8390.h"     /* the DP8390_TX_* return codes are the shared contract */
#include "n68k_iocopy.h"
#include "netdev_clock.h"
#include "netdev_verify.h"
#include "aminetxduo/anxs2ext.h"   /* the RX_FILLED flag bits */

#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/exec.h>

extern struct ExecBase *SysBase;

#ifdef NETDEV_TRACE
extern VOID netdev_trace_val(const char *tag, ULONG v);
#define GE_TRACE(t, v)  netdev_trace_val((t), (ULONG)(v))
#else
#define GE_TRACE(t, v)  ((VOID)0)
#endif

/* --------------------------------------------------------------- rings --- */

/*
 * 128 receive buffers is 190 us of minimum-size frames at a gigabit before the
 * chip pauses the wire (it stops on a full ring, it does not wrap), and about
 * 1.5 ms of full-size ones; 32 transmit slots is the same reserve the shell's
 * queue offered the LANCE eight times over.  2 KB buffers because the ring
 * register takes a buffer length and a 1518-byte tagged frame plus the
 * 2-byte alignment shift must fit in one descriptor.  The chip has 256
 * descriptors (the descriptor RAM ends at the ring registers, 0xc00 / 12);
 * all 256 were tried 2026-09-17 against a 262 KB window from a 1 Gbit peer
 * and received 598-600 Mbit/s against 589-602 with 128, so 128 stays.
 */
#define GE_RX_RING      128
#define GE_TX_RING      32
#define GE_BUFSZ        2048
#define GE_ALIGN        4096        /* a page: the cache op works in pages */
#define GE_Q            GENET_DMA_DEFAULT_QUEUE

/*
 * Receive interrupt coalescing: an interrupt after this many frames, or this
 * many 8.192 us ticks (125 MHz / 1024) after the first one waiting.  Every
 * interrupt on Emu68 is ~60 us of ROM before this driver sees it (the
 * profile puts it in timer.device's INT6 region, 11% of a transmit run at
 * 500 us), so fewer is the whole lever; the timeout bounds the latency a
 * lone frame pays, and under load the frame count fires first.
 *
 * First measured 2026-09-15 with 32 posted reads: 500 us 120-122 / 63-65
 * Mbit/s RX / TX, 750 us 117 / 69, 1000 us 112 / 73 -- the timeout traded
 * receive for transmit, and 500 us was chosen.  What it was really trading
 * was the read queue: a longer timeout means a longer burst, and past 32
 * frames the burst's tail was dropped (sana2_internal.h,
 * AMI_SANA2_RX_MAX_DEPTH).  With 128 reads and the held pass, 2026-09-16,
 * iperf on the A1200, eth0 offline:
 *
 *     timeout      RX               TX
 *      500 us      272              83
 *     1000 us      280 / 280        97
 *     2000 us      294 / 273       107.5 / 107.5 / 107.5
 *     4000 us      292             103.5
 *
 * Transmit is the acknowledgment stream, 60-byte frames far apart, which
 * only the timeout collects; past 2 ms the sender's 32-segment queue waits
 * on acknowledgments the chip is still holding.  Receive at these rates is
 * the 32-frame threshold, which fires every 1.3 ms at 300 Mbit/s whatever
 * the timeout says.
 *
 * What 2 ms cost was every reply shorter than the threshold: a request/
 * response protocol answers with a few frames and then waits for the next
 * request, so each answer sat the whole timeout in the ring.  A Fitz share
 * read (one 32 KB request in flight, 22 frames back) ran at 11.8 MB/s =
 * 32 KB per 2.7 ms, a ping answered in 2.3 ms.  2026-09-17, the same
 * device with only this constant changed, A1200, eth0 offline:
 *
 *     timeout   ping RTT   Fitz read / write   iperf RX / TX   frames per
 *                          32 KB, MB/s         Mbit/s          interrupt
 *     2000 us   2.30 ms    11.8 / 28.1         896 / 619       32
 *     1000 us   1.30 ms    18.6 / 27.5         864 / 610       22.5
 *      500 us   0.83 ms    26.5 / 29.4         832 / 541       19.6
 *      246 us   0.59 ms    28.8 / 24.4         728 / 481       14.0
 *       98 us   0.40 ms    31.4 / 22.7         115 / 377        1.0
 *
 * Below 500 us the stream side falls away (at 98 us the sender is paced to
 * one frame per interrupt); above it every exchange waits.  500 us: a lone
 * frame waits at most that long, a stream keeps the threshold.
 */
#ifndef GE_RX_COALESCE_FRAMES
#define GE_RX_COALESCE_FRAMES   32
#endif
#ifndef GE_RX_COALESCE_TICKS
#define GE_RX_COALESCE_TICKS    61      /* 500 us */
#endif

/*
 * The interrupt is the backstop; delivery when the CPU is free is a poll.
 *
 * Whatever the timeout above says, one policy serves two wires: a stream
 * wants frames batched, a lone reply wants delivering now, and the chip
 * cannot tell one from the other (it has no timer that restarts per frame,
 * and its other rings are steered by static filters).  So a task at the
 * lowest priority reads the producer index -- a plain load, the registers
 * are mapped straight through on Emu68 -- for GE_POLL_GRACE_US after a
 * frame last came or went, and runs the service pass itself the moment a
 * frame is there, with no interrupt taken.  It runs only when no other task
 * wants the CPU: a machine waiting on a reply is idle and sees the reply
 * within microseconds of its last byte; a machine busy receiving a stream
 * leaves the poller its gaps, and the interrupt delivers as before when the
 * gaps do not come.  A CPU-bound task above it starves it into exactly the
 * behaviour above.  It sleeps the moment the ring holds frames for a reader
 * that is behind: that reader needs the CPU more, and if anything ever put
 * the poller above it, spinning here would be what kept it behind.  What
 * Linux arrived at as NAPI with deferred re-arm and busy polling, with the
 * idle task standing in for the timer.
 *
 * Only the idle receive polling is for AmiNetXDuo's own shell (NetdevNic
 * anxd_openers).  The task also owns deferred PHY work for every opener.
 * Receive hooks run from it with interrupts enabled; plain SANA-II openers
 * still use the ordinary interrupt-driven receive path.
 *
 * A1200, 2026-09-17, on top of the 500 us timeout: Fitz read 26.5 -> 31.6
 * MB/s, iperf in 832 -> 910 Mbit/s, out 541 -> 564; in a 10 s receive run
 * 512k of 1.05M frames were delivered by the poller with no interrupt.
 */
#ifndef GE_POLL_GRACE_US
#define GE_POLL_GRACE_US        500
#endif
#define GE_POLL_STACK           32768   /* the service pass and the opener's
                                           receive hooks run on it: 275 bytes
                                           under AmiNetXDuo's own hooks, and
                                           another stack's hooks are unknown  */
#define GE_POLL_PRI             (-128)

/* The chip shifts every received frame two bytes into its buffer
   (GENET_RBUF_ALIGN_2B), which puts the IP header on a longword. */
#define GE_RX_PAD       2
/* ... after the 64-byte status block the RBUF writes first (RBUF_64B_EN). */
#define GE_RX_HEAD      (GENET_RX_STATUS64_LEN + GE_RX_PAD)
/* A transmit buffer: two bytes of nothing, the block the TBUF reads
   (TBUF_CTRL 64B_EN), the frame.  The two put the frame's IP header on a
   longword, where the stack's copy of the segment is too, so the copy
   between them moves aligned longwords on both sides. */
#define GE_TX_PAD       2
#define GE_TX_HEAD      (GE_TX_PAD + GENET_TX_STATUS64_LEN)

typedef struct GenetCore
{
    UBYTE  *rx_buf;         /* GE_RX_RING buffers, page aligned            */
    UBYTE  *tx_buf;         /* GE_TX_RING buffers                          */
    UWORD   rx_cidx;        /* the chip's 16-bit ring counters, not indices */
    UWORD   rx_clean;       /* ring counter up to which the receive buffers
                               have had their cache operation since the DMA
                               wrote them; frames between rx_cidx and here
                               are waiting for the opener to post a read   */
    UWORD   tx_cidx;
    UWORD   tx_pidx;        /* descriptors written                          */
    UWORD   tx_kicked;      /* the producer index the chip was last told    */
    UWORD   blanks;         /* toward the next link poll                    */
    UBYTE   link;           /* the PHY reported link                        */
    UBYTE   speed;          /* GENET_UMAC_CMD_SPEED_* currently programmed  */
    UBYTE   phy;            /* MDIO address                                 */
    UBYTE   in_mdio;        /* a transaction is in flight, tick stays out   */
    UBYTE   cache;          /* cache maintenance around DMA is on           */
    UBYTE   pageops;        /* supervised cpushp pages, not CacheClearE     */
    UBYTE   phy_set;        /* the PHY's delays and negotiation were set up */
    volatile UBYTE link_poll_due; /* poll task owes the PHY one look       */
    UBYTE   pre_saved;      /* the two below were read before this driver
                               changed anything                            */
    ULONG   pre_rgmii_oob;  /* EXT_RGMII_OOB_CTRL as the firmware left it   */
    ULONG   irq_pending;    /* status the top half took, for the bottom half */
    ULONG   phyid;

    UBYTE   held_blanks;    /* blanks the head of the ring has been held for */
    UBYTE   drop_held;      /* the next unclaimable head frame is dropped    */

    /* The poller (GE_POLL_GRACE_US above). */
    struct Task    *poll_task;      /* NULL: none (no clock, or no memory)   */
    APTR            poll_mem;       /* its Task and stack, one allocation    */
    ULONG           poll_sig;       /* the wake signal, allocated by itself  */
    volatile UBYTE  poll_asleep;    /* in Wait(): the next transmit wakes it */
    volatile ULONG *clock;          /* the Pi's system timer, 1 MHz, from the
                                       device tree; little-endian            */
} GenetCore;

/* Blanks a unicast frame may wait at the head of the ring for its reader to
   post a read before it is dropped: five is 100 ms, ten thousand frames at
   this wire's rate, and a reader that has not run in that time is not going
   to. */
#define GE_HOLD_BLANKS  5

#define GE(nic)         ((GenetCore *)(nic)->core)

/* What this core counts, in nic->core_stat[] order. */
enum
{
    GE_ST_BURSTS = 0,       /* receive batches taken off the ring          */
    GE_ST_KICKS,            /* transmit batches handed to the chip         */
    GE_ST_CACHE,            /* whole-cache operations paid                 */
    GE_ST_RX_CRC,           /* frames the chip flagged: CRC                */
    GE_ST_RX_ERR,           /* frames the chip flagged: any other error    */
    GE_ST_RX_LEN,           /* frames outside 16..2050 bytes or split      */
    GE_ST_LINK,             /* 0 down, else the UMAC speed code + 1        */
    GE_ST_RXPROD0,          /* RX producer index read back after init      */
    GE_ST_TXCONS0,          /* TX consumer index read back after init      */
    GE_ST_INTS,             /* interrupt status words seen non-zero        */
    GE_ST_VERIFIED,         /* frames delivered with their checksums checked */
    GE_ST_BURST_MAX,        /* the most frames one burst held               */
    GE_ST_UNCLAIMED,        /* frames nobody had a read posted for: dropped */
    GE_ST_HELD,             /* passes cut short with frames left in the ring
                               for a reader that was behind (no frame lost) */
    GE_ST_HELD_FRAMES,      /* frames left waiting, summed over those passes */
    GE_ST_TICK_RESUMES,     /* held passes resumed by the vertical blank     */
    GE_ST_TX_CSUM,          /* frames whose transport checksum the TBUF wrote */
    GE_ST_CPUSH_SUPERVISED, /* 1: page pushes enter through Supervisor()   */
    GE_ST_POLL_WAKES,       /* the poller woken by a transmit                */
    GE_ST_POLL_PASSES,      /* service passes it ran with frames waiting     */
    GE_ST_POLL_FRAMES,      /* frames those passes found in the ring         */
#ifdef GE_PROBE_ST
    /* The bottom half timed on the Pi's system timer (bus 0x7E003000, 68k
       0xF8003000 through Emu68's /scb mapping, 1 MHz, a 10 ns read):
       microseconds summed, per phase.  A measurement build only. */
    GE_ST_P_INTR_US,        /* genet_intr, whole                            */
    GE_ST_P_CACHE_US,       /* the burst's cache invalidate                 */
    GE_ST_P_DESC_US,        /* descriptor status reads and re-arms          */
    GE_ST_P_DELIVER_US,     /* ge_deliver: claim, copy+sum, verify, GRO mark */
    GE_ST_P_FLUSH_US,       /* the reply flush per burst                    */
    GE_ST_P_FRAMES,         /* frames ge_deliver was given                  */
    GE_ST_P_CLAIM_US,       /* the claim into the shell and the opener       */
    GE_ST_P_COPY_US,        /* the fused copy and sum                        */
    GE_ST_P_VERIFY_US,      /* checksum verification                         */
    GE_ST_P_CLAIMED_US,     /* rx_claimed: the completion into the opener    */
    GE_ST_P_HWSUM_A,        /* RXCHK checksum == the software sum, as read   */
    GE_ST_P_HWSUM_B,        /* ... == the software sum, halves swapped       */
    GE_ST_P_HWSUM_NE,       /* neither                                       */
    GE_ST_P_TX_US,          /* genet_tx, whole                               */
    GE_ST_P_TXCACHE_US,     /* the kick's page push                          */
    GE_ST_P_TXKICK_US,      /* the kick's producer write                     */
    GE_ST_P_TX_COPIES,      /* frames genet_tx had to copy into the ring     */
#endif
    GE_ST_COUNT
};

_Static_assert(GE_ST_COUNT <= NETDEV_CORE_STATS,
               "the GENET core has more counters than NetdevNic carries");

static const char *const ge_stat_names[GE_ST_COUNT + 1] =
{
    "GENET receive bursts",
    "GENET transmit kicks",
    "GENET cache operations",
    "GENET receive CRC errors",
    "GENET receive other errors",
    "GENET receive bad lengths",
    "GENET link (0 down, 1/2/3 = 10/100/1000)",
    "GENET RX producer after init",
    "GENET TX consumer after init",
    "GENET interrupts with status",
    "GENET frames verified in the driver",
    "GENET largest burst",
    "GENET frames with no read posted",
    "GENET passes held for a reader behind",
    "GENET frames left waiting in those passes",
    "GENET held passes resumed by the blank",
    "GENET transmit checksums by the chip",
    "GENET supervised page push (1)",
    "GENET poll wakes",
    "GENET poll passes",
    "GENET poll frames",
#ifdef GE_PROBE_ST
    "PROBE genet_intr us",
    "PROBE cache op us",
    "PROBE descriptor us",
    "PROBE deliver us",
    "PROBE reply flush us",
    "PROBE frames delivered",
    "PROBE claim us",
    "PROBE copy+sum us",
    "PROBE verify+mark us",
    "PROBE claimed us",
    "PROBE hw csum equal (as read)",
    "PROBE hw csum equal (swapped)",
    "PROBE hw csum unequal",
    "PROBE genet_tx us",
    "PROBE tx kick cache us",
    "PROBE tx kick write us",
    "PROBE tx copies into ring",
#endif
    NULL
};

#ifdef GE_PROBE_ST
static __inline__ ULONG ge_st_us(VOID)
{
    return __builtin_bswap32(*(volatile ULONG *)0xF8003004UL);
}
#define GE_P_START(v)       ULONG v = ge_st_us()
#define GE_P_ADD(nic, k, v) ((nic)->core_stat[k] += ge_st_us() - (v))
#else
#define GE_P_START(v)       do { } while (0)
#define GE_P_ADD(nic, k, v) do { } while (0)
#endif

static LONG genet_init(NetdevNic *nic);
static VOID genet_stop(NetdevNic *nic);
static __inline__ VOID ge_poll_wake(NetdevNic *nic, GenetCore *c);
static VOID ge_dma_stop(NetdevNic *nic);
static VOID genet_setfilter(NetdevNic *nic);

/* ----------------------------------------------------------- registers --- */

/*
 * Every register is a little-endian longword and the 68k is big-endian.
 * Emu68 turns a move.l into one 32-bit access, so the swap is all there is.
 */
static inline ULONG ge_rd(NetdevNic *nic, ULONG off)
{
    return __builtin_bswap32(*(const volatile ULONG *)(nic->board + off));
}

static inline VOID ge_wr(NetdevNic *nic, ULONG off, ULONG v)
{
    *(volatile ULONG *)(nic->board + off) = __builtin_bswap32(v);
}

/*
 * The reset sequence's ten-microsecond settling delays.  Below the beam
 * clock's resolution, so the floor is the wait: ten reads of the version
 * register are a microsecond on Emu68 1.1, each one a round trip to the Pi
 * that a faster host cannot shorten.  Nothing on the frame path waits.
 */
static VOID ge_delay_us(NetdevNic *nic, ULONG us)
{
    NetdevWait w;

    netdev_wait_begin(&w, us, us * 10u);
    do
        (VOID)ge_rd(nic, GENET_SYS_REV_CTRL);
    while (!netdev_wait_done(&w));
}

/* ---------------------------------------------------------------- MDIO --- */

/* A transaction is 25 us on a 2.5 MHz MDC; 2 ms is the reference driver's
   patience, and 20000 reads is that at 0.1 us a read. */
#define GE_MDIO_WAIT_US 2000u
#define GE_MDIO_SPINS   20000u
#define GE_MDIO_READ_FAIL (1UL << 28)

static BOOL ge_mii_wait(NetdevNic *nic)
{
    NetdevWait w;

    netdev_wait_begin(&w, GE_MDIO_WAIT_US, GE_MDIO_SPINS);
    do
    {
        if ((ge_rd(nic, GENET_MDIO_CMD) & GENET_MDIO_START_BUSY) == 0)
            return TRUE;
    }
    while (!netdev_wait_done(&w));
    return FALSE;
}

/* -1 on a timeout or a failed read, else the 16-bit register. */
static LONG ge_mii_read(NetdevNic *nic, UBYTE reg)
{
    ULONG v;

    ge_wr(nic, GENET_MDIO_CMD,
          GENET_MDIO_READ | GENET_MDIO_START_BUSY |
          GENET_MDIO_PMD(GE(nic)->phy) | GENET_MDIO_REG(reg));
    if (!ge_mii_wait(nic))
        return -1;
    v = ge_rd(nic, GENET_MDIO_CMD);
    if ((v & GE_MDIO_READ_FAIL) != 0)
        return -1;
    return (LONG)(v & 0xffffUL);
}

static BOOL ge_mii_write(NetdevNic *nic, UBYTE reg, UWORD val)
{
    ge_wr(nic, GENET_MDIO_CMD,
          (ULONG)val | GENET_MDIO_WRITE | GENET_MDIO_START_BUSY |
          GENET_MDIO_PMD(GE(nic)->phy) | GENET_MDIO_REG(reg));
    return ge_mii_wait(nic);
}

/* --------------------------------------------------------------- cache --- */

/*
 * WHAT A CACHE OPERATION COSTS ON EMU68, measured on the A1200 (cachebench2,
 * 2026-09-15), because it decides the shape of this whole core:
 *
 *     CacheClearE, any length, cache clean            43 us
 *     CacheClearE, any length, 512 KB dirty          290 us   (whole-cache)
 *     cpushp %dc,(a0), one 4 KB page, Supervisor()   1.7 us   (a range op)
 *     cpushl %dc,(a0), one 16-byte line               1.8 us   (trap-bound)
 *
 * Exec's CacheClearE is a whole-cache clean-and-invalidate whatever range it
 * is given, and under traffic the cache is full of the stack's dirty lines,
 * so every call pays for all of them.  The 68040 page push is translated by
 * Emu68 into a range operation over that page alone and costs the trap into
 * supervisor mode plus a microsecond.  So the rings are pushed a page at a
 * time from one supervisor entry: a burst of N frames is one trap and N/2
 * pages.
 *
 * Clean AND invalidate on both sides: a transmit buffer must reach memory
 * before the chip reads it, a receive buffer's stale lines must go before the
 * CPU reads what the chip wrote, and cpushp does both, so the receive side
 * never risks throwing away a dirty line that belongs to someone else on the
 * same page.  The ring buffers are page-multiples, so no page is shared with
 * anything but ring buffers anyway.
 *
 * Only a 68040-or-better executes cpushp; anything else takes CacheClearE.
 * The GENET is only ever behind Emu68, which emulates a 68040, so the fall
 * back is for the one machine that lied about its CPU.
 *
 * cpushp is privileged on a real 68040.  It therefore always enters through
 * Exec's Supervisor(), even on Emu68 versions which currently execute the
 * instruction from user mode.  Depending on that emulator behaviour made the
 * driver invalid on a conforming 68040 and vulnerable to an Emu68 fix.
 */
#define GE_PAGE         4096UL

__asm__(
"    .text\n"
"    .arch 68040\n"
"    .globl _ge_sup_cpushp\n"
"_ge_sup_cpushp:\n"
"1:  cpushp %dc,(%a0)\n"
"    add.l #4096,%a0\n"
"    subq.l #1,%d0\n"
"    bne 1b\n"
"    rte\n"
);
extern VOID ge_sup_cpushp(VOID);

/* Supervisor(): the routine runs in supervisor mode and ends in RTE. */
static VOID ge_sup_pages(ULONG first, ULONG pages)
{
    register ULONG            _d0 __asm("d0") = pages;
    register ULONG            _a0 __asm("a0") = first;
    register VOID           (*_a5)(VOID) __asm("a5") = ge_sup_cpushp;
    register struct ExecBase *_a6 __asm("a6") = SysBase;

    __asm__ __volatile__ ("jsr a6@(-30:W)"
                          : "+r" (_d0), "+r" (_a0)
                          : "r" (_a5), "r" (_a6)
                          : "cc", "memory", "d1", "a1");
}

static VOID ge_cache(NetdevNic *nic, APTR addr, ULONG len)
{
    GenetCore *c = GE(nic);

    if (!c->cache)
        return;
    nic->core_stat[GE_ST_CACHE]++;

    if (c->pageops)
    {
        ULONG first = (ULONG)addr & ~(GE_PAGE - 1UL);
        ULONG last  = ((ULONG)addr + len - 1UL) & ~(GE_PAGE - 1UL);
        ULONG pages = (last - first) / GE_PAGE + 1UL;

        ge_sup_pages(first, pages);
        return;
    }

    CacheClearE(addr, len, CACRF_ClearD);
}

/* ---------------------------------------------------------------- link --- */

static VOID ge_apply_link(NetdevNic *nic, UBYTE speed)
{
    ULONG v;

    v = ge_rd(nic, GENET_EXT_RGMII_OOB_CTRL);
    v &= ~GENET_EXT_RGMII_OOB_OOB_DISABLE;
    v |= GENET_EXT_RGMII_OOB_RGMII_LINK | GENET_EXT_RGMII_OOB_RGMII_MODE_EN;
    /* rgmii-rxid: the PHY supplies the receive delay and the MAC keeps its
       internal transmit one, so the ID-mode-disable bit stays clear. */
    v &= ~GENET_EXT_RGMII_OOB_ID_MODE_DISABLE;
    ge_wr(nic, GENET_EXT_RGMII_OOB_CTRL, v);

    v = ge_rd(nic, GENET_UMAC_CMD);
    v = (v & ~GENET_UMAC_CMD_SPEED_MASK) | speed;
    ge_wr(nic, GENET_UMAC_CMD, v);
}

/*
 * Ask the PHY.  BMSR's link bit is latched low, so a first read after a drop
 * reports the drop and a second the present state; the second is what counts.
 * The negotiated speed comes from the Broadcom auxiliary status register,
 * which says what was resolved without decoding both sides' advertisements.
 */
static VOID ge_link_poll(NetdevNic *nic)
{
    GenetCore *c = GE(nic);
    LONG       bmsr;
    UBYTE      up;
    UBYTE      speed = c->speed;

    if (c->in_mdio)
        return;
    c->in_mdio = 1;

    bmsr = ge_mii_read(nic, MII_BMSR);
    if (bmsr >= 0 && (bmsr & BMSR_LINK) == 0)
        bmsr = ge_mii_read(nic, MII_BMSR);
    up = (UBYTE)(bmsr >= 0 && (bmsr & BMSR_LINK) != 0);

    if (up)
    {
        LONG aux = ge_mii_read(nic, BRGPHY_MII_AUXSTS);

        if (aux >= 0)
        {
            switch (aux & BRGPHY_AUXSTS_AN_RES)
            {
            case BRGPHY_RES_1000FD:
            case BRGPHY_RES_1000HD:
                speed = GENET_UMAC_CMD_SPEED_1000;
                break;
            case BRGPHY_RES_100FD:
            case BRGPHY_RES_100T4:
            case BRGPHY_RES_100HD:
                speed = GENET_UMAC_CMD_SPEED_100;
                break;
            default:
                speed = GENET_UMAC_CMD_SPEED_10;
                break;
            }
        }
    }

    if (up != c->link || (up && speed != c->speed))
    {
        GE_TRACE("ge: link ", ((ULONG)up << 8) | speed);
        c->link  = up;
        c->speed = speed;
        nic->core_stat[GE_ST_LINK] = up ? (ULONG)(speed >> 2) + 1UL : 0UL;
        if (up)
            ge_apply_link(nic, speed);
    }

    c->in_mdio = 0;
}

/*
 * The BCM54213PE's RGMII delays, once, at attach: the receive clock skew on
 * (the tree says rgmii-rxid), the transmit clock delay off.  Both live behind
 * shadow registers reached through AUXCTL (0x18) and register 0x1c: select
 * the shadow, read, mask the data bits, write with the write-enable bit.
 */
#define BCM54_AUXCTL            0x18
#define  BCM54_AUXCTL_SHD_MISC  0x0007
#define  BCM54_AUXCTL_MISC_RD   (BCM54_AUXCTL_SHD_MISC << 12)
#define  BCM54_AUXCTL_MISC_WREN 0x8000
#define  BCM54_AUXCTL_MISC_DATA 0x7ff8
#define  BCM54_AUXCTL_MISC_RXSKEW 0x0200
#define BCM54_SHD1C             0x1c
#define  BCM54_SHD1C_CLKCTRL    (0x03 << 10)
#define  BCM54_SHD1C_WREN       0x8000
#define  BCM54_SHD1C_DATA       0x03ff
#define  BCM54_SHD1C_GTXCLK     0x0200

static VOID ge_phy_delays(NetdevNic *nic)
{
    LONG v;

    if (!ge_mii_write(nic, BCM54_AUXCTL,
                      BCM54_AUXCTL_SHD_MISC | BCM54_AUXCTL_MISC_RD))
        return;
    v = ge_mii_read(nic, BCM54_AUXCTL);
    if (v < 0)
        return;
    v = (v & BCM54_AUXCTL_MISC_DATA) | BCM54_AUXCTL_MISC_RXSKEW;
    (VOID)ge_mii_write(nic, BCM54_AUXCTL,
                       (UWORD)(BCM54_AUXCTL_MISC_WREN |
                               BCM54_AUXCTL_SHD_MISC | v));

    if (!ge_mii_write(nic, BCM54_SHD1C, BCM54_SHD1C_CLKCTRL))
        return;
    v = ge_mii_read(nic, BCM54_SHD1C);
    if (v < 0)
        return;
    v = (v & BCM54_SHD1C_DATA) & ~BCM54_SHD1C_GTXCLK;
    (VOID)ge_mii_write(nic, BCM54_SHD1C,
                       (UWORD)(BCM54_SHD1C_WREN | BCM54_SHD1C_CLKCTRL | v));
}

/* --------------------------------------------------------------- reset --- */

/* NetBSD's genet_reset: flush the receive buffer, soft-reset the UniMAC,
   clear the MIB, set the frame limit and the 2-byte receive shift. */
static VOID ge_reset(NetdevNic *nic)
{
    ULONG v;

    v = ge_rd(nic, GENET_SYS_RBUF_FLUSH_CTRL);
    ge_wr(nic, GENET_SYS_RBUF_FLUSH_CTRL, v | GENET_SYS_RBUF_FLUSH_RESET);
    ge_delay_us(nic, 10);
    ge_wr(nic, GENET_SYS_RBUF_FLUSH_CTRL, v & ~GENET_SYS_RBUF_FLUSH_RESET);
    ge_delay_us(nic, 10);
    ge_wr(nic, GENET_SYS_RBUF_FLUSH_CTRL, 0);
    ge_delay_us(nic, 10);

    ge_wr(nic, GENET_UMAC_CMD, 0);
    ge_wr(nic, GENET_UMAC_CMD,
          GENET_UMAC_CMD_LCL_LOOP_EN | GENET_UMAC_CMD_SW_RESET);
    ge_delay_us(nic, 10);
    ge_wr(nic, GENET_UMAC_CMD, 0);

    ge_wr(nic, GENET_UMAC_MIB_CTRL,
          GENET_UMAC_MIB_RESET_RUNT | GENET_UMAC_MIB_RESET_RX |
          GENET_UMAC_MIB_RESET_TX);
    ge_wr(nic, GENET_UMAC_MIB_CTRL, 0);

    ge_wr(nic, GENET_UMAC_MAX_FRAME_LEN, 1536);

    /*
     * SET, NOT OR'ED IN.  These two registers survive the UniMAC and RBUF
     * resets above and a warm reboot: whatever the previous driver left in
     * them is what this one finds.  Or'ing kept a stale RBUF_64B_EN, and a
     * driver without the status block then read 64 bytes of it as every
     * frame's Ethernet header -- link up, "unknown types" climbing, DHCP
     * never answered, on the A1200 on 2026-09-17 after a test build of
     * this driver, and the shape of the report from the field where
     * genet.device had run before anxgenet.device.  genet_stop() clears
     * them for the same reason in the other direction.
     */
    v = ge_rd(nic, GENET_RBUF_CTRL);
    v &= ~(GENET_RBUF_64B_EN | GENET_RBUF_ALIGN_2B | GENET_RBUF_BAD_DIS);
    ge_wr(nic, GENET_RBUF_CTRL, v | GENET_RBUF_ALIGN_2B | GENET_RBUF_64B_EN);
    v = ge_rd(nic, GENET_RBUF_CHK_CTRL);
    v &= ~(GENET_RBUF_RXCHK_EN | GENET_RBUF_SKIP_FCS | GENET_RBUF_L3_PARSE_DIS);
    ge_wr(nic, GENET_RBUF_CHK_CTRL,
          v | GENET_RBUF_RXCHK_EN | GENET_RBUF_L3_PARSE_DIS);
    /* The transmit status block, for the checksum the TBUF finishes; the
       same bit, the same reason to set it outright and clear it in stop. */
    v = ge_rd(nic, GENET_TBUF_CTRL);
    ge_wr(nic, GENET_TBUF_CTRL, v | GENET_RBUF_64B_EN);

    ge_wr(nic, GENET_RBUF_TBUF_SIZE_CTRL, 1);
}

/* -------------------------------------------------------------- filter --- */

/*
 * The UniMAC filters on up to 17 exact addresses and has no hash and no
 * all-multicast mode.  Slot 0 is broadcast, slot 1 our own address, and the
 * unit's exact multicast table follows; a table that does not fit, a range
 * too wide for it, or an opener that asked for everything all turn the
 * filter off.
 */
static VOID ge_mdf_set(NetdevNic *nic, UWORD slot, const UBYTE *ea)
{
    ge_wr(nic, GENET_UMAC_MDF_ADDR0(slot), ((ULONG)ea[0] << 8) | ea[1]);
    ge_wr(nic, GENET_UMAC_MDF_ADDR1(slot),
          ((ULONG)ea[2] << 24) | ((ULONG)ea[3] << 16) |
          ((ULONG)ea[4] << 8) | ea[5]);
}

static VOID genet_setfilter(NetdevNic *nic)
{
    static const UBYTE bcast[NETDEV_ADDR_LEN] =
        { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    ULONG cmd  = ge_rd(nic, GENET_UMAC_CMD);
    UWORD slot = 0;
    UWORD i;
    BOOL  all  = (BOOL)(nic->promisc || nic->all_multi);

    if (!all && nic->mc_table != NULL)
    {
        UWORD n = 2;

        for (i = 0; i < nic->mc_max; i++)
            if (nic->mc_table[i].refs != 0)
                n++;
        if (n > GENET_MAX_MDF_FILTER)
            all = TRUE;
    }

    if (all)
    {
        ge_wr(nic, GENET_UMAC_CMD, cmd | GENET_UMAC_CMD_PROMISC);
        ge_wr(nic, GENET_UMAC_MDF_CTRL, 0);
        return;
    }

    ge_mdf_set(nic, slot++, bcast);
    ge_mdf_set(nic, slot++, nic->mac);
    if (nic->mc_table != NULL)
    {
        for (i = 0; i < nic->mc_max; i++)
            if (nic->mc_table[i].refs != 0)
                ge_mdf_set(nic, slot++, nic->mc_table[i].addr);
    }

    /* Slot k is enabled by bit (16 - k): the top `slot` bits of 17. */
    ge_wr(nic, GENET_UMAC_CMD, cmd & ~GENET_UMAC_CMD_PROMISC);
    ge_wr(nic, GENET_UMAC_MDF_CTRL,
          ((1UL << GENET_MAX_MDF_FILTER) - 1UL) &
          ~((1UL << (GENET_MAX_MDF_FILTER - slot)) - 1UL));
}

/* --------------------------------------------------------------- rings --- */

static VOID ge_init_rings(NetdevNic *nic)
{
    GenetCore *c = GE(nic);
    ULONG      v;
    UWORD      i;

    /* Transmit: one queue, GE_TX_RING descriptors from descriptor 0. */
    c->tx_cidx = c->tx_pidx = c->tx_kicked = 0;
    nic->txb_inuse = 0;

    ge_wr(nic, GENET_TX_SCB_BURST_SIZE, 0x08);
    ge_wr(nic, GENET_TX_DMA_READ_PTR_LO(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_READ_PTR_HI(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_CONS_INDEX(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_PROD_INDEX(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_RING_BUF_SIZE(GE_Q),
          ((ULONG)GE_TX_RING << 16) | GE_BUFSZ);
    ge_wr(nic, GENET_TX_DMA_START_ADDR_LO(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_START_ADDR_HI(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_END_ADDR_LO(GE_Q),
          GE_TX_RING * GENET_DMA_DESC_SIZE / 4 - 1);
    ge_wr(nic, GENET_TX_DMA_END_ADDR_HI(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_FLOW_PERIOD(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_WRITE_PTR_LO(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_WRITE_PTR_HI(GE_Q), 0);
    /* No completion interrupt is taken (see GE_IRQ_WANTED); the threshold
       is written for the register's sake. */
    ge_wr(nic, GENET_TX_DMA_MBUF_DONE_THRES(GE_Q), 16);
    ge_wr(nic, GENET_TX_DMA_RING_CFG, 1UL << GE_Q);
    v = ge_rd(nic, GENET_TX_DMA_CTRL);
    ge_wr(nic, GENET_TX_DMA_CTRL,
          v | GENET_TX_DMA_CTRL_EN | GENET_TX_DMA_CTRL_RBUF_EN(GE_Q));

    /* Receive: every descriptor points at its own buffer for good. */
    c->rx_cidx  = 0;
    c->rx_clean = 0;
    nic->rx_behind = 0;
    for (i = 0; i < GE_RX_RING; i++)
    {
        ge_wr(nic, GENET_RX_DESC_ADDRESS_LO(i),
              (ULONG)(c->rx_buf + (ULONG)i * GE_BUFSZ));
        ge_wr(nic, GENET_RX_DESC_ADDRESS_HI(i), 0);
        ge_wr(nic, GENET_RX_DESC_STATUS(i), 0);
    }

    ge_wr(nic, GENET_RX_SCB_BURST_SIZE, 0x08);
    ge_wr(nic, GENET_RX_DMA_WRITE_PTR_LO(GE_Q), 0);
    ge_wr(nic, GENET_RX_DMA_WRITE_PTR_HI(GE_Q), 0);
    ge_wr(nic, GENET_RX_DMA_PROD_INDEX(GE_Q), 0);
    ge_wr(nic, GENET_RX_DMA_CONS_INDEX(GE_Q), 0);
    ge_wr(nic, GENET_RX_DMA_RING_BUF_SIZE(GE_Q),
          ((ULONG)GE_RX_RING << 16) | GE_BUFSZ);
    ge_wr(nic, GENET_RX_DMA_START_ADDR_LO(GE_Q), 0);
    ge_wr(nic, GENET_RX_DMA_START_ADDR_HI(GE_Q), 0);
    ge_wr(nic, GENET_RX_DMA_END_ADDR_LO(GE_Q),
          GE_RX_RING * GENET_DMA_DESC_SIZE / 4 - 1);
    ge_wr(nic, GENET_RX_DMA_END_ADDR_HI(GE_Q), 0);
    /* Pause the wire at 5 free buffers, resume at a sixteenth of the ring. */
    ge_wr(nic, GENET_RX_DMA_XON_XOFF_THRES(GE_Q),
          (5UL << 16) | (GE_RX_RING >> 4));
    ge_wr(nic, GENET_RX_DMA_READ_PTR_LO(GE_Q), 0);
    ge_wr(nic, GENET_RX_DMA_READ_PTR_HI(GE_Q), 0);
    /*
     * The reference driver says 10 frames and 57 us; at the rates this
     * machine receives (100 Mbit/s is a frame every 115 us) that was an
     * interrupt per frame.  GE_RX_COALESCE_* above says what was measured.
     */
    ge_wr(nic, GENET_RX_DMA_MBUF_DONE_THRES(GE_Q), GE_RX_COALESCE_FRAMES);
    v = ge_rd(nic, GENET_RX_DMA_RING_TIMEOUT(GE_Q));
    ge_wr(nic, GENET_RX_DMA_RING_TIMEOUT(GE_Q),
          (v & ~GENET_DMA_RING_TIMEOUT_MASK) | GE_RX_COALESCE_TICKS);
    ge_wr(nic, GENET_RX_DMA_RING_CFG, 1UL << GE_Q);
    v = ge_rd(nic, GENET_RX_DMA_CTRL);
    ge_wr(nic, GENET_RX_DMA_CTRL,
          v | GENET_RX_DMA_CTRL_EN | GENET_RX_DMA_CTRL_RBUF_EN(GE_Q));
}

/* ------------------------------------------------------------ init/stop --- */

#define GE_IRQ_LINK_UP      (1UL << 4)
#define GE_IRQ_LINK_DOWN    (1UL << 5)
/*
 * No transmit-completion interrupt.  A completed transmit needs nothing
 * from the CPU but a counter, and that counter is read on the next
 * transmit, on every receive interrupt and on every vertical blank, so the
 * ring is reclaimed wherever it is next needed.  At a frame every 100 us the
 * interrupt was a third of all of them.
 */
#define GE_IRQ_WANTED       (GENET_IRQ_RXDMA_DONE | \
                             GE_IRQ_LINK_UP | GE_IRQ_LINK_DOWN)

static LONG genet_init(NetdevNic *nic)
{
    GenetCore *c = GE(nic);
    ULONG      v;

    /*
     * The MAC is reset here and not at attach: attach runs at the probe,
     * which happens whenever the device is loaded, and on this machine
     * another driver may be holding the very same MAC up at that moment.  A
     * probe that only reads leaves it alone; going online is the claim.
     *
     * Both DMA engines are stopped FIRST.  A warm reboot resets the 68k and
     * nothing on the Pi: the previous driver's rings were still running,
     * their producer and consumer indices were wherever it left them, and a
     * write to a running engine's index is not honoured.  The first version
     * of this core learned that as 25,888 "received" frames of stale
     * descriptors before the ring counters met.
     */
    ge_dma_stop(nic);
    ge_reset(nic);

    if (!c->pre_saved)
    {
        /* What the next driver will find at genet_stop: see there. */
        c->pre_rgmii_oob = ge_rd(nic, GENET_EXT_RGMII_OOB_CTRL);
        c->pre_saved     = 1;
    }

    if (!c->phy_set && c->phyid != 0xffffffffUL)
    {
        LONG bmsr;

        ge_phy_delays(nic);
        /* A PHY that has already negotiated a link keeps it: a restart
           costs two to three seconds of no link, which is a DHCP timeout
           on a fast machine.  One that has not is told to start. */
        bmsr = ge_mii_read(nic, MII_BMSR);
        if (bmsr < 0 ||
            (bmsr & (BMSR_LINK | BMSR_ACOMP)) != (BMSR_LINK | BMSR_ACOMP))
            (VOID)ge_mii_write(nic, MII_BMCR, BMCR_AUTOEN | BMCR_STARTNEG);
        c->phy_set = 1;
    }

    ge_wr(nic, GENET_SYS_PORT_CTRL, GENET_SYS_PORT_MODE_EXT_GPHY);

    ge_wr(nic, GENET_UMAC_MAC0,
          ((ULONG)nic->mac[0] << 24) | ((ULONG)nic->mac[1] << 16) |
          ((ULONG)nic->mac[2] << 8) | nic->mac[3]);
    ge_wr(nic, GENET_UMAC_MAC1, ((ULONG)nic->mac[4] << 8) | nic->mac[5]);

    genet_setfilter(nic);
    ge_init_rings(nic);

    v = ge_rd(nic, GENET_UMAC_CMD);
    ge_wr(nic, GENET_UMAC_CMD, v | GENET_UMAC_CMD_TXEN | GENET_UMAC_CMD_RXEN);

    /* Interrupts: clear whatever is pending, then unmask the four wanted --
       only when a server will answer them.  With no interrupt line the
       source stays masked and the vertical blank is the whole service. */
    ge_wr(nic, GENET_INTRL2_CPU_SET_MASK, 0xffffffffUL);
    ge_wr(nic, GENET_INTRL2_CPU_CLEAR, 0xffffffffUL);
    if (nic->dt_irq != 0)
        ge_wr(nic, GENET_INTRL2_CPU_CLEAR_MASK, GE_IRQ_WANTED);

    c->irq_pending = 0;
    nic->running = TRUE;
    c->link = 0;
    ge_link_poll(nic);

    nic->core_stat[GE_ST_RXPROD0] =
        ge_rd(nic, GENET_RX_DMA_PROD_INDEX(GE_Q)) & 0xffffUL;
    nic->core_stat[GE_ST_TXCONS0] =
        ge_rd(nic, GENET_TX_DMA_CONS_INDEX(GE_Q)) & 0xffffUL;

    GE_TRACE("ge: running cmd ", ge_rd(nic, GENET_UMAC_CMD));
    return 0;
}

/* The receiver off, both DMA engines off, the transmit FIFO flushed. */
static VOID ge_dma_stop(NetdevNic *nic)
{
    ULONG v;

    v = ge_rd(nic, GENET_UMAC_CMD);
    ge_wr(nic, GENET_UMAC_CMD, v & ~GENET_UMAC_CMD_RXEN);

    v = ge_rd(nic, GENET_RX_DMA_CTRL);
    ge_wr(nic, GENET_RX_DMA_CTRL,
          v & ~(GENET_RX_DMA_CTRL_EN | GENET_RX_DMA_CTRL_RBUF_EN(GE_Q)));
    v = ge_rd(nic, GENET_TX_DMA_CTRL);
    ge_wr(nic, GENET_TX_DMA_CTRL,
          v & ~(GENET_TX_DMA_CTRL_EN | GENET_TX_DMA_CTRL_RBUF_EN(GE_Q)));
    ge_delay_us(nic, 100);          /* a burst in flight lands first */

    ge_wr(nic, GENET_UMAC_TX_FLUSH, 1);
    ge_delay_us(nic, 10);
    ge_wr(nic, GENET_UMAC_TX_FLUSH, 0);

    v = ge_rd(nic, GENET_UMAC_CMD);
    ge_wr(nic, GENET_UMAC_CMD, v & ~GENET_UMAC_CMD_TXEN);

    /*
     * THE RING POINTERS GO BACK TO ZERO, FOR THE NEXT DRIVER'S SAKE.
     *
     * The engine keeps two positions per ring: a 16-bit index the driver
     * reads, and a word pointer into the descriptor block that only it
     * advances, wrapping at the ring's END.  They agree at power-on and
     * they stay in step for one ring size.  This core's ring is 128
     * descriptors; genet.device 3.14's is 256, and on its start it takes
     * the producer index as it finds it and never writes the pointer, which
     * is what a driver following its own previous run can get away with.
     * Following this one it could not: measured on the A1200, index $CDCD
     * against a pointer at descriptor 63, every frame landing where the
     * other driver was not looking -- link up, IPv6 from the multicast
     * that got through, no DHCP, 11% "bad data" -- and a warm reboot does
     * not clear it.  Both positions zeroed with the engines stopped is the
     * state every driver starts from cleanly, ours included.
     */
    ge_wr(nic, GENET_RX_DMA_WRITE_PTR_LO(GE_Q), 0);
    ge_wr(nic, GENET_RX_DMA_WRITE_PTR_HI(GE_Q), 0);
    ge_wr(nic, GENET_RX_DMA_READ_PTR_LO(GE_Q), 0);
    ge_wr(nic, GENET_RX_DMA_READ_PTR_HI(GE_Q), 0);
    ge_wr(nic, GENET_RX_DMA_PROD_INDEX(GE_Q), 0);
    ge_wr(nic, GENET_RX_DMA_CONS_INDEX(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_READ_PTR_LO(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_READ_PTR_HI(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_WRITE_PTR_LO(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_WRITE_PTR_HI(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_CONS_INDEX(GE_Q), 0);
    ge_wr(nic, GENET_TX_DMA_PROD_INDEX(GE_Q), 0);
}

static VOID genet_stop(NetdevNic *nic)
{
    GenetCore *c = GE(nic);
    ULONG      v;

    ge_dma_stop(nic);

    /*
     * HANDED BACK AS THE POWER-ON RESET LEFT IT, not as this driver had it.
     * The stop above is what the next boot needs -- no DMA engine writing
     * into its RAM -- but a warm reboot from here goes on to load whichever
     * driver the user runs, and genet.device 3.14 twice came up with the
     * link up, frames arriving and NOT ONE DHCP DISCOVER reaching the wire
     * (2026-09-15 morning and 23:00), until its own close and re-open put
     * the MAC through a full cycle; a cold boot never did that, and a cold
     * boot hands it the chip in reset.  So: the MAC's TX and RX off, the
     * receive buffer and the UniMAC through their resets (the same sequence
     * genet_init starts with, NetBSD's), the address filter off, every
     * interrupt masked and cleared.  What the other driver assumes about
     * the chip it finds is then what it assumed when it worked.
     *
     * AND THE PHY, which no MAC reset touches.  genet_init writes the
     * BCM54213's receive skew on and its transmit clock delay off
     * (ge_phy_delays) and pairs that with EXT_RGMII_OOB_CTRL on the MAC
     * side; a driver that inherits the PHY half of that with its own MAC
     * half has a link that is up and frames that do not arrive, or do not
     * leave.  mja65, 2026-09-18, 0.28.5: anxgenet.device, NetShutdown,
     * genet.device, "cannot pick up an IP address" until a reboot.  A PHY
     * soft reset (BMCR_RESET) puts every PHY register, the shadow ones
     * included, back to its defaults and restarts negotiation -- the state
     * a cold boot presents -- and the pad control goes back to the value
     * read before this driver's first change.  Done while the MAC still
     * runs: MDIO is the UniMAC's, and the UniMAC is reset below.
     */
    if (c->phy_set)
    {
        (VOID)ge_mii_write(nic, MII_BMCR, BMCR_RESET);
        {
            UWORD i;

            /* The bit clears itself when the reset is done: under a
               millisecond on this PHY, ten at the outside. */
            for (i = 0; i < 100; i++)
            {
                LONG bmcr = ge_mii_read(nic, MII_BMCR);

                if (bmcr < 0 || (bmcr & BMCR_RESET) == 0)
                    break;
                ge_delay_us(nic, 100);
            }
        }
        c->phy_set = 0;                 /* genet_init sets it up again */
    }
    if (c->pre_saved)
        ge_wr(nic, GENET_EXT_RGMII_OOB_CTRL, c->pre_rgmii_oob);

    ge_wr(nic, GENET_UMAC_CMD, 0);

    v = ge_rd(nic, GENET_SYS_RBUF_FLUSH_CTRL);
    ge_wr(nic, GENET_SYS_RBUF_FLUSH_CTRL, v | GENET_SYS_RBUF_FLUSH_RESET);
    ge_delay_us(nic, 10);
    ge_wr(nic, GENET_SYS_RBUF_FLUSH_CTRL, v & ~GENET_SYS_RBUF_FLUSH_RESET);
    ge_delay_us(nic, 10);
    ge_wr(nic, GENET_SYS_RBUF_FLUSH_CTRL, 0);
    ge_delay_us(nic, 10);

    ge_wr(nic, GENET_UMAC_CMD,
          GENET_UMAC_CMD_LCL_LOOP_EN | GENET_UMAC_CMD_SW_RESET);
    ge_delay_us(nic, 10);
    ge_wr(nic, GENET_UMAC_CMD, 0);

    /* The status block and the checksum off again: neither reset above
       touches them, and a driver that does not know them would read the
       block as the frame (genet_init). */
    v = ge_rd(nic, GENET_RBUF_CTRL);
    ge_wr(nic, GENET_RBUF_CTRL, v & ~GENET_RBUF_64B_EN);
    v = ge_rd(nic, GENET_RBUF_CHK_CTRL);
    ge_wr(nic, GENET_RBUF_CHK_CTRL,
          v & ~(GENET_RBUF_RXCHK_EN | GENET_RBUF_L3_PARSE_DIS));
    v = ge_rd(nic, GENET_TBUF_CTRL);
    ge_wr(nic, GENET_TBUF_CTRL, v & ~GENET_RBUF_64B_EN);

    ge_wr(nic, GENET_UMAC_MDF_CTRL, 0);

    ge_wr(nic, GENET_INTRL2_CPU_SET_MASK, 0xffffffffUL);
    ge_wr(nic, GENET_INTRL2_CPU_CLEAR, 0xffffffffUL);

    nic->running   = FALSE;
    nic->txb_inuse = 0;
}

/*
 * The watchdog's recovery: stop and start.  Both halves are register writes
 * and short waits, which is what the vertical blank can afford.
 */
static VOID genet_reset(NetdevNic *nic)
{
    nic->resets++;
    genet_stop(nic);
    (VOID)genet_init(nic);
}

/* ------------------------------------------------------------- receive --- */

/*
 * Copy `count` longwords and return their ones-complement sum: the contract of
 * src/net68k's n68k_copy_sum_longwords(), written out here because that
 * routine lives behind the stack's headers and this device links none of
 * them.  A plain loop, and on Emu68's JIT -- the only place a GENET is --
 * that is what the movem version would become anyway.
 */
/*
 * WHAT THE COPY'S SUM IS WORTH ONCE THE HEADERS ARE IN CACHE.
 *
 * The copy has just moved the IP packet and the chip supplied the ones-complement
 * sum of every longword of it, which is also the ones-complement sum of its
 * sixteen-bit words.  The IP header's own sum is twenty bytes of that, the
 * TCP or UDP checksum wants the rest plus a pseudo header of the two
 * addresses, the protocol and the transport length, and both answers are
 * "0xffff or not".  Some forty adds, on words already in cache, against the
 * stack walking the frame again or the checksum being trusted blind.
 *
 * Refused, so that VERIFIED is never set on a frame it does not describe:
 * anything but IPv4 with a twenty-byte header; a fragment; an IP total
 * length that is not the whole payload (Ethernet padding is summed and is
 * not the datagram's); a protocol other than TCP or UDP; a UDP checksum of
 * zero, which means "none".
 *
 * The verifier is shared with the classic direct paths.  Stream matching is
 * now performed by the SANA-II receive layer, outside this driver's masked
 * service path, so an ordinary driver gets the same GRO behaviour.
 */
/* FALSE when the frame was left in the ring: the opener that reads this type
   has no read posted right now (NETDEV_CLAIM_BEHIND), and holding the frame
   until it does is what a 128-deep ring is for.  TRUE otherwise, whether the
   frame was claimed, staged or dropped. */
static BOOL ge_deliver(NetdevNic *nic, const UBYTE *frame, UWORD len)
{
    APTR   token = NULL;
    UBYTE  wanted = 0;
    GE_P_START(p1);
    UBYTE *dst   = (nic->rx_claim != NULL)
                 ? nic->rx_claim(nic->rx_arg, frame, len, &token, &wanted)
                 : NULL;
    GE_P_ADD(nic, GE_ST_P_CLAIM_US, p1);

    /*
     * UNICAST ONLY, AND NOT FOREVER.  The ring is one queue for every type,
     * so a frame held for one reader stands in front of every other reader's
     * frames.  A broadcast or multicast frame (an ARP request, a neighbour
     * solicitation) whose reader has fallen behind is dropped the way every
     * busy host drops one: its reader is planned two reads deep and a third
     * within one burst would otherwise stall a transfer for another reader
     * to wake and poll -- measured as 173 blank-resumed holds and 45
     * spurious retransmissions in ten seconds.  A unicast frame is data
     * somebody is waiting for and is worth the wait; if the wait passes
     * GE_HOLD_BLANKS the reader is stuck and the frame is dropped after all
     * (genet_tick), so a dead reader costs its own frames and not the wire.
     */
    if (dst == NULL && token == NETDEV_CLAIM_BEHIND &&
        (frame[0] & 1) == 0 && !GE(nic)->drop_held)
        return FALSE;
    GE(nic)->drop_held = 0;

    nic->rx_packets++;

    if (dst != NULL)
    {
        /*
         * Payload to the opener's slot with the movem copy, and the
         * ones-complement sum the stack's verifier wants from the chip: the
         * RBUF's checksum block (RBUF_RXCHK_EN, RBUF_L3_PARSE_DIS) sums the
         * frame from the end of its Ethernet header to its end -- exactly
         * the bytes `src[0..plen)` are -- and writes it into the status
         * block in front of the frame: the low half of the little-endian
         * word at 8, so the two bytes there read in little-endian order are
         * the folded sum.  Checked on the A1200, 2026-09-17: equal to the software sum
         * for 529,799 of 529,799 frames.  A summing copy cost 1.7 us a
         * segment in its best form (movem feeding an addx chain) and 4.3 in
         * its worst; the plain copy is 0.4.  frame + 14 is on a longword --
         * the buffer is page-aligned, the status block is 64 bytes and the
         * chip shifted the frame by two -- and the slot's payload pointer
         * is aligned by construction.
         */
        UWORD        plen = (UWORD)(len - NETDEV_HDR_LEN);
        UWORD        bulk = (UWORD)(plen & (UWORD)~3u);
        const UBYTE *src  = frame + NETDEV_HDR_LEN;
        ULONG        sum  = (ULONG)__builtin_bswap16(*(const UWORD *)(CONST_APTR)
                            (frame - GE_RX_HEAD + GENET_RX_STATUS64_CSUM));
        UWORD        i;
        GE_P_START(p2);

        if (bulk != 0)
            n68k_copy_longs(dst, src, (ULONG)(bulk >> 2));
        for (i = bulk; i < plen; i++)
            dst[i] = src[i];

        /*
         * The frame is in cache now, both copies of it.  Check it here, from
         * the sum the copy already produced, and say whether it continues the
         * stream the previous frame belonged to (aminetxduo/anxs2ext.h); the
         * device masks the answer to what the opener asked for.  The
         * ethertype is at frame + 12, big-endian.
         */
        GE_P_ADD(nic, GE_ST_P_COPY_US, p2);
#ifdef GE_PROBE_ST
        {
            /* The chip's sum against a software pass over what was copied,
               folded to sixteen bits: the measurement that put the chip's
               in charge, kept so a doubt can be settled on any frame. */
            ULONG sw = 0;
            ULONG k;

            for (k = 0; k < (ULONG)plen; k += 2)
            {
                ULONG w = ((ULONG)dst[k] << 8) |
                          ((k + 1 < (ULONG)plen) ? dst[k + 1] : 0UL);

                sw += w;
            }
            sw = (sw >> 16) + (sw & 0xffffUL);
            sw = (sw >> 16) + (sw & 0xffffUL);
            sw &= 0xffffUL;
            if (sum == sw)
                nic->core_stat[GE_ST_P_HWSUM_A]++;
            else
                nic->core_stat[GE_ST_P_HWSUM_NE]++;
        }
#endif
        {
            UBYTE flags = ANXD_S2_RXF_SUMMED;
            GE_P_START(p3);

            if ((wanted & ANXD_S2_RXF_VERIFIED) != 0 &&
                frame[12] == 0x08 && frame[13] == 0x00)
            {
                UBYTE v = netdev_rx_verify4(src, plen, sum);

                if (v != 0)
                    nic->core_stat[GE_ST_VERIFIED]++;
                flags |= v;
            }
            else if ((wanted & ANXD_S2_RXF_VERIFIED) != 0 &&
                     frame[12] == 0x86 && frame[13] == 0xdd)
            {
                UBYTE v = netdev_rx_verify6(src, plen, sum);

                if (v != 0)
                    nic->core_stat[GE_ST_VERIFIED]++;
                flags |= v;
            }

            GE_P_ADD(nic, GE_ST_P_VERIFY_US, p3);
            {
                GE_P_START(p4);
                nic->rx_claimed(nic->rx_arg, token, sum, flags);
                GE_P_ADD(nic, GE_ST_P_CLAIMED_US, p4);
            }
        }
        return TRUE;
    }

    /* Handed up where it lies, like the LANCE. */
    nic->core_stat[GE_ST_UNCLAIMED]++;
    if (nic->rx != NULL)
        nic->rx(nic->rx_arg, frame, len);
    return TRUE;
}

/* TRUE when the ring had frames. */
#ifdef GE_PROBE_ST
static BOOL ge_probe_deliver(NetdevNic *nic, const UBYTE *frame, UWORD len)
{
    ULONG t = ge_st_us();
    BOOL  r = ge_deliver(nic, frame, len);

    nic->core_stat[GE_ST_P_DELIVER_US] += ge_st_us() - t;
    nic->core_stat[GE_ST_P_FRAMES]++;
    return r;
}
#else
#define ge_probe_deliver(nic, frame, len)   ge_deliver((nic), (frame), (len))
#endif

static BOOL ge_rxintr(NetdevNic *nic)
{
    GenetCore *c = GE(nic);
    UWORD      pidx  = (UWORD)(ge_rd(nic, GENET_RX_DMA_PROD_INDEX(GE_Q)) & 0xffffu);
    UWORD      total = (UWORD)(pidx - c->rx_cidx);
    UWORD      n;

    if (total == 0)
        return FALSE;
    if (total > GE_RX_RING)
        total = GE_RX_RING;     /* cannot happen: the chip stops on a full ring */

    nic->core_stat[GE_ST_BURSTS]++;
    if (total > nic->core_stat[GE_ST_BURST_MAX])
        nic->core_stat[GE_ST_BURST_MAX] = total;
    /*
     * The DMA wrote these buffers behind the cache: one operation for the
     * whole burst, after the producer index said the writes are complete.
     * The burst is contiguous in the ring unless it wraps, so it is one range
     * or two, and the page op prices a range by its pages.
     *
     * ONLY WHAT IS NEW SINCE THE LAST PASS.  A pass that stopped with frames
     * waiting for a read (below) already did this for them, and the CPU has
     * only read those buffers since; rx_clean is where that pass got to, so
     * a resumed pass pays for the frames that arrived meanwhile and not for
     * the whole backlog again on every poll.
     */
    {
        UWORD done  = (UWORD)(c->rx_clean - c->rx_cidx);
        UWORD fresh;

        if (done > total)
            done = 0;               /* cannot happen; start over if it does */
        fresh = (UWORD)(total - done);
        if (fresh != 0)
        {
            UWORD first = (UWORD)(c->rx_clean & (GE_RX_RING - 1));
            UWORD room  = (UWORD)(GE_RX_RING - first);
            GE_P_START(pc);

            if (fresh <= room)
            {
                ge_cache(nic, c->rx_buf + (ULONG)first * GE_BUFSZ,
                         (ULONG)fresh * GE_BUFSZ);
            }
            else
            {
                ge_cache(nic, c->rx_buf + (ULONG)first * GE_BUFSZ,
                         (ULONG)room * GE_BUFSZ);
                ge_cache(nic, c->rx_buf, (ULONG)(fresh - room) * GE_BUFSZ);
            }
            GE_P_ADD(nic, GE_ST_P_CACHE_US, pc);
        }
        c->rx_clean = pidx;
    }

    nic->rx_behind = 0;

    for (n = 0; n < total; n++)
    {
        UWORD  idx    = (UWORD)(c->rx_cidx & (GE_RX_RING - 1));
        GE_P_START(pd);
        UBYTE *buf    = c->rx_buf + (ULONG)idx * GE_BUFSZ;
        /* The descriptor's status word, from the status block the RBUF
           wrote in front of the frame: memory the cache op above just
           made current, not a register read per frame. */
        ULONG  status = __builtin_bswap32(*(const ULONG *)(CONST_APTR)
                                          (buf + GENET_RX_STATUS64_LENGTH_STATUS));
        UWORD  len    = (UWORD)GENET_RX_DESC_STATUS_BUFLEN(status);
        GE_P_ADD(nic, GE_ST_P_DESC_US, pd);

        if ((status & GENET_RX_DESC_STATUS_ALL_ERRS) != 0)
        {
            nic->rx_errors++;
            if ((status & GENET_RX_DESC_STATUS_OVRUN_ERR) != 0)
                nic->overruns++;
            if ((status & GENET_RX_DESC_STATUS_CRC_ERR) != 0)
                nic->core_stat[GE_ST_RX_CRC]++;
            else
                nic->core_stat[GE_ST_RX_ERR]++;
        }
        else if ((status & (GENET_RX_DESC_STATUS_SOP |
                            GENET_RX_DESC_STATUS_EOP)) !=
                 (GENET_RX_DESC_STATUS_SOP | GENET_RX_DESC_STATUS_EOP) ||
                 len < GE_RX_HEAD + NETDEV_HDR_LEN ||
                 len > GE_RX_HEAD + NETDEV_RXBUF_MAX)
        {
            nic->rx_errors++;
            nic->core_stat[GE_ST_RX_LEN]++;
        }
        else if (!ge_probe_deliver(nic, buf + GE_RX_HEAD, (UWORD)(len - GE_RX_HEAD)))
        {
            /*
             * THE READER IS BEHIND, AND THE RING IS THE BACKLOG.  This frame
             * and everything after it stay where the DMA put them, their
             * descriptors untouched, and the consumer index handed back
             * below stops short of them; the chip keeps filling what is
             * left of the ring.  The pass resumes on the next interrupt, on
             * the opener's ANXD_CMD_RX_POLL once it has re-posted its
             * reads, or on the vertical blank.  Before this, a burst longer
             * than the reads posted lost its tail here, silently: 685
             * frames and 632 retransmissions in one ten-second transfer.
             */
            nic->rx_behind = 1;
            nic->core_stat[GE_ST_HELD]++;
            nic->core_stat[GE_ST_HELD_FRAMES] += (ULONG)(total - n);
            break;
        }

        /* The descriptor is re-armed by rewriting its address, the way the
           reference driver reloads it, before it is handed back. */
        {
            GE_P_START(pr);
            ge_wr(nic, GENET_RX_DESC_ADDRESS_LO(idx), (ULONG)buf);
            ge_wr(nic, GENET_RX_DESC_ADDRESS_HI(idx), 0);
            GE_P_ADD(nic, GE_ST_P_DESC_US, pr);
        }
        c->rx_cidx++;
    }

    ge_wr(nic, GENET_RX_DMA_CONS_INDEX(GE_Q), c->rx_cidx);
    return TRUE;
}

/* ------------------------------------------------------------ transmit --- */

/* Everything written since the last kick goes to the chip, after one clean. */
static VOID ge_tx_kick(NetdevNic *nic)
{
    GenetCore *c = GE(nic);

    UWORD      n;
    UWORD      first;
    UWORD      room;

    if (c->tx_pidx == c->tx_kicked)
        return;

    /* The frames written since the last kick, one range or two. */
    n     = (UWORD)(c->tx_pidx - c->tx_kicked);
    first = (UWORD)(c->tx_kicked & (GE_TX_RING - 1));
    room  = (UWORD)(GE_TX_RING - first);
    {
        GE_P_START(pk);
        if (n <= room)
        {
            ge_cache(nic, c->tx_buf + (ULONG)first * GE_BUFSZ, (ULONG)n * GE_BUFSZ);
        }
        else
        {
            ge_cache(nic, c->tx_buf + (ULONG)first * GE_BUFSZ,
                     (ULONG)room * GE_BUFSZ);
            ge_cache(nic, c->tx_buf, (ULONG)(n - room) * GE_BUFSZ);
        }
        GE_P_ADD(nic, GE_ST_P_TXCACHE_US, pk);
    }
    {
        GE_P_START(pw);
        ge_wr(nic, GENET_TX_DMA_PROD_INDEX(GE_Q), c->tx_pidx);
        GE_P_ADD(nic, GE_ST_P_TXKICK_US, pw);
    }
    c->tx_kicked = c->tx_pidx;
    nic->core_stat[GE_ST_KICKS]++;
}

/* The buffer the next transmit will use, for the shell to frame into. */
static UBYTE *genet_tx_at(NetdevNic *nic)
{
    GenetCore *c = GE(nic);

    if (nic->txb_inuse >= GE_TX_RING)
        return NULL;
    return c->tx_buf + (ULONG)(c->tx_pidx & (GE_TX_RING - 1)) * GE_BUFSZ +
           GE_TX_HEAD;
}

static BOOL ge_txintr(NetdevNic *nic);

static LONG genet_tx_body(NetdevNic *nic, const UBYTE *frame, UWORD len);

static LONG genet_tx(NetdevNic *nic, const UBYTE *frame, UWORD len)
{
    GE_P_START(pt);
    LONG rc = genet_tx_body(nic, frame, len);
    GE_P_ADD(nic, GE_ST_P_TX_US, pt);
    if (GE(nic)->poll_task != NULL)
        ge_poll_wake(nic, GE(nic));
    return rc;
}

static LONG genet_tx_body(NetdevNic *nic, const UBYTE *frame, UWORD len)
{
    GenetCore *c = GE(nic);
    UWORD      idx;
    UBYTE     *slot;
    UBYTE     *buf;
    ULONG      status;
    ULONG      info;

    if (!nic->running)
        return DP8390_TX_OFFLINE;
    /* No completion interrupt: what the chip has finished is found out
       here, one register read, before the ring is called full. */
    if (nic->txb_inuse >= GE_TX_RING)
    {
        (VOID)ge_txintr(nic);
        if (nic->txb_inuse >= GE_TX_RING)
            return DP8390_TX_BUSY;
    }

    if (len < NETDEV_FRAME_MIN)
        len = NETDEV_FRAME_MIN;

    idx  = (UWORD)(c->tx_pidx & (GE_TX_RING - 1));
    slot = c->tx_buf + (ULONG)idx * GE_BUFSZ;
    buf  = slot + GE_TX_HEAD;

    if (frame != buf)
    {
        UWORD bulk = (UWORD)(len & (UWORD)~3u);
        UWORD i;

#ifdef GE_PROBE_ST
        nic->core_stat[GE_ST_P_TX_COPIES]++;
#endif
        if (bulk != 0)
            n68k_copy_longs(buf, frame, (ULONG)(bulk >> 2));
        for (i = bulk; i < len; i++)
            buf[i] = frame[i];
    }

    /*
     * The transport checksum, by the TBUF: the opener's negotiated per-write
     * metadata has put the pseudo-header sum in the field, and the header
     * offsets are in the frame itself.
     * Anything else gets a zero word, which the block ignores.
     */
    status = GENET_TX_DESC_STATUS_SOP | GENET_TX_DESC_STATUS_EOP |
             GENET_TX_DESC_STATUS_CRC | GENET_TX_DESC_STATUS_QTAG |
             GENET_TX_DESC_STATUS_BUFLEN(len + GENET_TX_STATUS64_LEN);
    info = 0;
    if (nic->tx_csum != 0)
    {
        ULONG start = (ULONG)NETDEV_HDR_LEN + ((ULONG)(buf[14] & 0x0F) << 2);
        UBYTE proto = buf[23];

        if (proto == 6 && (nic->tx_csum & ANXD_S2_TXF_TCP) != 0)
            info = (start << GENET_TX_CSUM_START_SHIFT) | (start + 16UL);
        else if (proto == 17 && (nic->tx_csum & ANXD_S2_TXF_UDP) != 0)
            info = (start << GENET_TX_CSUM_START_SHIFT) | (start + 6UL) |
                   GENET_TX_CSUM_UDP;
        if (info != 0)
        {
            info   |= GENET_TX_CSUM_LEN_VALID;
            status |= GENET_TX_DESC_STATUS_CKSUM;
            nic->core_stat[GE_ST_TX_CSUM]++;
        }
    }
    {
        /* Little-endian, two words: the block sits two bytes into the slot. */
        UWORD *w = (UWORD *)(slot + GE_TX_PAD + GENET_TX_STATUS64_CSUM_INFO);

        w[0] = __builtin_bswap16((UWORD)(info & 0xffffUL));
        w[1] = __builtin_bswap16((UWORD)(info >> 16));
    }

    ge_wr(nic, GENET_TX_DESC_ADDRESS_LO(idx), (ULONG)(slot + GE_TX_PAD));
    ge_wr(nic, GENET_TX_DESC_ADDRESS_HI(idx), 0);
    ge_wr(nic, GENET_TX_DESC_STATUS(idx), status);

    c->tx_pidx++;
    nic->txb_inuse = (UWORD)(c->tx_pidx - c->tx_cidx);

    /* Straight to the chip: the page push is two microseconds, so nothing
       is gained by holding a frame back for company. */
    ge_tx_kick(nic);

    return 0;
}

/* TRUE when the chip retired frames. */
static BOOL ge_txintr(NetdevNic *nic)
{
    GenetCore *c = GE(nic);
    UWORD      cidx;
    UWORD      n;

    cidx = (UWORD)(ge_rd(nic, GENET_TX_DMA_CONS_INDEX(GE_Q)) & 0xffffu);
    n    = (UWORD)(cidx - c->tx_cidx);

    if (n > (UWORD)(c->tx_pidx - c->tx_cidx))
        n = (UWORD)(c->tx_pidx - c->tx_cidx);  /* cannot happen: the chip never runs ahead */

    if (n != 0)
    {
        nic->tx_packets   += n;
        nic->tx_completed += n;
        c->tx_cidx         = cidx;
    }

    /* The in-use count is the two indices' difference, never a shared
       read-modify-write: the task advances tx_pidx under Forbid()
       (tx_task_lock), this advances tx_cidx from either context. */
    nic->txb_inuse = (UWORD)(c->tx_pidx - c->tx_cidx);

    return (BOOL)(n != 0);
}

/* ----------------------------------------------------------- interrupt --- */

/*
 * The top half, on the GIC: acknowledge what is pending and MASK it, so the
 * line is quiet until the bottom half has drained the rings, and nothing in
 * here can storm.  The reference driver's server does the same and hands the
 * rest to a task; here it is a software interrupt.
 */
static BOOL genet_isr(NetdevNic *nic)
{
    ULONG stat;

    stat = ge_rd(nic, GENET_INTRL2_CPU_STAT) &
           ~ge_rd(nic, GENET_INTRL2_CPU_STAT_MASK);
    if (stat == 0)
        return FALSE;

    ge_wr(nic, GENET_INTRL2_CPU_CLEAR, stat);
    ge_wr(nic, GENET_INTRL2_CPU_SET_MASK, stat);
    GE(nic)->irq_pending |= stat;
    nic->core_stat[GE_ST_INTS]++;
    return TRUE;
}

/* The interrupt bottom half and the task-context idle poll share this body. */
static BOOL genet_intr_body(NetdevNic *nic);

static BOOL genet_intr(NetdevNic *nic)
{
    GE_P_START(pi);
    BOOL r = genet_intr_body(nic);

    GE_P_ADD(nic, GE_ST_P_INTR_US, pi);
    return r;
}

static BOOL genet_intr_body(NetdevNic *nic)
{
    GenetCore *c = GE(nic);
    ULONG      stat;
    BOOL       mine;

    if (!nic->running)
        return FALSE;

    stat = c->irq_pending;
    c->irq_pending = 0;
    mine = (BOOL)(stat != 0);

    if ((stat & (GE_IRQ_LINK_UP | GE_IRQ_LINK_DOWN)) != 0)
    {
        c->link_poll_due = 1;
        ge_poll_wake(nic, c);
    }

    /* Both rings are walked whether or not their bit was set: the vertical
       blank polls through here too, and a frame is a frame. */
    if (ge_rxintr(nic))
    {
        mine = TRUE;
        /* Frames came: more may follow.  The poller takes them from here. */
        if (c->poll_task != NULL)
            ge_poll_wake(nic, c);
    }
    /* Not while a task is mid-transmit: it reclaims for itself
       (tx_task_lock).  */
    if (!nic->tx_busy && ge_txintr(nic))
        mine = TRUE;

    /* Re-arm what the top half masked, now that the rings are drained. */
    if (stat != 0)
        ge_wr(nic, GENET_INTRL2_CPU_CLEAR_MASK, stat & GE_IRQ_WANTED);

    return mine;
}

/*
 * Once a blank.  The link is polled once a second -- the PHY's own interrupt
 * pin is not wired to anything on a Pi -- and the transmit ring is reclaimed.
 * TRUE asks the shell to pump its queue.
 */
static BOOL genet_tick(NetdevNic *nic)
{
    GenetCore *c = GE(nic);

    if (!nic->running)
        return FALSE;

    if (++c->blanks >= 50)
    {
        c->blanks = 0;
        c->link_poll_due = 1;
        ge_poll_wake(nic, c);
    }

    /* Frames held for a reader that was behind (ge_rxintr): the blank is the
       backstop when neither an interrupt nor the opener's poll came, and
       the clock on how long the head of the ring may wait. */
    if (nic->rx_behind)
    {
        nic->core_stat[GE_ST_TICK_RESUMES]++;
        if (++c->held_blanks >= GE_HOLD_BLANKS)
        {
            c->held_blanks = 0;
            c->drop_held   = 1;         /* ge_deliver lets the head go */
        }
        (VOID)ge_rxintr(nic);
    }
    else
    {
        c->held_blanks = 0;
    }

    /* Completed transmits are reclaimed here when nothing else has: with no
       completion interrupt this is what frees a full ring on a quiet wire. */
    return (BOOL)(!nic->tx_busy && nic->txb_inuse != 0 && ge_txintr(nic));
}

/* -------------------------------------------------------------- poller --- */

static __inline__ ULONG ge_clock(const GenetCore *c)
{
    return __builtin_bswap32(*c->clock);
}

/*
 * The task.  Asleep until a transmit wakes it (genet_tx); then, until
 * GE_POLL_GRACE_US pass with nothing new in the ring, it runs the service
 * pass whenever the producer index has moved.  Back to sleep as soon as the
 * ring holds frames for a reader that is behind: nothing changes until that
 * reader re-posts, it asks for its own pass then (ANXD_CMD_RX_POLL), and
 * the next frame in or out wakes this task again.  Nothing is held between
 * passes, so the detach may RemTask() it wherever it stands.
 */
static VOID ge_poll_task(VOID)
{
    struct Task *me  = FindTask(NULL);
    NetdevNic   *nic = (NetdevNic *)me->tc_UserData;
    GenetCore   *c   = GE(nic);
    BYTE         sig = AllocSignal(-1);

    if (sig < 0)
        for (;;)
            (VOID)Wait(0);              /* until the detach takes it away */
    c->poll_sig = 1UL << sig;

    for (;;)
    {
        ULONG last;
        BOOL  link_due;

        c->poll_asleep = 1;
        (VOID)Wait(c->poll_sig);
        c->poll_asleep = 0;
        nic->core_stat[GE_ST_POLL_WAKES]++;

        Disable();
        link_due = (BOOL)(c->link_poll_due != 0);
        c->link_poll_due = 0;
        Enable();
        if (link_due)
        {
            /* Stop/offline is task context too.  Keep it from tearing the
               MAC down halfway through an MDIO transaction, without masking
               hardware interrupts for the transaction's millisecond wait. */
            Forbid();
            if (nic->running)
                ge_link_poll(nic);
            Permit();
        }

        /* Emulators need no idle RX poll clock, but the same task is still
           the safe home for PHY work requested by the vertical blank. */
        if (c->clock == NULL)
            continue;

        last = ge_clock(c);
        while (nic->running && !nic->rx_behind && nic->anxd_openers != 0)
        {
            UWORD pidx = (UWORD)(ge_rd(nic, GENET_RX_DMA_PROD_INDEX(GE_Q)) &
                                 0xffffu);

            if (pidx != c->rx_cidx)
            {
                nic->core_stat[GE_ST_POLL_PASSES]++;
                nic->core_stat[GE_ST_POLL_FRAMES] += (UWORD)(pidx - c->rx_cidx);
                netdev_nic_poll(nic);
                last = ge_clock(c);
            }
            else if (ge_clock(c) - last > GE_POLL_GRACE_US)
            {
                break;
            }
        }
    }
}

/* A frame came or went: more may follow.  From genet_tx under its lock,
   or from the service pass under Disable(); Signal() is allowed from both. */
static __inline__ VOID ge_poll_wake(NetdevNic *nic, GenetCore *c)
{
    if (c->poll_task != NULL && c->poll_sig != 0 &&
        (c->link_poll_due || (c->poll_asleep && nic->anxd_openers != 0)))
    {
        c->poll_asleep = 0;
        Signal(c->poll_task, c->poll_sig);
    }
}

/* At attach, from the opener's task.  The task always exists for deferred
   PHY work; a device-tree clock additionally enables idle receive polling. */
static VOID ge_poll_start(NetdevNic *nic)
{
    GenetCore   *c = GE(nic);
    ULONG        st;
    struct Task *t;

    /* The SoC's system timer: 1 MHz, CLO at +4.  Emu68's tree has no node
       for it (netdev_dtree.h), so its bus address goes through /soc's ranges;
       the A1200's tree puts it at 0xF2003000. */
    if (netdev_dtree_bus_addr("/soc", 0x7e003000UL, &st))
        c->clock = (volatile ULONG *)(st + 4);

    c->poll_mem = AllocMem(sizeof(struct Task) + GE_POLL_STACK,
                           MEMF_PUBLIC | MEMF_CLEAR);
    if (c->poll_mem == NULL)
        return;
    t = (struct Task *)c->poll_mem;
    t->tc_Node.ln_Type = NT_TASK;
    t->tc_Node.ln_Pri  = GE_POLL_PRI;
    t->tc_Node.ln_Name = (char *)"AmiNetXDuo anxgenet poll";
    t->tc_SPLower      = (APTR)(t + 1);
    t->tc_SPUpper      = (APTR)((UBYTE *)(t + 1) + GE_POLL_STACK);
    t->tc_SPReg        = t->tc_SPUpper;
    t->tc_UserData     = nic;
    /* An empty list: nothing for RemTask() to free. */
    t->tc_MemEntry.lh_Head     = (struct Node *)&t->tc_MemEntry.lh_Tail;
    t->tc_MemEntry.lh_Tail     = NULL;
    t->tc_MemEntry.lh_TailPred = (struct Node *)&t->tc_MemEntry.lh_Head;
    if (AddTask(t, (APTR)ge_poll_task, NULL) == NULL)
    {
        FreeMem(c->poll_mem, sizeof(struct Task) + GE_POLL_STACK);
        c->poll_mem = NULL;
        return;
    }
    c->poll_task = t;
}

static VOID genet_detach(NetdevNic *nic)
{
    GenetCore   *c = GE(nic);
    struct Task *t;
    APTR         mem;

    Forbid();
    t   = c->poll_task;
    mem = c->poll_mem;
    c->poll_task = NULL;
    c->poll_mem  = NULL;
    if (t != NULL)
        RemTask(t);
    Permit();
    if (mem != NULL)
        FreeMem(mem, sizeof(struct Task) + GE_POLL_STACK);
}

/* -------------------------------------------------------------- attach --- */

#define GE_MEM_SIZE     (sizeof(GenetCore) + GE_ALIGN + \
                         (ULONG)(GE_RX_RING + GE_TX_RING) * GE_BUFSZ)

static LONG genet_attach(NetdevNic *nic)
{
    GenetCore *c;
    ULONG      rev;
    ULONG      maj;
    UBYTE     *mem;
    UWORD      i;

    rev = ge_rd(nic, GENET_SYS_REV_CTRL);
    maj = GENET_SYS_REV_MAJOR(rev);
    /* The reference driver's reading of the field: 0 means 1, and 5 and 6
       both mean 5. */
    if (maj == 0)
        maj = 1;
    else if (maj == 5 || maj == 6)
        maj--;
    netdev_diag_note(ANXDIAG_GENET_REV, netdev_diag_card(nic->card), rev);
    /* Whether the engine is still running from before the reboot: the
       reset guard's proof, read before anything here touches it. */
    netdev_diag_note(ANXDIAG_GENET_DMA, netdev_diag_card(nic->card),
                     ge_rd(nic, GENET_RX_DMA_CTRL));
    if (maj != 5)
    {
        nic->diag_why = (UBYTE)ANXDIAG_WHY_REV;
        return -1;
    }

    /* The station address: the tree's, else what the MAC registers hold. */
    if (nic->dt_mac_ok)
    {
        for (i = 0; i < NETDEV_ADDR_LEN; i++)
            nic->factory[i] = nic->dt_mac[i];
        nic->mac_source = (UBYTE)ANXDIAG_MAC_DTREE;
    }
    else
    {
        ULONG lo = 0;
        ULONG hi = 0;

        if ((ge_rd(nic, GENET_SYS_RBUF_FLUSH_CTRL) &
             GENET_SYS_RBUF_FLUSH_RESET) == 0)
        {
            lo = ge_rd(nic, GENET_UMAC_MAC0);
            hi = ge_rd(nic, GENET_UMAC_MAC1) & 0xffffUL;
        }
        if ((lo == 0 && hi == 0) || (lo & 0x01000000UL) != 0)
        {
            nic->diag_why = (UBYTE)ANXDIAG_WHY_ADDRESS;
            return -1;
        }
        nic->factory[0] = (UBYTE)(lo >> 24);
        nic->factory[1] = (UBYTE)(lo >> 16);
        nic->factory[2] = (UBYTE)(lo >> 8);
        nic->factory[3] = (UBYTE)lo;
        nic->factory[4] = (UBYTE)(hi >> 8);
        nic->factory[5] = (UBYTE)hi;
        nic->mac_source = (UBYTE)ANXDIAG_MAC_PROM;
    }
    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        nic->mac[i] = nic->factory[i];

    /*
     * The rings' buffers.  Fast RAM only: chip RAM is on the far side of the
     * PiStorm and no DMA reaches it, and a fallback to it would attach a
     * unit that corrupts what it receives.  Then the tree's own statement of
     * where physical RAM is, because a 68k address is only a DMA address
     * where Emu68 maps them one-to-one.
     */
    mem = AllocMem(GE_MEM_SIZE, MEMF_FAST | MEMF_PUBLIC | MEMF_CLEAR);
    if (mem == NULL)
    {
        nic->diag_why = (UBYTE)ANXDIAG_WHY_NOMEM;
        return -1;
    }
    if (!netdev_dtree_ram_covers((ULONG)mem, GE_MEM_SIZE))
    {
        FreeMem(mem, GE_MEM_SIZE);
        nic->diag_why = (UBYTE)ANXDIAG_WHY_NODMA;
        return -1;
    }
    nic->core_mem  = mem;
    nic->core_size = GE_MEM_SIZE;

    c = (GenetCore *)(APTR)mem;
    nic->core = c;
    c->rx_buf = (UBYTE *)(((ULONG)(mem + sizeof(GenetCore)) + GE_ALIGN - 1) &
                          ~(ULONG)(GE_ALIGN - 1));
    c->tx_buf = c->rx_buf + (ULONG)GE_RX_RING * GE_BUFSZ;
    c->cache  = 1;
    c->pageops = (UBYTE)((SysBase->AttnFlags & AFF_68040) != 0);
    nic->core_stat[GE_ST_CPUSH_SUPERVISED] = c->pageops;
    c->phy    = (nic->dt_phy != 0xff) ? nic->dt_phy : 1;
    nic->core_stat_names = ge_stat_names;
    nic->tx_reclaim      = ge_txintr;   /* no TX interrupt: retire on ask */
    nic->tx_short_build  = 1;           /* the copy is 0.4 us, the mask 5.5 */
    nic->tx_task_lock    = 1;           /* and Forbid() is 0.1: no interrupt produces */
    nic->rx_holds        = 1;           /* the ring keeps frames for a late read */
    /* A frame takes a whole buffer whatever its size, so what the ring holds
       is GE_RX_RING full frames, not GE_RX_RING * GE_BUFSZ bytes of them: the
       opener's page arithmetic (bsdsocket_window.h, ami_bsd_tcp_window_fit)
       would count a third more full-size segments than there are buffers. */
    nic->rx_capacity     = (ULONG)GE_RX_RING * (1500UL + 14UL);
    nic->isr = genet_isr;
#ifdef NETDEV_GENET_POLL_ONLY
    /* A bring-up arm: no server at all, the vertical blank is the whole of
       the service.  genet_isr stays referenced so the arm compiles. */
    nic->dt_irq = 0;
#endif

    netdev_diag_note(ANXDIAG_GENET_MEM, netdev_diag_card(nic->card),
                     (ULONG)c->rx_buf);
    netdev_diag_note(ANXDIAG_GENET_IRQ, netdev_diag_card(nic->card),
                     nic->dt_irq);

    /* The PHY, identified and nothing more: two MDIO reads, which disturb
       nothing.  Its delays and negotiation are set up when the unit goes
       online, for the reason genet_init() gives. */
    {
        LONG id1 = ge_mii_read(nic, MII_PHYIDR1);
        LONG id2 = ge_mii_read(nic, MII_PHYIDR2);

        c->phyid = (id1 < 0 || id2 < 0)
                 ? 0xffffffffUL
                 : (((ULONG)id1 << 16) | (ULONG)id2);
        netdev_diag_note(ANXDIAG_GENET_PHY, netdev_diag_card(nic->card),
                         c->phyid);
    }

    nic->txb_cnt       = GE_TX_RING;
    nic->init_task_context = 1;
    nic->tx_at         = genet_tx_at;
    nic->rx_flags_supported = ANXD_S2_RXF_VERIFIED;
    nic->tx_csum_supported  = (UBYTE)(ANXD_S2_TXF_TCP | ANXD_S2_TXF_UDP);
    nic->ring_copy_sum = NULL;
    nic->frame_at      = NULL;
    nic->read_hdr      = NULL;
    nic->ring_copy     = NULL;

    ge_poll_start(nic);

    GE_TRACE("ge: attached rev ", rev);
    return 0;
}

const struct NetdevNicOps netdev_nic_genet =
{
    genet_attach,
    genet_init,
    genet_stop,
    genet_tx,
    genet_setfilter,
    genet_intr,
    genet_reset,
    genet_tick,
    NULL,               /* Emu68's 68040 maps the MAC non-cacheable */
    genet_detach
};
