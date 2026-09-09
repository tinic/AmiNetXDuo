/*
 * The DP8390 core: the chip driver beneath ne2000.c AND ed.c, and therefore
 * beneath six of the nine cards the sweep drives -- X-Surf, X-Surf 100,
 * Ariadne II, Hydra, ASDG LAN Rover and the PCMCIA slot.
 *
 * IT HAD NO HOST TEST AT ALL, and the way it was missed is worth stating.
 * test_netdev_ne2000.c and test_netdev_ed.c both cover their card glue by
 * #including it whole -- and both STUB every entry point of the core beneath
 * it (test_netdev_ne2000.c:133-140, test_netdev_ed.c:33-37, seven functions
 * each).  So the two tests that look like they exercise a DP8390 are the two
 * that guarantee they do not, and an inventory by "which .c files does a test
 * name" counts this file as covered twice over.
 *
 * That is the exact shape the heartbeat warns about: shared code that ships to
 * every core while only the a2065 -- which is a LANCE and never reaches this
 * file -- can be measured on the rig.
 *
 * A BYTE ARRAY IS NOT A MODEL OF THIS CHIP.  The register file is four banks
 * of sixteen selected by CR bits 7..6, so PAR0 on page 1 and PSTART on page 0
 * are ONE register index: an array fuses them, and a driver that forgot to
 * select page 1 before writing the station address would still pass.  The
 * model below banks them, tracks the page from every CR write, and records
 * every access in order -- because the order is the National manual's and, as
 * dp8390.c:156 puts it, "is not negotiable".
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#include "netdev_nic.h"
#include "netdev_clock.h"

/* ------------------------------------------------------------- the chip -- */

#define PAGES 4
#define REGS  16

static UBYTE chip[PAGES][REGS];
static UBYTE chip_page;

#define TRACE_MAX 512
static struct { UBYTE page; UBYTE reg; UBYTE val; UBYTE read; } tr[TRACE_MAX];
static unsigned tr_n;

static void trace(UBYTE page, UBYTE reg, UBYTE val, UBYTE read)
{
    if (tr_n < (unsigned)TRACE_MAX)
    {
        tr[tr_n].page = page;
        tr[tr_n].reg  = reg;
        tr[tr_n].val  = val;
        tr[tr_n].read = read;
        tr_n++;
    }
}

/*
 * CR is register 0 in every bank and is what selects the bank, so it is held
 * outside them.  Everything else is banked.
 */
static UBYTE chip_cr;

/*
 * The NetdevNic is threaded through and ignored.  It has to be: the real
 * NIC_GET/NIC_PUT reach the bus through it, so a model that dropped the
 * argument would leave the parameter unused in every function that only
 * touches registers -- and ci-warnings.cmake builds these with
 * -Werror=unused-parameter.
 */
static void chip_put(const NetdevNic *n, UWORD reg, UBYTE val)
{
    UBYTE r = (UBYTE)(reg & 0x0fu);

    (VOID)n;

    if (r == 0)
    {
        chip_cr   = val;
        chip_page = (UBYTE)((val >> 6) & 0x03u);
        trace(chip_page, r, val, 0);
        return;
    }

    trace(chip_page, r, val, 0);
    chip[chip_page][r] = val;
}

static UBYTE chip_get(const NetdevNic *n, UWORD reg)
{
    UBYTE r = (UBYTE)(reg & 0x0fu);
    UBYTE v = (r == 0) ? chip_cr : chip[chip_page][r];

    (VOID)n;

    trace(chip_page, r, v, 1);

    return v;
}

#define NIC_GET(nic, reg)       chip_get((nic), (UWORD)(reg))
#define NIC_PUT(nic, reg, val)  chip_put((nic), (UWORD)(reg), (UBYTE)(val))

/* ------------------------------------------------------------- the deps -- */

static ULONG waits;

VOID netdev_wait_begin(NetdevWait *w, ULONG us, ULONG spins)
{
    (VOID)us;
    (VOID)spins;
    w->nw_Spins = 0;
    waits++;
}

BOOL netdev_wait_done(NetdevWait *w)
{
    (VOID)w;
    return TRUE;
}

#include "dp8390.c"

/* ------------------------------------------------------------ the checks - */

