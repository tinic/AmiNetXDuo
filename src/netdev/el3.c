/*
 * anxnet.device: the 3Com EtherLink III, for the 3C589 in the PCMCIA slot.
 *
 * A THIRD CHIP CORE, sharing nothing below the SANA-II shell with either of
 * the other two.  What it is, against the two that were already here:
 *
 *                     DP8390 family        LANCE            EtherLink III
 *   registers         16, paged banks      RAP/RDP pair     14, windowed
 *   who moves data    the CPU              the chip, DMA    the CPU
 *   buffers           a page ring on the   descriptor       two FIFOs, no
 *                     card                 rings in SRAM    addresses at all
 *   frame boundary    a 4-byte header      a descriptor     a length preamble
 *                     the chip writes      the CPU wrote    the CPU writes
 *   multicast         a 64-bit hash in     a 64-bit hash    NOTHING.  Four
 *                     the chip             in the init      filter bits and
 *                                          block            no hash at all
 *
 * WINDOWS.  The card is sixteen bytes of I/O space.  Offset 0x0e is Command
 * when written and Status when read, in every window; the fourteen bytes
 * under it are one of eight overlays, chosen by a command and read back in
 * the top three bits of Status.  There is no window register.  Window 1 is
 * where the driver lives once it is running, and every excursion out of it
 * (the filter is a command, so it is not one) comes back.
 *
 * MULTICAST IS DONE IN SOFTWARE, AND THAT IS NOT A SHORTCUT.  Set RX Filter
 * has four bits -- individual, group, broadcast, promiscuous -- and the part
 * has no per-group filtering of any kind.  So the hardware is told to accept
 * every group address and el3_rx_wanted() then tests each multicast
 * destination against nic->mar[], the same 64-bit hash netdev_mcaf.c builds
 * for the other two cores and the same netdev_ether_crc32_be() that fills it.
 * The seam above this file does not change and cannot tell the difference.
 *
 * BYTE ORDER IS MEASURED, NOT CONFIGURED.  This is the first 16-bit register
 * access to the PCMCIA slot in this driver, and Gayle's 0xA20000 window is
 * documented two ways: cnet.device's own comments say the register window is
 * swapped, and Amiberry decodes it 1:1 and would accept either. netdev_bus.h
 * names that trap. The window 0 manufacturer ID is a hard-wired 0x6d50, so
 * el3_answers() reads it and accepts 0x6d50 or 0x506d, and everything after
 * that goes through accessors that consult the flag.  Nothing here is a card
 * table knob for somebody to flip.
 *
 * FRAME DATA IS NOT SWAPPED.  The FIFO is a byte stream through one address
 * and the halves of a word off the wire arrive in the order they arrived in;
 * it goes through netdev_bus's burst path, which is the same code and the
 * same window the NE2000 row already reads its ring through.
 *
 * NOTHING EMULATES THIS CARD.  Amiberry's PCMCIA support is NE2000 only, so
 * everything below has been driven by the host test and by nothing else.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_nic.h"
#include "netdev_cards.h"
#include "netdev_mcaf.h"
#include "netdev_macgen.h"
#include "netdev_bsdtypes.h"
#include "el3.h"
#include "el3reg.h"
#include "dp8390.h"     /* the DP8390_TX_* return codes are the shared contract */

#include <aminetxduo/anxdiag.h>

#ifdef NETDEV_TRACE
extern VOID netdev_trace_val(const char *tag, ULONG v);
#define EL_TRACE(t, v)  netdev_trace_val((t), (ULONG)(v))
#else
#define EL_TRACE(t, v)  ((VOID)0)
#endif

/* ---------------------------------------------------------- registers ---- */

/*
 * A word register, through the swap the probe measured.
 *
 * Not netdev_bus's accessors: those are byte-wide and every register here but
 * one is a word, and half of a word register is not a register -- the same
 * reason lance.c reaches its RAP and RDP directly.  The one exception is the
 * transmit status byte, which is at an odd offset and goes through the bus
 * layer precisely so that it lands in Gayle's odd window.
 */
/*
 * The raw word access, and the ONE seam this file has for the host test.
 *
 * A window is not a memory: a write to the command register changes what the
 * next read of offset 0x04 means, and a test that models that has to see the
 * access happen rather than inspect an array afterwards.  Nothing else here
 * would give it that -- every other core is testable against a plain buffer
 * because its registers are a buffer.  test/test_netdev_el3.c defines these
 * two and includes this file; they are a plain load and store otherwise, and
 * the shipping driver compiles as if they were not here.
 */
