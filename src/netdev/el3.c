/*
 * anxnet.device: the 3Com EtherLink III, for the 3C589 in the PCMCIA slot.
 *
 * A third chip core, sharing nothing below the SANA-II shell with either of
 * the other two.  Against the two that were already here:
 *
 *                     DP8390 family        LANCE            EtherLink III
 *   registers         16, paged banks      RAP/RDP pair     14, windowed
 *   who moves data    the CPU              the chip, DMA    the CPU
 *   buffers           a page ring on the   descriptor       two FIFOs, no
 *                     card                 rings in SRAM    addresses at all
 *   frame boundary    a 4-byte header      a descriptor     a length preamble
 *                     the chip writes      the CPU wrote    the CPU writes
 *   multicast         a 64-bit hash in     a 64-bit hash    none.  Four
 *                     the chip             in the init      filter bits and
 *                                          block            no hash at all
 *
 * The card is sixteen bytes of I/O space.  Offset 0x0e is Command when written
 * and Status when read, in every window.  The fourteen bytes under it are one
 * of eight overlays, chosen by a command and read back in the top three bits
 * of Status.  There is no window register.  Window 1 is where the driver lives
 * once it is running, and every excursion out of it comes back.  The filter is
 * a command, so it is not an excursion.
 *
 * Multicast is done in software.  Set RX Filter has four bits, for individual,
 * group, broadcast and promiscuous, and the part has no per-group filtering.
 * The hardware is told to accept every group address, and el3_rx_wanted() then
 * tests each multicast destination against nic->mar[].  That is the same
 * 64-bit hash netdev_mcaf.c builds for the other three cores, and the same
 * netdev_ether_crc32_be() that fills it.  The seam above this file does not
 * change and cannot tell the difference.
 *
 * Byte order is measured, not configured.  This is the first 16-bit register
 * access to the PCMCIA slot in this driver, and Gayle's 0xA20000 window is
 * documented two ways: cnet.device's own comments say the register window is
 * swapped, and Amiberry decodes it 1:1 and would accept either.  netdev_bus.h
 * names that trap.  The window 0 manufacturer ID is a hard-wired 0x6d50, so
 * el3_answers() reads it and accepts 0x6d50 or 0x506d, and everything after
 * that goes through accessors that consult the flag.  Nothing here is a card
 * table knob.
 *
 * Frame data is not swapped.  The FIFO is a byte stream through one address,
 * and the halves of a word off the wire arrive in the order they arrived in.
 * It goes through netdev_bus's burst path, which is the same code and the same
 * window the NE2000 row already reads its ring through.
 *
 * Nothing emulates this card.  Amiberry's PCMCIA support is NE2000 only, so
 * everything below has been driven by the host test and by nothing else.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>     /* uintptr_t, for the fingerprint salt below */

#include "netdev_nic.h"
#include "n68k_iocopy.h"
#include "netdev_cards.h"
#include "netdev_mcaf.h"
#include "netdev_macgen.h"

#ifndef EL3_RX_DRAIN_SUM
#define EL3_RX_DRAIN_SUM(dst, port, len) \
    n68k_port_in_w_sum((dst), (port), (len))
#endif
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
 * Not netdev_bus's accessors: those are byte-wide, every register here but one
 * is a word, and half of a word register is not a register.  lance.c reaches
 * its RAP and RDP directly for the same reason.  The one exception is the
 * transmit status byte, which is at an odd offset and goes through the bus
 * layer so that it lands in Gayle's odd window.
 */
/*
 * The raw word access, and the one seam this file has for the host test.
 *
 * A window is not a memory.  A write to the command register changes what the
 * next read of offset 0x04 means, and a test that models that must see the
 * access happen rather than inspect an array afterwards.  Every other core is
 * testable against a plain buffer, because its registers are a buffer.
 * test/test_netdev_el3.c defines these two and includes this file.  Otherwise
 * they are a plain load and store, and the shipping driver compiles as if they
 * were not here.
 */
