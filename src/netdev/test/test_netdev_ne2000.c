/*
 * The NE2000 probe, in the order it asks its questions.
 *
 * ne2000_detect() resets the chip and then demands that the command register
 * read back as a reset chip, and only after that does it test whether the odd
 * registers can be read at all.  The reset port is whole-file register 31,
 * which is ODD, and the reset is strobed by READING it -- so on a clone that
 * answers 16-bit I/O cycles and nothing else, the strobe is not a cycle the
 * card sees, and the card is refused on the command register before the probe
 * that exists to find exactly that card is ever reached.  The fix strobes the
 * port again through the word path and asks once more.
 *
 * WHAT THIS FILE PROVES AND WHAT IT DOES NOT.
 *
 * It proves the ORDER, the RECOVERY and the STATE the bus is left in: that a
 * failed first reading is retried through the word path rather than refused,
 * that a card which fails the second reading leaves the word path turned back
 * off, that a bus which cannot take the word path is refused without touching
 * it, and that the first odd-register reading is labelled with the mode it was
 * actually taken in.  All four are properties of this driver.
 *
 * It does not prove that a CNet16 behaves the way the mock below does.  The
 * mock's rule -- the command register does not read back as a reset chip until
 * the reset port has been strobed with the word path in use -- is the condition
 * the fix exists for, supplied here as an INPUT.  Only the card can say whether
 * it is the condition the card presents, and no emulator can stand in: Amiberry
 * decodes both PCMCIA windows 1:1 and answers a byte read of an odd address as
 * readily as a word read of the even one, so it cannot hold a card that refuses
 * the first and the retry never fires there.
 *
 * The word-read arithmetic itself is NOT driven here.  A word read keeping the
 * odd register in its low half is a big-endian fact and a little-endian host
 * cannot hold both it and a byte read of the even register beside it in one
 * window; test_netdev_bus.c pins that arithmetic on its own terms.  So the chip
 * below is indexed by register number and the four accessors are replaced.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- the chip -- */

#define MOCK_REGS   32
#define MOCK_BUF    32768u
#define MOCK_MASK   (MOCK_BUF - 1u)

static unsigned char mock_reg[MOCK_REGS];
static unsigned char mock_buf[MOCK_BUF];

/* The card asserts -IOIS16 unconditionally: a byte read of an odd register is
   not a cycle it answers, so it hands back what the bus held. */
static int  mock_clone;

/* The chip has been reset.  Until it has, the command register holds what the
   previous owner of the socket left in it -- which is the whole condition. */
static int  mock_chip_reset;
static unsigned char mock_cr_stale;

/* The reset port was READ, which is what arms the pulse; the write-back then
   completes it.  A read that the card never saw arms nothing. */
static int  mock_reset_armed;
static int  mock_resets;

/* A card that answers no cycle at all: the pulse never lands however it is
   strobed, so the command register never becomes $21 in either mode. */
static int  mock_no_reset;

/* Odd-register byte reads the card refused, and word reads it answered. */
static int  mock_odd_refused;
static int  mock_odd_word;

/* Remote DMA. */
static unsigned long mock_dma;
static unsigned      mock_dma_left;

static int failures;

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    if (got == want)
    {
        printf("ok   %s = 0x%lx\n", what, got);
        return;
    }

    printf("FAIL %s: got 0x%lx, want 0x%lx\n", what, got, want);
    failures++;
}

static void ok(const char *what, int cond)
{
    if (cond)
    {
        printf("ok   %s\n", what);
        return;
    }

    printf("FAIL %s\n", what);
    failures++;
}

static void mock_reset_chip(void)
{
    mock_resets++;
    mock_chip_reset = 1;
    memset(mock_reg, 0, sizeof(mock_reg));
    mock_reg[0x00] = 0x20 | 0x01;       /* ED_CR_RD2 | ED_CR_STP */
    mock_reg[0x07] = 0x80;              /* ED_ISR_RST            */
}

struct NetdevNic;

