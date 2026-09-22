/*
 * anxnet.device, layer 2 of 3: the chip core interface.
 *
 * The SANA-II shell above never names a register.  It calls
 * attach/init/stop/tx/setfilter/intr and is handed whole Ethernet frames back
 * through a callback, so a second core is another NetdevNicOps table and no
 * change above this line.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_NIC_H
#define AMINETXDUO_NETDEV_NIC_H

#include <exec/types.h>

#include "aminetxduo/anxdiag.h"

#include "netdev_bus.h"
#include "netdev_cards.h"
#include "netdev_mcaf.h"

#define NETDEV_HDR_LEN      14
#define NETDEV_MTU          1500
#define NETDEV_FRAME_MAX    (NETDEV_HDR_LEN + NETDEV_MTU)   /* no FCS on RX */
#define NETDEV_FRAME_MIN    60

/*
 * The receive staging buffer takes anything the ring can hold, not just an
 * untagged MTU: an 802.1Q frame is 1518 bytes, and the alternative to accepting
 * it is resetting the chip.
 */
#define NETDEV_RXBUF_MAX    2048

/* How many times one interrupt can go round before it gives the machine back. */
#define NETDEV_DRAIN_MAX    32

/* Room for a core's own special-statistics records. */
#ifdef GE_PROBE_ST
#define NETDEV_CORE_STATS   42      /* + the GENET probe's thirteen RX, four TX */
#else
#define NETDEV_CORE_STATS   26      /* the GENET's shipping counter set, two GIC line counters and the held receive line 2026-09-21 */
#endif

/*
 * rx_claim's answer when NOBODY HAS A READ POSTED for the frame's type but an
 * opener with the direct pair has been reading that type: the reader is
 * behind, not absent.  A core with a ring of its own may then leave the frame
 * where it is, set rx_behind and stop the pass; the shell runs the core again
 * on ANXD_CMD_RX_POLL and the vertical blank, and the next interrupt does
 * too.  A core without a ring hands the frame to rx() as always.  Returned in
 * *token with a NULL result; never dereferenced.
 */
#define NETDEV_CLAIM_BEHIND ((APTR)1)

typedef struct NetdevNic NetdevNic;
struct NetdevMcast;

typedef VOID (*NetdevRxFn)(APTR arg, const UBYTE *frame, UWORD len);

/*
 * The 4-byte header the DP8390 writes in front of every received frame.  count
 * is byte-swapped by whichever buffer reader produced it, so by the time the
 * ring walk sees it the field is in host order.
 */
typedef struct NetdevRing
{
    UBYTE   rsr;
    UBYTE   next_packet;
    UWORD   count;
} NetdevRing;

struct NetdevNicOps
{
    /* Identify the chip and read the factory station address. 0 = present. */
    LONG  (*attach)(NetdevNic *nic);
    /* Program the chip from mac/mar/promisc and start it. 0 = running. */
    LONG  (*init)(NetdevNic *nic);
    VOID  (*stop)(NetdevNic *nic);
    /* One linear frame, header included, no FCS. 0 = queued to the chip. */
    LONG  (*tx)(NetdevNic *nic, const UBYTE *frame, UWORD len);
    /* Push mac/mar/promisc into the chip without disturbing the ring. */
    VOID  (*setfilter)(NetdevNic *nic);
    /* Drain the ring and every ISR bit. TRUE if this board had work. */
    BOOL  (*intr)(NetdevNic *nic);
    /*
     * Recover a wedged transmitter: a known state and txb_inuse cleared.  The
     * vertical-blank watchdog is the only caller and runs under Disable() at
     * INT3 against a card server at INT2, so this must not poll for
     * milliseconds.  A core that cannot wedge supplies a no-op, never NULL.
     */
    VOID  (*reset)(NetdevNic *nic);
    /*
     * Once a vertical blank, under Disable(), for a core whose link state is
     * behind a PHY that nothing interrupts on -- the GENET's is polled -- or
     * whose transmit completions are found rather than delivered.  TRUE when
     * transmit slots were freed and the shell's queue should be pumped.  Same
     * budget as reset: this must not poll for milliseconds.  NULL for a core
     * that has nothing to say between interrupts, which is all the others.
     */
    BOOL  (*tick)(NetdevNic *nic);
    /*
     * Do reads of this chip return what the chip holds, or a stale copy?
     * Asked before attach, on a chip that may be in any state, by
     * netdev_cache.c on a 68030 driving a Zorro III board: write a few words
     * to the card and read them back through the port.  NULL for a core
     * that cannot be asked; such a core is not guarded.
     */
    BOOL  (*coherent)(NetdevNic *nic);
    /*
     * The unit is going away, after stop and before its rings are freed:
     * a core that started a task at attach ends it here.  NULL for a core
     * with nothing to end.
     */
    VOID  (*detach)(NetdevNic *nic);
};


