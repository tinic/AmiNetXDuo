/*
 * anxzz9000.device, the chip core: the MNT ZZ9000's Ethernet.
 *
 * There is no Ethernet chip on the Zorro bus here.  The MAC is the Zynq's
 * own GEM, driven by the card's ARM firmware (ZZ9000OS, ethernet.c); what the
 * 68k sees is a register block and two frame windows in the board's first
 * 64 KB:
 *
 *   +0x0004  UWORD  interrupt control: a write with bit 3 clear is the enable
 *                   word (bit 0 Ethernet); with bit 3 set it acknowledges
 *                   (bit 4 Ethernet); a read is the pending mask (bit 0)
 *   +0x0080  UWORD  transmit: write the frame length, the ARM sends what is
 *                   in the TX window; a read answers the result
 *   +0x0082  UWORD  receive acknowledge: write the serial of the frame just
 *                   taken and the ARM presents the next one
 *   +0x0084  UWORD  station address, octets 0..1; +0x86 2..3; +0x88 4..5
 *   +0x008c  UWORD  receive status: frames waiting in the low byte
 *   +0x2000  the presented receive frame: UWORD length, UWORD serial, then
 *            the frame from +4, header first, no FCS
 *   +0x8000  the transmit window, 2048 bytes
 *
 * The ARM keeps a ring of received frames and presents one at a time in the
 * receive window; a slot it has not filled, or has already handed over, reads
 * serial 0, and the serial generator skips 0 and 1 (firmware ethernet.c:
 * ethernet_receive_frame), so "serial != 0" is the whole of "there is a
 * frame".  The acknowledge carries that serial back: the ARM advances only
 * when it matches the frame it presented, which is what stops a late ack
 * from dropping a frame the 68k never read.
 *
 * Reads of the frame windows run at Zorro III speed -- 4.6-4.9 MB/s with
 * CopyMemQuick on an A3000/030-25, the same as the framebuffer -- while the
 * register block is served by the ARM one access at a time (1.1-1.3 MB/s).
 * A frame is therefore copied from the window straight into the opener's
 * buffer with the fused sum, and touched in the register block exactly twice:
 * the serial in and the acknowledge out.  MNT's ZZ9000Net.device copies
 * every frame twice (window -> its buffer -> the stack) and runs a task to
 * do it; measured on the A3000 that task alone was 23% of the CPU at
 * 3.5 Mbit/s.
 *
 * Transmit, on MNT's firmware, is one frame at a time by construction: the
 * ARM services the window writes and the length write in the order they
 * arrive, and does not answer the length write's bus cycle until the GEM
 * has sent -- 100 us to 1 ms with the CPU stalled, 16% of an A3000 at
 * 4.6 Mbit/s of receive, all of it ACKs.  This project's firmware
 * (tinic/zz9000-firmware, branch aminetxduo) adds an asynchronous send:
 * bit 15 of the length word names one of the four 2 KB slots of the TX
 * window in bits 12..11 and gets the bus back at once, and +0x8a counts the
 * frames finished (bit 15 set says the firmware has the path; MNT's reads
 * the register as 0).  The core takes whichever it finds at attach.
 *
 * The interrupt is INT6 (INTB_EXTER), which is what the card raises; the
 * server only masks and acknowledges at the card and the drain runs in the
 * shell's software interrupt, so a burst of 32 frames is not copied at
 * level 6.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>

#include "netdev_nic.h"
#include "n68k_iocopy.h"
#include "netdev_cards.h"
#include "netdev_mcaf.h"
#include "netdev_macgen.h"
#include "netdev_verify.h"
#include "netdev_clock.h"
#include "dp8390.h"     /* the DP8390_TX_* return codes are the shared contract */
#include "aminetxduo/anxs2ext.h"
#include <aminetxduo/anxdiag.h>

static BOOL zz_isr(NetdevNic *nic);
static BOOL zz_tx_reclaim(NetdevNic *nic);

/* -------------------------------------------------------------- board ---- */

#define ZZ_REG_INT          0x0004UL
#define ZZ_REG_TX           0x0080UL
#define ZZ_REG_RX_ACK       0x0082UL
#define ZZ_REG_MAC_HI       0x0084UL
#define ZZ_REG_MAC_MID      0x0086UL
#define ZZ_REG_MAC_LO       0x0088UL
#define ZZ_REG_RX_STATUS    0x008cUL
#define ZZ_REG_RX_META      0x00a6UL    /* fork: current GEM RX verdict */

#define ZZ_REG_TX_STATUS    0x008aUL    /* the fork's firmware, else reads 0 */

#define ZZ_RX_WINDOW        0x2000UL
#define ZZ_RX_PAD           4           /* UWORD length, UWORD serial */
#define ZZ_TX_WINDOW        0x8000UL
#define ZZ_TX_WINDOW_LEN    2048        /* one slot; the window holds four */
#define ZZ_TX_SLOTS         4