static int failures;
static int checks;

static void expect(int ok, const char *what)
{
    checks++;
    if (ok)
        return;

    printf("FAIL %s\n", what);
    failures++;
}

static void expect_hex(const char *what, unsigned long got, unsigned long want)
{
    checks++;
    if (got == want)
        return;

    printf("FAIL %s: got $%02lx, want $%02lx\n", what, got, want);
    failures++;
}

/* The index of the nth write of `val` to (page, reg), or -1. */
static int find_w(UBYTE page, UBYTE reg, int from)
{
    int i;

    for (i = from; i < (int)tr_n; i++)
    {
        if (!tr[i].read && tr[i].page == page && tr[i].reg == reg)
            return i;
    }

    return -1;
}

static int find_w_val(UBYTE page, UBYTE reg, UBYTE val, int from)
{
    int i;

    for (i = from; i < (int)tr_n; i++)
    {
        if (!tr[i].read && tr[i].page == page && tr[i].reg == reg &&
            tr[i].val == val)
            return i;
    }

    return -1;
}

/* ------------------------------------------------------------ the fixture */

static NetdevNic nic;

/* The frame the ring hands up, and where the hooks were told to find it. */
static UBYTE  hdr_status;
static UBYTE  hdr_next;
static UWORD  hdr_count;
static int    frames_up;
static LONG   frame_at_saw;
static UWORD  frame_len_saw;

static VOID t_read_hdr(NetdevNic *n, LONG src, NetdevRing *r)
{
    (VOID)n;
    (VOID)src;
    r->rsr         = hdr_status;
    r->next_packet = hdr_next;
    r->count       = hdr_count;
}

/*
 * NULL, which is what every DP8390 core answers: an NE2000's buffer is behind
 * a port, and the mapped-buffer boards cannot be byte-addressed.  The staging
 * path through ring_copy() is therefore the one this test drives, and it is
 * the one every card in the tree actually takes.
 */
static const volatile UBYTE *t_frame_at(NetdevNic *n, LONG src, UWORD len)
{
    (VOID)n;
    frame_at_saw  = src;
    frame_len_saw = len;

    return NULL;
}

static int   ring_copies;
static LONG  ring_src;
static UWORD ring_len;

static LONG t_ring_copy(NetdevNic *n, LONG src, UBYTE *dst, UWORD amount)
{
    (VOID)n;
    (VOID)dst;
    ring_copies++;
    ring_src = src;
    ring_len = amount;

    return src + (LONG)amount;
}

static UWORD t_write_buf(NetdevNic *n, const UBYTE *frame, UWORD len, LONG buf)
{
    (VOID)n;
    (VOID)frame;
    (VOID)buf;

    return len;
}

static VOID t_rx(APTR arg, const UBYTE *frame, UWORD len)
{
    (VOID)arg;
    (VOID)frame;
    frames_up++;
    frame_len_saw = len;
}

static void reset(void)
{
    memset(chip, 0, sizeof(chip));
    memset(&nic, 0, sizeof(nic));
    chip_cr   = 0;
    chip_page = 0;
    tr_n      = 0;
    waits     = 0;
    ring_copies = 0;
    frames_up = 0;

    /* An NE2000's geometry: 16 KB at page $40, six transmit pages. */
    nic.cr_proto       = ED_CR_RD2;
    nic.rcr_proto      = 0;
    nic.dcr_reg        = ED_DCR_FT1 | ED_DCR_WTS | ED_DCR_LS;
    nic.mem_start      = 0;
    nic.mem_size       = 16384;
    nic.tx_page_start  = 0x40;
    nic.rec_page_start = (UBYTE)(0x40 + 2 * ED_TXBUF_SIZE);
    nic.rec_page_stop  = 0x80;
    nic.txb_cnt        = 2;

    nic.read_hdr  = t_read_hdr;
    nic.frame_at  = t_frame_at;
    nic.ring_copy = t_ring_copy;
    nic.write_buf = t_write_buf;
    nic.rx        = t_rx;

    memcpy(nic.mac, "\x02\x41\x4d\x49\x00\x01", 6);
    memset(nic.mar, 0xa5, sizeof(nic.mar));
}

/* ================================================================ init === */

