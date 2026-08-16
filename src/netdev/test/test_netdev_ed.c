/*
 * The ED core's buffer arithmetic, on the host.
 *
 * The three functions this core adds over the NE2000 one are read_hdr,
 * ring_copy and write_buf.  All three are address arithmetic over a mapped
 * window, and every one of their failure modes is silent.  A header decoded
 * wrong reads as an over-length frame and is counted as an error, a wrap
 * computed wrong shreds one packet in sixty-four, and a pad that overwrites
 * the last byte of an odd frame corrupts one in two.  None of that shows up in
 * a boot log.
 *
 * The window is a 16-bit port on a 68000 and everything here goes through word
 * accesses, so the test states its expectations in words: the value a 68k
 * `move.w` would load is the value this test stores.  Byte-level claims are
 * made only where the code itself extracts a byte from a word by shifting,
 * which is the same expression on either endianness.  A test that laid the
 * window out as bytes would assert the host's byte order, not the card's.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

/*
 * The core is pulled in whole rather than linked: read_hdr, ring_copy and
 * write_buf are static, and exposing them for a test would be the test
 * changing the code it tests.
 */
#include "ed.c"

/* The chip half, which this file does not exercise. */
VOID dp8390_config(NetdevNic *nic) { (VOID)nic; }
LONG dp8390_init(NetdevNic *nic) { (VOID)nic; return 0; }
VOID dp8390_halt(NetdevNic *nic) { (VOID)nic; }
VOID dp8390_reset(NetdevNic *nic) { (VOID)nic; }
VOID dp8390_setfilter(NetdevNic *nic) { (VOID)nic; }
LONG dp8390_tx(NetdevNic *nic, const UBYTE *f, UWORD l)
{ (VOID)nic; (VOID)f; (VOID)l; return 0; }
BOOL dp8390_intr(NetdevNic *nic) { (VOID)nic; return 0; }

static int failures;

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    if (got == want)
    {
        printf("ok   %s = %lu\n", what, got);
        return;
    }

    printf("FAIL %s: got %lu, want %lu\n", what, got, want);
    failures++;
}

/* ------------------------------------------------------------- the board -- */

#define WIN_WORDS   (0x10000u / 2u)
#define MEM_SIZE    16384u
#define PROM_OFF    0x8000u
#define TX_SLOT     0

static UWORD  window[WIN_WORDS];
static UBYTE  regs[64];
static NetdevCard card;
static NetdevNic  nic;

/*
 * Two views of the same window, and the difference is not cosmetic.
 *
 * The buffer is reached with word accesses, so what a 68k `move.w` loads is
 * the word this array holds.  win_get/win_put are the model, and win_byte()
 * takes the byte apart the way the 68k lays it down, with the even address in
 * the high half.  The PROM is reached with byte accesses at stride 2, which
 * have no endianness, so it is planted straight into the byte image.  Mixed
 * up, the test asserts x86's byte order rather than the card's.
 */
static UWORD win_get(ULONG off) { return window[off / 2u]; }
static void  win_put(ULONG off, UWORD v) { window[off / 2u] = v; }

static UBYTE win_byte(ULONG off)
{
    UWORD w = win_get(off & ~1u);

    return (UBYTE)((off & 1u) != 0 ? (w & 0xffu) : (w >> 8));
}

static void board_reset(ULONG mem_size)
{
    memset(window, 0, sizeof(window));
    memset(regs, 0, sizeof(regs));
    memset(&nic, 0, sizeof(nic));

    card.name     = "test";
    card.stride   = 2;
    card.chip     = NETDEV_CHIP_ED;
    card.mem_off  = 0;
    card.mem_size = mem_size;
    card.prom_off = PROM_OFF;

    nic.card  = &card;
    nic.board = (volatile UBYTE *)window;
    netdev_bus_setup(&nic.bus, regs, 2, NULL);

    /* What dp8390_config() computes, spelled out so the stub can stay a stub. */
    nic.mem_start = 0;
    nic.mem_size  = (LONG)mem_size;
    nic.txb_cnt   = (UWORD)(mem_size < 16384 ? 1 : (mem_size < 8192 * 3 ? 2 : 3));
    nic.mem_ring  = (LONG)(nic.txb_cnt * ED_TXBUF_SIZE) << ED_PAGE_SHIFT;
    nic.mem_end   = nic.mem_start + nic.mem_size;
}