static unsigned char mock_get(struct NetdevNic *nic, unsigned reg);
static void          mock_put(struct NetdevNic *nic, unsigned reg,
                              unsigned char val);

#define NIC_GET(nic, reg)       mock_get((nic), (unsigned)(reg))
#define NIC_PUT(nic, reg, val)  mock_put((nic), (unsigned)(reg), (UBYTE)(val))
#define ASIC_GET(nic, reg)      mock_get((nic), 16u + ((unsigned)(reg) & 15u))
#define ASIC_PUT(nic, reg, val) \
    mock_put((nic), 16u + ((unsigned)(reg) & 15u), (UBYTE)(val))

/*
 * The core is pulled in whole rather than linked: ne2000_detect() and
 * ne2000_probe_reset() are static, and exposing them for a test would be the
 * test changing the code it tests.
 */
#include "ne2000.c"

/* The chip half this file does not exercise. */
VOID dp8390_config(NetdevNic *nic) { (VOID)nic; }
LONG dp8390_init(NetdevNic *nic) { (VOID)nic; return 0; }
VOID dp8390_halt(NetdevNic *nic) { (VOID)nic; }
VOID dp8390_reset(NetdevNic *nic) { (VOID)nic; }
VOID dp8390_setfilter(NetdevNic *nic) { (VOID)nic; }
LONG dp8390_tx(NetdevNic *nic, const UBYTE *f, UWORD l)
{ (VOID)nic; (VOID)f; (VOID)l; return 0; }
BOOL dp8390_intr(NetdevNic *nic) { (VOID)nic; return 0; }

/* The probe record.  Every note is kept, because which notes were made and in
   what order is half of what this file is about. */
#define MOCK_NOTES  64
static UWORD mock_note_code[MOCK_NOTES];
static ULONG mock_note_val[MOCK_NOTES];
static int   mock_notes;

VOID netdev_diag_note(UWORD code, UWORD c, ULONG v)
{
    (VOID)c;
    if (mock_notes < MOCK_NOTES)
    {
        mock_note_code[mock_notes] = code;
        mock_note_val[mock_notes]  = v;
    }
    mock_notes++;
}

UWORD netdev_diag_card(const NetdevCard *c) { (VOID)c; return 0; }

/* netdev_cards.c is linked for the card row and names the other three cores
   in netdev_nic_ops_for().  Same three tentative definitions as
   test_netdev_el3.c: nothing here calls one. */
const struct NetdevNicOps netdev_nic_ed;
const struct NetdevNicOps netdev_nic_lance;
const struct NetdevNicOps netdev_nic_el3;

/*
 * Both live in netdev_device.c, which reads SysBase, Expansion and the PCMCIA
 * CIS -- none of which a host has -- and neither is on the path this file
 * drives.  ne2000_attach() is the only caller and it is not called here.
 */
BOOL netdev_mac_cis_node_id(UBYTE *mac) { (VOID)mac; return FALSE; }
UWORD netdev_mac_fingerprint(UBYTE *buf, UWORD max, ULONG salt)
{
    UWORD n = 0;

    while (n < 4 && n < max)
    {
        buf[n] = (UBYTE)((salt >> (n * 8)) & 0xffUL);
        n++;
    }

    return n;
}

static int note_count(UWORD code)
{
    int i;
    int n = 0;

    for (i = 0; i < mock_notes && i < MOCK_NOTES; i++)
    {
        if (mock_note_code[i] == code)
            n++;
    }

    return n;
}

static ULONG note_last(UWORD code)
{
    int   i;
    ULONG v = 0xdeadbeefUL;

    for (i = 0; i < mock_notes && i < MOCK_NOTES; i++)
    {
        if (mock_note_code[i] == code)
            v = mock_note_val[i];
    }

    return v;
}

/* ------------------------------------------------------- the accessors --- */