/*
 * dp8390.c:156 -- "This order is the National manual's and is not negotiable."
 * A comment saying so is not a gate.  These are the steps whose ORDER the
 * manual fixes, each asserted against the one before it.
 */
static void a_init_follows_the_manual(void)
{
    int stp, dcr, rbcr0, rbcr1, rcr_mon, tcr_lb, bnry, pstart, pstop;
    int imr, isr, page1, par0, curr, back0, rcr_run, tcr_run, sta;

    reset();
    expect(dp8390_init(&nic) == 0, "init reports success");
    expect(nic.running == TRUE, "and leaves the chip running");

    /* The chip is stopped before anything else is touched. */
    stp = find_w_val(0, ED_P0_CR,
                     (UBYTE)(nic.cr_proto | ED_CR_PAGE_0 | ED_CR_STP), 0);
    expect(stp == 0, "the first write of all stops the chip on page 0");

    dcr     = find_w(0, ED_P0_DCR, stp);
    rbcr0   = find_w(0, ED_P0_RBCR0, stp);
    rbcr1   = find_w(0, ED_P0_RBCR1, stp);
    rcr_mon = find_w(0, ED_P0_RCR, stp);
    tcr_lb  = find_w(0, ED_P0_TCR, stp);
    bnry    = find_w(0, ED_P0_BNRY, stp);
    pstart  = find_w(0, ED_P0_PSTART, stp);
    pstop   = find_w(0, ED_P0_PSTOP, stp);
    imr     = find_w(0, ED_P0_IMR, stp);
    isr     = find_w(0, ED_P0_ISR, stp);

    expect(dcr > stp, "the data configuration register comes after the stop");
    expect(rbcr0 > dcr && rbcr1 > dcr,
           "then the remote byte count is cleared");
    expect(rcr_mon > rbcr1, "then the receiver is put in monitor mode");
    expect(tcr_lb > rcr_mon, "then the transmitter is looped back");
    expect(bnry > tcr_lb && pstart > bnry && pstop > pstart,
           "then the ring is described, boundary first");
    expect(imr > pstop && isr > imr,
           "then interrupts are masked and the status cleared");

    /* The DCR the caller asked for, because ED_DCR_LS is set in it. */
    expect_hex("the caller's DCR is used when it names a loopback setting",
               tr[dcr].val, nic.dcr_reg);

    /* The ring registers carry the geometry, not a default. */
    expect_hex("BNRY is the first receive page", tr[bnry].val,
               nic.rec_page_start);
    expect_hex("PSTART is the first receive page", tr[pstart].val,
               nic.rec_page_start);
    expect_hex("PSTOP is one past the last", tr[pstop].val,
               nic.rec_page_stop);
    expect_hex("ISR is cleared with all ones", tr[isr].val, 0xffu);

    /*
     * PAGE 1, and this is the assertion a fused register file cannot make:
     * the station address and the hash go to page 1, and CURR with them.
     */
    page1 = find_w_val(1, ED_P1_CR,
                       (UBYTE)(nic.cr_proto | ED_CR_PAGE_1 | ED_CR_STP), isr);
    expect(page1 > isr, "page 1 is selected after the page-0 programming");

    par0 = find_w(1, ED_P1_PAR0, page1);
    curr = find_w(1, ED_P1_CURR, page1);
    expect(par0 > page1, "the station address is written on page 1");
    expect(curr > par0, "and the current page after it");

    {
        int i;
        int good = 1;

        for (i = 0; i < 6; i++)
        {
            int at = find_w(1, (UBYTE)(ED_P1_PAR0 + i), page1);

            if (at < 0 || tr[at].val != nic.mac[i])
                good = 0;
        }
        expect(good, "all six PAR bytes carry the address in force");

        good = 1;
        for (i = 0; i < 8; i++)
        {
            int at = find_w(1, (UBYTE)(ED_P1_MAR0 + i), page1);

            if (at < 0 || tr[at].val != nic.mar[i])
                good = 0;
        }
        expect(good, "and all eight MAR bytes carry the hash");
    }

    expect_hex("CURR is one page past the ring's start", tr[curr].val,
               (unsigned long)(UBYTE)(nic.rec_page_start + 1));
    expect_hex("and next_packet agrees with it", nic.next_packet,
               (unsigned long)(UWORD)(nic.rec_page_start + 1));

    /* Back to page 0, and only then is the receiver taken out of monitor
       mode and the transmitter out of loopback. */
    back0 = find_w_val(0, ED_P1_CR,
                       (UBYTE)(nic.cr_proto | ED_CR_PAGE_0 | ED_CR_STP), curr);
    expect(back0 > curr, "page 0 is restored");

    rcr_run = find_w(0, ED_P0_RCR, back0);
    tcr_run = find_w(0, ED_P0_TCR, back0);
    expect(rcr_run > back0, "the receiver leaves monitor mode on page 0");
    expect_hex("accepting broadcast and multicast", tr[rcr_run].val,
               (unsigned long)(UBYTE)(ED_RCR_AB | ED_RCR_AM | nic.rcr_proto));
    expect(tcr_run > back0, "and the transmitter leaves loopback");
    expect_hex("with a zero TCR", tr[tcr_run].val, 0x00u);

    /* The very last write starts it. */
    sta = find_w_val(0, ED_P0_CR,
                     (UBYTE)(nic.cr_proto | ED_CR_PAGE_0 | ED_CR_STA), tcr_run);
    expect(sta > tcr_run, "and the chip is started last of all");
    expect(sta == (int)tr_n - 1, "with nothing written after it");
}