/* ------------------------------------------------------------- read_hdr --- */

/*
 * The layout is not symmetric and that is the whole point of testing it:
 * status/next comes back byte-swapped and the count does not.  These word
 * values are what the card presents for rsr=0x21, next=0x1a, count=0x05ee --
 * derived from Amiberry's DP8390 (`s->dcfg & 2` arm of ne2000_receive) and
 * from NetBSD's and Linux's readers, all three of which agree.
 */
static void test_read_hdr(void)
{
    NetdevRing hdr;
    LONG       at = 0x1a00;

    board_reset(MEM_SIZE);

    win_put((ULONG)at + 0, 0x1a21);     /* next_packet, rsr           */
    win_put((ULONG)at + 2, 0xee05);     /* count low, count high      */

    ed_read_hdr(&nic, at, &hdr);
    expect_u32("read_hdr rsr", hdr.rsr, 0x21);
    expect_u32("read_hdr next_packet", hdr.next_packet, 0x1a);
    expect_u32("read_hdr count", hdr.count, 0x05ee);

    /* A count whose halves differ in only one bit: a uniform swap survives a
       palindrome and this is not one. */
    win_put((ULONG)at + 2, 0x0102);
    ed_read_hdr(&nic, at, &hdr);
    expect_u32("read_hdr count not byte-swapped", hdr.count, 0x0201);
}

/* ------------------------------------------------------------ ring_copy --- */

static void fill_ring_pattern(void)
{
    ULONG off;

    for (off = 0; off < MEM_SIZE; off += 2u)
        win_put(off, (UWORD)(0x1000u + (off / 2u)));
}

static void test_ring_copy(void)
{
    ULONG dst[512];
    LONG  ret;
    UWORD i;

    board_reset(MEM_SIZE);
    fill_ring_pattern();

    /* No wrap: contiguous, and the return is one past the last word read. */
    memset(dst, 0, sizeof(dst));
    ret = ed_ring_copy(&nic, 0x1000, (UBYTE *)dst, 64);
    expect_u32("ring_copy flat return", (unsigned long)ret, 0x1040);
    for (i = 0; i < 32; i++)
    {
        if (((UWORD *)dst)[i] != win_get(0x1000u + i * 2u))
        {
            printf("FAIL ring_copy flat word %u\n", i);
            failures++;
            return;
        }
    }
    printf("ok   ring_copy flat contents\n");

    /*
     * Wrapped: 40 bytes starting 16 short of the end.  The tail must resume at
     * mem_ring and not at mem_start.  The transmit slots live between them, and
     * a copy that restarted at zero would deliver a fragment of the frame being
     * transmitted.
     */
    memset(dst, 0, sizeof(dst));
    ret = ed_ring_copy(&nic, MEM_SIZE - 16, (UBYTE *)dst, 40);
    expect_u32("ring_copy wrapped return",
               (unsigned long)ret, (unsigned long)(nic.mem_ring + 24));

    for (i = 0; i < 8; i++)
    {
        if (((UWORD *)dst)[i] != win_get((ULONG)(MEM_SIZE - 16) + i * 2u))
        {
            printf("FAIL ring_copy wrapped head word %u\n", i);
            failures++;
            return;
        }
    }
    for (i = 0; i < 12; i++)
    {
        if (((UWORD *)dst)[8 + i] != win_get((ULONG)nic.mem_ring + i * 2u))
        {
            printf("FAIL ring_copy wrapped tail word %u\n", i);
            failures++;
            return;
        }
    }
    printf("ok   ring_copy wrapped contents\n");

    /* An odd amount takes the last byte from the high half of its word. */
    memset(dst, 0, sizeof(dst));
    (VOID)ed_ring_copy(&nic, 0x2000, (UBYTE *)dst, 7);
    expect_u32("ring_copy odd tail byte",
               ((UBYTE *)dst)[6], (UBYTE)(win_get(0x2006) >> 8));

    /*
     * An odd destination.  No caller in the driver produces one, because the
     * staging buffer is longword-aligned and a ring wrap resumes on a page
     * boundary, so this branch would otherwise be code that reads as handled
     * and has never run.  It splits the word by shifting, which is the same on
     * either endianness.
     */
    {
        UBYTE odd[64];
        int   bad = 0;

        memset(odd, 0, sizeof(odd));
        (VOID)ed_ring_copy(&nic, 0x3000, odd + 1, 16);
        for (i = 0; i < 8; i++)
        {
            UWORD w = win_get(0x3000u + i * 2u);

            bad |= odd[1 + i * 2] != (UBYTE)(w >> 8);
            bad |= odd[2 + i * 2] != (UBYTE)w;
        }
        bad |= odd[0] != 0;
        bad |= odd[17] != 0;
        expect_u32("ring_copy into an odd destination",
                   (unsigned long)bad, 0);
    }
}