/* The length word of an asynchronous send, and the status register. */
#define ZZ_TX_ASYNC         0x8000u
#define ZZ_TX_SLOT_SHIFT    11
#define ZZ_TX_LEN_MASK      0x07ffu
#define ZZ_TXS_PRESENT      0x8000u
#define ZZ_TXS_COUNT        0x7fffu

#define ZZ_RXM_PRESENT      0x8000u
#define ZZ_RXM_MASK         0x0003u
#define ZZ_RXM_TCP          2u
#define ZZ_RXM_UDP          3u

#define ZZ_INT_ETH          0x0001      /* enable, and "pending" on a read */
#define ZZ_INT_ETH_ACK      (8 | 16)    /* what MNT's driver writes to clear */

/*
 * Frames the ARM holds before it loses one, what the opener's window
 * arithmetic is told (ANXD_CMD_RX_CAPACITY).  MNT's release firmware: a
 * 32-slot ring and 32 receive descriptors.  This project's fork: 128 slots,
 * 64 descriptors armed, and no more armed once 120 are pending (ethernet.c
 * ETH_BACKLOG_HIGH_WATERMARK), so 56 may sit queued for the 68k before the
 * card pauses the wire -- and a pause is not felt in time by a gigabit
 * sender behind a switch.  What the number buys: a sender on the same
 * switch puts the whole window on the wire back to back, and every frame
 * of that burst past the armed descriptors is lost in the GEM with nothing
 * counting it; with 32 armed and a 47 KB window, frames 33-35 of every
 * burst went missing (25 retransmissions in ten seconds; 275 with the
 * window sized to the whole ring).  Sized to what is taken without a pause
 * and without the ARM's help, the window fits.
 */
#define ZZ_ARM_RING_FRAMES_MNT  32UL
#define ZZ_ARM_RING_FRAMES_FORK 56UL

/* How long zz_intr waits for a header the ARM has counted but the L2 cache
   still hides, measured against the beam (netdev_clock.h). */
#define ZZ_STALE_WAIT_US    2000UL

/* The receive-status low byte, frames the ARM holds for us. */
#define ZZ_RX_READY(status) ((UWORD)((status) & 0x00ffu))

/* ------------------------------------------------------------ the core --- */

/* What this core counts, in nic->core_stat[] order. */
enum
{
    ZZ_ST_SERIAL = 0,   /* the serial last acknowledged                     */
    ZZ_ST_GAPS,         /* serials that skipped: the ARM's ring overflowed   */
    ZZ_ST_OVERSIZE,     /* a length the window cannot hold: dropped          */
    ZZ_ST_UNWANTED,     /* group frames the hash did not take                */
    ZZ_ST_TXERR,        /* the ARM's transmit result was not 0               */
    ZZ_ST_ISR,          /* top halves that found the Ethernet bit            */
    ZZ_ST_CONTINUES,    /* frames marked CONTINUES (netdev_verify.h)         */
    ZZ_ST_RUNS,         /* runs of two or more the mark made                 */
    ZZ_ST_SOFT_EMPTY,   /* passes after a top half that found no frame       */
    ZZ_ST_POLL_WORK,    /* passes with no top half before them that found one */
    ZZ_ST_BURST_MAX,    /* most frames one pass took                         */
    ZZ_ST_LATE_HIT,     /* empty passes where the header appeared on a spin  */
    ZZ_ST_LATE_READS,   /* most window reads it took to appear               */
    ZZ_ST_LATE_MISS,    /* empty passes where it never did within the budget */
    ZZ_ST_EMPTY,        /* service passes that found no frame                */
    ZZ_ST_ACK_RECOVER,  /* rejected serial handshake recovered compatibly    */
    ZZ_ST_HW_VERIFIED,  /* frames certified by the GEM descriptor             */
    ZZ_ST_HW_FALLBACK,  /* GEM verdict outside the published RX contract      */
    ZZ_ST_COUNT
};

static const char *const zz_stat_names[] =
{
    "last serial acknowledged",
    "serial gaps (ARM ring overflow)",
    "frames dropped for size",
    "group frames not wanted",
    "transmit results not 0",
    "top halves: Ethernet pending",
    "frames marked CONTINUES",
    "runs the mark made",
    "passes after a top half, empty",
    "polled passes that found frames",
    "most frames in one pass",
    "empty pass, header appeared on spin",
    "most window reads until it appeared",
    "empty pass, header never appeared",
    "service passes with no frame",
    "rejected serial acknowledgements recovered",
    "frames verified by GEM hardware",
    "GEM verdicts checked again in software",
    NULL
};

/*
 * The core's own state.  Static rather than allocated: the shell frees and
 * detaches only a core that set core_mem, and core_mem also arms the
 * bus-master reset guard, which this card does not want.  Two, because the
 * table has two rows for the card (Zorro III and Zorro II records); a
 * machine has one ZZ9000, and the image is unloaded whole.
 */