/* A caller whose DCR does not name a loopback setting gets the driver's. */
static void b_init_supplies_a_dcr_when_the_caller_did_not(void)
{
    int dcr;

    reset();
    nic.dcr_reg = ED_DCR_FT1;           /* no ED_DCR_LS */
    (VOID)dp8390_init(&nic);

    dcr = find_w(0, ED_P0_DCR, 0);
    expect(dcr > 0, "a DCR was written");
    expect_hex("and it is the driver's own, not the caller's",
               tr[dcr].val, (unsigned long)(UBYTE)(ED_DCR_FT1 | ED_DCR_LS));
}

/* ================================================================ halt === */

static void c_halt_leaves_nothing_asserted(void)
{
    int imr, isr;

    reset();
    (VOID)dp8390_init(&nic);
    tr_n = 0;

    dp8390_halt(&nic);

    imr = find_w(0, ED_P0_IMR, 0);
    isr = find_w(0, ED_P0_ISR, 0);

    expect(imr >= 0, "halt masks the interrupts");
    expect_hex("with a zero IMR", tr[imr].val, 0x00u);
    expect(isr > imr, "and clears the status after masking, not before");
    expect_hex("with all ones", tr[isr].val, 0xffu);
    expect(nic.running == FALSE, "and the chip is no longer running");
}

/* A reset is a halt and an init, and it is counted where netstat can see it. */
static void d_reset_is_counted(void)
{
    reset();
    (VOID)dp8390_init(&nic);

    expect_hex("no resets yet", nic.resets, 0);
    dp8390_reset(&nic);
    expect_hex("a reset is counted", nic.resets, 1);
    expect(nic.running == TRUE, "and leaves the chip running again");
}

/* =========================================================== the filter == */

/*
 * setfilter runs on a LIVE chip, so it must select page 1, write, and put
 * page 0 back -- and it must keep the run state it found.  A version that
 * left page 1 selected would send every later page-0 access to the wrong
 * bank, which on this chip is silent.
 */
static void e_setfilter_banks_and_restores(void)
{
    int to1, par0, mar0, to0, rcr;
    int i;
    int good;

    reset();
    (VOID)dp8390_init(&nic);
    tr_n = 0;

    memcpy(nic.mac, "\x02\x41\x4d\x49\x0c\x0c", 6);
    memset(nic.mar, 0x5a, sizeof(nic.mar));

    dp8390_setfilter(&nic);

    to1 = find_w_val(1, ED_P0_CR,
                     (UBYTE)(nic.cr_proto | ED_CR_PAGE_1 | ED_CR_STA), 0);
    expect(to1 == 0, "page 1 is selected first, and the chip kept running");

    par0 = find_w(1, ED_P1_PAR0, to1);
    mar0 = find_w(1, ED_P1_MAR0, to1);
    expect(par0 > to1 && mar0 > to1, "the address and the hash go to page 1");

    good = 1;
    for (i = 0; i < 8; i++)
    {
        int at = find_w(1, (UBYTE)(ED_P1_MAR0 + i), to1);

        if (at < 0 || tr[at].val != 0x5au)
            good = 0;
    }
    expect(good, "all eight hash bytes, from the state the shell keeps");

    to0 = find_w_val(0, ED_P1_CR,
                     (UBYTE)(nic.cr_proto | ED_CR_PAGE_0 | ED_CR_STA), mar0);
    expect(to0 > mar0, "page 0 is restored before anything else is touched");

    rcr = find_w(0, ED_P0_RCR, to0);
    expect(rcr > to0, "and the receive configuration is written on page 0");
    expect_hex("accepting broadcast and multicast", tr[rcr].val,
               (unsigned long)(UBYTE)(ED_RCR_AB | ED_RCR_AM | nic.rcr_proto));

    expect_hex("the chip is left on page 0", chip_page, 0);
}

