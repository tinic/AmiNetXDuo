/*
 * anxnet.device: the ISA Plug and Play bridge in front of an X-Surf's DP8390.
 *
 * An Individual Computers X-Surf is a Zorro II board carrying an RTL8019AS --
 * an NE2000 on a chip -- on a private ISA bus.  The board maps as soon as
 * autoconfig places it.  The chip does not follow.  Whether it decodes
 * anything depends on how the board strapped it, and until something takes it
 * through Plug and Play there is nothing to say it did: the window reads as a
 * floating bus, ne2000.c's detection reads $ff for the command register, and
 * attach refuses.  This file gives attach a chip to find.
 *
 * The row's reg_off says where the register file is, and this file makes that
 * true rather than reporting back where the chip ended up.  The I/O base
 * programmed below is (reg_off - io_win) / stride, which is $300 -- the port
 * NetBSD's if_ne_xsurf.c maps (NE_XSURF_NICBASE 0x0300, NE_XSURF_ASICBASE
 * 0x0310) and the port Linux's zorro8390.c encodes as the board offset 0x8600.
 *
 * The board's addresses come from wiki.icomp.de/wiki/X-Surf, the card's
 * published memory map, and from Amiberry's model of the same board in
 * src/qemuvga/ne2000.cpp.  The two agree:
 *
 *   $8000..$8fff   the ISA I/O space, "addresses multiplied by two" -- so the
 *                  4 KB window carries ISA ports $000..$7ff and no more.
 *   $007e          on WRITE, the A11 line of the local ISA bus: "$80 adds
 *                  $800 to the address, $00 gives you the normal IO address
 *                  space".  On READ it is the IDE interrupt status.
 *
 * Amiberry computes exactly that -- `isa_addr = (flags ? 0x1000 : 0) + (addr -
 * 0x8000)` with the flag taken from bit 7 of a byte written where `(addr &
 * 0x80ff) == 0x007e` -- and matches it against `0x279 * 2` and `0xa79 * 2`.
 * The two PnP ports differ only in A11, so on this board they are one address,
 * $84f2, with the latch deciding which of the two it is.  The chip's own
 * registers appear at `io_port * 2` in the same window, 32 ports wide, and
 * only while the logical device is activated.
 *
 * The wiki warns: "Reading this register may also affect the A11 line, so
 * after a read, the A11 status must be restored."  Nothing in this driver
 * reads $7e -- netdev_device.c's interrupt server asks the chip's ISR and no
 * board register at all, and says why -- so the latch has exactly one writer,
 * pnp_port() below.
 *
 * The sequence is ISA Plug and Play specification 1.0a sections 4.3 and 4.4,
 * in the order Linux's drivers/pnp/isapnp/core.c drives it: reset the LFSR
 * with two zero writes, write the 32-byte initiation key, Wake[0] the
 * un-numbered cards into isolation, set the read-data port, read the 72-bit
 * serial identifier, assign a Card Select Number, then Wake[CSN] into
 * configuration and write the logical device's I/O base and Activate.
 *
 * The isolation reads are kept, although this board does not need them: there
 * is one card on the bus, it is soldered down, and the specification's
 * precondition for a CSN -- "the card has been isolated and is the only card
 * in the Isolation state" -- is met the moment Wake[0] moves it there.  They
 * are here because the 72 bits are the answer to "what is behind the bridge".
 * The vendor ID, the serial and the checksum over both go into the probe
 * record, so a CheckNetDevice report tells a card that was configured from
 * one that was merely written at.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_cards.h"
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
 * $203..$3ff the specification allows.  $203 is the lowest, and it cannot
 * collide with the $300..$31f the chip is about to be given.
 *
 * All three are below $400, which the RTL8019AS requires of the first two:
 * "In RTL8019AS, SA10 should be 0 for a valid access to PnP ports" (datasheet
 * 4.2), and $279 and $a79 both have bit 10 clear.
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
 * The initiation key is Realtek's and not the specification's.  The RTL8019AS
 * answers two keys, and its datasheet (section 6.1) says which works when:
 * "the RT initiation key is supported in all configuration modes
 * while the PnP initiation key is only supported in the PnP mode.  By using
 * the RT initiation key, the software can put RTL8019AS to the PnP Config
 * state and access the logical device configuration registers even in the
 * jumper and RT jumperless modes."
 *
 * Which of the three modes an X-Surf straps its chip into is not published and
 * not visible from here.  The RT key is the one that does not need to know, so
 * it is the only key sent.  It is also the key Amiberry's model of this card
 * is built around, `rt_pnp_init_key`, which is this table byte for byte.
 *
 * The bytes are the datasheet's, section 6.2.1.  They are an LFSR like the
 * specification's, same taps, seeded $da and with the feedback inverted.  That
 * rule reproduces the table exactly, but the datasheet only tabulates, so this
 * tabulates too.
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
 * There is no timer open at probe time and a device cannot Delay(), so a wait
 * is a spin on a real bus cycle -- the arrangement ne2000.c and
 * netdev_pcmcia.c already use, for the same reason.  A Zorro II read is 280 ns
 * at the fastest a board can be, so four per microsecond is a lower bound on
 * the time.
 *
 * The address read is the register file's own base.  Before activation nothing
 * decodes there and the read is a float.  After it, the read is the command
 * register.  Neither has a side effect, and it cannot collide with the
 * read-data port, which is a long way below it.
 */