typedef struct ZzCore
{
    NetdevRxGro gro;        /* the CONTINUES key, netdev_verify.h           */
    UBYTE       after_isr;  /* set by the top half, cleared by the pass that
                               follows it: which context a pass ran in     */
    UBYTE       rx_meta;    /* firmware exposes REG_ZZ_ETH_RX_META          */
} ZzCore;

static ZzCore zz_cores[2];
static UWORD  zz_cores_used;

#define ZZ(nic) ((ZzCore *)(nic)->core)

/* Frames the mark may chain before it starts a new run: what the opener
   holds at most (src/sana2 AMI_SANA2_GRO_MAX). */
#define ZZ_GRO_MAX      16

static volatile UWORD *zz_reg(NetdevNic *nic, ULONG off)
{
    return (volatile UWORD *)(volatile void *)(nic->board + off);
}

static UWORD zz_get(NetdevNic *nic, ULONG off)
{
    return *zz_reg(nic, off);
}

static VOID zz_put(NetdevNic *nic, ULONG off, UWORD val)
{
    *zz_reg(nic, off) = val;
}

/* ----------------------------------------------------------- station ---- */

static VOID zz_read_mac(NetdevNic *nic, UBYTE *mac)
{
    UWORD hi  = zz_get(nic, ZZ_REG_MAC_HI);
    UWORD mid = zz_get(nic, ZZ_REG_MAC_MID);
    UWORD lo  = zz_get(nic, ZZ_REG_MAC_LO);

    mac[0] = (UBYTE)(hi >> 8);
    mac[1] = (UBYTE)hi;
    mac[2] = (UBYTE)(mid >> 8);
    mac[3] = (UBYTE)mid;
    mac[4] = (UBYTE)(lo >> 8);
    mac[5] = (UBYTE)lo;
}

/*
 * Only when it differs: the last register's write is what makes the firmware
 * reprogram the GEM and restart its DMA (ethernet.c: ethernet_update_mac_
 * address), which drops whatever was in flight.
 */
static VOID zz_write_mac(NetdevNic *nic)
{
    UBYTE cur[NETDEV_ADDR_LEN];
    UWORD i;

    zz_read_mac(nic, cur);
    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        if (cur[i] != nic->mac[i])
            break;
    if (i == NETDEV_ADDR_LEN)
        return;

    zz_put(nic, ZZ_REG_MAC_HI,  (UWORD)(((UWORD)nic->mac[0] << 8) | nic->mac[1]));
    zz_put(nic, ZZ_REG_MAC_MID, (UWORD)(((UWORD)nic->mac[2] << 8) | nic->mac[3]));
    zz_put(nic, ZZ_REG_MAC_LO,  (UWORD)(((UWORD)nic->mac[4] << 8) | nic->mac[5]));
}

/* -------------------------------------------------------------- attach -- */

static LONG zz_attach(NetdevNic *nic)
{
    UBYTE ored = 0;
    UBYTE anded = 0xff;
    UWORD i;

    nic->core = &zz_cores[zz_cores_used & 1u];
    zz_cores_used++;
    ZZ(nic)->gro.live  = 0;
    ZZ(nic)->after_isr = 0;
    ZZ(nic)->rx_meta   = 0;

    /*
     * The station address the firmware programmed into the GEM: the card's
     * own from its configuration, or what a driver wrote before us.  Blank
     * or a group address means the ARM has nothing yet, and the autoconfig
     * serial number gives a derived one, the way the LANCE boards get theirs.
     */
    zz_read_mac(nic, nic->factory);
    for (i = 0; i < NETDEV_ADDR_LEN; i++)
    {
        ored  |= nic->factory[i];
        anded &= nic->factory[i];
    }
    if (ored == 0 || anded == 0xff || (nic->factory[0] & 1u) != 0)
    {
        UBYTE fp[NETDEV_MAC_FP_MAX];
        UWORD n;
        ULONG salt = ((ULONG)nic->card->manid << 16) ^
                     (ULONG)nic->card->prodid ^ nic->serial;

        n = netdev_mac_fingerprint(fp, (UWORD)sizeof(fp), salt);
        netdev_mac_derive(fp, n, nic->mac);
        nic->mac_derived++;
        nic->mac_source = (UBYTE)ANXDIAG_MAC_DERIVED;
    }
    else
    {
        for (i = 0; i < NETDEV_ADDR_LEN; i++)
            nic->mac[i] = nic->factory[i];
        nic->mac_source = (UBYTE)ANXDIAG_MAC_PROM;
    }

    /*
     * How many frames may be in flight: one on MNT's firmware, whose length
     * write returns with the frame sent; the four window slots on the
     * fork's, which counts completions in the status register.  No ring
     * and no packet buffer to describe: the window has no address of its
     * own.
     */
    {
        UWORD txs  = zz_get(nic, ZZ_REG_TX_STATUS);
        BOOL  fork = (BOOL)((txs & ZZ_TXS_PRESENT) != 0);
        UWORD rxm  = zz_get(nic, ZZ_REG_RX_META);

        nic->txb_cnt = fork ? ZZ_TX_SLOTS : 1;
        nic->tx_done = (UWORD)(txs & ZZ_TXS_COUNT);
        nic->tx_next = 0;
        /* What the ARM holds for us before it drops (or, on the fork,
           pauses the wire). */
        nic->rx_capacity = (fork ? ZZ_ARM_RING_FRAMES_FORK
                                 : ZZ_ARM_RING_FRAMES_MNT) * (1500UL + 14UL);
        ZZ(nic)->rx_meta = (UBYTE)((rxm & ZZ_RXM_PRESENT) != 0);
    }
    nic->txb_inuse = 0;
    nic->read_hdr  = NULL;
    nic->ring_copy = NULL;
    nic->ring_copy_sum = NULL;
    nic->frame_at  = NULL;
    nic->tx_at     = NULL;
    nic->write_buf = NULL;
    nic->core_stat_names = zz_stat_names;
    nic->rx_flags_supported = (UBYTE)(ANXD_S2_RXF_VERIFIED |
                                      ANXD_S2_RXF_CONTINUES);
    nic->isr = zz_isr;
    nic->tx_reclaim = zz_tx_reclaim;    /* completions are counted, not delivered */
    /* tx() holds the bus for the ARM's whole send (see it): under Forbid(),
       with interrupts on, never under the shell's Disable().  Nothing on
       the interrupt side transmits, so the lock is sound. */
    nic->tx_task_lock = 1;

    /* The interrupt is quiet until init(). */
    zz_put(nic, ZZ_REG_INT, 0);
    return 0;
}