#if NETDEV_HAS_ZZ9000
/* Current firmware can route the card's shared interrupt to INT2 through
   ZZ9000.CFG.  Only the ZZ9000 core knows that firmware selection. */
BOOL netdev_zz9000_uses_int2(const NetdevNic *nic);
#endif

struct NetdevNic
{
    NetdevBus           bus;
    const NetdevCard   *card;
    const struct NetdevNicOps *ops;
    volatile UBYTE     *board;          /* the Zorro board base */

    NetdevRxFn          rx;
    APTR                rx_arg;

    /*
     * The top half, for a core whose interrupt must be quietened in the
     * server and serviced later: it acknowledges and masks the source and
     * answers whether the interrupt was this board's, nothing more.  The
     * shell then raises a software interrupt that runs ops->intr under
     * Disable(), the same way the vertical blank does.  A core may also ask
     * its own task to run ops->intr with nu_InIsr held.  NULL for a core whose
     * intr() is the server, which is every core on a Zorro or PCMCIA bus.
     */
    BOOL              (*isr)(NetdevNic *nic);

    /*
     * The direct-receive path.  rx_claim asks, from the frame's first
     * NETDEV_HDR_LEN bytes, where the payload should be drained to; a NULL
     * answer or a NULL hook means stage-and-rx() as always.  A claimed frame is
     * always finished with rx_claimed.  Both run in the same context rx() does.
     * rx_claim writes the optional ANXD_S2_RXF_* bits the opener negotiated
     * to `wanted`, so a core need not calculate capabilities nobody consumes.
     * `flags` on completion are SUMMED when `sum` is the fused sum, plus the
     * subset of `wanted` the core can say about this frame.
     */
    UBYTE            *(*rx_claim)(APTR arg, const UBYTE *hdr, UWORD frame_len,
                                  APTR *token, UBYTE *wanted);
    VOID              (*rx_claimed)(APTR arg, APTR token, ULONG sum,
                                    UBYTE flags);
    /* ANXD_S2_RXF_* verdicts this core can produce.  The shell intersects
       negotiation with this after it knows which unit was opened. */
    UBYTE               rx_flags_supported;
    /* ANXD_S2_TXF_* checksums this core's chip can write on the way out, and
       the shell's request for the frame ops->tx is being handed: the
       negotiated bits returned by the opener's per-write extension callback,
       else zero. */
    UBYTE               tx_csum_supported;
    UBYTE               tx_csum;
    ULONG               rx_verified;   /* direct frames carrying VERIFIED */
    /*
     * A core whose transmit completions are found by reading the chip, not
     * by an interrupt (the GENET takes no TX interrupt), retires them here on
     * request: the shell asks before it calls the ring full and queues a
     * write.  Without it a write that met a momentarily full ring waited for
     * the next receive interrupt or the vertical blank to be issued, and the
     * shim's sender slept a tick behind it -- transmit ran in bursts of one
     * ring at a time and the A1200 sent 65 Mbit/s while receiving 270.
     * TRUE when something was retired.  NULL for a core with an interrupt.
     */
    BOOL              (*tx_reclaim)(NetdevNic *nic);
    /*
     * The write's frame is built inside the same Disable() that issues it.
     * netdev_tx_direct() takes the mask twice per write so that the opener's
     * copy -- 135 of the 219 us a transmit took on a real 68020 -- runs with
     * interrupts on; on Emu68 that copy is under a microsecond and the second
     * Disable() pair is a 5.5 us trap, so a core there asks for one section.
     */
    UBYTE               tx_short_build;
    /*
     * The core's transmit side needs no interrupt mask from a task: its
     * interrupt reclaims finished frames (the consumer index) and never
     * produces, it stands off while a task-level transmit is under way
     * (tx_busy below), and the in-use count is recomputed from the two
     * indices rather than shared.  netdev_tx_direct() then takes Forbid()
     * -- 0.1 us on Emu68 where the Disable() pair is a 5.5 us trap, 13% of
     * the CPU at 24,000 frames a second -- and Disable() only around the
     * unit's write list, which the vertical blank's pump shares.  A core
     * whose interrupt transmits (the DP8390's completion interrupt pumps
     * the list) leaves this clear.
     */
    UBYTE               tx_task_lock;
    /*
     * Initialising this core may wait or call task-context-only code.  Most
     * classic chips need init serialised with their interrupt server under
     * Disable(); a bus-master whose source stays masked until init finishes
     * can ask the shell to call it in the opener's task instead.
     */
    UBYTE               init_task_context;
    /* Set by netdev_tx_direct() for the whole of a task-level build and
       issue under tx_task_lock; the core's interrupt-side reclaim and the
       blank's pump stand off while it is set. */
    volatile UBYTE      tx_busy;
    /*
     * The core keeps received frames in its own ring while the opener has
     * no read posted (NETDEV_CLAIM_BEHIND), so the opener can re-post the
     * completed read and explicitly ask the core to resume before sleeping.
     * A core without this receives each read back immediately, as an ordinary
     * SANA-II device does.
     */
    /*
     * Bytes of received frames the card's own memory holds before it must
     * drop one: the DP8390 ring, the LANCE's buffers, the EtherLink III's
     * FIFO, the GENET's ring.  Set at attach, answered to
     * ANXD_CMD_RX_CAPACITY; 0 when the core cannot say.
     */
    ULONG               rx_capacity;
    UBYTE               rx_holds;
    /* The core delivers bursts -- many frames in one service pass -- so
       ANXD_CMD_RX_BATCH is worth offering: one reply per pass instead of
       one per frame.  A core that raises an interrupt per frame gains
       nothing from a batch and loses a pool of posted reads to it, so it
       leaves this 0 and its openers keep CMD_READs. */
    UBYTE               rx_batches;
    /* The opener's write is one of a run (ANXD_S2_TXF_MORE, from the shell
       per write): a core may hold the hardware start for company.  Meaning
       only on a core that sets tx_flush, the start it holds back. */
    UBYTE               tx_more;
    VOID              (*tx_flush)(struct NetdevNic *nic);

