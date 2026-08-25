/*
 * anxnet.device: the ISA Plug and Play bridge in front of an X-Surf's DP8390.
 *
 * The chip decodes nothing until Plug and Play configures it, so this file
 * gives ne2000.c's attach a chip to find.  The I/O base programmed below is
 * (reg_off - io_win) / stride == $300.  Sequence: ISA PnP 1.0a 4.3 and 4.4.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_cards.h"
#include "netdev_clock.h"
#include "netdev_nic.h"

#include <exec/types.h>

extern VOID netdev_trace_val(const char *tag, ULONG v);
#ifdef NETDEV_TRACE
#define PNP_TRACE(t, v) netdev_trace_val((t), (ULONG)(v))
#else
#define PNP_TRACE(t, v) ((VOID)0)
#endif

/*
 * The three ports the protocol lives on.  READ_DATA is ours to choose from the
 * $203..$3ff the specification allows; $203 cannot collide with the $300..$31f
 * the chip is about to be given.  All three are below $400, which the RTL8019AS
 * requires of the first two (datasheet 4.2: SA10 must be 0).
 */
#define PNP_ADDRESS         0x0279
#define PNP_WRITE_DATA      0x0a79
#define PNP_READ_DATA       0x0203

/* Configuration registers, specification 1.0a appendix A. */
#define PNP_SET_RD_DATA     0x00
#define PNP_SERIAL_ISO      0x01
#define PNP_CONFIG_CONTROL  0x02
#define PNP_WAKE            0x03
#define PNP_CSN             0x06
#define PNP_LDN             0x07
#define PNP_ACTIVATE        0x30
#define PNP_IO_BASE_HI      0x60
#define PNP_IO_BASE_LO      0x61

#define PNP_CC_RESET_CSN    0x04
#define PNP_CC_WAIT_FOR_KEY 0x02

/* The CSN handed out.  Any number from 1: there is one card on this bus. */
#define PNP_OUR_CSN         0x01

/*
 * The RT initiation key, not the specification's: the RTL8019AS supports the
 * PnP key only in PnP mode, while the RT key also reaches the configuration
 * registers in the jumper and RT jumperless modes (datasheet 6.1).  The bytes
 * are the datasheet's, section 6.2.1.
 */
static const UBYTE pnp_init_key[32] =
{
    0xda, 0x6d, 0x36, 0x1b, 0x8d, 0x46, 0x23, 0x91,
    0x48, 0xa4, 0xd2, 0x69, 0x34, 0x9a, 0x4d, 0x26,
    0x13, 0x89, 0x44, 0xa2, 0x51, 0x28, 0x94, 0xca,
    0x65, 0x32, 0x19, 0x0c, 0x86, 0x43, 0xa1, 0x50
};

/* ------------------------------------------------------------- the bus --- */

/*
 * No timer is open at probe time and a device cannot Delay(), so a wait is a
 * spin on a real bus cycle, measured against the beam with the count kept as
 * the floor: the isolation protocol has real minimum delays between its writes.
 * The address read is the register file's own base -- a float before
 * activation, the command register after -- and neither has a side effect.
 */
static VOID pnp_delay(const NetdevCard *card, volatile UBYTE *board, ULONG us)
{
    volatile UBYTE *p = board + card->reg_off;
    NetdevWait      w;

    netdev_wait_begin(&w, us, us * 4u);

    do
        (VOID)*p;
    while (!netdev_wait_done(&w));
}

/*
 * ISA port n in the board window, with the latch set to match.  The window is
 * not wide enough for the port space, so bit 11 of the port lives in a
 * write-only latch on the board.  Written on every access rather than tracked:
 * a stale latch makes the ADDRESS and WRITE_DATA ports the same register.
 */
static volatile UBYTE *pnp_port(const NetdevCard *card, volatile UBYTE *board,
                                UWORD port)
{
    const NetdevIsaPnp *pnp = card->pnp;

    board[pnp->hi_reg] = (UBYTE)((port & 0x0800u) != 0 ? pnp->hi_bit : 0);

    return board + pnp->io_win +
           (ULONG)(port & 0x07ffu) * (ULONG)card->stride;
}

static VOID pnp_write_address(const NetdevCard *card, volatile UBYTE *board,
                              UBYTE v)
{
    *pnp_port(card, board, PNP_ADDRESS) = v;
}

/* Address then data, which is every configuration write there is. */
static VOID pnp_write_reg(const NetdevCard *card, volatile UBYTE *board,
                          UBYTE reg, UBYTE v)
{
    pnp_write_address(card, board, reg);
    *pnp_port(card, board, PNP_WRITE_DATA) = v;
}

static UBYTE pnp_read_data(const NetdevCard *card, volatile UBYTE *board)
{
    return *pnp_port(card, board, PNP_READ_DATA);
}

