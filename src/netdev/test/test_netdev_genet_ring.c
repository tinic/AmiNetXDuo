/*
 * The arithmetic under the GENET's ring walk (genet_ring.h), pinned.
 *
 * The chip's counters are 16 bits wide and the rings are 128 and 32 deep, so
 * every difference here has a wrap in it that no frame on the wire ever
 * shows until the counter passes 65535 -- about a minute of full-size frames
 * at a gigabit.  The values below are what the sums must give on both sides
 * of it, plus the page count the supervised cache push walks, the transmit
 * descriptor's words and the receive line's mask while a reader is behind.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "genet_ring.h"

static int failures;
static int checks;

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    checks++;
    if (got == want)
        return;

    printf("FAIL %s: got 0x%lx (%lu), want 0x%lx (%lu)\n",
           what, got, got, want, want);
    failures++;
}

#define RX  128
#define TX  32

/* ---------------------------------------------------------- the counters --- */

static void a_fresh(void)
{
    /* clean == cidx: no pass has cleaned anything since. */
    expect_u32("fresh: nothing cleaned", genet_ring_fresh(10, 100, 100), 10);
    /* clean == cidx + total: the held pass cleaned them all. */
    expect_u32("fresh: all cleaned", genet_ring_fresh(10, 100, 110), 0);
    expect_u32("fresh: four cleaned of ten", genet_ring_fresh(10, 100, 104), 6);
    /* clean behind cidx is a stale value: start over. */
    expect_u32("fresh: clean behind the consumer",
               genet_ring_fresh(10, 100, 99), 10);
    /* clean past the producer cannot happen: start over. */
    expect_u32("fresh: clean past the producer",
               genet_ring_fresh(10, 100, 111), 10);
    expect_u32("fresh: across the wrap", genet_ring_fresh(8, 0xfffc, 0x0002), 2);
    expect_u32("fresh: zero total", genet_ring_fresh(0, 100, 100), 0);
}

static void b_slot_room(void)
{
    expect_u32("slot 0", genet_ring_slot(0, RX), 0);
    expect_u32("slot 127", genet_ring_slot(127, RX), 127);
    expect_u32("slot 128 wraps", genet_ring_slot(128, RX), 0);
    expect_u32("slot 0xffff", genet_ring_slot(0xffff, RX), 127);
    expect_u32("slot 0x1234", genet_ring_slot(0x1234, RX), 0x34);
    expect_u32("slot 33 of 32", genet_ring_slot(33, TX), 1);
    expect_u32("slot 0xffff of 32", genet_ring_slot(0xffff, TX), 31);

    expect_u32("room at 0", genet_ring_room(0, RX), 128);
    expect_u32("room at 127", genet_ring_room(127, RX), 1);
    expect_u32("room at 0xffff", genet_ring_room(0xffff, RX), 1);
    expect_u32("room at 100", genet_ring_room(100, RX), 28);
    expect_u32("room at 31 of 32", genet_ring_room(31, TX), 1);
    expect_u32("room at 32 of 32", genet_ring_room(32, TX), 32);
}

/* ------------------------------------------------------------- the pages --- */

static void c_pages(void)
{
    expect_u32("page first: aligned", genet_page_first(0x00401000UL), 0x00401000UL);
    expect_u32("page first: last byte", genet_page_first(0x00401fffUL), 0x00401000UL);
    expect_u32("page first: zero", genet_page_first(0), 0);

    expect_u32("pages: one buffer", genet_page_count(0x00401000UL, 2048), 1);
    expect_u32("pages: one page", genet_page_count(0x00401000UL, 4096), 1);
    expect_u32("pages: one page and a byte", genet_page_count(0x00401000UL, 4097), 2);
    expect_u32("pages: second buffer of a page",
               genet_page_count(0x00401800UL, 2048), 1);
    expect_u32("pages: a page from mid-page", genet_page_count(0x00401800UL, 4096), 2);
    expect_u32("pages: the whole receive ring",
               genet_page_count(0x00401000UL, 128UL * 2048UL), 64);
    expect_u32("pages: the whole transmit ring",
               genet_page_count(0x00441000UL, 32UL * 2048UL), 16);
    expect_u32("pages: one byte", genet_page_count(0x00401000UL, 1), 1);
    expect_u32("pages: four bytes over a boundary",
               genet_page_count(0x00401ffeUL, 4), 2);
}

/* ------------------------------------------------------- the ring words --- */

static void d_config_words(void)
{
    expect_u32("ring size: receive", genet_ring_size_word(128, 2048), 0x00800800UL);
    expect_u32("ring size: transmit", genet_ring_size_word(32, 2048), 0x00200800UL);
    expect_u32("ring size: all 256", genet_ring_size_word(256, 2048), 0x01000800UL);

    expect_u32("ring end: receive", genet_ring_end_word(128), 383);
    expect_u32("ring end: transmit", genet_ring_end_word(32), 95);
    expect_u32("ring end: all 256 is the descriptor RAM", genet_ring_end_word(256), 767);

    expect_u32("xon/xoff: receive", genet_ring_xon_xoff_word(128), 0x00050008UL);
    expect_u32("xon/xoff: 32", genet_ring_xon_xoff_word(32), 0x00050002UL);
    expect_u32("xon/xoff: 256", genet_ring_xon_xoff_word(256), 0x00050010UL);

    expect_u32("timeout: into all ones", genet_ring_timeout_word(0xffffffffUL, 61), 0xffff003dUL);
    expect_u32("timeout: the upper half kept",
               genet_ring_timeout_word(0x1234abcdUL, 61), 0x1234003dUL);
    expect_u32("timeout: zero", genet_ring_timeout_word(0, 0), 0);
}

