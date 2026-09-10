/*
 * anxnet.device: the ISA Plug and Play sequence in front of an X-Surf's
 * RTL8019AS, which decodes nothing until it has been configured.
 *
 * WHY THIS FILE EXISTS.  netdev_isapnp.c was the last file in src/netdev that
 * a host could compile and nothing did.  It is not covered by the emulated
 * xsurf arm in the way that sounds: that arm proves the card came up, so it
 * proves the sequence WORKS on Amiberry's model of the board -- it says
 * nothing about which byte went to which address, and the two ports this
 * protocol lives on are THE SAME BOARD ADDRESS with one latch bit between
 * them.  A sequence that wrote the whole initiation key to WRITE_DATA instead
 * of ADDRESS would still be a plausible-looking log.
 *
 * THE NUMBER THE ROW COMMENT STATES.  netdev_cards.c says the PnP ADDRESS
 * port ($279) and WRITE_DATA port ($a79) "are one board address, $84f2", and
 * that the chip is given ISA port $300 because (reg_off - io_win) / stride
 * works out to it.  Both are asserted here against the REAL row -- this test
 * links netdev_cards.c rather than carrying a fixture, so an edit to the row
 * that breaks the arithmetic fails here and not in a comment.
 *
 * WHAT A MEMORY BOARD CAN AND CANNOT MODEL.  Every access in netdev_isapnp.c
 * is a plain volatile read or write of the board window, so a byte array is a
 * faithful model of writes and of the latch -- and NOT of the isolation
 * protocol, where each bit is two reads of ONE address that a real card
 * answers $55 then $aa.  An array returns the same byte twice, so every bit
 * reads zero.  That is stated rather than worked around: the identifier comes
 * back all zeroes, which drives the checksum-mismatch path, and the
 * specification's own rule that a mismatch is recorded and not acted on is
 * then what this file checks.  pnp_checksum() is exercised directly instead.
 *
 * netdev_isapnp.c is #included whole for its statics, the same reason
 * test_netdev_el3.c includes el3.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

/*
 * The command-register probe reaches the board through these, and
 * netdev_float.h leaves them overridable for exactly this.  Routed through a
 * model so the test can be a window that ANSWERS (a real chip: separate
 * storage per register) and one that does not, which is the difference
 * between netdev_isapnp_configure() returning TRUE and FALSE.
 */
static int   cr_window_answers = 1;
static UBYTE cr_keeper;                 /* one latch for the whole window */

static void cr_put(volatile UBYTE *p, UBYTE v)
{
    cr_keeper = v;
    if (cr_window_answers)
        *p = v;
}

static UBYTE cr_get(volatile UBYTE *p)
{
    return cr_window_answers ? *p : cr_keeper;
}

#define NETDEV_CR_PUT(p, v)     cr_put((p), (UBYTE)(v))
#define NETDEV_CR_GET(p)        cr_get((p))

/*
 * Every board write the sequence makes, in order.  The ADDRESS port is
 * written forty times and only the last byte would survive in the array, so
 * the order has to be recorded as it happens or the protocol is unreadable.
 */
#define TRACE_MAX 512
static struct { unsigned long off; UBYTE val; } wtrace[TRACE_MAX];
static unsigned wtrace_n;
static UBYTE   *trace_base;

static void trace_w8(volatile UBYTE *p, UBYTE v)
{
    if (wtrace_n < (unsigned)TRACE_MAX && trace_base != NULL)
    {
        wtrace[wtrace_n].off = (unsigned long)((volatile UBYTE *)p -
                                               (volatile UBYTE *)trace_base);
        wtrace[wtrace_n].val = v;
        wtrace_n++;
    }
    *p = v;
}

#define PNP_W8(p, v)            trace_w8((p), (UBYTE)(v))

#include "netdev_isapnp.c"

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

    printf("FAIL %s: got $%lx, want $%lx\n", what, got, want);
    failures++;
}

/* ------------------------------------------------------------- the stubs - */

/*
 * The four chip cores netdev_cards.c names.  Referenced by address only, so
 * empty tables are enough -- the same three tentative definitions
 * test_netdev_cards.c and test_netdev_ne2000.c carry.
 */
#include "netdev_nic.h"
const struct NetdevNicOps netdev_nic_ne2000;
const struct NetdevNicOps netdev_nic_ed;
const struct NetdevNicOps netdev_nic_lance;
const struct NetdevNicOps netdev_nic_el3;

/* The probe record.  Only that a note was made, and which. */
#define DIAG_MAX 32
static struct { UWORD code; UWORD card; ULONG value; } diag[DIAG_MAX];
static UWORD diag_n;