/* ---------------------------------------------------------------- init --- */

static LONG zz_init(NetdevNic *nic)
{
    zz_write_mac(nic);
    nic->core_stat[ZZ_ST_SERIAL] = 0;
    ZZ(nic)->gro.live = 0;
    nic->txb_inuse = 0;
    nic->tx_next   = 0;
    nic->tx_done   = (UWORD)(zz_get(nic, ZZ_REG_TX_STATUS) & ZZ_TXS_COUNT);
    nic->running   = TRUE;
    zz_put(nic, ZZ_REG_INT, ZZ_INT_ETH);
    return 0;
}

static VOID zz_stop(NetdevNic *nic)
{
    zz_put(nic, ZZ_REG_INT, 0);
    nic->running = FALSE;
}

/* The ARM does the filtering it does; the station address is the one thing
   the card takes from us, and the group hash is applied on the way in. */
static VOID zz_setfilter(NetdevNic *nic)
{
    zz_write_mac(nic);
}

static VOID zz_reset(NetdevNic *nic)
{
    /* Nothing wedges on this side, but a frame the ARM lost would leave a
       slot counted in flight for ever: take the count as it stands and
       start again, which is the contract -- txb_inuse cleared. */
    nic->txb_inuse = 0;
    nic->tx_done   = (UWORD)(zz_get(nic, ZZ_REG_TX_STATUS) & ZZ_TXS_COUNT);
    if (nic->running)
        zz_put(nic, ZZ_REG_INT, ZZ_INT_ETH);
}

/* -------------------------------------------------------------- receive -- */

/* The same decision as the DP8390 cores make in hardware, from the header. */
static BOOL zz_rx_wanted(const NetdevNic *nic, const UBYTE *frame)
{
    ULONG crc;
    UWORD idx;

    if (nic->promisc)
        return TRUE;
    if ((frame[0] & 1u) == 0)
    {
        UWORD i;
        for (i = 0; i < NETDEV_ADDR_LEN; i++)
            if (frame[i] != nic->mac[i])
                return FALSE;
        return TRUE;
    }
    if (frame[0] == 0xff && frame[1] == 0xff && frame[2] == 0xff &&
        frame[3] == 0xff && frame[4] == 0xff && frame[5] == 0xff)
        return TRUE;
    if (nic->all_multi)
        return TRUE;
    crc = netdev_ether_crc32_be(frame, NETDEV_ADDR_LEN) >> 26;
    idx = (UWORD)(crc & 0x3fu);
    return (BOOL)((nic->mar[idx >> 3] & (UBYTE)(1u << (idx & 7u))) != 0);
}

/*
 * THE PAYLOAD IS TWO BYTES OUT OF PHASE, AND THE WINDOW SIDE STAYS ALIGNED.
 *
 * The frame sits at +4 in the slot, so its payload begins at +18: word
 * aligned, 2 mod 4.  The opener's slot is longword aligned.  One side of
 * the fused copy has to be out of phase, and it is the destination: a
 * 2 mod 4 longword read on Zorro III is two word cycles on a bus that is
 * the wall at 210 ns a byte, while the same store into fast RAM realigns
 * in the 68020+'s bus unit at a fraction of that.  So the first payload
 * word goes over by itself, the bulk goes from an aligned window address
 * to dst+2 through n68k_copy_longs_sum, and the tail by words.
 *
 * The sum: 16-bit lanes at even offsets throughout, so the longword
 * accumulator folds to the same Internet checksum whatever the grouping.
 */