/* Promiscuous adds the three bits that make it promiscuous, and nothing is
   promiscuous without them. */
static void f_promiscuous_is_three_bits(void)
{
    int rcr;

    reset();
    (VOID)dp8390_init(&nic);
    nic.promisc = 1;
    tr_n = 0;

    dp8390_setfilter(&nic);

    rcr = find_w(0, ED_P0_RCR, 0);
    expect(rcr > 0, "a receive configuration was written");
    expect_hex("carrying promiscuous, all-runt and separate-error",
               tr[rcr].val,
               (unsigned long)(UBYTE)(ED_RCR_AB | ED_RCR_AM | nic.rcr_proto |
                                      ED_RCR_PRO | ED_RCR_AR | ED_RCR_SEP));

    /* And a chip that is stopped is programmed stopped, not started. */
    reset();
    nic.running = FALSE;
    tr_n = 0;
    dp8390_setfilter(&nic);
    expect(find_w_val(1, ED_P0_CR,
                      (UBYTE)(nic.cr_proto | ED_CR_PAGE_1 | ED_CR_STP), 0) == 0,
           "a stopped chip is not started by a filter change");
}

/* ============================================================ transmit === */

static void g_transmit(void)
{
    static const UBYTE frame[64] = { 0 };
    int tpsr, tbcr0, tbcr1, txp;

    /* Offline is refused without touching the chip. */
    reset();
    tr_n = 0;
    expect_hex("a transmit on a stopped chip",
               (unsigned long)(ULONG)dp8390_tx(&nic, frame, sizeof(frame)),
               (unsigned long)(ULONG)DP8390_TX_OFFLINE);
    expect(tr_n == 0, "and the chip was not touched");

    reset();
    (VOID)dp8390_init(&nic);
    tr_n = 0;

    expect_hex("the first frame is accepted",
               (unsigned long)(ULONG)dp8390_tx(&nic, frame, sizeof(frame)), 0);

    tpsr  = find_w(0, ED_P0_TPSR, 0);
    tbcr0 = find_w(0, ED_P0_TBCR0, 0);
    tbcr1 = find_w(0, ED_P0_TBCR1, 0);

    expect(tpsr >= 0, "the transmit page start was written");
    expect_hex("as the first transmit buffer", tr[tpsr].val,
               nic.tx_page_start);
    expect_hex("the low half of the byte count", tr[tbcr0].val,
               (unsigned long)(UBYTE)sizeof(frame));
    expect_hex("and the high half", tr[tbcr1].val, 0x00u);

    txp = find_w_val(0, ED_P0_CR,
                     (UBYTE)(nic.cr_proto | ED_CR_PAGE_0 | ED_CR_TXP |
                             ED_CR_STA), 0);
    expect(txp > tbcr1, "and the transmit is started after the count is set");

    /* A second frame fits the other buffer and does NOT start the chip: it is
       started when the first completes. */
    tr_n = 0;
    expect_hex("the second frame is accepted",
               (unsigned long)(ULONG)dp8390_tx(&nic, frame, sizeof(frame)), 0);
    expect(find_w_val(0, ED_P0_CR,
                      (UBYTE)(nic.cr_proto | ED_CR_PAGE_0 | ED_CR_TXP |
                              ED_CR_STA), 0) < 0,
           "without starting a transmit while one is running");
    expect_hex("two buffers are now in use", nic.txb_inuse, 2);

    /* A third has nowhere to go. */
    expect_hex("and a third is refused",
               (unsigned long)(ULONG)dp8390_tx(&nic, frame, sizeof(frame)),
               (unsigned long)(ULONG)DP8390_TX_BUSY);
}