VOID netdev_diag_note(UWORD code, UWORD card, ULONG value)
{
    if (diag_n < (UWORD)DIAG_MAX)
    {
        diag[diag_n].code  = code;
        diag[diag_n].card  = card;
        diag[diag_n].value = value;
        diag_n++;
    }
}

UWORD netdev_diag_card(const NetdevCard *card)
{
    (VOID)card;
    return 7;
}

VOID netdev_trace_val(const char *tag, ULONG v)
{
    (VOID)tag;
    (VOID)v;
}

/*
 * The delay is a spin on a real bus cycle measured against the beam.  Nothing
 * here is about timing, and a host has no beam: the wait completes at once.
 * The READ that pnp_delay() does is still performed by the file under test,
 * which is what matters -- it is the access that keeps the latch honest.
 */
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

/* ------------------------------------------------------------ the board -- */

#define BOARD_BYTES 0x10000

static UBYTE board[BOARD_BYTES];

static const NetdevCard *xsurf_row(void)
{
    const NetdevCard *c = netdev_card_by_name("xsurf");

    return c;
}

static void board_reset(void)
{
    memset(board, 0, sizeof(board));
    wtrace_n   = 0;
    trace_base = board;
    diag_n     = 0;
    waits    = 0;
    cr_window_answers = 1;
    cr_keeper = 0;
}

/* ============================================ the two ports, one address = */

/*
 * netdev_cards.c: "the PnP ADDRESS ($279) and WRITE_DATA ($a79) ports are one
 * board address, $84f2".  If they ever stop being, the latch is the only
 * thing separating a register SELECT from a register WRITE, and a stale one
 * turns every configuration write into a select.
 */
static void a_ports_and_latch(void)
{
    const NetdevCard *card = xsurf_row();
    volatile UBYTE   *p;

    board_reset();

    expect(card != NULL, "the xsurf row is in the table");
    if (card == NULL)
        return;

    expect(card->pnp != NULL, "and it carries an ISA PnP bridge");
    if (card->pnp == NULL)
        return;

    p = pnp_port(card, board, PNP_ADDRESS);
    expect_hex("ADDRESS ($279) lands at board+$84f2",
               (unsigned long)(p - board), 0x84f2u);
    expect_hex("and leaves the A11 latch clear",
               board[card->pnp->hi_reg], 0x00u);

    p = pnp_port(card, board, PNP_WRITE_DATA);
    expect_hex("WRITE_DATA ($a79) is the SAME board address",
               (unsigned long)(p - board), 0x84f2u);
    expect_hex("and sets the A11 latch", board[card->pnp->hi_reg],
               card->pnp->hi_bit);

    p = pnp_port(card, board, PNP_READ_DATA);
    expect_hex("READ_DATA ($203) lands at board+$8406",
               (unsigned long)(p - board), 0x8406u);
    expect_hex("with the latch clear again", board[card->pnp->hi_reg], 0x00u);

    /* The window carries ports $000..$7ff and the latch carries bit 11, so
       every port must land inside the window whatever the caller asks for. */
    {
        ULONG n;

        for (n = 0; n < 0x1000u; n++)
        {
            p = pnp_port(card, board, (UWORD)n);
            if ((ULONG)(p - board) < card->pnp->io_win ||
                (ULONG)(p - board) >= card->pnp->io_win + 0x1000u)
            {
                expect(0, "a port landed outside the board's ISA window");
                break;
            }
        }
        checks++;
    }
}

/* ================================================= the derived I/O base == */

/*
 * netdev_cards.c states the register base once and derives the ISA port from
 * it: "ISA port = (reg_off - pnp->io_win) / stride == $300".  Two copies of
 * one fact, and the row is where a card is added.
 */
static void b_the_io_base_is_derived(void)
{
    const NetdevCard *card = xsurf_row();
    UWORD             port;

    if (card == NULL || card->pnp == NULL)
        return;

    port = (UWORD)((card->reg_off - card->pnp->io_win) /
                   (ULONG)card->stride);

    expect_hex("the row derives ISA port $300", port, 0x0300u);

    /* And $300 is what the part's own resource data allows: $220..$380 on a
       $20 alignment.  A row edited to an unaligned or out-of-range base would
       be accepted by this driver and refused by the chip. */
    expect(port >= 0x0220u && port <= 0x0380u,
           "which is inside the range the part offers");
    expect_hex("and on its $20 alignment", port & 0x1fu, 0u);
}

