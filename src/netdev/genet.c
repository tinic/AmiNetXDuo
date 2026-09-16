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
 * 2-byte alignment shift must fit in one descriptor.
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
 * the timeout says.  So 2 ms: a lone frame waits at most that long, and a
 * round trip to this machine grows by it.
 */
#ifndef GE_RX_COALESCE_FRAMES
#define GE_RX_COALESCE_FRAMES   32
#endif
#ifndef GE_RX_COALESCE_TICKS
#define GE_RX_COALESCE_TICKS    244     /* 2000 us */
#endif

/* The chip shifts every received frame two bytes into its buffer
   (GENET_RBUF_ALIGN_2B), which puts the IP header on a longword. */
#define GE_RX_PAD       2

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
    UBYTE   pageops;        /* cpushp a page at a time, not CacheClearE     */
    UBYTE   phy_set;        /* the PHY's delays and negotiation were set up */
    ULONG   irq_pending;    /* status the top half took, for the bottom half */
    ULONG   phyid;

    /*
     * The stream the previous delivered frame belonged to, for the CONTINUES
     * mark (aminetxduo/anxs2ext.h): the four-tuple, the sequence number the
     * next in-order segment must carry, and the acknowledgment, window and
     * flags it must repeat.  Valid while gro_live; cleared at the end of every
     * burst, because the opener flushes what it holds at the end of its own
     * drain and a mark across bursts would only ever be a false one.
     */
    ULONG   gro_addr[8];    /* source, destination: two words for IPv4,
                               eight for IPv6                               */
    ULONG   gro_ports;      /* source port << 16 | destination port         */
    ULONG   gro_seq;        /* the next in-order sequence number            */
    ULONG   gro_ack;
    UWORD   gro_win;
    UBYTE   gro_flags;      /* the TCP flag byte: ACK, or ACK|PSH           */
    UBYTE   gro_live;
    UBYTE   gro_run;        /* frames marked in this run, against GE_GRO_MAX */
    UBYTE   gro_words;      /* address words that make the key: 2 or 8      */
    UBYTE   held_blanks;    /* blanks the head of the ring has been held for */
    UBYTE   drop_held;      /* the next unclaimable head frame is dropped    */
} GenetCore;

/*
 * How many segments an opener is asked to chain into one.  Sixteen full
 * segments is 23 KB, which the stack's window (256 KB on a long path, 64 KB
 * on this LAN) holds several times over; the mark is a hint and the opener
 * has its own cap.
 */
#define GE_GRO_MAX      16

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
    GE_ST_CONTINUES,        /* frames marked as continuing the previous one */
    GE_ST_RUNS,             /* runs of two or more frames (continues/runs
                               is the mean number of frames a run saved) */
    GE_ST_BURST_MAX,        /* the most frames one burst held               */
    GE_ST_UNCLAIMED,        /* frames nobody had a read posted for: dropped */
    GE_ST_HELD,             /* passes cut short with frames left in the ring
                               for a reader that was behind (no frame lost) */
    GE_ST_HELD_FRAMES,      /* frames left waiting, summed over those passes */
    GE_ST_TICK_RESUMES,     /* held passes resumed by the vertical blank     */
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
    "GENET frames marked as continuing",
    "GENET runs of continuing frames",
    "GENET largest burst",
    "GENET frames with no read posted",
    "GENET passes held for a reader behind",
    "GENET frames left waiting in those passes",
    "GENET held passes resumed by the blank",
    NULL
};

static LONG genet_init(NetdevNic *nic);
static VOID genet_stop(NetdevNic *nic);
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
        register ULONG            _d0 __asm("d0") = (last - first) / GE_PAGE + 1UL;
        register ULONG            _a0 __asm("a0") = first;
        register VOID           (*_a5)(VOID) __asm("a5") = ge_sup_cpushp;
        register struct ExecBase *_a6 __asm("a6") = SysBase;

        /* Supervisor(): the routine runs in supervisor mode and ends in RTE. */
        __asm__ __volatile__ ("jsr a6@(-30:W)"
                              : "+r" (_d0), "+r" (_a0)
                              : "r" (_a5), "r" (_a6)
                              : "cc", "memory", "d1", "a1");
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

    v = ge_rd(nic, GENET_RBUF_CTRL);
    ge_wr(nic, GENET_RBUF_CTRL, v | GENET_RBUF_ALIGN_2B);

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
    ULONG v;

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
     */
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
static ULONG ge_copy_sum(ULONG *to, const ULONG *from, ULONG count)
{
    ULONG acc = 0;

    while (count-- != 0)
    {
        ULONG w = *from++;

        *to++ = w;
        acc += w;
        if (acc < w)
            acc++;                      /* end-around carry */
    }
    return acc;
}