    /* Even, and stated rather than inherited from what precedes them:
       netdev_device.c copies both as a longword and a word, which is an
       address error on a 68000 if a field reorder ever lands them odd. */
    UBYTE               factory[NETDEV_ADDR_LEN] __attribute__((aligned(2)));
    UBYTE               mac[NETDEV_ADDR_LEN] __attribute__((aligned(2)));
    UBYTE               mar[8];         /* the multicast hash, host order */
    BOOL                promisc;
    BOOL                running;
    /* Set by a core that left received frames in its ring for want of a
       posted read (NETDEV_CLAIM_BEHIND); cleared by the pass that drains
       them.  The shell's ANXD_CMD_RX_POLL runs the core when it is set. */
    UBYTE               rx_behind;
    /*
     * The unit's exact multicast table, for a core that filters on addresses
     * rather than a hash (the GENET has 17 exact slots and no hash).  Set by
     * the shell beside rx/rx_arg; all_multi is the shell's "a range too wide
     * for the table" latch, refreshed with every filter rebuild.
     */
    const struct NetdevMcast *mc_table;
    UWORD               mc_max;
    UBYTE               all_multi;
    UBYTE               pad_mc;

    /* DP8390 ring state, the names are NetBSD's. */
    /*
     * Where the chip's remote-DMA pointer is, and how much of the burst is
     * left: a read of the 4-byte ring header leaves the pointer exactly at the
     * frame body.  dma_left is 0 whenever the position is not to be trusted.
     */
    LONG                dma_pos;
    UWORD               dma_left;

    LONG                mem_start;
    LONG                mem_end;
    LONG                mem_size;
    LONG                mem_ring;
    UWORD               txb_cnt;
    ULONG               serial;     /* the board's autoconfig serial number */
    UWORD               txb_inuse;

    /* netdev_cache.c: how the 68030's data cache is kept off this board,
       NETDEV_CACHE_*, and what to put back when it is let go. */
    UBYTE               cache_guard;
    UBYTE               cache_why;      /* NETDEV_CACHE_WHY_* */
    ULONG               cache_saved;

    /*
     * Hardware transmit completions, successful or not.  Unlike tx_packets this
     * means the same thing in every core and is never cleared by a chip reset;
     * the vertical-blank watchdog compares snapshots of it.
     */
    ULONG               tx_completed;

    /*
     * EtherLink III.  el3_swap is measured at attach from the window 0
     * manufacturer ID and is not a card-table knob.  el3_win is the window last
     * selected, because the part has no readable window register.  el3_media is
     * read-only.
     */
    UBYTE               el3_swap;
    UBYTE               el3_win;
    UWORD               el3_media;