static unsigned char mock_get(struct NetdevNic *nic, unsigned reg)
{
    int word = (nic->bus.getodd != 0);

    reg &= 31u;

    if ((reg & 1u) != 0)
    {
        if (mock_clone && !word)
        {
            /* Not a cycle the card answers.  A PCMCIA socket with nothing
               driving it reads as all ones. */
            mock_odd_refused++;
            return 0xff;
        }
        if (word)
            mock_odd_word++;
    }

    if (reg == 16u)                     /* the data port */
    {
        unsigned char b = mock_buf[mock_dma & MOCK_MASK];

        mock_dma++;
        if (mock_dma_left != 0 && --mock_dma_left == 0)
            mock_reg[0x07] |= 0x40;     /* ED_ISR_RDC */
        return b;
    }

    if (reg == 31u)                     /* the reset port: a read arms it */
    {
        mock_reset_armed = 1;
        return mock_reg[31];
    }

    /*
     * THE CONDITION THE FIX EXISTS FOR.  Until the chip has been reset the
     * command register holds what the previous owner of the socket left in
     * it, whatever has been written to it since.  A cold boot is the same
     * model with mock_cr_stale already at the value a reset chip shows, which
     * is why a cold boot hides the defect.
     */
    if (reg == 0x00u && !mock_chip_reset)
        return mock_cr_stale;

    return mock_reg[reg];
}

static void mock_put(struct NetdevNic *nic, unsigned reg, unsigned char val)
{
    (VOID)nic;
    reg &= 31u;

    if (reg == 31u)                     /* the write-back completes the pulse */
    {
        if (mock_reset_armed && !mock_no_reset)
            mock_reset_chip();
        mock_reset_armed = 0;
        return;
    }

    if (reg == 16u)                     /* the data port */
    {
        mock_buf[mock_dma & MOCK_MASK] = val;
        mock_dma++;
        if (mock_dma_left != 0 && --mock_dma_left == 0)
            mock_reg[0x07] |= 0x40;
        return;
    }

    if (reg == 0x07u)                   /* ISR is write-one-to-clear */
    {
        mock_reg[0x07] &= (unsigned char)~val;
        return;
    }

    mock_reg[reg] = val;

    if (reg == 0x00u && (val & (0x08u | 0x10u)) != 0)    /* RD0 or RD1 */
    {
        mock_dma = (unsigned long)mock_reg[0x08] |
                   ((unsigned long)mock_reg[0x09] << 8);
        mock_dma_left = (unsigned)mock_reg[0x0a] |
                        ((unsigned)mock_reg[0x0b] << 8);
        mock_reg[0x07] &= (unsigned char)~0x40u;
    }
}

/* The data port through the bus ops, which is how the bulk paths reach it. */
static VOID mock_rdata(const NetdevBus *bus, UBYTE *dst, UWORD len)
{
    UWORD i;

    (VOID)bus;
    for (i = 0; i < len; i++)
    {
        dst[i] = mock_buf[mock_dma & MOCK_MASK];
        mock_dma++;
    }
    if (mock_dma_left <= len)
    {
        mock_dma_left = 0;
        mock_reg[0x07] |= 0x40;
    }
    else
    {
        mock_dma_left = (unsigned)(mock_dma_left - len);
    }
}

static VOID mock_wdata(const NetdevBus *bus, const UBYTE *src, UWORD len)
{
    UWORD i;

    (VOID)bus;
    for (i = 0; i < len; i++)
    {
        mock_buf[mock_dma & MOCK_MASK] = src[i];
        mock_dma++;
    }
    if (mock_dma_left <= len)
    {
        mock_dma_left = 0;
        mock_reg[0x07] |= 0x40;
    }
    else
    {
        mock_dma_left = (unsigned)(mock_dma_left - len);
    }
}

static const struct NetdevBusOps mock_bus_ops = { mock_rdata, mock_wdata };

/* ------------------------------------------------------------ the board -- */

/*
 * The PCMCIA row: stride 1 and a second window for the odd registers, which is
 * what netdev_bus_set_getodd() insists on.  The window addresses are never
 * dereferenced -- the accessors above are the bus -- but they must be the
 * shape the row has, because the refusal this file tests is made on them.
 */