#ifndef EL3_RAW_GET
#define EL3_RAW_GET(p)      (*(p))
#define EL3_RAW_PUT(p, v)   (*(p) = (v))
#endif

static volatile UWORD *el3_at(NetdevNic *nic, UWORD off)
{
    return (volatile UWORD *)(volatile void *)
           (nic->board + nic->card->reg_off + off);
}

static UWORD el3_swap16(const NetdevNic *nic, UWORD v)
{
    return nic->el3_swap ? (UWORD)((v >> 8) | (v << 8)) : v;
}

static UWORD el3_get(NetdevNic *nic, UWORD off)
{
    return el3_swap16(nic, EL3_RAW_GET(el3_at(nic, off)));
}

static VOID el3_put(NetdevNic *nic, UWORD off, UWORD v)
{
    EL3_RAW_PUT(el3_at(nic, off), el3_swap16(nic, v));
}

static VOID el3_cmd(NetdevNic *nic, UWORD op, UWORD arg)
{
    el3_put(nic, EL3_COMMAND, EL3_CMD(op, arg));
}

static UWORD el3_status(NetdevNic *nic)
{
    return el3_get(nic, EL3_STATUS);
}

/*
 * SELECT A WINDOW, AND REMEMBER WHICH ONE.
 *
 * The bookkeeping is not an optimisation.  A register access in the wrong
 * window is not an error the chip reports -- it reads whatever the other
 * window has there -- so the only defence is that exactly one place changes
 * the window and every reader knows what it left behind.  el3_window is what
 * that place wrote; it is invalidated wherever the chip may have changed it
 * without being asked, which is a reset.
 */
static VOID el3_window(NetdevNic *nic, UBYTE win)
{
    if (nic->el3_win == win)
        return;

    el3_cmd(nic, EL3_C_WINDOW, win);
    nic->el3_win = win;
}

/*
 * Wait out a multi-cycle command.
 *
 * BOUNDED, AND SHORT, BECAUSE THIS RUNS AT INTERRUPT LEVEL.  Receive discard
 * is a multi-cycle command and the receive drain issues one per frame, so
 * this is on the interrupt path of every packet.  NetBSD spins here for up to
 * 100 ms; nothing of the sort may happen inside an INT2 server on a 14 MHz
 * 68020, where 100 ms is five vertical blanks with interrupts held off.
 *
 * The count is register reads and not time, deliberately: a slower machine
 * takes longer per read and so waits longer in wall clock, which is the way
 * round that is wanted.  Running out is not fatal here -- the caller carries
 * on and the next interrupt finds the chip wherever it got to -- so this
 * returns the verdict rather than acting on it.
 */
#define EL3_CMD_SPINS   4000

static BOOL el3_wait_cmd(NetdevNic *nic)
{
    UWORD n = EL3_CMD_SPINS;

    while (n-- != 0)
    {
        if ((el3_status(nic) & EL3_S_CMD_BUSY) == 0)
            return TRUE;
    }

    EL_TRACE("el3: command stuck ", (ULONG)el3_status(nic));

    return FALSE;
}

/* ---------------------------------------------------------- detection ---- */

/*
 * IS AN ETHERLINK III DECODING HERE, AND WHICH WAY ROUND?
 *
 * Called from netdev_pcmcia.c with no NetdevNic in existence yet -- the slot
 * has to be identified before a unit can be built for it -- so this addresses
 * the card straight off the row and answers a plain yes or no.  el3_attach()
 * repeats the measurement into the unit; doing it twice costs two word reads
 * and means neither caller depends on the other having run.
 *
 * The global reset first, and it is the only command that can be issued
 * before the byte order is known: opcode 0 with a zero argument is the word
 * 0x0000, which is itself in either order.  It also leaves the part in window
 * 0, which is where the manufacturer ID is, and it is what makes this test
 * independent of whatever state a warm-booted card was left in.
 *
 * On this part the busy bit is not visible during a global reset, so there is
 * nothing to poll: the manual asks for a millisecond and the wait below is
 * register reads, which cost at least a bus cycle each and cannot be
 * optimised away.
 */