/* ==================================================== the checksum ======= */

/*
 * The ninth identifier byte, ISA PnP 1.0a appendix B.  There is no published
 * vector in this tree to check a value against, and computing an expected
 * value with the same shift register would only assert that the code equals
 * itself.  What IS checkable without a vector is the property that makes it a
 * checksum rather than a hash of convenience: every single-bit error in the
 * 64 bits it covers must change it.  A feedback tap dropped or a shift
 * direction reversed loses that immediately.
 */
/*
 * The specification's shift register, transcribed here a second time and
 * independently of netdev_isapnp.c's expression of it.
 *
 * ISA PnP 1.0a appendix B, and the same recurrence Linux writes in one line
 * in drivers/pnp/isapnp/core.c: an 8-bit register preset to $6a, the new
 * high bit is (LFSR[0] XOR LFSR[1] XOR datum) and the register shifts right,
 * clocked once per identifier bit, least significant bit of each byte first.
 *
 * WHY A SECOND COPY IS WORTH ITS KEEP.  The avalanche property below holds
 * for a shift register with the WRONG tap just as well as the right one -- a
 * mutation moving the feedback from LFSR[1] to LFSR[2] passed every check
 * here until this was added.  Two transcriptions of one specification
 * disagree when either drifts; one transcription plus a property only says
 * the thing is a checksum, not that it is this checksum.
 */
static UBYTE ref_checksum(const UBYTE *id)
{
    UBYTE lfsr = 0x6a;
    int   i;
    int   j;

    for (i = 0; i < 8; i++)
    {
        for (j = 0; j < 8; j++)
        {
            UBYTE datum = (UBYTE)((id[i] >> j) & 1u);
            UBYTE tap   = (UBYTE)((lfsr & 1u) ^ ((lfsr >> 1) & 1u) ^ datum);

            lfsr = (UBYTE)((lfsr >> 1) | (UBYTE)(tap << 7));
        }
    }

    return lfsr;
}

static void c_checksum_detects_every_single_bit_error(void)
{
    static const UBYTE base[9] =
    {
        0x49, 0x14, 0x80, 0x19, 0x12, 0x34, 0x56, 0x78, 0x00
    };
    UBYTE id[9];
    UBYTE sum0;
    UWORD i;
    UWORD j;
    int   missed = 0;

    memcpy(id, base, sizeof(id));
    sum0 = pnp_checksum(id);

    /* Pure: the same eight bytes give the same answer. */
    expect(pnp_checksum(id) == sum0, "the checksum is a function of the bytes");

    /* And the ninth byte is not an input -- it is what the card drives OUT. */
    id[8] = 0xff;
    expect(pnp_checksum(id) == sum0,
           "the ninth byte is the output, not an input");

    for (i = 0; i < 8; i++)
    {
        for (j = 0; j < 8; j++)
        {
            memcpy(id, base, sizeof(id));
            id[i] ^= (UBYTE)(1u << j);
            if (pnp_checksum(id) == sum0)
                missed++;
        }
    }

    expect(missed == 0,
           "every single-bit error in the identifier changes the checksum");
    if (missed != 0)
        printf("     %d of 64 single-bit errors were missed\n", missed);

    /*
     * And it is the SPECIFICATION's register, not merely a working one.  Over
     * the base identifier, every single-bit variant of it, all-zeroes and
     * all-ones -- 67 identifiers, each 64 clocks.
     */
    {
        int disagreed = 0;

        memcpy(id, base, sizeof(id));
        if (pnp_checksum(id) != ref_checksum(id))
            disagreed++;

        for (i = 0; i < 8; i++)
        {
            for (j = 0; j < 8; j++)
            {
                memcpy(id, base, sizeof(id));
                id[i] ^= (UBYTE)(1u << j);
                if (pnp_checksum(id) != ref_checksum(id))
                    disagreed++;
            }
        }

        memset(id, 0x00, sizeof(id));
        if (pnp_checksum(id) != ref_checksum(id))
            disagreed++;
        memset(id, 0xff, sizeof(id));
        if (pnp_checksum(id) != ref_checksum(id))
            disagreed++;

        expect(disagreed == 0,
               "and it is the shift register ISA PnP 1.0a appendix B defines");
        if (disagreed != 0)
            printf("     %d of 67 identifiers disagreed with the reference\n",
                   disagreed);
    }
}

/* ============================================ the sequence on a board ==== */