/* ------------------------------------------------------------ write_buf --- */

/*
 * The whole slot, checked the way the copy wrote it.  For an even source the
 * body is a raw word move, byte order preserving on a 68000 and not on this
 * host, so the claim it can carry here is that the words arrived in order.
 * Everything else in the function goes through an explicit shift and is
 * byte-exact on either machine: an odd source's whole body, the last byte of
 * an odd length, and the pad.  Returns non-zero on any mismatch.
 */
static int check_slot(const UBYTE *frame, UWORD len, UWORD sent)
{
    UWORD i;
    int   bad = 0;

    if (!ED_ODD(frame))
    {
        const UWORD *s = (const UWORD *)(const void *)frame;

        for (i = 0; i + 2 <= len; i += 2)
            bad |= win_get(TX_SLOT + i) != s[i / 2];
    }
    else
    {
        for (i = 0; i + 2 <= len; i += 2)
        {
            bad |= win_byte(TX_SLOT + i) != frame[i];
            bad |= win_byte(TX_SLOT + i + 1) != frame[i + 1];
        }
    }

    if ((len & 1u) != 0)
    {
        bad |= win_byte(TX_SLOT + len - 1u) != frame[len - 1];
        bad |= win_byte(TX_SLOT + len) != 0;
    }

    for (i = (UWORD)((len + 1u) & ~1u); i < sent; i += 2)
        bad |= win_get(TX_SLOT + i) != 0;

    return bad;
}

static void test_write_buf(void)
{
    static UBYTE frame[NETDEV_FRAME_MAX + 1];
    UWORD sent;
    UWORD i;

    for (i = 0; i < sizeof(frame); i++)
        frame[i] = (UBYTE)(i + 1);

    /* Runt: padded to the Ethernet minimum, and the pad is what goes out. */
    board_reset(MEM_SIZE);
    sent = ed_write_buf(&nic, frame, 14, TX_SLOT);
    expect_u32("write_buf runt length", sent, NETDEV_FRAME_MIN);
    expect_u32("write_buf runt payload and pad",
               (unsigned long)check_slot(frame, 14, sent), 0);

    /*
     * An odd runt, which is where a pad that rounds the wrong way eats the
     * last byte of the frame: byte 14 shares its word with the first pad
     * byte.
     */
    board_reset(MEM_SIZE);
    sent = ed_write_buf(&nic, frame, 15, TX_SLOT);
    expect_u32("write_buf odd runt length", sent, NETDEV_FRAME_MIN);
    expect_u32("write_buf odd runt keeps its last byte",
               (unsigned long)check_slot(frame, 15, sent), 0);

    /* An odd full-size frame is not padded and not truncated. */
    board_reset(MEM_SIZE);
    sent = ed_write_buf(&nic, frame, 1513, TX_SLOT);
    expect_u32("write_buf odd frame length", sent, 1513);
    expect_u32("write_buf odd frame payload",
               (unsigned long)check_slot(frame, 1513, sent), 0);

    /* An unaligned source is the one the word path cannot take. */
    board_reset(MEM_SIZE);
    sent = ed_write_buf(&nic, frame + 1, 101, TX_SLOT);
    expect_u32("write_buf odd source length", sent, 101);
    expect_u32("write_buf odd source payload",
               (unsigned long)check_slot(frame + 1, 101, sent), 0);
}