/* The buffer index wraps at txb_cnt rather than running off the array. */
static void h_transmit_buffers_wrap(void)
{
    static const UBYTE frame[64] = { 0 };
    int i;

    reset();
    (VOID)dp8390_init(&nic);

    for (i = 0; i < 8; i++)
    {
        nic.txb_inuse = 0;              /* pretend each completed */
        (VOID)dp8390_tx(&nic, frame, sizeof(frame));
        if (nic.txb_new >= nic.txb_cnt || nic.txb_next_tx >= nic.txb_cnt)
        {
            expect(0, "a transmit buffer index ran past the count");
            return;
        }
    }
    checks++;
}

/* ============================================================= receive === */

/*
 * The ring walk stops when CURR catches next_packet, and that comparison is
 * the whole loop guard: get it wrong and the drain either spins on an empty
 * ring or drops every frame in it.
 */
static void i_an_empty_ring_hands_up_nothing(void)
{
    reset();
    (VOID)dp8390_init(&nic);
    tr_n = 0;

    /* CURR equals next_packet: nothing has arrived. */
    chip[1][ED_P1_CURR] = (UBYTE)nic.next_packet;

    dp8390_rint(&nic);

    expect(frames_up == 0, "an empty ring hands up no frame");
    expect(ring_copies == 0, "and copies nothing");
    expect(find_w(0, ED_P0_BNRY, 0) < 0,
           "and does not move the boundary pointer");
}

/*
 * ONE FRAME, and the boundary arithmetic that follows it.  BNRY must trail
 * CURR by a page: the chip stops receiving when they meet, so a boundary
 * written one page too far forward silently discards the frame the driver
 * just handed up, and one too far back re-reads it forever.
 */
static void j_one_frame_moves_the_boundary(void)
{
    UBYTE start;

    reset();
    (VOID)dp8390_init(&nic);
    start = nic.rec_page_start;
    tr_n = 0;

    /*
     * A 60-byte frame plus the 4-byte ring header, occupying ONE page, with
     * the next packet at start+2.  The length is recomputed from the page
     * delta and only the low byte of count is trusted -- old chips duplicate
     * the low byte into the high one -- so count is 64 and not a page.
     */
    hdr_status = ED_RSR_PRX;
    hdr_next   = (UBYTE)(start + 2);
    hdr_count  = (UWORD)(60 + sizeof(NetdevRing));
    chip[1][ED_P1_CURR] = (UBYTE)(start + 2);

    dp8390_rint(&nic);

    expect(frames_up == 1, "one frame is handed up");
    expect_hex("of the length the header describes, less the ring header",
               frame_len_saw, 60);
    expect(ring_copies >= 1, "and it was staged through the ring copy");
    expect_hex("next_packet follows the header", nic.next_packet,
               (unsigned long)(UBYTE)(start + 2));

    {
        int bnry = find_w(0, ED_P0_BNRY, 0);

        expect(bnry >= 0, "the boundary pointer was written");
        expect_hex("one page behind next_packet", tr[bnry].val,
                   (unsigned long)(UBYTE)(start + 1));
    }
}

/*
 * THE WRAP.  When the next packet is the ring's FIRST page, the boundary is
 * the page before it -- which is below the ring -- and has to become the last
 * page instead.  Off by one here points the chip at a page outside its own
 * ring.
 */
static void k_the_boundary_wraps_at_the_bottom(void)
{
    UBYTE start;
    UBYTE stop;

    reset();
    (VOID)dp8390_init(&nic);
    start = nic.rec_page_start;
    stop  = nic.rec_page_stop;

    /* The walk is already near the top and the next header points at the
       ring's first page. */
    nic.next_packet = (UWORD)(stop - 1);
    tr_n = 0;

    hdr_status = ED_RSR_PRX;
    hdr_next   = start;
    hdr_count  = (UWORD)(60 + sizeof(NetdevRing));
    chip[1][ED_P1_CURR] = start;

    dp8390_rint(&nic);

    expect_hex("next_packet wrapped to the ring's first page",
               nic.next_packet, (unsigned long)start);

    {
        int bnry = find_w(0, ED_P0_BNRY, 0);

        expect(bnry >= 0, "a boundary was written");
        expect_hex("and it is the ring's LAST page, not the one below it",
                   tr[bnry].val, (unsigned long)(UBYTE)(stop - 1));
    }
}