/*
 * WHAT THE COPY'S SUM IS WORTH ONCE THE HEADERS ARE IN CACHE.
 *
 * ge_copy_sum() has just moved the IP packet and produced the ones-complement
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
 * The stateless half is shared with the classic direct paths now.  GENET adds
 * only the stream key and state which make CONTINUES possible.
 */
typedef NetdevRxSegment GeSegment;

/*
 * THE CONTINUES MARK.  A verified TCP segment with no options, carrying data,
 * whose flags are ACK or ACK+PSH, is the next of the same stream as the
 * previous one when the four-tuple, the acknowledgment, the window and the
 * flags repeat and its sequence number is where the previous one ended.
 * Anything else starts a new run (a TCP segment) or ends the run (anything
 * else): the opener holds at most one head, and a frame that is not the next
 * of that stream makes it deliver the head, so a mark across it would be a
 * lie.
 */
static UBYTE ge_continues(GenetCore *c, const GeSegment *seg, UBYTE verified)
{
    BOOL candidate = (BOOL)(verified != 0 && seg->tcp != 0 &&
                            seg->data != 0 &&
                            (seg->flags == 0x10 || seg->flags == 0x18));

    if (!candidate)
    {
        c->gro_live = 0;
        return 0;
    }

    if (c->gro_live &&
        c->gro_run < GE_GRO_MAX &&
        c->gro_words == seg->words &&
        c->gro_ports == seg->ports &&
        c->gro_seq   == seg->seq &&
        c->gro_ack   == seg->ack &&
        c->gro_win   == seg->win &&
        c->gro_flags == seg->flags)
    {
        UBYTE i;

        for (i = 0; i < seg->words; i++)
            if (c->gro_addr[i] != seg->addr[i])
                break;
        if (i == seg->words)
        {
            c->gro_seq = seg->seq + seg->data;
            c->gro_run++;
            return ANXD_S2_RXF_CONTINUES;
        }
    }

    {
        UBYTE i;

        for (i = 0; i < seg->words; i++)
            c->gro_addr[i] = seg->addr[i];
    }
    c->gro_words   = seg->words;
    c->gro_ports   = seg->ports;
    c->gro_seq     = seg->seq + seg->data;
    c->gro_ack     = seg->ack;
    c->gro_win     = seg->win;
    c->gro_flags   = seg->flags;
    c->gro_live    = 1;
    c->gro_run     = 1;
    return 0;
}

/* FALSE when the frame was left in the ring: the opener that reads this type
   has no read posted right now (NETDEV_CLAIM_BEHIND), and holding the frame
   until it does is what a 128-deep ring is for.  TRUE otherwise, whether the
   frame was claimed, staged or dropped. */