static ULONG zz_copy_payload_sum(UBYTE *dst, const volatile UBYTE *src,
                                 UWORD len)
{
    ULONG sum = 0;
    UWORD done = 0;

    if (len >= 2)
    {
        UWORD w = *(const volatile UWORD *)(const volatile void *)src;
        *(UWORD *)(APTR)dst = w;
        sum  = w;
        done = 2;
    }
    {
        ULONG longs = (ULONG)((len - done) >> 2);
        if (longs != 0)
        {
            ULONG bulk = n68k_copy_longs_sum(dst + done, src + done, longs);
            sum += bulk;
            if (sum < bulk)
                sum++;
            done = (UWORD)(done + (longs << 2));
        }
    }
    if (done + 2 <= len)
    {
        UWORD w = *(const volatile UWORD *)(const volatile void *)(src + done);
        *(UWORD *)(APTR)(dst + done) = w;
        sum  += w;
        done  = (UWORD)(done + 2);
    }
    if (done < len)
    {
        UWORD w = *(const volatile UWORD *)(const volatile void *)(src + done);
        dst[done] = (UBYTE)(w >> 8);
        sum += (UWORD)(w & 0xff00u);
    }
    return sum;
}

/* A plain copy of a header-and-all frame into the staging buffer. */
static VOID zz_copy_frame(UBYTE *dst, const volatile UBYTE *src, UWORD len)
{
    UWORD bulk = (UWORD)(len & (UWORD)~3u);
    UWORD i;

    if (bulk != 0)
        n68k_copy_longs(dst, src, (ULONG)(bulk >> 2));
    for (i = bulk; i + 2 <= len; i += 2)
        *(UWORD *)(APTR)(dst + i) =
            *(const volatile UWORD *)(const volatile void *)(src + i);
    if (i < len)
        dst[i] = (UBYTE)(*(const volatile UWORD *)(const volatile void *)(src + i) >> 8);
}

/*
 * One presented frame, or nothing.  TRUE when a frame was taken and
 * acknowledged, whether it was wanted or not.
 */