    /*
     * LANCE.  le_rap is the register the address port last selected, cached
     * for the same reason el3_win above is: RAP is write-only, so every CSR
     * access had to set it, and a CSR access is two Zorro transactions where
     * one would do.  In the steady state EVERY access is CSR0 -- the
     * interrupt handler's read, acknowledge and re-read, and lance_tx's
     * demand write -- so after the first the port already holds what the next
     * access wants.
     *
     * SAFE AGAINST THE INTERRUPT, and no more exposed than the code it
     * replaces: an interrupt that lands between a RAP write and its RDP
     * access already corrupted that access before this cache existed, and
     * both sides of that window use CSR0 anyway.  Every writer goes through
     * le_csr_get/le_csr_put, so a mismatch simply writes RAP again and the
     * cache heals itself.  LE_RAP_UNKNOWN is stored wherever the chip may
     * have been reset under us, because a reset leaves RAP undefined.
     */
    UWORD               le_rap;

    /* LANCE ring cursors.  The DP8390 cores do not use them: their ring is
       the chip's own page walk, not an indexed descriptor list. */
    UWORD               rx_next;
    UWORD               tx_next;
    UWORD               tx_done;
    UWORD               txb_new;
    UWORD               txb_next_tx;
    UWORD               txb_len[3];
    UWORD               tx_page_start;
    UWORD               rec_page_start;
    UWORD               rec_page_stop;
    UWORD               next_packet;
    UBYTE               cr_proto;
    UBYTE               rcr_proto;
    UBYTE               dcr_reg;
    UBYTE               filter_pending; /* LANCE: apply after TX ring drains */

    /*
     * How the packet buffer is reached.  NE2000 fills these with its remote
     * DMA; a shared-memory DP8390 board fills them with moves through its
     * mapped window, and that is the whole of the difference.
     */
    VOID  (*read_hdr)(NetdevNic *nic, LONG src, NetdevRing *hdr);
    LONG  (*ring_copy)(NetdevNic *nic, LONG src, UBYTE *dst, UWORD amount);

    /*
     * ring_copy for the direct-receive destination, with the Internet checksum
     * of what was moved for the price of the move.  Exactly `amount` bytes are
     * written, so the caller needs no netdev_ring_copy_exact() around it.
     *
     * FALSE, having touched neither the chip nor the destination, whenever the
     * fusion does not apply -- a wrapped read, an 8-bit port, an odd
     * destination -- and the caller then takes the ordinary path and the frame
     * is walked for its checksum as before.  NULL for a core that has no fused
     * form at all.
     */
    BOOL  (*ring_copy_sum)(NetdevNic *nic, LONG src, UBYTE *dst, UWORD amount,
                           ULONG *sum);

    /*
     * A pointer straight into the card's own buffer, or NULL when the frame
     * must be staged.  Set only by cores whose buffer is memory-mapped; the
     * receive path then makes one pass over the frame.  NULL for a wrapped
     * frame, and NULL for every port-driven core.
     */
    const volatile UBYTE *(*frame_at)(NetdevNic *nic, LONG src, UWORD len);

    /*
     * Where to frame the next transmit, or NULL to use the unit's staging
     * buffer.  NULL for the DP8390 cores: an NE2000's buffer is behind a port,
     * and the Hydra and LAN Rover map a buffer whose byte lanes are not
     * separately selectable, so a byte write would corrupt both halves.
     */
    UBYTE *(*tx_at)(NetdevNic *nic);
    UWORD (*write_buf)(NetdevNic *nic, const UBYTE *frame, UWORD len,
                       LONG buf);

    /* Counters the shell reports through S2_GETGLOBALSTATS / SPECIALSTATS. */
    ULONG               rx_packets;
    ULONG               tx_packets;
    ULONG               rx_errors;
    ULONG               tx_errors;
    ULONG               overruns;       /* receive FIFO/ring only */
    ULONG               tx_underruns;
    ULONG               collisions;
    ULONG               resets;

    /*
     * What had to be done to the station address before the chip could be given
     * one.  All three are 0 or 1 and all three are reported, because a silently
     * repaired or invented hardware address is hard to diagnose later.
     */
    ULONG               mac_group_fix;  /* group bit cleared out of the PROM  */
    ULONG               mac_from_cis;   /* PROM blank, address taken from CIS */
    ULONG               mac_derived;    /* PROM blank, address derived here   */

    /*
     * The two fields the probe record reads out of a core.  Both are set on the
     * way through attach() and neither is used for anything else.
     */
    UBYTE               diag_why;       /* ANXDIAG_WHY_*, 0 = did not say     */
    UBYTE               mac_source;     /* ANXDIAG_MAC_*                      */