/*
 * A header whose next-packet pointer is outside the ring, or points at its own
 * page, is a corrupt ring and nothing short of a reset recovers.  The
 * self-pointing case is the one that matters: next_packet is only ever
 * assigned hdr.next_packet, so it leaves the loop condition unchanged and
 * spins forever inside the INT2 server with interrupts masked.
 */
static void l_a_corrupt_header_resets_rather_than_spins(void)
{
    UBYTE start;
    UWORD i;

    /* Points at its own page. */
    reset();
    (VOID)dp8390_init(&nic);
    start = nic.rec_page_start;
    nic.next_packet = (UWORD)(start + 1);
    tr_n = 0;

    hdr_status = ED_RSR_PRX;
    hdr_next   = (UBYTE)(start + 1);
    hdr_count  = (UWORD)(60 + sizeof(NetdevRing));
    chip[1][ED_P1_CURR] = (UBYTE)(start + 4);

    dp8390_rint(&nic);

    expect_hex("a self-pointing header is counted as a receive error",
               nic.rx_errors, 1);
    expect_hex("and the chip is reset", nic.resets, 1);
    expect(frames_up == 0, "with nothing handed up");

    /* Below the ring, and at or past its end. */
    for (i = 0; i < 2u; i++)
    {
        reset();
        (VOID)dp8390_init(&nic);
        start = nic.rec_page_start;
        tr_n = 0;

        hdr_status = ED_RSR_PRX;
        hdr_next   = (UBYTE)(i == 0 ? (UBYTE)(start - 1) : nic.rec_page_stop);
        hdr_count  = (UWORD)(60 + sizeof(NetdevRing));
        chip[1][ED_P1_CURR] = (UBYTE)(start + 4);

        dp8390_rint(&nic);

        expect_hex("a next-packet outside the ring resets the chip",
                   nic.resets, 1);
        expect(frames_up == 0, "and hands nothing up");
    }
}

/*
 * A frame too long or impossibly short is SKIPPED, not reset on.  Resetting
 * flushes the receive ring and discards every transmit already reported as
 * sent, and one 802.1Q-tagged frame from any host on the LAN was enough to
 * trigger it.
 */
static void m_an_overlong_frame_is_skipped_not_reset_on(void)
{
    UBYTE start;

    reset();
    (VOID)dp8390_init(&nic);
    start = nic.rec_page_start;
    tr_n = 0;

    hdr_status = ED_RSR_PRX;
    hdr_next   = (UBYTE)(start + 2);
    hdr_count  = 0xffffu;               /* far past NETDEV_RXBUF_MAX */
    chip[1][ED_P1_CURR] = (UBYTE)(start + 2);

    dp8390_rint(&nic);

    expect(frames_up == 0, "an overlong frame is not handed up");
    expect_hex("it is counted as a receive error", nic.rx_errors, 1);
    expect_hex("and the chip is NOT reset", nic.resets, 0);
    expect(find_w(0, ED_P0_BNRY, 0) >= 0,
           "and the ring still moves past it");
}

int main(void)
{
    a_init_follows_the_manual();
    b_init_supplies_a_dcr_when_the_caller_did_not();
    c_halt_leaves_nothing_asserted();
    d_reset_is_counted();
    e_setfilter_banks_and_restores();
    f_promiscuous_is_three_bits();
    g_transmit();
    h_transmit_buffers_wrap();
    i_an_empty_ring_hands_up_nothing();
    j_one_frame_moves_the_boundary();
    k_the_boundary_wraps_at_the_bottom();
    l_a_corrupt_header_resets_rather_than_spins();
    m_an_overlong_frame_is_skipped_not_reset_on();

    if (failures != 0)
    {
        printf("netdev_dp8390: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("netdev_dp8390: %d checks ok\n", checks);

    return 0;
}
