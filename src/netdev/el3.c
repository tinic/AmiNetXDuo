/*
 * anxnet.device: the 3Com EtherLink III, for the 3C589 in the PCMCIA slot.
 *
 * Offset 0x0e is Command when written and Status when read in every window; the
 * fourteen bytes under it are one of eight overlays.  Byte order is measured,
 * not configured (el3_answers()); frame data through the FIFO is never swapped.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>     /* uintptr_t, for the fingerprint salt below */

#include "netdev_nic.h"
#include "n68k_iocopy.h"
#include "netdev_cards.h"
#include "netdev_clock.h"
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
 * A word register, through the swap the probe measured.  Not netdev_bus's
 * accessors: those are byte-wide and half of a word register is not a register.
 * The transmit status byte is the exception -- an odd offset, so it goes
 * through the bus layer to land in Gayle's odd window.
 */
/*
 * The raw word access, and the one seam this file has for the host test.
 * test/test_netdev_el3.c defines these two and includes this file; otherwise
 * they are a plain load and store.
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
 * Select a window, and remember which one.  A register access in the wrong
 * window is not an error the chip reports, so exactly one place changes the
 * window; the saved value is invalidated wherever a reset can change it.
 */
static VOID el3_window(NetdevNic *nic, UBYTE win)
{
    if (nic->el3_win == win)
        return;

    el3_cmd(nic, EL3_C_WINDOW, win);
    nic->el3_win = win;
}

/*
 * Wait out a multi-cycle command.  The receive and transmit resets are the long
 * ones, EL3_RESET_US in the part's own terms; measured, with the old 4000
 * register reads kept as the floor.  An expired bound is not fatal.
 */
/* Interrupt level, so the clock must already be measured: el3_answers() arms a
   timed wait at task level before any card of this family is ever attached. */
#define EL3_CMD_WAIT_US EL3_RESET_US
#define EL3_CMD_SPINS   4000u

static BOOL el3_wait_cmd(NetdevNic *nic)
{
    NetdevWait w;

    netdev_wait_begin(&w, EL3_CMD_WAIT_US, EL3_CMD_SPINS);

    do
    {
        if ((el3_status(nic) & EL3_S_CMD_BUSY) == 0)
            return TRUE;
    }
    while (!netdev_wait_done(&w));

    EL_TRACE("el3: command stuck ", (ULONG)el3_status(nic));

    return FALSE;
}

/* ---------------------------------------------------------- detection ---- */

/*
 * Is an EtherLink III decoding here, and which way round?  Global reset first:
 * opcode 0 with a zero argument is the word 0x0000, itself in either order, and
 * it leaves the part in window 0 where the manufacturer ID is.  The busy bit is
 * not visible during a global reset, so there is nothing to poll.
 */
/*
 * Task level only.  20 ms of measured time, with the 2048 register reads kept
 * as the floor.  el3_init() runs from the vertical-blank watchdog under
 * Disable() and uses the receive and transmit resets, whose busy bit is visible.
 */
#define EL3_RESET_WAIT_US   (20u * EL3_RESET_US)
#define EL3_RESET_SPINS     2048u

static VOID el3_reset_wait(volatile UWORD *cmd)
{
    NetdevWait w;

    netdev_wait_begin(&w, EL3_RESET_WAIT_US, EL3_RESET_SPINS);

    do
        (VOID)EL3_RAW_GET(cmd);
    while (!netdev_wait_done(&w));
}

BOOL el3_answers(ULONG regs)
{
    /* unsigned long, not ULONG: ULONG is 32 bits and test/test_netdev_el3.c
       compiles this file for a 64-bit host, where the narrower cast is a
       -Werror diagnostic. */
    volatile UWORD *base = (volatile UWORD *)(volatile void *)
                           (unsigned long)regs;
    UWORD           id;

    EL3_RAW_PUT(&base[EL3_COMMAND / 2], EL3_CMD(EL3_C_RESET, 0)); /* 0 either way */
    el3_reset_wait(&base[EL3_COMMAND / 2]);

    id = EL3_RAW_GET(&base[EL3_W0_MFG_ID / 2]);

    return (BOOL)(id == EL3_MFG_ID || id == EL3_MFG_ID_SWAPPED);
}