/*
 * THE LATCH IS WRITTEN ON EVERY ACCESS, so the trace is not a list of
 * protocol bytes -- it is pairs.  pnp_port() sets A11 to match the port
 * before returning the address, which means one configuration write is FOUR
 * entries:
 *
 *     $007e = $00    select the ADDRESS port  (A11 clear)
 *     $84f2 = reg    the register number
 *     $007e = $80    select WRITE_DATA        (A11 set)
 *     $84f2 = val    the value
 *
 * That is deliberate and it is the thing worth asserting: written on every
 * access rather than tracked, because a stale latch makes the two ports the
 * same register and every configuration write a select.
 */
#define PORT_ADDR   0x84f2u
#define LATCH_ADDR  0x007eu

static int is_latch(int i, UBYTE want)
{
    return i >= 0 && i < (int)wtrace_n &&
           wtrace[i].off == LATCH_ADDR && wtrace[i].val == want;
}

static int is_port(int i, UBYTE want)
{
    return i >= 0 && i < (int)wtrace_n &&
           wtrace[i].off == PORT_ADDR && wtrace[i].val == want;
}

/* The index of the latch write that begins a select of `reg`, or -1. */
static int find_select(UBYTE reg, int from)
{
    int i;

    for (i = from; i + 1 < (int)wtrace_n; i++)
    {
        if (is_latch(i, 0x00) && is_port(i + 1, reg))
            return i;
    }

    return -1;
}

/*
 * The value written to a register, checking the whole four-entry shape rather
 * than reading two ahead: a select whose value went out with the latch still
 * clear would land on the ADDRESS port and select again.
 */
static int reg_value(UBYTE reg, int from, UBYTE *out)
{
    int at = find_select(reg, from);

    if (at < 0 || at + 3 >= (int)wtrace_n)
        return -1;

    if (!is_latch(at + 2, 0x80) || wtrace[at + 3].off != PORT_ADDR)
        return -1;

    *out = wtrace[at + 3].val;

    return at;
}

/*
 * The whole sequence against a window that answers.  The board is a byte
 * array, so the isolation reads come back as zeroes and the checksum will not
 * match -- which is the specification's "recorded and not acted on" path, and
 * the run must still configure the chip.
 */
static void d_the_sequence(void)
{
    const NetdevCard *card = xsurf_row();
    BOOL              ok;
    UWORD             i;
    int               at;
    int               key_start;
    UBYTE             v = 0;

    if (card == NULL || card->pnp == NULL)
        return;

    board_reset();
    ok = netdev_isapnp_configure(card, board);

    expect(ok == TRUE, "a window that answers is configured");

    /*
     * The initiation key: two zero writes to ADDRESS, then the 32 bytes, all
     * to the ADDRESS port with the latch CLEAR every time.  Read from the
     * trace, because the board keeps only the last byte written there.
     */
    key_start = -1;
    for (i = 0; i + 1 < (UWORD)wtrace_n; i++)
    {
        if (is_latch((int)i, 0x00) && is_port((int)i + 1, pnp_init_key[0]))
        {
            key_start = (int)i;
            break;
        }
    }
    expect(key_start > 0, "the initiation key was written");

    if (key_start > 0)
    {
        int   good = 1;
        UWORD k;

        for (k = 0; k < 32u; k++)
        {
            int at = key_start + (int)k * 2;

            if (!is_latch(at, 0x00) || !is_port(at + 1, pnp_init_key[k]))
            {
                good = 0;
                break;
            }
        }
        expect(good,
               "all 32 key bytes, in order, to the ADDRESS port with A11 clear");

        /* And the two zero writes that reset the key's own LFSR come first,
           each with its own latch write. */
        expect(key_start >= 4 &&
               is_port(key_start - 1, 0x00) && is_latch(key_start - 2, 0x00) &&
               is_port(key_start - 3, 0x00) && is_latch(key_start - 4, 0x00),
               "preceded by the two zero writes that arm the key LFSR");
    }

    /*
     * The configuration writes.  The I/O base is the one that decides where
     * the chip decodes: $0300 split high and low.
     */
    at = reg_value(PNP_IO_BASE_LO, 0, &v);
    expect(at > 0, "the low half of the I/O base was written");
    if (at > 0)
        expect_hex("as $00", v, 0x00u);

    at = reg_value(PNP_IO_BASE_HI, 0, &v);
    expect(at > 0, "the high half of the I/O base was written");
    if (at > 0)
        expect_hex("as $03, which with the low half is ISA port $300",
                   v, 0x03u);

    at = reg_value(PNP_ACTIVATE, 0, &v);
    expect(at > 0, "the part was activated");
    if (at > 0)
        expect_hex("with a 1", v, 0x01u);

    at = reg_value(PNP_CSN, 0, &v);
    expect(at > 0, "a Card Select Number was handed out");
    if (at > 0)
        expect_hex("and it is ours", v, PNP_OUR_CSN);

    at = reg_value(PNP_LDN, 0, &v);
    expect(at > 0, "the logical device was selected");
    if (at > 0)
        expect_hex("and it is the row's", v, card->pnp->ldn);

    /*
     * ORDER, not just presence.  Wake[0] moves a card to isolation only while
     * its CSN is zero, so Reset CSN must come before it; and the part is
     * activated only after it has been given a base to decode at.
     */
    expect(find_select(PNP_CONFIG_CONTROL, 0) < find_select(PNP_WAKE, 0),
           "Reset CSN comes before the first Wake");
    expect(find_select(PNP_IO_BASE_HI, 0) < find_select(PNP_ACTIVATE, 0) &&
           find_select(PNP_IO_BASE_LO, 0) < find_select(PNP_ACTIVATE, 0),
           "and the I/O base is written before the part is activated");

    /*
     * THE LATCH MUST BE CLEAR WHEN THIS FILE RETURNS.  The chip is given a
     * port below $800, so the register file the rest of the driver reaches is
     * only there while the latch is clear -- and the last WRITE_DATA of the
     * sequence sets it.
     */
    expect_hex("the A11 latch is clear on the way out",
               board[card->pnp->hi_reg], 0x00u);
    expect(wtrace_n > 0 && is_latch((int)wtrace_n - 1, 0x00),
           "and clearing it is the last write the sequence makes");

    /* The identifier a byte array drives is all zeroes, so the checksum does
       not match -- and the run is expected to carry on regardless. */
    {
        int seen_csum = 0;
        int seen_ok   = 0;

        for (i = 0; i < diag_n; i++)
        {
            if (diag[i].code == (UWORD)ANXDIAG_PNP_CSUM)
                seen_csum = 1;
            if (diag[i].code == (UWORD)ANXDIAG_PNP_OK)
                seen_ok = 1;
        }
        expect(seen_csum, "the checksum comparison was recorded");
        expect(seen_ok,
               "and a mismatch did not stop the sequence -- what decides is"
               " whether a chip answers");
    }
}