/*
 * TASK LEVEL ONLY, AND THE COUNT IS NOT ARBITRARY.
 *
 * A scalar register access to a PCMCIA card costs about 8.3 us on a 14 MHz
 * 68020 -- netdev_nic.h prices it, against 0.5 us for a word through a data
 * port -- and the reset wants a millisecond.  2048 reads is 17 ms at that
 * price and still a millisecond at the fastest access this bus could
 * plausibly manage, which is the bracket to be on the right side of when the
 * only alternative is a clock this driver deliberately does not open.
 *
 * NOTHING AT INTERRUPT LEVEL MAY CALL THIS.  el3_init() runs from the
 * vertical-blank watchdog under Disable(), so it uses the receive and
 * transmit resets instead, whose busy bit IS visible and whose wait is
 * therefore as short as the chip needs rather than as long as the worst case.
 */
static VOID el3_reset_wait(volatile UWORD *cmd)
{
    UWORD n = 2048;

    while (n-- != 0)
        (VOID)EL3_RAW_GET(cmd);
}

BOOL el3_answers(const NetdevCard *card)
{
    /* unsigned long, not ULONG: ULONG is 32 bits and test/test_netdev_el3.c
       compiles this file for a 64-bit host, where the narrower cast is a
       -Werror diagnostic. */
    volatile UWORD *base = (volatile UWORD *)(volatile void *)
                           (unsigned long)(card->base + card->reg_off);
    UWORD           id;

    EL3_RAW_PUT(&base[EL3_COMMAND / 2], EL3_CMD(EL3_C_RESET, 0)); /* 0 either way */
    el3_reset_wait(&base[EL3_COMMAND / 2]);

    id = EL3_RAW_GET(&base[EL3_W0_MFG_ID / 2]);

    return (BOOL)(id == EL3_MFG_ID || id == EL3_MFG_ID_SWAPPED);
}

/* ------------------------------------------------------------- EEPROM ---- */

/*
 * NOT BIT-BANGED.  One word says "read word N" and the answer is in the data
 * register once the busy bit clears.  A read is quoted at 162 us, so the
 * bound below is generous and this only ever runs from attach, at task level.
 */
static BOOL el3_eeprom(NetdevNic *nic, UBYTE word, UWORD *out)
{
    UWORD n = 20000;

    el3_window(nic, 0);
    el3_put(nic, EL3_W0_EEPROM_CMD,
            (UWORD)(EL3_EE_READ | (word & EL3_EE_ADDR_MASK)));

    while (n-- != 0)
    {
        if ((el3_get(nic, EL3_W0_EEPROM_CMD) & EL3_EE_BUSY) == 0)
        {
            *out = el3_get(nic, EL3_W0_EEPROM_DATA);
            return TRUE;
        }
    }

    EL_TRACE("el3: eeprom stuck ", (ULONG)word);

    return FALSE;
}

/*
 * Three EEPROM words into six octets, the earlier octet in each high half,
 * and keep them only if what comes out is a station address.
 *
 * THE GROUP-BIT REPAIR HAPPENS BEFORE THE VERDICT, not after it.  An address
 * with bit 0 of octet 0 set is not one the part can match its own unicast
 * frames against, and every frame sent with it carries a group source that
 * switches mislearn -- the other two cores clear it and count it, and so does
 * this one.  Testing usability first would send a merely-repairable address
 * to the fallback and hide the repair.
 *
 * Nothing is committed until the address is accepted, so a rejected first
 * attempt leaves neither the address nor the repair counter touched.
 */
static BOOL el3_take_addr(NetdevNic *nic, UBYTE word0)
{
    UBYTE addr[NETDEV_ADDR_LEN];
    BOOL  fixed = FALSE;
    UWORD i;

    for (i = 0; i < 3; i++)
    {
        UWORD w;

        if (!el3_eeprom(nic, (UBYTE)(word0 + i), &w))
            return FALSE;

        addr[i * 2]     = (UBYTE)(w >> 8);
        addr[i * 2 + 1] = (UBYTE)w;
    }

    if ((addr[0] & 1u) != 0)
    {
        addr[0] &= (UBYTE)~1u;
        fixed = TRUE;
    }

    if (!netdev_mac_usable(addr))
        return FALSE;

    for (i = 0; i < NETDEV_ADDR_LEN; i++)
        nic->factory[i] = addr[i];

    if (fixed)
        nic->mac_group_fix++;
    nic->mac_source = (UBYTE)(fixed ? ANXDIAG_MAC_PROM_FIXED
                                    : ANXDIAG_MAC_PROM);

    return TRUE;
}