/* ------------------------------------------------------------- EEPROM ---- */

/*
 * Not bit-banged: one word says "read word N" and the answer is in the data
 * register once the busy bit clears.  Attach only, at task level.
 */
/* 162 us in the EtherLink III reference and demonstrably longer on some parts,
   so 2 ms measured, with the old 20000 register reads kept as the floor. */
#define EL3_EEPROM_WAIT_US  (2u * EL3_RESET_US)
#define EL3_EEPROM_SPINS    20000u

static BOOL el3_eeprom(NetdevNic *nic, UBYTE word, UWORD *out)
{
    NetdevWait w;

    el3_window(nic, 0);
    el3_put(nic, EL3_W0_EEPROM_CMD,
            (UWORD)(EL3_EE_READ | (word & EL3_EE_ADDR_MASK)));

    netdev_wait_begin(&w, EL3_EEPROM_WAIT_US, EL3_EEPROM_SPINS);

    do
    {
        if ((el3_get(nic, EL3_W0_EEPROM_CMD) & EL3_EE_BUSY) == 0)
        {
            *out = el3_get(nic, EL3_W0_EEPROM_DATA);
            return TRUE;
        }
    }
    while (!netdev_wait_done(&w));

    EL_TRACE("el3: eeprom stuck ", (ULONG)word);

    return FALSE;
}

/*
 * Three EEPROM words into six octets, the earlier octet in each high half.
 * The group-bit repair happens before the verdict: the part cannot match its
 * own unicast frames against an address with bit 0 of octet 0 set.
 */
/*
 * One EEPROM word, read twice and only believed when both reads agree: reads
 * made straight after the attach-time reset return stale or lagging words that
 * look like an address.
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
 * Word 7 is the manufacturer ID, 0x6d50 on every EtherLink III.  Poll until two
 * consecutive reads agree: that is when the part's post-reset internals have
 * settled.  2 ms per attempt, measured, with the read count kept as the floor.
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
            NetdevWait      w;

            netdev_wait_begin(&w, 2000UL, 2000UL * 4UL);   /* 2 ms */

            do
                (VOID)EL3_SETTLE_READ(attr);
            while (!netdev_wait_done(&w));
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
 * the only granularity the part has; el3_rx_wanted() makes it act like a hash.
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
     * Group only when something is joined: it is all multicast or none on this
     * part, so a group bit left on hands the CPU every group frame on the
     * segment to hash and throw away.
     */
    if (el3_any_group(nic))
        filter |= EL3_FIL_GROUP;
    if (nic->promisc)
        filter |= EL3_FIL_PROMISC;

    el3_cmd(nic, EL3_C_SET_RX_FILTER, filter);
}

/*
 * Would the DP8390's filter have taken this frame?  Unicast and broadcast the
 * chip has already decided; every group address arrived, so netdev_mcaf.c's
 * hash is applied here, with the same CRC, the same six bits, byte and bit.
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
 * On a 3C589 neither the address nor the resource configuration is loaded from
 * the EEPROM, and the part answers no I/O while its I/O base field is non-zero,
 * so both are written every time: a global reset resets the ASIC behind the
 * PCMCIA interface chip and not the interface chip itself.
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
 * No global reset here: it does not show its busy bit, so waiting one out costs
 * a millisecond, and el3_reset() calls this from the vertical blank under
 * Disable().  The receive and transmit resets do show the bit and suffice.
 */