static BOOL zz_rint(NetdevNic *nic)
{
    const volatile UBYTE *win = nic->board + ZZ_RX_WINDOW;
    UWORD len    = *(const volatile UWORD *)(const volatile void *)(win + 0);
    UWORD serial = *(const volatile UWORD *)(const volatile void *)(win + 2);
    const volatile UBYTE *frame = win + ZZ_RX_PAD;
    UBYTE *buf = (UBYTE *)nic->rxbuf;
    UWORD last = (UWORD)nic->core_stat[ZZ_ST_SERIAL];

    if (serial == 0)
        return FALSE;                   /* nothing presented */

    /*
     * A register write does not return to the 68k until the ARM has handled
     * it and selected the next receive slot.  Seeing the same non-zero serial
     * twice therefore means the exact acknowledgement from the preceding
     * pass was rejected; it cannot be the next frame.  This happens when the
     * ACP serves a stale serial from L2 despite the firmware's invalidate.
     *
     * Re-copying it forever is fatal: every pass re-enables the level-six
     * source while the same frame remains pending, producing an INT6/software
     * interrupt storm that leaves the whole machine apparently frozen.  The
     * firmware deliberately reserves acknowledgement value 1 as its legacy
     * bare-advance operation.  Use it only for this proven rejection, without
     * delivering the frame a second time.  This is a bounded compatibility
     * recovery for every handshake-capable firmware revision.
     */
    if (last != 0 && serial == last)
    {
        nic->core_stat[ZZ_ST_ACK_RECOVER]++;
        ZZ(nic)->gro.live = 0;          /* an unseen frame is being discarded */
        zz_put(nic, ZZ_REG_RX_ACK, 1);
        return TRUE;
    }

    /* The generator skips 0 and 1, so the successor of 0xffff is 2. */
    if (last != 0)
    {
        UWORD next = (UWORD)(last + 1);
        if (next < 2)
            next = 2;
        if (serial != next)
            nic->core_stat[ZZ_ST_GAPS]++;
    }
    nic->core_stat[ZZ_ST_SERIAL] = serial;

    if (len < NETDEV_HDR_LEN || len > NETDEV_RXBUF_MAX)
    {
        nic->core_stat[ZZ_ST_OVERSIZE]++;
        nic->rx_errors++;
        zz_put(nic, ZZ_REG_RX_ACK, serial);
        return TRUE;
    }

    /* The header first: the filter and the claim both decide from it. */
    zz_copy_frame(buf, frame, NETDEV_HDR_LEN);

    if (zz_rx_wanted(nic, buf))
    {
        APTR   token  = NULL;
        UBYTE  wanted = 0;
        UBYTE *dst    = (nic->rx_claim != NULL)
                      ? nic->rx_claim(nic->rx_arg, buf, len, &token, &wanted)
                      : NULL;

        if (dst != NULL)
        {
            UWORD  plen   = (UWORD)(len - NETDEV_HDR_LEN);
            ULONG  sum    = 0;
            UBYTE  flags  = 0;
            UBYTE  v      = 0;
            UBYTE  hw     = 0;
            BOOL   copied = FALSE;
            ZzCore *c     = ZZ(nic);

            if (c->rx_meta)
            {
                UWORD rxm = zz_get(nic, ZZ_REG_RX_META);

                if ((rxm & ZZ_RXM_PRESENT) != 0)
                    hw = (UBYTE)(rxm & ZZ_RXM_MASK);
            }

            /* The GEM has already read every byte and discards a frame whose
             * IPv4 or transport checksum fails.  Its descriptor codes 2 and
             * 3 certify TCP and UDP respectively.  Copy without the 68k's
             * ADD/ADDX per longword, then check only the structural promises
             * ANXD_S2_RXF_VERIFIED makes.  The uncommon mismatch (options,
             * padding, or malformed length) is copied again with a sum so the
             * established software path remains the exact fallback. */
            if ((wanted & ANXD_S2_RXF_VERIFIED) != 0 &&
                buf[12] == 0x08 && buf[13] == 0x00 &&
                (hw == ZZ_RXM_TCP || hw == ZZ_RXM_UDP))
            {
                zz_copy_frame(dst, frame + NETDEV_HDR_LEN, plen);
                copied = TRUE;
                v = netdev_rx_trust4(dst, plen, hw);
                if (v != 0)
                {
                    flags = v;
                    nic->core_stat[ZZ_ST_HW_VERIFIED]++;
                }
                else
                    nic->core_stat[ZZ_ST_HW_FALLBACK]++;
            }

            if (v == 0)
            {
                sum = zz_copy_payload_sum(dst, frame + NETDEV_HDR_LEN, plen);
                flags = ANXD_S2_RXF_SUMMED;
            }

            /*
             * The verdict, then the mark: a verified TCP segment that is the
             * next of the stream the previous one belonged to is chained by
             * the opener onto that one, and TCP sees the run once
             * (netdev_verify.h netdev_rx_continues).  On this machine the
             * stack's per-segment work, not the copy, is what the clock
             * goes to.
             */
            if ((wanted & ANXD_S2_RXF_VERIFIED) != 0 &&
                buf[12] == 0x08 && buf[13] == 0x00)
            {
                NetdevRxSegment seg;

                if (!copied || v == 0)
                    v = netdev_rx_verify4(dst, plen, sum);

                if (v != 0)
                    netdev_rx_segment4(dst, &seg);
                flags |= v;
                if ((wanted & ANXD_S2_RXF_CONTINUES) != 0)
                    flags |= netdev_rx_continues(&c->gro, &seg, v, ZZ_GRO_MAX);
                else
                    c->gro.live = 0;
            }
            else if ((wanted & ANXD_S2_RXF_VERIFIED) != 0 &&
                     buf[12] == 0x86 && buf[13] == 0xdd)
            {
                NetdevRxSegment seg;

                v = netdev_rx_verify6(dst, plen, sum);

                if (v != 0)
                    netdev_rx_segment6(dst, &seg);
                flags |= v;
                if ((wanted & ANXD_S2_RXF_CONTINUES) != 0)
                    flags |= netdev_rx_continues(&c->gro, &seg, v, ZZ_GRO_MAX);
                else
                    c->gro.live = 0;
            }
            else
                c->gro.live = 0;

            if ((flags & ANXD_S2_RXF_CONTINUES) != 0)
            {
                nic->core_stat[ZZ_ST_CONTINUES]++;
                if (c->gro.run == 2)
                    nic->core_stat[ZZ_ST_RUNS]++;
            }

            zz_put(nic, ZZ_REG_RX_ACK, serial);
            nic->rx_packets++;
            nic->rx_claimed(nic->rx_arg, token, sum, flags);
            return TRUE;
        }

        zz_copy_frame(buf + NETDEV_HDR_LEN, frame + NETDEV_HDR_LEN,
                      (UWORD)(len - NETDEV_HDR_LEN));
        ZZ(nic)->gro.live = 0;      /* a frame between: the run is over */
        zz_put(nic, ZZ_REG_RX_ACK, serial);
        nic->rx_packets++;
        nic->rx(nic->rx_arg, buf, len);
        return TRUE;
    }

    nic->core_stat[ZZ_ST_UNWANTED]++;
    zz_put(nic, ZZ_REG_RX_ACK, serial);
    return TRUE;
}

/* --------------------------------------------------------- interrupt ---- */

/*
 * The top half, at INT6: was it ours, and quieten it.  Masking the card's
 * enable bit is what MNT's server does too; re-armed at the end of the
 * drain below.
 */