/* --------------------------------------------------------------- stop ---- */

VOID el3_halt(NetdevNic *nic)
{
    el3_cmd(nic, EL3_C_SET_INTR_MASK, 0);
    el3_cmd(nic, EL3_C_RX_DISABLE, 0);
    el3_cmd(nic, EL3_C_TX_DISABLE, 0);

    /*
     * The interrupt latch is dropped last and by hand.  Gayle's INT2 is level
     * driven: a cause left standing in Status holds the line down after the
     * server has been removed, and nothing is then left to lift it.
     */
    el3_cmd(nic, EL3_C_ACK_INTR, EL3_S_INT_LATCH | EL3_S_TX_AVAIL |
                                 EL3_S_RX_EARLY | EL3_S_INT_REQ);

    nic->running   = FALSE;
    nic->txb_inuse = 0;
}

/* ------------------------------------------------------------- filter ---- */

/*
 * The hardware half.  Group is on whenever anything at all is joined, because
 * that is the only granularity the part has; the software half in
 * el3_rx_wanted() is what makes it behave like a hash.
 */
static BOOL el3_any_group(const NetdevNic *nic)
{
    UWORD i;

    for (i = 0; i < 8; i++)
    {
        if (nic->mar[i] != 0)
            return TRUE;
    }

    return FALSE;
}

VOID el3_setfilter(NetdevNic *nic)
{
    UWORD filter = EL3_FIL_INDIVIDUAL | EL3_FIL_BROADCAST;

    /*
     * GROUP ONLY WHEN SOMETHING IS JOINED.  It is all multicast or none on
     * this part, so leaving it on hands the CPU every group frame on the
     * segment to hash and throw away -- and on a 14 MHz 68020 with a 2 KB
     * receive FIFO that is not free.  An empty hash is exactly "no opener
     * wants a group address", which is the condition for turning it off.
     */
    if (el3_any_group(nic))
        filter |= EL3_FIL_GROUP;
    if (nic->promisc)
        filter |= EL3_FIL_PROMISC;

    el3_cmd(nic, EL3_C_SET_RX_FILTER, filter);
}

/*
 * Would the DP8390's filter have taken this frame?
 *
 * Unicast and broadcast are the chip's business and it has already made the
 * decision.  A group address is not: every one of them arrived, so the hash
 * the openers built has to be applied here.  Promiscuous takes everything, as
 * it does on every other core.
 *
 * netdev_mcaf.c is used exactly as it stands -- same CRC, same six bits, same
 * byte and bit -- so a group that reaches an opener through an Ariadne II
 * reaches it through this card and one that does not, does not.
 */
static BOOL el3_rx_wanted(const NetdevNic *nic, const UBYTE *frame)
{
    ULONG crc;
    UWORD idx;

    if (nic->promisc)
        return TRUE;
    if ((frame[0] & 1u) == 0)
        return TRUE;            /* unicast: the chip matched it */

    if (frame[0] == 0xff && frame[1] == 0xff && frame[2] == 0xff &&
        frame[3] == 0xff && frame[4] == 0xff && frame[5] == 0xff)
        return TRUE;            /* broadcast */

    crc = netdev_ether_crc32_be(frame, NETDEV_ADDR_LEN) >> 26;
    idx = (UWORD)(crc & 0x3fu);

    return (BOOL)((nic->mar[idx >> 3] & (UBYTE)(1u << (idx & 7u))) != 0);
}

/* --------------------------------------------------------------- init ---- */

/*
 * WHAT PCMCIA NEEDS THAT THE ISA CARDS DO NOT.
 *
 * On a 3C589 neither the address configuration nor the resource
 * configuration is loaded from the EEPROM -- the manual says so -- and the
 * part will not answer I/O at all while its I/O base field is non-zero.  So
 * both are written here, every time, and a global reset (which does not reset
 * the PCMCIA interface chip, only the ASIC behind it) means writing them
 * again.
 *
 * Zeroing the I/O base is also what makes the card row's 0x300 safe: with the
 * field zero the part decodes on the bottom four address lines only, so it
 * answers in every sixteen-byte block of the window Gayle maps and the exact
 * one this driver picked stops mattering.
 */