/* The coaxial converter's own timer: 3.2 us a tick, saturating at 255. */
#define EL3_COAX_WAIT_US    816u
#define EL3_COAX_SPINS      4000u

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
     * Activate the board.  Nothing ever asserts CC_RESET to an A1200's slot, so
     * the card wakes with whatever this bit last was, and it decides per boot
     * whether the receiver hears.  Read-modify-write, keeping the media bits.
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
     * The media the card was built with decides what to switch on.  The coaxial
     * converter needs 800 us before it carries anything.  Link beat is only
     * meaningful on twisted pair.
     */
    /*
     * Written absolutely, never read-modify-write: the resource configuration
     * is not inherited, the transceiver is selected by writing the address
     * configuration, and the media register's high bits are status, not control.
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
     * The coaxial converter needs 800 us before it carries anything, measured
     * by the chip's own timer: 3.2 us a tick, saturating at 255, so 816 us.  A
     * hardware clock rather than a spin, because this can run from the watchdog.
     * The timer is the deadline; the old 4000 register reads are only the floor.
     */
    if ((nic->el3_media & EL3_CC_BNC_PRESENT) != 0 &&
        (nic->el3_media & EL3_CC_UTP_PRESENT) == 0)
    {
        NetdevWait w;

        el3_cmd(nic, EL3_C_COAX_START, 0);
        netdev_wait_begin(&w, EL3_COAX_WAIT_US, EL3_COAX_SPINS);

        while (netdev_bus_r8(&nic->bus, EL3_W1_TIMER) != 0xff &&
               !netdev_wait_done(&w))
            ;
    }

    /*
     * Start transmitting only once a whole frame is in the FIFO: a 1514-byte
     * frame is about 380 us of programmed I/O on a 14 MHz 68020, and the wire
     * would run dry in the middle of it, which is an underrun and a reset.
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
     * The read-zero mask runs the opposite way to its name: a CLEAR bit forces
     * a status bit to read as zero, so all ones reports everything and zero
     * blinds the interrupt handler.  The power-up default is zero.
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
 * The watchdog's recovery.  el3_tx() sets txb_inuse when the FIFO could not
 * take a frame and arms the transmit-available threshold; that interrupt's
 * acknowledgement disarms it, so a lost TX Available wedges the transmitter.
 */
VOID el3_reset(NetdevNic *nic)
{
    nic->resets++;
    el3_halt(nic);
    (VOID)el3_init(nic);
}

/* ----------------------------------------------------------- transmit ---- */

/*
 * The transmit status stack is 31 deep and is popped by writing to it: read the
 * byte, write anything back, repeat until the read is zero.  It is a byte at an
 * odd offset and so goes through netdev_bus, into Gayle's second window.
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
            nic->tx_underruns++;

        /* Any value pops it.  Zero is the value, so that nothing here reads
           as a bit written back into a register that has none. */
        netdev_bus_w8(&nic->bus, EL3_W1_TX_STATUS, 0);
        nic->tx_completed++;

        /*
         * Any error bit disables the transmitter, and jabber and underrun need
         * it reset before it can be enabled again.  The recovery runs here,
         * with its own entry, because the next entry can be clean.
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
         * Not enough room.  Ask to be told when there is.  The threshold is
         * disarmed by its own acknowledgement, so it is set again every time
         * this path is taken and never once at init.
         */
        el3_cmd(nic, EL3_C_SET_TX_AVAIL, need);
        nic->txb_inuse = 1;

        return DP8390_TX_BUSY;
    }

    /*
     * Length counts the frame, not the padding: the card pads to the Ethernet
     * minimum itself, and a second pad would fall inside the length.
     */
    el3_put(nic, EL3_W1_FIFO, (UWORD)(len & EL3_TX_LEN_MASK));
    el3_put(nic, EL3_W1_FIFO, 0);

    /*
     * The body, through netdev_bus's burst path, with no swap.  The byte count
     * is rounded up to the transfer unit by the caller, and the dword pad is
     * what absorbs it.
     */
    netdev_bus_wdata(&nic->bus, frame, (UWORD)((len + 1u) & (UWORD)~1u));

    /* The rest of the pad to the dword boundary, if any. */
    for (pad = (UWORD)((len + 1u) & (UWORD)~1u); pad < (need - 4u); pad += 2)
        el3_put(nic, EL3_W1_FIFO, 0);

    nic->tx_packets++;

    return 0;
}