static BOOL zz_isr(NetdevNic *nic)
{
    UWORD status = zz_get(nic, ZZ_REG_INT);

    if ((status & ZZ_INT_ETH) == 0)
        return FALSE;
    nic->core_stat[ZZ_ST_ISR]++;
    ZZ(nic)->after_isr = 1;

    /* Mask, then acknowledge.  A write with bit 3 clear is the enable word
       and bit 0 of it is Ethernet; nothing else of the status goes back,
       because bit 7 of an enable write selects the vertical blank's. */
    zz_put(nic, ZZ_REG_INT, 0);
    zz_put(nic, ZZ_REG_INT, ZZ_INT_ETH_ACK);
    return TRUE;
}

/* The drain, in the software interrupt or the vertical blank: every frame
   the ARM holds, up to the pass budget, then the interrupt re-armed. */
static BOOL zz_intr(NetdevNic *nic)
{
    BOOL  work = FALSE;
    UWORD n;

    if (!nic->running)
        return FALSE;

    for (n = 0; n < NETDEV_DRAIN_MAX; n++)
    {
        if (!zz_rint(nic))
            break;
        work = TRUE;
    }
    if (n > nic->core_stat[ZZ_ST_BURST_MAX])
        nic->core_stat[ZZ_ST_BURST_MAX] = n;
    if (!work)
    {
        UWORD rxs = zz_get(nic, ZZ_REG_RX_STATUS);

        nic->core_stat[ZZ_ST_EMPTY]++;
        if (ZZ(nic)->after_isr)
        {
            nic->core_stat[ZZ_ST_SOFT_EMPTY]++;
            /*
             * THE ARM SAYS A FRAME WAITS AND THE WINDOW SAYS NONE.  On
             * MNT's firmware from 1.6 on, the Zorro window is read through
             * the Zynq's ACP with the read-allocate bit set (mntzorro.v,
             * m00_axi_arcache = 0xf), so the header this driver polled
             * while the slot was empty sits in the ARM's L2 cache, and the
             * ARM's strongly-ordered write of the real header goes past
             * that cache to DDR.  The window then reads the stale zero
             * until something evicts the line: measured 0.1-4 ms, and
             * often longer, per frame.  MNT's own driver survives it by
             * spinning in its task; the firmware fix (this project's fork,
             * branch aminetxduo: the slot's lines dropped from L2 after
             * each header write) makes this path never run.  On the stock
             * firmware the bounded spin below is what keeps the interface
             * usable; the counters say which firmware is underneath.
             */
            if (ZZ_RX_READY(rxs) != 0)
            {
                const volatile UWORD *ser = (const volatile UWORD *)
                    (const volatile void *)(nic->board + ZZ_RX_WINDOW + 2);
                NetdevWait w;
                ULONG      reads = 0;
                BOOL       seen  = FALSE;

                netdev_wait_begin(&w, ZZ_STALE_WAIT_US, 0);
                do
                {
                    reads++;
                    if (*ser != 0)
                    {
                        seen = TRUE;
                        break;
                    }
                } while (!netdev_wait_done(&w));
                if (seen)
                {
                    nic->core_stat[ZZ_ST_LATE_HIT]++;
                    if (reads > nic->core_stat[ZZ_ST_LATE_READS])
                        nic->core_stat[ZZ_ST_LATE_READS] = reads;
                    for (n = 0; n < NETDEV_DRAIN_MAX; n++)
                    {
                        if (!zz_rint(nic))
                            break;
                        work = TRUE;
                    }
                }
                else
                    nic->core_stat[ZZ_ST_LATE_MISS]++;
            }
        }
    }
    else if (!ZZ(nic)->after_isr)
        nic->core_stat[ZZ_ST_POLL_WORK]++;
    ZZ(nic)->after_isr = 0;

    zz_put(nic, ZZ_REG_INT, ZZ_INT_ETH);
    return work;
}

/* ------------------------------------------------------------- transmit -- */

/* Into slot `slot` of the window: the staging buffer is longword aligned and
   so is every slot. */
static VOID zz_tx_fill(NetdevNic *nic, UWORD slot, const UBYTE *frame,
                       UWORD len)
{
    volatile UBYTE *win = nic->board + ZZ_TX_WINDOW +
                          (ULONG)slot * ZZ_TX_WINDOW_LEN;
    UWORD bulk = (UWORD)(len & (UWORD)~3u);
    UWORD i;

    if (bulk != 0)
        n68k_copy_longs(win, frame, (ULONG)(bulk >> 2));
    for (i = bulk; i + 2 <= len; i += 2)
        *(volatile UWORD *)(volatile void *)(win + i) =
            *(const UWORD *)(const void *)(frame + i);
    if (i < len)
        *(volatile UWORD *)(volatile void *)(win + i) =
            (UWORD)((UWORD)frame[i] << 8);
}

/*
 * The fork's firmware: frames finished since the last look, sent or
 * dropped, retire that many slots.  Asked by the shell when the ring looks
 * full, and once a blank by zz_tick while anything is in flight.  TRUE when
 * a slot came free, which is the shell's cue to pump its queue.
 */