/*
 * The latch must be left clear on every exit from this file: the chip is given
 * a port below $800, so the register file the rest of the driver reaches is
 * only there while the latch is clear, and the last WRITE_DATA sets it.
 */
static VOID pnp_latch_clear(const NetdevCard *card, volatile UBYTE *board)
{
    board[card->pnp->hi_reg] = 0;
}

/* ------------------------------------------------------ the isolation --- */

/*
 * The checksum the card drives out as the ninth identifier byte, computed over
 * the eight before it.  Specification 1.0a appendix B: an 8-bit shift register
 * reset to $6a, feedback taps at bits 0 and 1, clocked once per identifier bit.
 */
static UBYTE pnp_checksum(const UBYTE *id)
{
    UBYTE sum = 0x6a;
    UWORD i;
    UWORD j;

    for (i = 0; i < 8; i++)
    {
        for (j = 0; j < 8; j++)
        {
            UBYTE bit = (UBYTE)((id[i] & (UBYTE)(1u << j)) != 0 ? 1u : 0u);

            sum = (UBYTE)((UBYTE)((((sum ^ (UBYTE)(sum >> 1)) & 0x01u) ^ bit)
                                  << 7) |
                          (UBYTE)(sum >> 1));
        }
    }

    return sum;
}

/*
 * Read the 72-bit serial identifier.  Each bit is two reads of the read-data
 * port: $55 then $aa is a one, anything else a zero.  Bits arrive least
 * significant first within a byte; the nine bytes are four of vendor and device
 * ID, four of serial number, and the checksum.  1.0a 3.3.2 requires 1 msec
 * before the first pair and 250 usec between subsequent pairs (the RTL8019AS
 * datasheet's "250 msec" reprint is a typo).
 */
static VOID pnp_isolate(const NetdevCard *card, volatile UBYTE *board,
                        UBYTE *id)
{
    UWORD i;
    UWORD j;

    pnp_write_address(card, board, PNP_SERIAL_ISO);
    pnp_delay(card, board, 1000);

    for (i = 0; i < 9; i++)
    {
        UBYTE b = 0;

        for (j = 0; j < 8; j++)
        {
            UBYTE lo;
            UBYTE hi;

            pnp_delay(card, board, 250);
            lo = pnp_read_data(card, board);
            pnp_delay(card, board, 250);
            hi = pnp_read_data(card, board);

            if (lo == 0x55 && hi == 0xaa)
                b = (UBYTE)(b | (UBYTE)(1u << j));
        }

        id[i] = b;
    }
}

/* ------------------------------------------------------------- settling -- */

/*
 * Is a DP8390 decoding where the row says it is?  A read of the command
 * register cannot answer this: a chip out of reset and an address nothing
 * decodes both read $00.  CR is written instead -- it is read/write and STP|RD2
 * is the state the chip is already in -- with the START bit masked out of the
 * comparison for the clones that come up with it stuck.
 */
static UBYTE pnp_last_cr;

static BOOL pnp_chip_answers(const NetdevCard *card, volatile UBYTE *board)
{
    volatile UBYTE *cr = board + card->reg_off;
    UBYTE           v;

    *cr = 0x21;             /* ED_CR_STP | ED_CR_RD2 */
    v   = *cr;
    pnp_last_cr = v;

    return (BOOL)((v & (UBYTE)~0x02u) == 0x21);
}

/*
 * How long a card can take to come up after Activate.  Nothing documents a
 * figure, so this asks, waits 2 ms and asks again, up to 250 ms, and records
 * the round count.  A part ready at once costs one register write and no time.
 */
#define PNP_SETTLE_ROUNDS   125
#define PNP_SETTLE_US       2000

static BOOL pnp_chip_settles(const NetdevCard *card, volatile UBYTE *board,
                             UWORD *rounds)
{
    UWORD i;

    for (i = 0; i < PNP_SETTLE_ROUNDS; i++)
    {
        if (pnp_chip_answers(card, board))
        {
            *rounds = i;
            return TRUE;
        }
        pnp_delay(card, board, PNP_SETTLE_US);
    }

    *rounds = i;

    return FALSE;
}

/* ------------------------------------------------------- the sequence --- */