/* A card with no bridge is left alone: the chip is already decoding. */
static void e_no_bridge_is_not_touched(void)
{
    const NetdevCard *card = netdev_card_by_name("ariadne2");

    if (card == NULL)
        return;

    board_reset();
    expect(card->pnp == NULL, "the ariadne2 row has no ISA PnP bridge");
    expect(netdev_isapnp_configure(card, board) == TRUE,
           "and a row without one is configured trivially");
    expect(wtrace_n == 0, "with no access to the board at all");
}

/*
 * A window that does NOT answer.  The settle loop asks 125 times over 250 ms
 * and then gives up, and the caller must be told -- a driver that reported
 * success here would enumerate a unit that never receives a frame.
 */
static void f_a_silent_window_is_reported(void)
{
    const NetdevCard *card = xsurf_row();
    UWORD             i;
    int               seen_silent = 0;
    int               seen_ok     = 0;

    if (card == NULL || card->pnp == NULL)
        return;

    board_reset();
    cr_window_answers = 0;              /* one keeper for the whole window */

    expect(netdev_isapnp_configure(card, board) == FALSE,
           "a window that does not answer is refused");

    for (i = 0; i < diag_n; i++)
    {
        if (diag[i].code == (UWORD)ANXDIAG_PNP_SILENT)
            seen_silent = 1;
        if (diag[i].code == (UWORD)ANXDIAG_PNP_OK)
            seen_ok = 1;
    }

    expect(seen_silent, "and says so in the probe record");
    expect(!seen_ok, "without also claiming it came up");

    /* It gave the part the full 125 rounds before deciding. */
    for (i = 0; i < diag_n; i++)
    {
        if (diag[i].code == (UWORD)ANXDIAG_PNP_SETTLE)
            expect_hex("after the full settle budget", diag[i].value,
                       (unsigned long)PNP_SETTLE_ROUNDS);
    }
}

int main(void)
{
    a_ports_and_latch();
    b_the_io_base_is_derived();
    c_checksum_detects_every_single_bit_error();
    d_the_sequence();
    e_no_bridge_is_not_touched();
    f_a_silent_window_is_reported();

    if (failures != 0)
    {
        printf("netdev_isapnp: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("netdev_isapnp: %d checks ok\n", checks);

    return 0;
}