/* -------------------------------------------------------- the descriptors --- */

static void e_tx_status(void)
{
    /* SOP | EOP | CRC | QTAG is 0x7fc0; the buffer length, plus the 64-byte
       status block, sits at bit 16. */
    expect_u32("tx status: minimum frame", genet_tx_desc_status(60), 0x007c7fc0UL);
    expect_u32("tx status: full frame", genet_tx_desc_status(1514), 0x062a7fc0UL);
    expect_u32("tx status: tagged frame", genet_tx_desc_status(1518), 0x062e7fc0UL);
    expect_u32("tx status: the field is twelve bits",
               genet_tx_desc_status(4032), 0x00007fc0UL);
}

static void f_tx_csum(void)
{
    /* A 20-byte IPv4 header: the transport header starts at 34, TCP's
       checksum is 16 in, UDP's 6 in. */
    expect_u32("csum: TCP", genet_tx_csum_info(0x45, 50, ANXD_S2_TXF_TCP), 0x00220032UL);
    expect_u32("csum: UDP", genet_tx_csum_info(0x45, 40, ANXD_S2_TXF_UDP), 0x00228028UL);
    expect_u32("csum: TCP does not set the UDP bit",
               genet_tx_csum_info(0x45, 50, ANXD_S2_TXF_TCP) & GENET_TX_CSUM_UDP, 0);
    expect_u32("csum: a 24-byte IP header",
               genet_tx_csum_info(0x46, 54, ANXD_S2_TXF_TCP), 0x00260036UL);
    expect_u32("csum: the longest IP header",
               genet_tx_csum_info(0x4f, 90, ANXD_S2_TXF_TCP), 0x004a005aUL);
    expect_u32("csum: the version bits are ignored",
               genet_tx_csum_info(0xf5, 50, ANXD_S2_TXF_TCP), 0x00220032UL);
    expect_u32("csum: never LEN_VALID by itself",
               genet_tx_csum_info(0x45, 50, ANXD_S2_TXF_UDP) & GENET_TX_CSUM_LEN_VALID,
               0);
}

/* ------------------------------------------------------------- the line --- */

#define DONE    GENET_IRQ_RXDMA_DONE
#define LINKS   ((1UL << 4) | (1UL << 5))
#define WANTED  (DONE | LINKS)

static void line(const char *what, UBYTE held, UBYTE line_in, ULONG rearm,
                 ULONG want_rearm, UBYTE want_line, ULONG want_count)
{
    UBYTE line_held = line_in;
    ULONG withheld  = 100;
    ULONG got       = genet_rx_line_rearm(held, &line_held, rearm, &withheld);
    char  buf[96];

    snprintf(buf, sizeof(buf), "%s: rearm", what);
    expect_u32(buf, got, want_rearm);
    snprintf(buf, sizeof(buf), "%s: line_held", what);
    expect_u32(buf, line_held, want_line);
    snprintf(buf, sizeof(buf), "%s: withheld", what);
    expect_u32(buf, withheld - 100, want_count);
}

static void g_rx_line(void)
{
    /* Nothing held, nothing remembered: what the top half masked comes back. */
    line("idle pass, all wanted", 0, 0, WANTED, WANTED, 0, 0);
    line("idle pass, nothing to re-arm", 0, 0, 0, 0, 0, 0);
    line("idle pass, links only", 0, 0, LINKS, LINKS, 0, 0);
    /* A reader behind: DONE is taken out and remembered, once per pass. */
    line("held pass, all wanted", 1, 0, WANTED, LINKS, 1, 1);
    line("held pass, DONE was not masked", 1, 0, LINKS, LINKS, 0, 0);
    line("held pass, nothing to re-arm", 1, 0, 0, 0, 0, 0);
    line("held again, DONE pending", 1, 1, WANTED, LINKS, 1, 1);
    line("held again, the blank's call", 1, 1, 0, 0, 1, 0);
    /* The head moved: the remembered bit comes back, whatever was masked. */
    line("released by the blank", 0, 1, 0, DONE, 0, 0);
    line("released with links pending", 0, 1, LINKS, WANTED, 0, 0);
    line("released with DONE pending", 0, 1, WANTED, WANTED, 0, 0);
}

int main(void)
{
    a_fresh();
    b_slot_room();
    c_pages();
    d_config_words();
    e_tx_status();
    f_tx_csum();
    g_rx_line();

    if (failures != 0)
    {
        printf("netdev_genet_ring: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("netdev_genet_ring: %d checks ok\n", checks);

    return 0;
}