#ifndef EL3_RAW_GET
#define EL3_RAW_GET(p)      (*(p))
#define EL3_RAW_PUT(p, v)   (*(p) = (v))
#endif
#ifndef EL3_SETTLE_READ
#define EL3_SETTLE_READ(p)  (*(p))
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
 * Select a window, and remember which one.
 *
 * The bookkeeping is not an optimisation.  A register access in the wrong
 * window is not an error the chip reports, because it reads whatever the other
 * window has there.  The only defence is that exactly one place changes the
 * window and every reader knows what it left behind.  The saved window is what
 * that place wrote, and it is invalidated wherever the chip can have changed
 * it without being asked, which is a reset.
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
 * Bounded and short, because this runs at interrupt level.  Receive discard is
 * a multi-cycle command and the receive drain issues one per frame, so this is
 * on the interrupt path of every packet.  NetBSD spins here for up to 100 ms.
 * That cannot happen inside an INT2 server on a 14 MHz 68020, where 100 ms is
 * five vertical blanks with interrupts held off.
 *
 * The count is register reads and not time.  A slower machine takes longer per
 * read and so waits longer in wall clock, which is the wanted behaviour.  An
 * exhausted count is not fatal: the caller carries on and the next interrupt
 * finds the chip wherever it got to.  This returns the verdict rather than
 * acting on it.
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
 * Is an EtherLink III decoding here, and which way round?
 *
 * Called from netdev_pcmcia.c with no NetdevNic in existence yet, because the
 * slot must be identified before a unit can be built for it.  This addresses
 * the card straight off the row and answers a plain yes or no.  el3_attach()
 * repeats the measurement into the unit.  Twice costs two word reads and means
 * neither caller depends on the other having run.
 *
 * The global reset comes first, and it is the only command that can be issued
 * before the byte order is known: opcode 0 with a zero argument is the word
 * 0x0000, which is itself in either order.  It also leaves the part in window
 * 0, where the manufacturer ID is, and it makes this test independent of
 * whatever state a warm-booted card was left in.
 *
 * On this part the busy bit is not visible during a global reset, so there is
 * nothing to poll.  The manual asks for a millisecond, and the wait below is
 * register reads, which cost at least a bus cycle each and cannot be optimised
 * away.
 */
/*
 * Task level only, and the count is not arbitrary.
 *
 * A scalar register access to a PCMCIA card costs about 8.3 us on a 14 MHz
 * 68020, against 0.5 us for a word through a data port, and netdev_nic.h
 * prices both.  The reset wants a millisecond.  2048 reads is 17 ms at that
 * price, and still a millisecond at the fastest access this bus can manage,
 * which brackets the requirement without a clock this driver does not open.
 *
 * Nothing at interrupt level can call this.  el3_init() runs from the
 * vertical-blank watchdog under Disable(), so it uses the receive and transmit
 * resets instead.  Their busy bit is visible, so their wait is as short as the
 * chip needs rather than as long as the worst case.
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
 * Not bit-banged.  One word says "read word N", and the answer is in the data
 * register once the busy bit clears.  A read is quoted at 162 us, so the bound
 * below is generous.  This runs only from attach, at task level.
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
 * The group-bit repair happens before the verdict, not after it.  The part
 * cannot match its own unicast frames against an address with bit 0 of octet 0
 * set, and every frame sent with it carries a group source that switches
 * mislearn.  ne2000.c clears it and counts it and so does this one; ed.c
 * refuses the address outright, and lance.c derives one from a serial number
 * and cannot produce a set group bit at all.
 * A usability test first would send a repairable address to the fallback and
 * hide the repair.
 *
 * Nothing is committed until the address is accepted, so a rejected first
 * attempt leaves neither the address nor the repair counter touched.
 */
/*
 * One EEPROM word, read twice and only believed when both reads agree.
 *
 * On the real 3c589 the EEPROM answers correctly once the card is warm, but
 * reads made straight after the attach-time reset race whatever the part is
 * still doing internally: the data register then returns stale or lagging
 * words that LOOK like an address (measured 2026-08-22 -- two boots adopted
 * two different garbage stations, one of them the true address shifted by a
 * word, and the DHCP identity changed with every boot).  A single read
 * cannot see that; two agreeing reads of moving data can only collide while
 * the data has stopped moving.
 */
static BOOL el3_eeprom_stable(NetdevNic *nic, UBYTE word, UWORD *out)
{
    UWORD a, b;

    if (!el3_eeprom(nic, word, &a) || !el3_eeprom(nic, word, &b))
        return FALSE;
    if (a != b)
        return FALSE;

    *out = a;
    return TRUE;
}

/*
 * The EEPROM carries its own truth marker: word 7 is the manufacturer ID,
 * 0x6d50 on every EtherLink III.  Attach polls it until two consecutive
 * reads both say so, which is the moment the part's post-reset internals
 * have settled and the address words can be believed.  The spin between
 * attempts is the pc_settle() shape: attribute-memory reads, each a real
 * Gayle cycle, because there is no timer at attach time.
 */