static BOOL ge_deliver(NetdevNic *nic, const UBYTE *frame, UWORD len)
{
    APTR   token = NULL;
    UBYTE  wanted = 0;
    UBYTE *dst   = (nic->rx_claim != NULL)
                 ? nic->rx_claim(nic->rx_arg, frame, len, &token, &wanted)
                 : NULL;

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
         * Payload to the opener's slot, summed on the way: the copy's loads
         * pay for the ones-complement sum the stack's verifier wants, and a
         * frame handed up summed is not walked a second time.  frame + 14 is
         * on a longword -- the buffer is page-aligned and the chip shifted
         * the frame by two -- and the slot's payload pointer is aligned by
         * construction.  The 1..3 bytes past the last longword are summed as
         * a zero-padded final longword, which is what the verifier's
         * "sum of `copied` bytes" means.
         */
        UWORD        plen = (UWORD)(len - NETDEV_HDR_LEN);
        UWORD        bulk = (UWORD)(plen & (UWORD)~3u);
        const UBYTE *src  = frame + NETDEV_HDR_LEN;
        ULONG        sum  = 0;
        UWORD        i;

        if (bulk != 0)
            sum = ge_copy_sum((ULONG *)(APTR)dst,
                              (const ULONG *)(CONST_APTR)src,
                              (ULONG)(bulk >> 2));
        if (bulk != plen)
        {
            ULONG tail = 0;
            ULONG was  = sum;

            for (i = bulk; i < plen; i++)
            {
                dst[i] = src[i];
                tail |= (ULONG)src[i] << (24 - 8 * (i - bulk));
            }
            sum += tail;
            if (sum < was)
                sum++;                  /* end-around carry */
        }

        /*
         * The frame is in cache now, both copies of it.  Check it here, from
         * the sum the copy already produced, and say whether it continues the
         * stream the previous frame belonged to (aminetxduo/anxs2ext.h); the
         * device masks the answer to what the opener asked for.  The
         * ethertype is at frame + 12, big-endian.
         */
        {
            GenetCore *c     = GE(nic);
            UBYTE      flags = ANXD_S2_RXF_SUMMED;

            if ((wanted & ANXD_S2_RXF_VERIFIED) != 0 &&
                frame[12] == 0x08 && frame[13] == 0x00)
            {
                GeSegment seg;
                UBYTE     v = netdev_rx_verify4(src, plen, sum);

                if (v != 0)
                    netdev_rx_segment4(src, &seg);

                if (v != 0)
                    nic->core_stat[GE_ST_VERIFIED]++;
                flags |= v;
                if ((wanted & ANXD_S2_RXF_CONTINUES) != 0)
                    flags |= ge_continues(c, &seg, v);
                else
                    c->gro_live = 0;
                if ((flags & ANXD_S2_RXF_CONTINUES) != 0)
                {
                    nic->core_stat[GE_ST_CONTINUES]++;
                    if (c->gro_run == 2)
                        nic->core_stat[GE_ST_RUNS]++;
                }
            }
            else if ((wanted & ANXD_S2_RXF_VERIFIED) != 0 &&
                     frame[12] == 0x86 && frame[13] == 0xdd)
            {
                GeSegment seg;
                UBYTE     v = netdev_rx_verify6(src, plen, sum);

                if (v != 0)
                    netdev_rx_segment6(src, &seg);

                if (v != 0)
                    nic->core_stat[GE_ST_VERIFIED]++;
                flags |= v;
                if ((wanted & ANXD_S2_RXF_CONTINUES) != 0)
                    flags |= ge_continues(c, &seg, v);
                else
                    c->gro_live = 0;
                if ((flags & ANXD_S2_RXF_CONTINUES) != 0)
                {
                    nic->core_stat[GE_ST_CONTINUES]++;
                    if (c->gro_run == 2)
                        nic->core_stat[GE_ST_RUNS]++;
                }
            }
            else
            {
                c->gro_live = 0;
            }

            nic->rx_claimed(nic->rx_arg, token, sum, flags);
        }
        return TRUE;
    }

    /* Handed up where it lies, like the LANCE: no staging copy.  Not a
       frame the opener will chain, so the run ends here. */
    GE(nic)->gro_live = 0;
    nic->core_stat[GE_ST_UNCLAIMED]++;
    if (nic->rx != NULL)
        nic->rx(nic->rx_arg, frame, len);
    return TRUE;
}

/* TRUE when the ring had frames. */
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
        }
        c->rx_clean = pidx;
    }

    nic->rx_behind = 0;

    for (n = 0; n < total; n++)
    {
        UWORD  idx    = (UWORD)(c->rx_cidx & (GE_RX_RING - 1));
        ULONG  status = ge_rd(nic, GENET_RX_DESC_STATUS(idx));
        UWORD  len    = (UWORD)GENET_RX_DESC_STATUS_BUFLEN(status);
        UBYTE *buf    = c->rx_buf + (ULONG)idx * GE_BUFSZ;

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
                 len < GE_RX_PAD + NETDEV_HDR_LEN ||
                 len > GE_RX_PAD + NETDEV_RXBUF_MAX)
        {
            nic->rx_errors++;
            nic->core_stat[GE_ST_RX_LEN]++;
        }
        else if (!ge_deliver(nic, buf + GE_RX_PAD, (UWORD)(len - GE_RX_PAD)))
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
        ge_wr(nic, GENET_RX_DESC_ADDRESS_LO(idx), (ULONG)buf);
        ge_wr(nic, GENET_RX_DESC_ADDRESS_HI(idx), 0);
        c->rx_cidx++;
    }

    ge_wr(nic, GENET_RX_DMA_CONS_INDEX(GE_Q), c->rx_cidx);
    c->gro_live = 0;                    /* a run does not span bursts */

    /* The burst's completions, replied together (NetdevNic reply_batch). */
    if (nic->rx_flush != NULL)
        nic->rx_flush(nic->rx_arg);
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
    ge_wr(nic, GENET_TX_DMA_PROD_INDEX(GE_Q), c->tx_pidx);
    c->tx_kicked = c->tx_pidx;
    nic->core_stat[GE_ST_KICKS]++;
}