    /*
     * NETDEV_BUS_DTREE: what the device tree said about the board, filled by
     * the probe before attach() runs.  dt_irq is the interrupt controller's
     * own number (a GIC SPI is its number plus 32), 0 when the tree named none.
     * dt_irq_live says AddIntServerEx accepted this unit's server.  They are
     * separate so a failed first open can fall back to polling and a later
     * open can retry registration without rediscovering the device tree.
     */
    ULONG               dt_irq;
    UBYTE               dt_irq_live;
    UBYTE               dt_mac[NETDEV_ADDR_LEN] __attribute__((aligned(2)));
    UBYTE               dt_mac_ok;      /* the tree carried local-mac-address */
    UBYTE               dt_phy;         /* MDIO address of the PHY, from the tree */

    /*
     * Memory a core allocated for itself at attach, freed by the shell after
     * detach at expunge.  This is allocation ownership only: bus_master says
     * independently whether the chip can keep writing RAM through a reboot.
     */
    APTR                core_mem;
    ULONG               core_size;
    APTR                core;           /* the core's own state inside it     */
    UBYTE               bus_master;

    /*
     * Counters only this core has names for, appended to S2_GETSPECIALSTATS
     * after the shell's own so nothing learned by index moves.  core_stat_names
     * is NULL-terminated; core_stat[i] goes with core_stat_names[i].
     */
    const char *const  *core_stat_names;
    ULONG               core_stat[NETDEV_CORE_STATS];

    /* One frame at a time comes out of the ring, into here. */
    ULONG               rxbuf[(NETDEV_RXBUF_MAX + 7) / 4];
};

/*
 * Copy exactly `amount` bytes out of a ring into a caller-sized destination.
 * A 16-bit remote-DMA core may consume and store a whole final word for an odd
 * request and a direct-receive destination has no padding for it, so the odd
 * byte goes through an aligned two-byte scratch object.
 */
static inline LONG netdev_ring_copy_exact(NetdevNic *nic, LONG src,
                                          UBYTE *dst, UWORD amount)
{
    if ((amount & 1u) != 0)
    {
        UWORD  scratch = 0;
        UBYTE *tail    = (UBYTE *)(APTR)&scratch;
        UWORD  bulk    = (UWORD)(amount - 1u);

        if (bulk != 0)
        {
            src  = nic->ring_copy(nic, src, dst, bulk);
            dst += bulk;
        }

        src = nic->ring_copy(nic, src, tail, 1);
        *dst = tail[0];
        return src;
    }

    return nic->ring_copy(nic, src, dst, amount);
}

/*
 * The probe record, netdev_diag.c.  Declared here rather than in
 * netdev_internal.h, because the chip cores are half of what records into it.
 * netdev_diag_note() is safe to call from anywhere the probe reaches, cannot
 * fail and allocates nothing.
 */
VOID  netdev_diag_reset(AnxDiagMark *mark);
VOID  netdev_diag_note(UWORD code, UWORD card, ULONG value);
VOID  netdev_diag_counts(UWORD units, UWORD dropped);
UWORD netdev_diag_card(const NetdevCard *card);
VOID  netdev_diag_publish(AnxDiagMark *mark);
VOID  netdev_diag_unpublish(AnxDiagMark *mark);

/* The six cores. netdev_nic_ops_for() returns NULL for a chip with no core. */
extern const struct NetdevNicOps netdev_nic_ne2000;
extern const struct NetdevNicOps netdev_nic_ed;
extern const struct NetdevNicOps netdev_nic_lance;
extern const struct NetdevNicOps netdev_nic_el3;
extern const struct NetdevNicOps netdev_nic_genet;
extern const struct NetdevNicOps netdev_nic_zz9000;

const struct NetdevNicOps *netdev_nic_ops_for(UBYTE chip);


/*
 * The LANCE entry points that netdev_device.c dispatches to and
 * tests/netdev drives directly.  Declared so the compiler checks each
 * definition against what its callers are told; lance_reset() is not here
 * because nothing outside lance.c calls it.
 */
VOID lance_halt(NetdevNic *nic);
VOID lance_setfilter(NetdevNic *nic);
LONG lance_tx(NetdevNic *nic, const UBYTE *frame, UWORD len);
BOOL lance_intr(NetdevNic *nic);
LONG lance_attach(NetdevNic *nic);

/* One tagged value on the serial trace, netdev_device.c.  A real function in
   a NETDEV_TRACE or NETDEV_TIME build and absent otherwise; the chip cores
   call it only under their own *_TRACE macros, which the same defines gate. */
VOID netdev_trace_val(const char *tag, ULONG v);

#endif /* AMINETXDUO_NETDEV_NIC_H */