/* ------------------------------------------------------------- receive --- */

/* The only thing that advances the FIFO past the frame's dword padding. */
static VOID el3_discard(NetdevNic *nic)
{
    el3_cmd(nic, EL3_C_RX_DISCARD, 0);
    (VOID)el3_wait_cmd(nic);
}

/*
 * One frame out of the receive FIFO, or nothing.  Discard is issued for every
 * frame, good or bad, as soon as the last byte is off the FIFO: the head packet
 * occupies receive memory until it is popped.
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
         * The header first: the filter and the direct-receive claim both decide
         * from it, and it is the same seven words off the FIFO whichever path
         * the rest of the frame takes.  A declined claim reassembles the
         * staging buffer, header already at its start.
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
                 * The fused drain: the FIFO reads pay for the checksum.  The
                 * slot's payload pointer is longword aligned by construction,
                 * which is the routine's one requirement.
                 */
                ULONG sum = EL3_RX_DRAIN_SUM(dst,
                                             (const volatile void *)
                                                 nic->bus.asic,
                                             (ULONG)(len - NETDEV_HDR_LEN));

                el3_discard(nic);
                nic->rx_packets++;
                nic->rx_claimed(nic->rx_arg, token, sum, 1);

                return TRUE;
            }

            netdev_bus_rdata(&nic->bus, buf + NETDEV_HDR_LEN,
                             (UWORD)(len - NETDEV_HDR_LEN));
            el3_discard(nic);
            nic->rx_packets++;
            nic->rx(nic->rx_arg, buf, len);

            return TRUE;
        }
    }

    el3_discard(nic);

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
             * restart clears every case.
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
     * runs for the fixed-address rows too, where nothing claimed anything.
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
     * Two station addresses: words 0..2 are 3Com's own and words 10..12 the OEM
     * one.  The OEM address is taken first, as 3c589.device does, with 3Com's
     * own as the fallback.  Each word holds two octets, the earlier one high.
     */
    if (!el3_eeprom_ready(nic) ||
        (!el3_take_addr(nic, EL3_EE_OEM_ADDR_0) &&
         !el3_take_addr(nic, EL3_EE_NODE_ADDR_0)))
    {
        /*
         * The EEPROM did not validate.  An address invented from a floating
         * data register changes per boot, so the address is derived from the
         * machine fingerprint instead, as ne2000.c does for a card with no PROM.
         */
        UBYTE fp[NETDEV_MAC_FP_MAX];
        UWORD n;
        /* uintptr_t, not APTR: the host tier compiles this file for a 64-bit
           pointer and -Werror rejects the narrowing.  The salt only needs its
           low half to differ between boards, so the truncation is deliberate. */
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
     * addresses.  txb_cnt is 1 and the other buffer fields stay zero, so
     * anything reading them as a ring reads an empty one.
     */
    /*
     * bus->asic is the address netdev_bus_rdata/wdata hammer, and setup derives
     * it as the register base plus sixteen strides -- true for a DP8390, false
     * here: this part's FIFO is window 1 offset 0, the register base itself.
     */
    netdev_bus_regmap(&nic->bus, NULL,
                      (APTR)(volatile void *)(nic->board + nic->card->reg_off +
                                              EL3_W1_FIFO));

    nic->txb_cnt   = 1;
    nic->txb_inuse = 0;
    nic->read_hdr  = NULL;
    nic->ring_copy = NULL;
    nic->ring_copy_sum = NULL;  /* el3_rint() fuses its own FIFO drain */
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
