/*
 * anxnet.device, layer 3 of 3: bus access.
 *
 * Everything above this file addresses the chip by register index, 0..15 for
 * the NIC and 0..15 for the ASIC.  Only this layer knows that the indices are
 * not adjacent bytes: an Amiga NE2000 card wires the chip's 8-bit port onto
 * one byte lane of a 16- or 32-bit Zorro bus, so consecutive registers land
 * 2 or 4 bytes apart, and a driver that assumes 1 reads a gap.
 *
 *   Ariadne II, X-Surf   stride 2   (ISA address line n -> Zorro line n+1)
 *   X-Surf 100           stride 4
 *
 * The ASIC block always starts NE2000_ASIC_OFFSET (16) registers past the NIC
 * block, so one base plus one stride describes both.  A card that needs more
 * than that supplies its own NetdevBusOps.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_NETDEV_BUS_H
#define AMINETXDUO_NETDEV_BUS_H

#include <exec/types.h>

struct NetdevBus;

/*
 * How the auto-advancing data port is driven.  This is measured, not
 * configured: netdev_bus_probe_wide() writes through the wide window and reads
 * back through the narrow one, and only a match promotes the mode.  The value
 * reaches the user through S2_GETSPECIALSTATS, so a disabled fast window is
 * visible rather than inferred from the throughput.
 */
#define NETDEV_DMODE_BYTE   0       /* 8-bit port, NE1000-style              */
#define NETDEV_DMODE_WORD   1       /* 16-bit port at the ASIC data register */
#define NETDEV_DMODE_LONG   2       /* 32-bit mirrored window, movem-driven  */

typedef struct NetdevBus NetdevBus;

struct NetdevBusOps
{
    /*
     * Bursts only.  The four scalar accessors used to live here and are now
     * inline below: one implementation, and the call cost more than the
     * access it was wrapping.
     *
     * The port does not advance an address, the chip does, so a burst is
     * repeated access to one location.  len is rounded up to the transfer
     * unit by the caller, never here.
     */
    VOID  (*rdata)(const NetdevBus *bus, UBYTE *dst, UWORD len);
    VOID  (*wdata)(const NetdevBus *bus, const UBYTE *src, UWORD len);
};

struct NetdevBus
{
    volatile UBYTE *nic;        /* register file, index 0 */
    volatile UBYTE *asic;       /* nic + 16 * stride, unless overridden */
    volatile UBYTE *wide;       /* 32-bit mirrored data window, or NULL */

    /*
     * The odd-register window, or NULL when the register file is contiguous.
     *
     * Gayle splits PCMCIA I/O in two: 0xA20000 carries 16-bit accesses and the
     * even 8-bit registers, 0xA30000 the odd ones, and both are addressed at
     * even offsets.  An odd register reached at an odd address in the even
     * window is not a register access on the real card.  Amiberry decodes it
     * 1:1 and accepts it either way, which is why this is written down.
     * cnet.device, written for the hardware, uses an even address in both
     * windows and never anything else.
     */
    volatile UBYTE *odd;

    /*
     * A 32-entry offset table when the register file is not evenly spaced,
     * NULL otherwise.  The X-Surf 500 is the case that needs it: an ACA500
     * exposes so few address lines that the register number's bits land
     * scattered across the address, from $0204 to $6E94 apart with no stride
     * to compute.  Entries 0..15 are the NIC file, 16..31 the ASIC block.
     */
    const ULONG *regmap;
    UWORD           stride;     /* bytes between consecutive register indices */
    UBYTE           shift;      /* log2(stride).  1, 2 and 4 are the only ones */
    UBYTE           dmode;      /* NETDEV_DMODE_*, set by the probe */

    /*
     * Read odd registers as the low half of a word, which is cnet16's GETODD.
     *
     * A Fast-Ethernet NE2000 clone that asserts -IOIS16 unconditionally
     * decodes 16-bit I/O cycles and nothing else, so a byte read of an odd
     * register returns whatever the bus held.  The CR readback, the ISR polls
     * and the CURR/BNRY ring pointers are all odd registers, and all of them
     * come back wrong.  cnet ships a second binary for this
     * (cnetdevice.asm:551-556, "Netgear FA411, CNet SinglePoint 10/100"),
     * which leaves the user to diagnose the card.  This is set by a probe
     * instead, in ne2000_detect().
     *
     * It is not a 16-bit data mode.  DSDC_WTS is set unconditionally in both
     * of cnet's binaries and the data port is 16 bits wide either way.  What
     * changes is control register reads and nothing else.  It therefore cannot
     * be dmode, which the data-port probe measures and S2_GETSPECIALSTATS
     * reports.  Nor can it be `odd`, whose non-NULL already means a second
     * window exists, and which must stay set alongside this, because writes
     * are unaffected and still go byte-wide into that window.
     *
     * The arithmetic below is valid only at stride 1.  netdev_bus_set_getodd()
     * refuses anything else.
     */
    UBYTE           getodd;

    const struct NetdevBusOps *ops;
};

/* The stride-driven implementation every card in the family uses today. */
extern const struct NetdevBusOps netdev_bus_generic;

/* base is the board's register window, and stride is 1, 2 or 4. */
VOID netdev_bus_setup(NetdevBus *bus, APTR base, UWORD stride, APTR wide);

/* Call after setup for a card whose odd registers live in a second window. */
VOID netdev_bus_split(NetdevBus *bus, APTR odd);

/* Call after setup for a card whose register file is not evenly spaced. */
VOID netdev_bus_regmap(NetdevBus *bus, const ULONG *map, APTR data_port);