static unsigned char even_window[64];
static unsigned char odd_window[64];

static void board_pcmcia(NetdevNic *nic, const NetdevCard *card)
{
    memset(nic, 0, sizeof(*nic));
    nic->card = card;
    netdev_bus_setup(&nic->bus, even_window, 1, NULL);
    netdev_bus_split(&nic->bus, odd_window);
    nic->bus.ops = &mock_bus_ops;
}

/* And a row with no odd window at all, which is xsurf500's shape: the word
   path is arithmetic this bus cannot do, so it must be refused. */
static void board_contiguous(NetdevNic *nic, const NetdevCard *card)
{
    memset(nic, 0, sizeof(*nic));
    nic->card = card;
    netdev_bus_setup(&nic->bus, even_window, 1, NULL);
    nic->bus.ops = &mock_bus_ops;
}

static void chip_begin(int clone, unsigned char cr_stale)
{
    memset(mock_reg, 0, sizeof(mock_reg));
    memset(mock_buf, 0, sizeof(mock_buf));
    mock_clone       = clone;
    mock_chip_reset  = 0;
    mock_cr_stale    = cr_stale;
    mock_reset_armed = 0;
    mock_no_reset    = 0;
    mock_resets      = 0;
    mock_odd_refused = 0;
    mock_odd_word    = 0;
    mock_dma         = 0;
    mock_dma_left    = 0;
    mock_notes       = 0;
}

/* CR after a reset: ED_CR_RD2 | ED_CR_STP. */
#define CR_RESET    0x21
/* What a chip that has already been driven once shows: page 0, started. */
#define CR_WARM     0x22

/* ------------------------------------------------------------- the cases -- */

/*
 * A warm-booted clone.  This is the reported defect: the first command-register
 * reading fails because the reset never landed, and before the fix the card was
 * refused there -- with the word-read probe that exists to find it sitting
 * unreached below.
 */
static void test_clone_warm(void)
{
    NetdevNic nic;

    chip_begin(1, CR_WARM);
    board_pcmcia(&nic, netdev_card_by_name("pcmcia"));

    ok("warm clone: detected", ne2000_detect(&nic) != FALSE);
    ok("warm clone: the reset landed exactly once", mock_resets == 1);
    ok("warm clone: and the word path is what landed it",
       nic.bus.getodd != 0);
    expect_u32("warm clone: the retry was recorded once",
               (unsigned long)note_count(ANXDIAG_CR_RETRY), 1);
    expect_u32("warm clone: the second command-register reading is $21",
               note_last(ANXDIAG_CR_READ), CR_RESET);
    expect_u32("warm clone: two command-register readings were recorded",
               (unsigned long)note_count(ANXDIAG_CR_READ), 2);

    /*
     * The first odd-register reading is taken in the word path already, and a
     * probe record that called it a byte one would send the next reader after
     * the wrong thing.  ANXDIAG_ODD_PLAIN must not appear at all.
     */
    expect_u32("warm clone: the first odd reading is labelled a word one",
               (unsigned long)note_count(ANXDIAG_ODD_WORD), 1);
    expect_u32("warm clone: and not a byte one",
               (unsigned long)note_count(ANXDIAG_ODD_PLAIN), 0);
    expect_u32("warm clone: so no odd-register retry was needed either",
               (unsigned long)note_count(ANXDIAG_ODD_RETRY), 0);
    expect_u32("warm clone: the mode it settled in is recorded",
               note_last(ANXDIAG_GETODD), 1);
    ok("warm clone: and no odd byte read was ever answered by the card",
       mock_odd_refused > 0 && mock_odd_word > 0);
}

/*
 * The same card, cold.  The chip powers up with CR at $21, so the first reading
 * passes without the reset having landed and the retry never fires -- which is
 * exactly why the defect could not be reproduced from a cold boot, and why a
 * green cold-boot run is consistent with the bug and with the fix alike.
 */