static VOID el3_pcmcia_setup(NetdevNic *nic)
{
    UWORD ac;

    el3_window(nic, 0);

    ac = el3_get(nic, EL3_W0_ADDR_CFG);
    el3_put(nic, EL3_W0_ADDR_CFG, (UWORD)(ac & (UWORD)~EL3_AC_ROM_IO_MASK));
    el3_put(nic, EL3_W0_RESOURCE_CFG, EL3_RC_PCMCIA);
}

/*
 * NO GLOBAL RESET HERE, AND THAT IS THE WHOLE REASON THIS IS SHAPED AS IT IS.
 *
 * A global reset on this part does not show its busy bit, so the only way to
 * wait one out is to burn a millisecond; el3_reset() calls this from the
 * vertical-blank watchdog, under Disable(), at INT3 above the card's own INT2
 * server.  A millisecond there is not payable.  The receive and transmit
 * resets do show the busy bit, they are what the transmitter actually needs
 * after an underrun or a jabber, and between them they put both FIFOs back
 * where a global reset would have -- so the global reset stays at attach,
 * which runs once, at task level.
 */
LONG el3_init(NetdevNic *nic)
{
    UWORD i;

    /* The window register is not readable as such and nothing here has
       touched it since whatever ran last: state it rather than assume it. */
    nic->el3_win = 0xff;

    el3_cmd(nic, EL3_C_RX_DISABLE, 0);
    el3_cmd(nic, EL3_C_TX_DISABLE, 0);
    el3_cmd(nic, EL3_C_SET_INTR_MASK, 0);

    el3_pcmcia_setup(nic);

    /* The station address, three words, low octet first. */
    el3_window(nic, 2);
    for (i = 0; i < 3; i++)
    {
        el3_put(nic, (UWORD)(EL3_W2_ADDR_0 + i * 2),
                (UWORD)(nic->mac[i * 2] | ((UWORD)nic->mac[i * 2 + 1] << 8)));
    }

    el3_cmd(nic, EL3_C_RX_RESET, 0);
    (VOID)el3_wait_cmd(nic);
    el3_cmd(nic, EL3_C_TX_RESET, 0);
    (VOID)el3_wait_cmd(nic);

    /*
     * The media the card was built with decides what to switch on.  The
     * coaxial transceiver's converter needs 800 us before it carries
     * anything, which is why it is started here and not on the first
     * transmit; link beat is only meaningful on twisted pair.
     */
    el3_window(nic, 4);
    if ((nic->el3_media & EL3_CC_UTP_PRESENT) != 0)
    {
        el3_put(nic, EL3_W4_MEDIA,
                (UWORD)(el3_get(nic, EL3_W4_MEDIA) | EL3_MEDIA_LINK_ENABLE |
                        EL3_MEDIA_JABBER_ENABLE));
    }

    el3_window(nic, 1);

    /*
     * The coaxial transceiver, on a card that has one and no twisted pair.
     * Its converter needs 800 us before it carries anything, and the chip's
     * own timer is what measures it: 3.2 us a tick, saturating at 255, which
     * is 816 us and is the method the manual names.  A hardware clock rather
     * than a spin count, because this can run from the watchdog.
     */
    if ((nic->el3_media & EL3_CC_BNC_PRESENT) != 0 &&
        (nic->el3_media & EL3_CC_UTP_PRESENT) == 0)
    {
        UWORD guard = 4000;

        el3_cmd(nic, EL3_C_COAX_START, 0);
        while (guard-- != 0 &&
               netdev_bus_r8(&nic->bus, EL3_W1_TIMER) != 0xff)
            ;
    }

    /*
     * Start transmitting once a whole frame is in the FIFO rather than
     * streaming it out under the CPU.  A 1514-byte frame is about 380 us of
     * programmed I/O on a 14 MHz 68020 and the wire would run dry in the
     * middle of it, which is an underrun and a chip that then needs a reset.
     */
    el3_cmd(nic, EL3_C_SET_TX_START, EL3_TX_LEN_MASK);
    el3_cmd(nic, EL3_C_SET_RX_EARLY, 0);        /* whole frames only */

    el3_cmd(nic, EL3_C_STATS_DISABLE, 0);

    /* Everything that was pending before this driver existed. */
    el3_cmd(nic, EL3_C_ACK_INTR, EL3_S_INT_LATCH | EL3_S_TX_AVAIL |
                                 EL3_S_RX_EARLY | EL3_S_INT_REQ);
    el3_drain_tx_status(nic);

    el3_setfilter(nic);
    el3_cmd(nic, EL3_C_RX_ENABLE, 0);
    el3_cmd(nic, EL3_C_TX_ENABLE, 0);

    /*
     * THE READ-ZERO MASK IS THE WAY ROUND ITS NAME DOES NOT SUGGEST: a CLEAR
     * bit is what forces a status bit to read as zero, so all ones is "report
     * everything" and zero would blind the interrupt handler to every cause
     * it exists to service.  The power-up default is zero, which is harmless
     * only because the interrupt mask is zero beside it.
     */
    el3_cmd(nic, EL3_C_SET_ZERO_MASK, 0x00ff);
    el3_cmd(nic, EL3_C_SET_INTR_MASK,
            EL3_S_ADAPTER_FAIL | EL3_S_TX_COMPLETE | EL3_S_TX_AVAIL |
            EL3_S_RX_COMPLETE);

    nic->running   = TRUE;
    nic->txb_inuse = 0;
    nic->txb_cnt   = 1;

    return 0;
}