/*
 * Turn on the word-read path for odd registers.  FALSE, and nothing changed,
 * on a bus where the arithmetic does not hold: it reads the word at reg-1 out
 * of the even window, which is the same register pair only when consecutive
 * indices are one byte apart and the file is not a scatter table.
 */
BOOL netdev_bus_set_getodd(NetdevBus *bus);

/*
 * Promote to NETDEV_DMODE_LONG only if the wide window really is the same
 * FIFO.  Called with the chip already set up for a remote-DMA write of
 * NETDEV_BUS_PROBE_LEN bytes.  Returns the mode it settled on.
 */
#define NETDEV_BUS_PROBE_LEN    32

/*
 * The four scalar accessors do not go through ops.  There has only ever been
 * one implementation, and the indirection cost more than the access: -O2
 * compiled bus_r8 to a call whose body was
 *
 *   movea.l 4(sp),a0 / move.w 10(sp),d0 / mulu.w 12(a0),d0 / add.l (a0),d0
 *   / movea.l d0,a0 / move.b (a0),d0 / rts
 *
 * That mulu.w costs 28 cycles on a 68020, once for every register touched, and
 * a frame touches around twenty-five.  The stride is 1, 2 or 4, so it is a
 * shift, and inline it is one indexed move.
 */
#ifdef NETDEV_TIME
extern ULONG netdev_time_regs;      /* scalar register accesses, per report */
#define NETDEV_BUS_COUNT()  (netdev_time_regs++)
#else
#define NETDEV_BUS_COUNT()  ((VOID)0)
#endif

/* Where register `reg` is.  One predictable test for the cards that need the
   split, and NULL for every board whose file is contiguous. */
static inline volatile UBYTE *netdev_bus_at(const NetdevBus *bus, UWORD reg)
{
    if (bus->regmap != NULL)
        return &bus->nic[bus->regmap[reg & 31u]];

    if (bus->odd != NULL && (reg & 1) != 0)
        return &bus->odd[(ULONG)(reg - 1) << bus->shift];

    return &bus->nic[(ULONG)reg << bus->shift];
}

/*
 * GETODD, and why it is here rather than inside netdev_bus_at().
 *
 * netdev_bus_at() returns a UBYTE *, and this is a word access narrowed
 * afterwards.  No address it could return makes a byte load do the right
 * thing.  The two read accessors therefore branch ahead of it, and the two
 * write accessors do not branch at all, because cnet's trick is read-only and
 * a byte write to the odd window is what the card wants.
 *
 * The word comes out of the even window at reg-1, and the odd register is its
 * low half.  A 68k `move.w` loads the even address into the high byte, so the
 * word cast to UBYTE is the byte at reg.  That is cnet's GETODD exactly:
 * `move.w reg-odd-1+even,-(sp) / move.b 1(sp),d`.
 *
 * The burst functions are untouched: they address bus->asic, which is
 * register 16 and even.
 */
static inline UBYTE netdev_bus_r8(const NetdevBus *bus, UWORD reg)
{
    NETDEV_BUS_COUNT();
    if (bus->getodd != 0 && bus->regmap == NULL && (reg & 1) != 0)
        return (UBYTE)*(volatile UWORD *)
                       &bus->nic[(ULONG)(reg & ~1u) << bus->shift];

    return *netdev_bus_at(bus, reg);
}

static inline VOID netdev_bus_w8(const NetdevBus *bus, UWORD reg, UBYTE val)
{
    NETDEV_BUS_COUNT();
    *netdev_bus_at(bus, reg) = val;
}

/*
 * The ASIC block is part of the same register file, so it takes the same
 * even/odd split.  ASIC register r is register 16 + r of the one file, and on
 * a bus with a split window the parity that decides which window it lands in
 * is the parity of 16 + r, which is the parity of r.
 *
 * These used to index bus->asic directly and never look at bus->odd, so the
 * NE2000 reset register, ASIC 15 and file register 31, came out at 0xA2031F on
 * the PCMCIA slot.  That is an odd address in the window Gayle reserves for
 * 16-bit and even 8-bit accesses, which is not a register access on a real
 * card.  cnet.device, written against the hardware, puts it at 31+odd =
 * 0xA3031E (cnet.i:29,85).  A route through netdev_bus_at() with the
 * whole-file index gives that, and is a no-op for every card whose odd window
 * is NULL, because asic == nic + 16 * stride by construction.
 */
static inline UBYTE netdev_bus_ra8(const NetdevBus *bus, UWORD reg)
{
    UWORD whole = (UWORD)(16u + (reg & 15u));

    NETDEV_BUS_COUNT();
    if (bus->getodd != 0 && bus->regmap == NULL && (whole & 1) != 0)
        return (UBYTE)*(volatile UWORD *)
                       &bus->nic[(ULONG)(whole & ~1u) << bus->shift];

    return *netdev_bus_at(bus, whole);
}

static inline VOID netdev_bus_wa8(const NetdevBus *bus, UWORD reg, UBYTE val)
{
    NETDEV_BUS_COUNT();
    *netdev_bus_at(bus, (UWORD)(16u + (reg & 15u))) = val;
}

static inline VOID netdev_bus_rdata(const NetdevBus *bus, UBYTE *dst, UWORD len)
{
    bus->ops->rdata(bus, dst, len);
}

static inline VOID netdev_bus_wdata(const NetdevBus *bus, const UBYTE *src,
                                    UWORD len)
{
    bus->ops->wdata(bus, src, len);
}

#endif /* AMINETXDUO_NETDEV_BUS_H */