static BOOL el3_eeprom_ready(NetdevNic *nic)
{
    UWORD tries;

    for (tries = 0; tries < 8; tries++)
    {
        UWORD id;

        if (el3_eeprom_stable(nic, EL3_EE_MFG_ID, &id) &&
            id == EL3_MFG_ID)
            return TRUE;

        {
            volatile UBYTE *attr = (volatile UBYTE *)0x00a00000UL;
            ULONG           n    = 2000UL * 4UL;   /* ~2 ms */

            while (n-- != 0)
                (VOID)EL3_SETTLE_READ(attr);
        }
    }

    netdev_diag_note(ANXDIAG_EL3_MFG, netdev_diag_card(nic->card), 0xEEEEUL);
    return FALSE;
}

static BOOL el3_take_addr(NetdevNic *nic, UBYTE word0)
{
    UBYTE addr[NETDEV_ADDR_LEN];
    BOOL  fixed = FALSE;
    UWORD i;

    for (i = 0; i < 3; i++)
    {
        UWORD w;

        if (!el3_eeprom_stable(nic, (UBYTE)(word0 + i), &w))
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
 * The hardware half.  Group is on whenever anything is joined, because that is
 * the only granularity the part has.  The software half in el3_rx_wanted()
 * makes it behave like a hash.
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
     * Group only when something is joined.  It is all multicast or none on
     * this part, so a group bit left on hands the CPU every group frame on the
     * segment to hash and throw away.  On a 14 MHz 68020 with a 2 KB receive
     * FIFO that is not free.  An empty hash means no opener wants a group
     * address, which is the condition for turning it off.
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
 * netdev_mcaf.c is used exactly as it stands, with the same CRC, the same six
 * bits, and the same byte and bit.  A group address therefore reaches an
 * opener through this card whenever it reaches one through an Ariadne II.
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
 * What PCMCIA needs that the ISA cards do not.
 *
 * On a 3C589 the manual says that neither the address configuration nor the
 * resource configuration is loaded from the EEPROM, and the part does not
 * answer I/O while its I/O base field is non-zero.  Both are therefore written
 * here every time.  A global reset resets the ASIC behind the PCMCIA interface
 * chip and not the interface chip itself, so both must be written again.
 *
 * A zeroed I/O base also makes the card row's 0x300 safe.  With the field zero
 * the part decodes on the bottom four address lines only, so it answers in
 * every sixteen-byte block of the window Gayle maps, and the exact one this
 * driver picked stops mattering.
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
 * No global reset here, which is why this function is shaped as it is.
 *
 * A global reset on this part does not show its busy bit, so the only way to
 * wait one out is to burn a millisecond.  el3_reset() calls this from the
 * vertical-blank watchdog, under Disable(), at INT3 above the card's own INT2
 * server.  A millisecond there is not payable.  The receive and transmit
 * resets do show the busy bit, they are what the transmitter needs after an
 * underrun or a jabber, and between them they put both FIFOs back where a
 * global reset would have.  The global reset stays at attach, which runs once,
 * at task level.
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

    /*
     * Activate the board.  Linux does this unconditionally and first
     * (3c589_cs.c: outw(0x0001, ioaddr + 4)), and on a PC the card's
     * power-up state makes it redundant.  On an A1200 nothing ever asserts
     * CC_RESET to the slot -- Commodore's unfixed Gayle bug -- so the card
     * wakes with whatever this bit last was, and it decides per boot whether
     * the receiver hears the wire: measured on real hardware, 2026-08-22,
     * transmit up and DHCP offers vanishing, CONFIG_CTRL one bit short of a
     * boot that worked.  Read-modify-write, keeping the media-present bits
     * the attach probe reads.
     */
    el3_window(nic, 0);
    el3_put(nic, EL3_W0_CONFIG_CTRL,
            (UWORD)(el3_get(nic, EL3_W0_CONFIG_CTRL) | EL3_CC_ACTIVATE));

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
     * transmit.  Link beat is only meaningful on twisted pair.
     */
    /*
     * What the drivers that have worked on this card for decades write, in
     * the order they write it -- 3c589.device (Cafferkey, 2000) and Linux's
     * 3c589_cs agree on all three, and this driver's own sequence differed
     * on each:
     *
     *   The resource configuration is written, 0x3f00, not inherited.  This
     *   card wakes with 0x3000 on the machine that found all of this.
     *
     *   The transceiver is selected by writing the address-configuration
     *   register, not assumed from its power-up value.
     *
     *   The media register is written ABSOLUTELY, enable bits only.  The
     *   register's high bits are status, and a read-modify-write hands them
     *   back as control: on a board the machine never resets -- Gayle
     *   asserts no CC_RESET, ever -- whatever undefined state those bits
     *   power up in was being lovingly preserved across every
     *   reinitialisation this driver performed.
     */
    el3_window(nic, 0);
    el3_put(nic, EL3_W0_RESOURCE_CFG, EL3_RC_PCMCIA);
    if ((nic->el3_media & EL3_CC_UTP_PRESENT) != 0)
        el3_put(nic, EL3_W0_ADDR_CFG, EL3_AC_XCVR_UTP);

    el3_window(nic, 4);
    if ((nic->el3_media & EL3_CC_UTP_PRESENT) != 0)
    {
        el3_put(nic, EL3_W4_MEDIA,
                (UWORD)(EL3_MEDIA_LINK_ENABLE | EL3_MEDIA_JABBER_ENABLE));
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
     * The read-zero mask runs the opposite way to its name.  A clear bit is
     * what forces a status bit to read as zero, so all ones reports everything
     * and zero blinds the interrupt handler to every cause it services.  The
     * power-up default is zero, which is harmless only because the interrupt
     * mask is zero beside it.
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
 * el3_tx() sets txb_inuse when the FIFO could not take a frame, and arms the
 * transmit-available threshold to hear about it.  That interrupt's
 * acknowledgement disarms the threshold, so a lost or never-delivered TX
 * Available leaves the transmitter blocked with nothing left to unblock it.
 * That is the wedge netdev_tick() exists for.
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

        /* Any value pops it.  Zero is the value, so that nothing here reads
           as a bit written back into a register that has none. */
        netdev_bus_w8(&nic->bus, EL3_W1_TX_STATUS, 0);
        nic->tx_completed++;

        /*
         * Any error bit disables the transmitter.  Jabber and underrun need
         * the transmitter reset before it can be enabled again.  The recovery
         * runs here rather than after the loop, so that it stays with its own
         * entry, because the next entry can be clean.
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
     * Length counts the frame, not the padding.  Short frames are not padded
     * here: the card pads to the Ethernet minimum itself, and a second pad
     * would fall inside the length.
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
 * Discard is issued for every frame, good or bad, and it is the only thing
 * that advances the FIFO past the dword padding, so the error paths below
 * fall through to it rather than returning.  A read past the pad is an
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

        /*
         * The header first.  The filter and the direct-receive claim both
         * decide from it, and it is the same seven words off the FIFO
         * whichever path the rest of the frame takes.  A claimed frame is
         * drained straight into the stack's packet -- one copy where there
         * were two, and the second one was the larger half of what this
         * interrupt used to cost (flight 4, 2026-08-22: the staging copy
         * plus the hook copy were ~1.4 ms of a ~2 ms frame).  A declined
         * claim reassembles the staging buffer exactly as before: the header
         * is already at its start, and the payload lands behind it.
         */
        netdev_bus_rdata(&nic->bus, buf, NETDEV_HDR_LEN);

        if (el3_rx_wanted(nic, buf))
        {
            APTR   token = NULL;
            UBYTE *dst   = (nic->rx_claim != NULL)
                         ? nic->rx_claim(nic->rx_arg, buf, len, &token)
                         : NULL;

            if (dst != NULL)
            {
                /*
                 * The fused drain: the FIFO reads pay for the checksum, and
                 * the verifier gets the sum the copy hook would have made,
                 * so nothing anywhere walks these bytes again.  The slot's
                 * payload pointer is longword aligned by construction, which
                 * is the routine's one requirement.
                 */
                ULONG sum = EL3_RX_DRAIN_SUM(dst,
                                             (const volatile void *)
                                                 nic->bus.asic,
                                             (ULONG)(len - NETDEV_HDR_LEN));

                nic->rx_packets++;
                nic->rx_claimed(nic->rx_arg, token, sum, 1);
            }
            else
            {
                netdev_bus_rdata(&nic->bus, buf + NETDEV_HDR_LEN,
                                 (UWORD)(len - NETDEV_HDR_LEN));
                nic->rx_packets++;
                nic->rx(nic->rx_arg, buf, len);
            }
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
            /* An acknowledgement of receive-complete does nothing on this
               part.  The frame must be taken out and discarded, and the bit
               follows. */
            if (!el3_rint(nic))
                break;
            continue;
        }

        if ((status & EL3_S_TX_COMPLETE) != 0)
        {
            /* Likewise: the stack must be popped, not acknowledged. */
            el3_drain_tx_status(nic);
            continue;
        }

        if ((status & EL3_S_TX_AVAIL) != 0)
        {
            /* This one is cleared by its acknowledgement, and the same
               acknowledgement disarms the threshold behind it. */
            el3_cmd(nic, EL3_C_ACK_INTR, EL3_S_TX_AVAIL);
            nic->txb_inuse = 0;
            continue;
        }

        if ((status & EL3_S_ADAPTER_FAIL) != 0)
        {
            /*
             * A FIFO over- or underran, and the bit behind it lives in a
             * diagnostic register in another window.  Nothing narrower than a
             * restart clears every case, and the condition is rare, so the
             * restart is the cheaper answer.
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
     * The reset is repeated here rather than assumed from the claim.  Attach
     * runs for the fixed-address rows too, where nothing claimed anything, and
     * a core that works only after some other file ran carries an undocumented
     * precondition.
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
     * There are two station addresses in this EEPROM.
     *
     * Words 0..2 are 3Com's own node address and words 10..12 are the OEM one,
     * and the manual does not say which a driver must use.  The answer is a
     * fact about the cards rather than about the part.  3c589.device, the
     * AmigaOS driver that has been in front of these cards since 2000, takes
     * the OEM address, and a rebadged card is where the two differ.  Most of
     * these cards are rebadged.
     *
     * They are taken in that order, with 3Com's own as the fallback, so a card
     * that has one and not the other still comes up.
     *
     * Each word holds two octets, the earlier one in the high half.
     */
    if (!el3_eeprom_ready(nic) ||
        (!el3_take_addr(nic, EL3_EE_OEM_ADDR_0) &&
         !el3_take_addr(nic, EL3_EE_NODE_ADDR_0)))
    {
        /*
         * The EEPROM did not validate, so nothing it says can be believed --
         * and an address invented from whatever the data register floats to
         * is worse than none: it changes per boot, and a DHCP server that
         * sees a new MAC on every boot hands out a new lease each time and
         * eventually throttles the port (measured on the real 3c589,
         * 2026-08-22, two boots, two different garbage stations).  Derive
         * the address from the machine fingerprint instead, the way
         * ne2000.c does when a card has no PROM: stable across boots, and
         * the card still comes up.
         */
        UBYTE fp[NETDEV_MAC_FP_MAX];
        UWORD n;
        /* uintptr_t, not APTR: on m68k both are 32 bits and (ULONG)(APTR)
           is silent, but the host tier compiles this file for a 64-bit
           pointer and -Werror rejects the narrowing. The salt only needs the
           low half to differ between boards, so the truncation is deliberate
           and now says so. */
        ULONG salt = ((ULONG)nic->card->manid << 16) ^
                     (ULONG)nic->card->prodid ^
                     (ULONG)(uintptr_t)nic->board;

        n = netdev_mac_fingerprint(fp, (UWORD)sizeof(fp), salt);
        netdev_mac_derive(fp, n, nic->factory);
        nic->mac_derived++;
        nic->mac_source = (UBYTE)ANXDIAG_MAC_DERIVED;
    }

    /*
     * There is no ring and no packet buffer to describe: the FIFOs have no
     * addresses.  txb_cnt is 1 because one frame is either in the FIFO or it
     * is not, and the other buffer fields stay zero so that anything reading
     * them as a ring reads an empty one.
     */
    /*
     * The burst path does not point where netdev_bus_setup() put it.
     * bus->asic is the address netdev_bus_rdata/wdata hammer, and setup
     * derives it as the register base plus sixteen strides.  That is true for
     * a DP8390, whose data port is ASIC register 0 past the NIC block, and
     * false here.  This part's FIFO is window 1 offset 0, which is the
     * register base itself.  Left uncorrected, every frame goes into and out
     * of whatever decodes sixteen bytes further on, and nothing in the
     * register path looks wrong.
     *
     * netdev_bus_regmap() with no map is the seam for this: it says the data
     * port is here without claiming the register file is scattered.
     */
    netdev_bus_regmap(&nic->bus, NULL,
                      (APTR)(volatile void *)(nic->board + nic->card->reg_off +
                                              EL3_W1_FIFO));

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