static BOOL zz_tx_reclaim(NetdevNic *nic)
{
    UWORD now;
    UWORD n;

    if (nic->txb_inuse == 0)
        return FALSE;

    now = (UWORD)(zz_get(nic, ZZ_REG_TX_STATUS) & ZZ_TXS_COUNT);
    n   = (UWORD)((now - nic->tx_done) & ZZ_TXS_COUNT);
    nic->tx_done = now;
    if (n > nic->txb_inuse)
        n = nic->txb_inuse;
    nic->txb_inuse   = (UWORD)(nic->txb_inuse - n);
    nic->tx_completed += n;
    return (BOOL)(n != 0);
}

/* Once a blank, under Disable().  A task inside tx() under Forbid() owns
   the counters until it clears tx_busy (NetdevNic tx_task_lock), so the
   blank stands off while it is set. */
static BOOL zz_tick(NetdevNic *nic)
{
    if (!nic->running || nic->txb_cnt == 1 || nic->tx_busy)
        return FALSE;
    return zz_tx_reclaim(nic);
}

static LONG zz_tx(NetdevNic *nic, const UBYTE *frame, UWORD len)
{
    UWORD slot;

    if (!nic->running)
        return DP8390_TX_OFFLINE;
    if (len > ZZ_TX_LEN_MASK)
        return DP8390_TX_FAILED;

    if (nic->txb_cnt == 1)
    {
        UWORD result;

        zz_tx_fill(nic, 0, frame, len);

        /*
         * MNT's firmware: the length write is the send, and the ARM does
         * not answer the bus cycle until the GEM has taken the frame
         * (ethernet.c: ethernet_send_frame waits on FramesTx), so when
         * this store completes the frame is on the wire and the window is
         * free: txb_inuse never goes to 1.  The result register then reads
         * what the send returned; 0 is success.
         */
        zz_put(nic, ZZ_REG_TX, len);
        result = zz_get(nic, ZZ_REG_TX);

        nic->tx_completed++;
        if (result != 0)
        {
            nic->core_stat[ZZ_ST_TXERR]++;
            nic->tx_errors++;
            return DP8390_TX_FAILED;
        }
        nic->tx_packets++;
        return 0;
    }

    /* The fork's firmware: four slots, the bus back at once. */
    if (nic->txb_inuse >= nic->txb_cnt && !zz_tx_reclaim(nic))
        return DP8390_TX_BUSY;
    if (nic->txb_inuse >= nic->txb_cnt)
        return DP8390_TX_BUSY;

    slot = (UWORD)(nic->tx_next & (ZZ_TX_SLOTS - 1));
    zz_tx_fill(nic, slot, frame, len);
    zz_put(nic, ZZ_REG_TX,
           (UWORD)(ZZ_TX_ASYNC | (UWORD)(slot << ZZ_TX_SLOT_SHIFT) | len));
    nic->tx_next++;
    nic->txb_inuse++;
    nic->tx_packets++;
    return 0;
}

/* -------------------------------------------------------------- cache ---- */

/*
 * netdev_cache.c's question for a 68030 driving a Zorro III board: do reads
 * come back from the card, or from the data cache?  Asked before attach and
 * again after each remedy.
 *
 * A write-and-read-back cannot tell: a write that hits the cache updates the
 * entry, so the read-back agrees whether or not the board was consulted.
 * What tells is a value the BOARD changes while the CPU only reads it, and
 * the card has one that changes without any traffic: the vertical-blank
 * status, high during the output pipeline's blanking and low otherwise, and
 * the pipeline runs from power-up (the firmware's own console is on it).
 * Read it back to back until it differs from the first reading.  Served by
 * the ARM at about a microsecond a read, the loop below spans several
 * fields; served from a cache entry it returns the first reading for ever.
 *
 * Nothing but the register is touched inside the loop -- the pointer is
 * held in a register and the counter is one -- so no other access can evict
 * the very entry the test is about and answer "coherent" by accident.
 */
#define ZZ_REG_VBLANK       0x004cUL
#define ZZ_COHERENT_READS   100000UL    /* ~100 ms at the ARM's pace: 5+ fields */

static BOOL zz_coherent(NetdevNic *nic)
{
    volatile UWORD *reg   = zz_reg(nic, ZZ_REG_VBLANK);
    UWORD           first = *reg;
    ULONG           n;

    for (n = 0; n < ZZ_COHERENT_READS; n++)
        if (*reg != first)
            return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------- the table -- */

const struct NetdevNicOps netdev_nic_zz9000 =
{
    zz_attach,
    zz_init,
    zz_stop,
    zz_tx,
    zz_setfilter,
    zz_intr,
    zz_reset,
    zz_tick,        /* transmit completions are counted, once a blank */
    zz_coherent,
    NULL            /* detach: nothing was started */
};