static VOID pnp_delay(const NetdevCard *card, volatile UBYTE *board, ULONG us)
{
    volatile UBYTE *p = board + card->reg_off;
    ULONG           n = us * 4u;

    while (n-- != 0)
        (VOID)*p;
}

/*
 * ISA port n in the board window, with the latch set to match.  The window is
 * not wide enough for the port space, so bit 11 of the port lives in a
 * write-only latch on the board and the rest is an address.  The latch is
 * written on every access rather than tracked: it is one byte write to a
 * Zorro board, and a stale latch is the failure that makes the ADDRESS port
 * and the WRITE_DATA port the same register.
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
 * The latch is left clear on every exit from this file.  The chip is given a
 * port below $800, so the register file the rest of the driver reaches is
 * only there while the latch is clear.  A card that leaves the sequence with
 * the latch set configures correctly and then answers nothing.  The last
 * thing done before that is a WRITE_DATA, which sets the latch.
 */
static VOID pnp_latch_clear(const NetdevCard *card, volatile UBYTE *board)
{
    board[card->pnp->hi_reg] = 0;
}

/* ------------------------------------------------------ the isolation --- */

/*
 * The checksum the card drives out as the ninth identifier byte, computed over
 * the eight before it.  Specification 1.0a appendix B: an 8-bit shift register
 * reset to $6a, feedback taps at bits 0 and 1, clocked once per identifier
 * bit.
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
 * port: $55 then $aa is a one and anything else is a zero, which is how a bus
 * full of cards drives one wire without shorting.  Bits arrive least
 * significant first within a byte, and the nine bytes are four of vendor and
 * device ID, four of serial number, and the checksum.
 *
 * The delays are the specification's, not the emulator's.  1.0a section 3.3.2:
 * "The software must delay 1 msec prior to starting the first pair of
 * isolation reads, and must wait 250 usec between each subsequent pair of
 * isolation reads.  This delay gives the ISA card time to access information
 * from possibly very slow storage devices."  Linux spends it before each read
 * of the pair rather than between pairs, and so does this: 144 reads, about
 * 36 ms, once, at probe time.
 *
 * Amiberry answers on the instruction after, so none of it is measurable in
 * the lab.  The same missing wait already broke the PCMCIA option-register
 * write in this driver, so the figures come from the specification and not
 * from what the emulator tolerates.
 *
 * The RTL8019AS datasheet reprints this passage with "250 msec" for the second
 * figure.  That is a typo in the datasheet -- it is copying 1.0a almost word
 * for word, and 1.0a says usec.
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
 * Is a DP8390 decoding where the row says it is?
 *
 * The same test netdev_pcmcia.c makes after its own configuration write, made
 * the same way and for the same reason: a read of the command register cannot
 * answer this, because a chip out of reset and an address nothing decodes both
 * read $00.  It is written instead -- CR is read/write and STP|RD2 is the
 * state the chip is already in -- and a window with nothing behind it does not
 * remember what was written to it.
 *
 * This is not shared with that file as one function.  This one is reached
 * through a board pointer the probe has just been handed, and that one through
 * a fixed window named in a card row.  Threading either shape through the
 * other buys one three-line function.  What has to be the same is the
 * comparison, and it is, including masking the START bit out for the clones
 * that come up with it stuck (cnetdevice.asm:3611-3615).
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
 * figure.  Specification 1.0a names four delays and none of them is this one.
 * The RTL8019AS datasheet reprints the same four and adds none.  1.0a's
 * wording is that setting the bit "forces the logical device to become active
 * on the ISA bus at its assigned resources", which reads as immediate.
 *
 * So this is a measurement.  It asks, waits 2 ms, and asks again, up to
 * 250 ms, and records how many rounds it took.  A part that is ready at once
 * -- which is what the specification implies and what Amiberry does -- costs
 * one register write and no time at all.  A part that needs time gets it, and
 * the round count reaches a report through CheckNetDevice, so what a real
 * X-Surf needs stops being a guess after one report.
 *
 * The bound is 250 ms because the same failure already happened in this
 * driver, on the PCMCIA option register, where reading back on the next
 * instruction reported a working card as an empty slot.  The fix was to wait.
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
     * port, and that is the only way into it (1.0a section 3.2): a card in
     * Wait for Key compares each byte against its own LFSR and starts over on
     * a mismatch.
     *
     * No delay after the key.  The specification's 1 msec is anchored to the
     * first isolation read pair and not to the key, and there is nothing else
     * between the two here.
     */
    pnp_write_address(card, b, 0x00);
    pnp_write_address(card, b, 0x00);

    for (i = 0; i < 32; i++)
        pnp_write_address(card, b, pnp_init_key[i]);

    /*
     * Put the Card Select Number back to zero.  This is about the second time
     * this file runs on one machine.
     *
     * Wake[0] moves a card to Isolation only while its CSN is still zero, and
     * a CSN survives the Wait for Key this file ends on.  So on a machine
     * where anxnet.device is expunged and loaded again, without this the card
     * sits in Sleep through the isolation reads.  The identifier then comes
     * back as whatever the bus drives.  The configuration below is written
     * with no check that a card is there.  It still works, because Wake[CSN]
     * finds it, but the one step that says what is behind the bridge stops
     * saying anything.
     *
     * Bus-wide by definition, and that is exactly one soldered chip here.  No
     * delay after it: this is the Reset CSN command and not the Reset command,
     * and both the specification and the RTL8019AS datasheet say it causes no
     * state transition -- the 2 msec they do name belongs to RESET_DRV and to
     * Config Control bit 0, neither of which is written here.
     */
    pnp_write_reg(card, b, PNP_CONFIG_CONTROL, PNP_CC_RESET_CSN);

    /*
     * Wake the cards that have no CSN into isolation, then tell them where to
     * answer.  This order is the specification's own recipe for Set RD_DATA
     * Port -- "Issue the Initiation Key / Send command Wake[0] / Send command
     * Set RD_DATA Port" -- and it is Linux's.
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
     * A checksum that does not match is recorded and not acted on.  It says
     * the isolation reads did not come back as a card driving them.  On a
     * board with one soldered part that is likelier to be this driver
     * mis-reading the window than a card that cannot be configured, and the
     * write below is harmless either way.  What decides is whether a chip
     * answers at the end.
     */
    pnp_write_reg(card, b, PNP_CSN, PNP_OUR_CSN);

    /* Out of isolation, into configuration, and pick the logical device. */
    pnp_write_reg(card, b, PNP_WAKE, PNP_OUR_CSN);
    pnp_write_reg(card, b, PNP_LDN, pnp->ldn);

    /*
     * The I/O base.  $300 is inside what the part accepts -- its resource
     * data offers $220..$380 on a $20 alignment, $20 long, which is the whole
     * NE2000 file -- and it is where both free drivers for this board already
     * assume the chip is.  On a card whose 9346 put it there at power-up this
     * write changes nothing.  In Amiberry, whose model has no base until one
     * is written, it is what makes the window decode at all.
     */
    pnp_write_reg(card, b, PNP_IO_BASE_HI, (UBYTE)(port >> 8));
    pnp_write_reg(card, b, PNP_IO_BASE_LO, (UBYTE)(port & 0xffu));
    netdev_diag_note(ANXDIAG_PNP_IO, ci, (ULONG)port);
    PNP_TRACE("pnp: io ", (ULONG)port);

    /*
     * No interrupt is assigned, and that is deliberate.
     *
     * Which ISA interrupt line this board wires the chip's INT pin to is not
     * published, and the emulator cannot say -- it stores the number written
     * to register $70 and never reads it again.  The board knows: the part
     * takes its power-up configuration, interrupt select included, from the
     * 9346 serial EEPROM beside it, and register $70 keeps whatever is in it
     * across the isolation and configuration above.  A guess written here
     * replaces the board's own answer with ours and routes the chip's
     * interrupt to a pin that can be unconnected, so the card comes up and
     * then never interrupts.  Leaving it alone keeps the board's answer.
     */

    pnp_write_reg(card, b, PNP_ACTIVATE, 0x01);

    /*
     * Back to Wait for Key before the chip is touched.  1.0a section 4.6.1:
     * "When finished programming configuration registers, all cards must be
     * set to the Wait for Key state."  Activation and the I/O base survive it
     * -- it is a state change in the PnP block, not a reset.  If the block is
     * left awake, every later write into this window that lands on $279 is a
     * configuration write.
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