/* The buffer the next transmit will use, for the shell to frame into. */
static UBYTE *genet_tx_at(NetdevNic *nic)
{
    GenetCore *c = GE(nic);

    if (nic->txb_inuse >= GE_TX_RING)
        return NULL;
    return c->tx_buf + (ULONG)(c->tx_pidx & (GE_TX_RING - 1)) * GE_BUFSZ;
}

static BOOL ge_txintr(NetdevNic *nic);

static LONG genet_tx(NetdevNic *nic, const UBYTE *frame, UWORD len)
{
    GenetCore *c = GE(nic);
    UWORD      idx;
    UBYTE     *buf;

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

    idx = (UWORD)(c->tx_pidx & (GE_TX_RING - 1));
    buf = c->tx_buf + (ULONG)idx * GE_BUFSZ;

    if (frame != buf)
    {
        UWORD bulk = (UWORD)(len & (UWORD)~3u);
        UWORD i;

        if (bulk != 0)
            n68k_copy_longs(buf, frame, (ULONG)(bulk >> 2));
        for (i = bulk; i < len; i++)
            buf[i] = frame[i];
    }

    ge_wr(nic, GENET_TX_DESC_ADDRESS_LO(idx), (ULONG)buf);
    ge_wr(nic, GENET_TX_DESC_ADDRESS_HI(idx), 0);
    ge_wr(nic, GENET_TX_DESC_STATUS(idx),
          GENET_TX_DESC_STATUS_SOP | GENET_TX_DESC_STATUS_EOP |
          GENET_TX_DESC_STATUS_CRC | GENET_TX_DESC_STATUS_QTAG |
          GENET_TX_DESC_STATUS_BUFLEN(len));

    c->tx_pidx++;
    nic->txb_inuse++;

    /* Straight to the chip: the page push is two microseconds, so nothing
       is gained by holding a frame back for company. */
    ge_tx_kick(nic);

    return 0;
}

/* TRUE when the chip retired frames. */
static BOOL ge_txintr(NetdevNic *nic)
{
    GenetCore *c    = GE(nic);
    UWORD      cidx = (UWORD)(ge_rd(nic, GENET_TX_DMA_CONS_INDEX(GE_Q)) & 0xffffu);
    UWORD      n    = (UWORD)(cidx - c->tx_cidx);

    if (n > nic->txb_inuse)
        n = nic->txb_inuse;     /* cannot happen: the chip never runs ahead */

    if (n != 0)
    {
        nic->tx_packets   += n;
        nic->tx_completed += n;
        nic->txb_inuse     = (UWORD)(nic->txb_inuse - n);
        c->tx_cidx         = cidx;
    }

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

/* The bottom half, and the vertical blank's poll: under Disable(). */
static BOOL genet_intr(NetdevNic *nic)
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
        ge_link_poll(nic);

    /* Both rings are walked whether or not their bit was set: the vertical
       blank polls through here too, and a frame is a frame. */
    if (ge_rxintr(nic))
        mine = TRUE;
    if (ge_txintr(nic))
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
        ge_link_poll(nic);
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
    return (BOOL)(nic->txb_inuse != 0 && ge_txintr(nic));
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
    c->phy    = (nic->dt_phy != 0xff) ? nic->dt_phy : 1;
    nic->core_stat_names = ge_stat_names;
    nic->reply_batch     = 1;           /* Emu68: an Exec call is a trap */
    nic->tx_reclaim      = ge_txintr;   /* no TX interrupt: retire on ask */
    nic->tx_short_build  = 1;           /* the copy is 0.4 us, the mask 5.5 */
    nic->rx_holds        = 1;           /* the ring keeps frames for a late read */
    nic->rx_capacity     = (ULONG)GE_RX_RING * GE_BUFSZ;
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
    nic->tx_at         = genet_tx_at;
    nic->rx_flags_supported = (UBYTE)(ANXD_S2_RXF_VERIFIED |
                                      ANXD_S2_RXF_CONTINUES);
    nic->ring_copy_sum = NULL;
    nic->frame_at      = NULL;
    nic->read_hdr      = NULL;
    nic->ring_copy     = NULL;

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
    NULL                /* Emu68's 68040 maps the MAC non-cacheable */
};