/*
 * The watchdog's recovery, and this core needs a real one.
 *
 * el3_tx() sets txb_inuse when the FIFO could not take a frame and arms the
 * transmit-available threshold to hear about it; acknowledging that interrupt
 * disarms the threshold, so a lost or never-delivered TX Available leaves the
 * transmitter blocked with nothing left to unblock it.  That is exactly the
 * wedge netdev_tick() exists for.
 */
VOID el3_reset(NetdevNic *nic)
{
    nic->resets++;
    el3_halt(nic);
    (VOID)el3_init(nic);
}

/* ----------------------------------------------------------- transmit ---- */

/*
 * The transmit status stack, which is 31 deep and is popped by writing to it.
 *
 * Read the byte, write anything back, repeat until the read is zero.  It is a
 * byte at an odd offset and so goes through netdev_bus, which puts an odd
 * register in Gayle's second window -- the one place in this core where the
 * byte order does not arise, because a byte has none.
 */
VOID el3_drain_tx_status(NetdevNic *nic)
{
    UWORD guard = 32;

    while (guard-- != 0)
    {
        UBYTE st = netdev_bus_r8(&nic->bus, EL3_W1_TX_STATUS);

        if (st == 0)
            return;

        if ((st & (EL3_TXS_JABBER | EL3_TXS_UNDERRUN |
                   EL3_TXS_MAX_COLLISION)) != 0)
            nic->tx_errors++;
        if ((st & EL3_TXS_MAX_COLLISION) != 0)
            nic->collisions++;
        if ((st & EL3_TXS_UNDERRUN) != 0)
            nic->overruns++;

        /* Any value pops it; zero is the value, so that nothing here can be
           read as writing a bit back into a register that has none. */
        netdev_bus_w8(&nic->bus, EL3_W1_TX_STATUS, 0);

        /*
         * Any error bit disables the transmitter; jabber and underrun need
         * the transmitter reset before it can be enabled again.  Doing that
         * here rather than after the loop keeps one entry's recovery with
         * that entry, which matters because the next entry may be clean.
         */
        if ((st & EL3_TXS_FATAL) != 0)
        {
            el3_cmd(nic, EL3_C_TX_RESET, 0);
            (VOID)el3_wait_cmd(nic);
            el3_cmd(nic, EL3_C_TX_ENABLE, 0);
        }
        else if ((st & EL3_TXS_MAX_COLLISION) != 0)
        {
            el3_cmd(nic, EL3_C_TX_ENABLE, 0);
        }
    }
}

/* What one frame costs the FIFO: two preamble words and a dword-padded body. */
static UWORD el3_fifo_cost(UWORD len)
{
    return (UWORD)(4u + ((len + 3u) & (UWORD)~3u));
}