/* ---------------------------------------------------------------- probe --- */

/*
 * ed_test_mem() claims to catch a window that decodes fewer address lines
 * than the buffer has, with the top half aliasing the bottom, which is what
 * the LAN Rover's 32K looked like until Amiberry ef28da7e.  That claim rests
 * on every page's pattern being different from every other page's.  If the
 * seed collided, the whole sweep would still pass on an aliased board and the
 * probe would measure nothing.  So: assert the seeds are distinct over the
 * largest buffer any row in the table asks for.
 */
static void test_mem_seed_distinct(void)
{
    LONG a, b;
    int  collisions = 0;

    for (a = 0; a < 32768; a += ED_PAGE_SIZE)
    {
        for (b = a + ED_PAGE_SIZE; b < 32768; b += ED_PAGE_SIZE)
        {
            if (ed_mem_seed(a) == ed_mem_seed(b))
                collisions++;
        }
    }

    expect_u32("ed_mem_seed collisions over 32K", (unsigned long)collisions, 0);
}

static void plant_prom(const UBYTE *mac)
{
    UBYTE *image = (UBYTE *)window;
    UWORD  i;

    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        image[PROM_OFF + i * 2u] = mac[i];
}

/*
 * attach's station-address gates.  A board that answers 0x00 or 0xff to
 * everything, or hands back a group address, is a board that would come
 * online and never be delivered a frame.  In a user's machine that is
 * indistinguishable from a driver bug.
 */
static void test_attach_station_address(void)
{
    static const UBYTE good[6]  = { 0x00, 0x80, 0x10, 0x12, 0x34, 0x56 };
    static const UBYTE zero[6]  = { 0, 0, 0, 0, 0, 0 };
    static const UBYTE ones[6]  = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const UBYTE group[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };

    board_reset(MEM_SIZE);
    plant_prom(good);
    expect_u32("attach accepts a station address",
               (unsigned long)(ed_attach(&nic) == 0), 1);
    expect_u32("attach kept the address",
               (unsigned long)(memcmp((const void *)nic.factory, good, 6) == 0), 1);
    expect_u32("attach set BOS", (unsigned long)(nic.dcr_reg & ED_DCR_BOS),
               ED_DCR_BOS);
    expect_u32("attach wired read_hdr",
               (unsigned long)(nic.read_hdr == ed_read_hdr), 1);

    board_reset(MEM_SIZE);
    plant_prom(zero);
    expect_u32("attach refuses an all-zero PROM",
               (unsigned long)(ed_attach(&nic) != 0), 1);

    board_reset(MEM_SIZE);
    plant_prom(ones);
    expect_u32("attach refuses an all-ones PROM",
               (unsigned long)(ed_attach(&nic) != 0), 1);

    board_reset(MEM_SIZE);
    plant_prom(group);
    expect_u32("attach refuses a group address",
               (unsigned long)(ed_attach(&nic) != 0), 1);
}

int main(void)
{
    test_read_hdr();
    test_ring_copy();
    test_write_buf();
    test_mem_seed_distinct();
    test_attach_station_address();

    if (failures != 0)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }

    printf("all ok\n");
    return 0;
}