static void test_clone_cold(void)
{
    NetdevNic nic;

    chip_begin(1, CR_RESET);
    board_pcmcia(&nic, netdev_card_by_name("pcmcia"));

    ok("cold clone: detected", ne2000_detect(&nic) != FALSE);
    expect_u32("cold clone: the command-register retry never fired",
               (unsigned long)note_count(ANXDIAG_CR_RETRY), 0);
    /* It reaches the odd-register probe as a byte reader, fails there, and
       recovers through the SECOND retry -- the one that already existed, and
       which is what strobed the reset that the first pass never landed. */
    ok("cold clone: the reset landed exactly once, and not on the first pass",
       mock_resets == 1);
    expect_u32("cold clone: the first odd reading is labelled a byte one",
               (unsigned long)note_count(ANXDIAG_ODD_PLAIN), 1);
    expect_u32("cold clone: and the odd-register retry is what saved it",
               (unsigned long)note_count(ANXDIAG_ODD_RETRY), 1);
    ok("cold clone: ending in the word path", nic.bus.getodd != 0);
}

/* A card whose odd registers are plain readable bytes.  Nothing about it may
   change: no retry, no word path, and the reading labelled a byte one. */
static void test_plain_card(void)
{
    NetdevNic nic;

    chip_begin(0, CR_WARM);
    board_pcmcia(&nic, netdev_card_by_name("pcmcia"));

    ok("plain card: detected", ne2000_detect(&nic) != FALSE);
    expect_u32("plain card: no command-register retry",
               (unsigned long)note_count(ANXDIAG_CR_RETRY), 0);
    expect_u32("plain card: no odd-register retry",
               (unsigned long)note_count(ANXDIAG_ODD_RETRY), 0);
    ok("plain card: the byte path is what it was left in", nic.bus.getodd == 0);
    ok("plain card: and the card answered every odd byte read",
       mock_odd_refused == 0);
    /*
     * The reset landed on the first strobe: this card answers the read, so the
     * defect the retry exists for cannot arise here at all.
     */
    ok("plain card: the reset landed on the first strobe", mock_resets == 1);
}

/*
 * A card that fails the command register in BOTH modes.  The word path is
 * never the state a failure is left in: a probe that turned it on and then
 * gave up would hand the next attempt a mode it did not earn.
 */
static void test_dead_card(void)
{
    NetdevNic nic;

    chip_begin(1, 0x00);
    mock_no_reset = 1;
    board_pcmcia(&nic, netdev_card_by_name("pcmcia"));

    ok("dead card: refused", ne2000_detect(&nic) == FALSE);
    expect_u32("dead card: refused on the command register",
               (unsigned long)nic.diag_why, (unsigned long)ANXDIAG_WHY_CR);
    ok("dead card: and the word path was turned back off",
       nic.bus.getodd == 0);
    expect_u32("dead card: the retry was attempted once",
               (unsigned long)note_count(ANXDIAG_CR_RETRY), 1);
}

/*
 * A bus the word path cannot be done on: no second window, so a word read at
 * reg-1 is not the odd register but the byte beside it.  Refused without the
 * flag ever being set -- xsurf500 is the row in the table with this shape.
 */
static void test_no_odd_window(void)
{
    NetdevNic nic;

    chip_begin(1, CR_WARM);
    board_contiguous(&nic, netdev_card_by_name("pcmcia"));

    ok("no odd window: refused", ne2000_detect(&nic) == FALSE);
    expect_u32("no odd window: refused on the command register",
               (unsigned long)nic.diag_why, (unsigned long)ANXDIAG_WHY_CR);
    ok("no odd window: and the word path was never turned on",
       nic.bus.getodd == 0);
    expect_u32("no odd window: and no retry was recorded",
               (unsigned long)note_count(ANXDIAG_CR_RETRY), 0);
}

int main(void)
{
    test_clone_warm();
    test_clone_cold();
    test_plain_card();
    test_dead_card();
    test_no_odd_window();

    printf("%s\n", failures == 0 ? "PASS" : "FAIL");

    return failures == 0 ? 0 : 1;
}