LONG el3_tx(NetdevNic *nic, const UBYTE *frame, UWORD len)
{
    UWORD need = el3_fifo_cost(len);
    UWORD pad;

    if (!nic->running)
        return DP8390_TX_OFFLINE;

    el3_window(nic, 1);

    if (el3_get(nic, EL3_W1_TX_FREE) < need)
    {
        /*
         * Not enough room.  Ask to be told when there is, and tell the shell
         * the transmitter is occupied so it stops pumping and requeues.  The
         * threshold is disarmed by its own acknowledgement, so it is set
         * again every time this path is taken and never once at init.
         */
        el3_cmd(nic, EL3_C_SET_TX_AVAIL, need);
        nic->txb_inuse = 1;

        return DP8390_TX_BUSY;
    }

    /*
     * Length counts the frame, not the padding.  Short frames are NOT padded
     * here: the card pads to the Ethernet minimum itself, and padding them
     * twice would put the pad inside the length.
     */
    el3_put(nic, EL3_W1_FIFO, (UWORD)(len & EL3_TX_LEN_MASK));
    el3_put(nic, EL3_W1_FIFO, 0);

    /*
     * The body, through netdev_bus's burst path: the same code, the same
     * window and the same absence of any swap as the NE2000 row's ring reads.
     * A byte count is rounded up to the transfer unit by the caller, which is
     * here, and the dword pad is what absorbs it.
     */
    netdev_bus_wdata(&nic->bus, frame, (UWORD)((len + 1u) & (UWORD)~1u));

    /* The rest of the pad to the dword boundary, if any. */
    for (pad = (UWORD)((len + 1u) & (UWORD)~1u); pad < (need - 4u); pad += 2)
        el3_put(nic, EL3_W1_FIFO, 0);

    nic->tx_packets++;

    return 0;
}

/* ------------------------------------------------------------- receive --- */

/*
 * One frame out of the receive FIFO, or nothing.
 *
 * Discard is issued for EVERY frame, good or bad, and it is the only thing
 * that advances the FIFO past the dword padding -- so the error paths below
 * fall through to it rather than returning.  Reading past the pad is an
 * underrun and an adapter failure, which is why the read is rounded up to a
 * word and no further.
 */
static BOOL el3_rint(NetdevNic *nic)
{
    UWORD status = el3_get(nic, EL3_W1_RX_STATUS);
    UWORD len;

    if ((status & EL3_RXS_INCOMPLETE) != 0)
        return FALSE;

    len = (UWORD)(status & EL3_RXS_LEN_MASK);

    if ((status & EL3_RXS_ERROR) != 0)
    {
        nic->rx_errors++;
        if (((status & EL3_RXS_ERR_MASK) >> EL3_RXS_ERR_SHIFT) ==
            EL3_RXE_OVERRUN)
            nic->overruns++;
    }
    else if (len < NETDEV_HDR_LEN || len > NETDEV_RXBUF_MAX)
    {
        nic->rx_errors++;
    }
    else
    {
        UBYTE *buf = (UBYTE *)nic->rxbuf;

        netdev_bus_rdata(&nic->bus, buf, (UWORD)((len + 1u) & (UWORD)~1u));

        if (el3_rx_wanted(nic, buf))
        {
            nic->rx_packets++;
            nic->rx(nic->rx_arg, buf, len);
        }
    }

    el3_cmd(nic, EL3_C_RX_DISCARD, 0);
    (VOID)el3_wait_cmd(nic);

    return TRUE;
}

/* ----------------------------------------------------------- interrupt --- */

BOOL el3_intr(NetdevNic *nic)
{
    UWORD rounds = NETDEV_DRAIN_MAX;
    UWORD status;
    BOOL  mine = FALSE;

    if (!nic->running)
        return FALSE;

    el3_window(nic, 1);

    while (rounds-- != 0)
    {
        status = el3_status(nic);

        if ((status & EL3_S_INTS) == 0)
            break;

        mine = TRUE;

        if ((status & EL3_S_RX_COMPLETE) != 0)
        {
            /* Acknowledging receive-complete does nothing on this part; the
               frame has to be taken out and discarded, and the bit follows. */
            if (!el3_rint(nic))
                break;
            continue;
        }

        if ((status & EL3_S_TX_COMPLETE) != 0)
        {
            /* Likewise: the stack has to be popped, not acknowledged. */
            el3_drain_tx_status(nic);
            continue;
        }

        if ((status & EL3_S_TX_AVAIL) != 0)
        {
            /* This one IS cleared by its acknowledgement, and the same
               acknowledgement disarms the threshold behind it. */
            el3_cmd(nic, EL3_C_ACK_INTR, EL3_S_TX_AVAIL);
            nic->txb_inuse = 0;
            continue;
        }

        if ((status & EL3_S_ADAPTER_FAIL) != 0)
        {
            /*
             * A FIFO over- or underran and the bit behind it lives in a
             * diagnostic register in another window.  Nothing narrower than
             * a restart clears every case, and this is rare enough that
             * paying for the restart is cheaper than getting the cases wrong.
             */
            nic->rx_errors++;
            el3_reset(nic);
            return TRUE;
        }

        /* Anything else that can raise the line and that this driver did not
           ask for: acknowledge it so the level-driven INT2 lifts. */
        el3_cmd(nic, EL3_C_ACK_INTR,
                (UWORD)(status & (EL3_S_RX_EARLY | EL3_S_INT_REQ |
                                  EL3_S_UPD_STATS)));
    }

    if (mine)
        el3_cmd(nic, EL3_C_ACK_INTR, EL3_S_INT_LATCH);

    return mine;
}