BOOL netdev_isapnp_configure(const NetdevCard *card, APTR board)
{
    volatile UBYTE     *b = (volatile UBYTE *)board;
    const NetdevIsaPnp *pnp;
    UWORD               ci = netdev_diag_card(card);
    UWORD               port;
    UWORD               rounds = 0;
    UBYTE               id[9];
    UBYTE               sum;
    UWORD               i;

    if (card->pnp == NULL)
        return TRUE;            /* no bridge: the chip is already there */

    pnp  = card->pnp;
    port = (UWORD)((card->reg_off - pnp->io_win) / (ULONG)card->stride);

    /*
     * The LFSR that matches the key is reset by two zero writes to the ADDRESS
     * port, and that is the only way into it (1.0a 3.2).  No delay after the
     * key: the specification's 1 msec is anchored to the first isolation pair.
     */
    pnp_write_address(card, b, 0x00);
    pnp_write_address(card, b, 0x00);

    for (i = 0; i < 32; i++)
        pnp_write_address(card, b, pnp_init_key[i]);

    /*
     * Put the Card Select Number back to zero.  Wake[0] moves a card to
     * Isolation only while its CSN is still zero, and a CSN survives the Wait
     * for Key this file ends on, so a second run would find the card asleep.
     * No delay after it: Reset CSN causes no state transition.
     */
    pnp_write_reg(card, b, PNP_CONFIG_CONTROL, PNP_CC_RESET_CSN);

    /*
     * Wake the cards that have no CSN into isolation, then tell them where to
     * answer.  This order is the specification's own recipe for Set RD_DATA
     * Port: Initiation Key, Wake[0], Set RD_DATA Port.
     */
    pnp_write_reg(card, b, PNP_WAKE, 0x00);
    pnp_write_reg(card, b, PNP_SET_RD_DATA, (UBYTE)(PNP_READ_DATA >> 2));

    pnp_isolate(card, b, id);

    sum = pnp_checksum(id);
    netdev_diag_note(ANXDIAG_PNP_VENDOR, ci,
                     ((ULONG)id[0] << 24) | ((ULONG)id[1] << 16) |
                     ((ULONG)id[2] << 8) | (ULONG)id[3]);
    netdev_diag_note(ANXDIAG_PNP_SERIAL, ci,
                     ((ULONG)id[4] << 24) | ((ULONG)id[5] << 16) |
                     ((ULONG)id[6] << 8) | (ULONG)id[7]);
    netdev_diag_note(ANXDIAG_PNP_CSUM, ci, ((ULONG)id[8] << 8) | (ULONG)sum);
    PNP_TRACE("pnp: vendor ", ((ULONG)id[0] << 24) | ((ULONG)id[1] << 16) |
                              ((ULONG)id[2] << 8) | (ULONG)id[3]);

    /*
     * A checksum that does not match is recorded and not acted on: it says the
     * isolation reads did not come back as a card driving them, and the write
     * below is harmless either way.  What decides is whether a chip answers.
     */
    pnp_write_reg(card, b, PNP_CSN, PNP_OUR_CSN);

    /* Out of isolation, into configuration, and pick the logical device. */
    pnp_write_reg(card, b, PNP_WAKE, PNP_OUR_CSN);
    pnp_write_reg(card, b, PNP_LDN, pnp->ldn);

    /*
     * The I/O base.  $300 is inside what the part accepts -- its resource data
     * offers $220..$380 on a $20 alignment, $20 long -- and is where both free
     * drivers for this board already assume the chip is.
     */
    pnp_write_reg(card, b, PNP_IO_BASE_HI, (UBYTE)(port >> 8));
    pnp_write_reg(card, b, PNP_IO_BASE_LO, (UBYTE)(port & 0xffu));
    netdev_diag_note(ANXDIAG_PNP_IO, ci, (ULONG)port);
    PNP_TRACE("pnp: io ", (ULONG)port);

    /*
     * No interrupt is assigned, and that is deliberate.  The part takes its
     * interrupt select from the 9346 serial EEPROM beside it, and register $70
     * keeps it across the sequence above; a guess written here would route the
     * chip's interrupt to a pin that can be unconnected.
     */

    pnp_write_reg(card, b, PNP_ACTIVATE, 0x01);

    /*
     * Back to Wait for Key before the chip is touched (1.0a 4.6.1).  Activation
     * and the I/O base survive it -- it is a state change in the PnP block, not
     * a reset.  Left awake, every later write in this window landing on $279 is
     * a configuration write.
     */
    pnp_write_reg(card, b, PNP_CONFIG_CONTROL, PNP_CC_WAIT_FOR_KEY);
    pnp_latch_clear(card, b);

    if (!pnp_chip_settles(card, b, &rounds))
    {
        netdev_diag_note(ANXDIAG_PNP_CR, ci, (ULONG)pnp_last_cr);
        netdev_diag_note(ANXDIAG_PNP_SETTLE, ci, (ULONG)rounds);
        netdev_diag_note(ANXDIAG_PNP_SILENT, ci,
                         (ULONG)(APTR)(b + card->reg_off));
        PNP_TRACE("pnp: silent ", (ULONG)(APTR)(b + card->reg_off));
        return FALSE;
    }

    netdev_diag_note(ANXDIAG_PNP_CR, ci, (ULONG)pnp_last_cr);
    netdev_diag_note(ANXDIAG_PNP_SETTLE, ci, (ULONG)rounds);
    netdev_diag_note(ANXDIAG_PNP_OK, ci, (ULONG)(APTR)(b + card->reg_off));
    PNP_TRACE("pnp: configured ", (ULONG)(APTR)(b + card->reg_off));

    return TRUE;
}