/* -------------------------------------------------------------- attach --- */

LONG el3_attach(NetdevNic *nic)
{
    UWORD id;

    /*
     * The reset is repeated here rather than assumed from the claim: attach
     * runs for the fixed-address rows too, where nothing claimed anything,
     * and a core that only works after some other file ran is a core with an
     * undocumented precondition.
     */
    nic->el3_swap = 0;
    nic->el3_win  = 0xff;

    EL3_RAW_PUT(el3_at(nic, EL3_COMMAND), EL3_CMD(EL3_C_RESET, 0));
    el3_reset_wait(el3_at(nic, EL3_COMMAND));

    /* Window 0 after a reset, which is where the manufacturer ID is. */
    nic->el3_win = 0;

    id = EL3_RAW_GET(el3_at(nic, EL3_W0_MFG_ID));
    netdev_diag_note(ANXDIAG_EL3_MFG, netdev_diag_card(nic->card), (ULONG)id);

    if (id == EL3_MFG_ID)
    {
        nic->el3_swap = 0;
    }
    else if (id == EL3_MFG_ID_SWAPPED)
    {
        nic->el3_swap = 1;
    }
    else
    {
        nic->diag_why = (UBYTE)ANXDIAG_WHY_MFGID;
        return -1;
    }

    netdev_diag_note(ANXDIAG_EL3_ORDER, netdev_diag_card(nic->card),
                     (ULONG)nic->el3_swap);
    EL_TRACE("el3: swap ", (ULONG)nic->el3_swap);

    el3_pcmcia_setup(nic);

    /* Which transceivers the card was built with.  Read-only, and the only
       thing that decides what el3_init() switches on. */
    nic->el3_media = (UWORD)(el3_get(nic, EL3_W0_CONFIG_CTRL) &
                             EL3_CC_MEDIA_MASK);
    netdev_diag_note(ANXDIAG_EL3_MEDIA, netdev_diag_card(nic->card),
                     (ULONG)nic->el3_media);

    /*
     * THE STATION ADDRESS, AND THERE ARE TWO OF THEM IN THIS EEPROM.
     *
     * Words 0..2 are 3Com's own node address and words 10..12 are the OEM
     * one, and the manual does not say which a driver should use.  The answer
     * is a fact about the cards rather than about the part: 3c589.device, the
     * AmigaOS driver that has been in front of these cards since 2000, takes
     * the OEM address, and a rebadged card -- which is most of them -- is
     * exactly the case where the two differ.
     *
     * Taken in that order, then, with 3Com's own as the fallback rather than
     * as the choice: a card that has one and not the other still comes up.
     *
     * Each word holds two octets, the earlier one in the high half.
     */
    if (!el3_take_addr(nic, EL3_EE_OEM_ADDR_0) &&
        !el3_take_addr(nic, EL3_EE_NODE_ADDR_0))
    {
        nic->diag_why = (UBYTE)ANXDIAG_WHY_ADDRESS;
        return -1;
    }

    /*
     * There is no ring and no packet buffer to describe: the FIFOs have no
     * addresses.  txb_cnt is 1 because one frame is either in the FIFO or it
     * is not, and the other buffer fields stay zero so that anything reading
     * them as a ring reads an empty one.
     */
    nic->txb_cnt   = 1;
    nic->txb_inuse = 0;
    nic->read_hdr  = NULL;
    nic->ring_copy = NULL;
    nic->frame_at  = NULL;
    nic->tx_at     = NULL;
    nic->write_buf = NULL;

    return 0;
}

const struct NetdevNicOps netdev_nic_el3 =
{
    el3_attach,
    el3_init,
    el3_halt,
    el3_tx,
    el3_setfilter,
    el3_intr,
    el3_reset
};
