/*
 * anxnet.device, layer 3 of 3: bus access.
 *
 * Everything above this file addresses the chip by REGISTER INDEX, 0..15 for
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
 * reaches the user through S2_GETSPECIALSTATS, so "the fast window is off" is
 * something a user can see rather than something they have to infer from the
 * throughput.
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
     * repeated access to ONE location.  len is rounded up to the transfer
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
     * Gayle splits PCMCIA I/O in two: 0xA20000 carries 16-bit accesses and
     * the EVEN 8-bit registers, 0xA30000 the odd ones, and both are addressed
     * at even offsets.  An odd register reached at an odd address in the even
     * window is not a register access on the real card.  Amiberry decodes it
     * 1:1 and accepts it either way, which is exactly why this is written
     * down: cnet.device, which was written for the hardware, uses an even
     * address in both windows and never anything else.
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
    UBYTE           shift;      /* log2(stride); 1, 2 and 4 are the only ones */
    UBYTE           dmode;      /* NETDEV_DMODE_*, set by the probe */

    const struct NetdevBusOps *ops;
};

/* The stride-driven implementation every card in the family uses today. */
extern const struct NetdevBusOps netdev_bus_generic;

/* base is the board's register window; stride is 1, 2 or 4. */
VOID netdev_bus_setup(NetdevBus *bus, APTR base, UWORD stride, APTR wide);

/* Call after setup for a card whose odd registers live in a second window. */
VOID netdev_bus_split(NetdevBus *bus, APTR odd);

/* Call after setup for a card whose register file is not evenly spaced. */
VOID netdev_bus_regmap(NetdevBus *bus, const ULONG *map, APTR data_port);

/*
 * Promote to NETDEV_DMODE_LONG only if the wide window really is the same
 * FIFO.  Called with the chip already set up for a remote-DMA write of
 * NETDEV_BUS_PROBE_LEN bytes; returns the mode it settled on.
 */
#define NETDEV_BUS_PROBE_LEN    32

/*
 * The four scalar accessors do NOT go through ops.  There is one
 * implementation and there has only ever been one, and the indirection cost
 * more than the access: -O2 compiled bus_r8 to a call whose body was
 *
 *   movea.l 4(sp),a0 / move.w 10(sp),d0 / mulu.w 12(a0),d0 / add.l (a0),d0
 *   / movea.l d0,a0 / move.b (a0),d0 / rts
 *
 * -- a mulu.w, 28 cycles on a 68020, once for every register touched, and a
 * frame touches around twenty-five.  The stride is 1, 2 or 4, so it is a
 * shift, and inline it is one indexed move.
 */
#ifdef NETDEV_TIME
extern ULONG netdev_time_regs;      /* scalar register accesses, per report */
#define NETDEV_BUS_COUNT()  (netdev_time_regs++)
#else
#define NETDEV_BUS_COUNT()  ((VOID)0)
#endif

/* Where register `reg` is.  One predictable test for the cards that need the
   split; NULL for every board whose file is contiguous. */
static inline volatile UBYTE *netdev_bus_at(const NetdevBus *bus, UWORD reg)
{
    if (bus->regmap != NULL)
        return &bus->nic[bus->regmap[reg & 31u]];

    if (bus->odd != NULL && (reg & 1) != 0)
        return &bus->odd[(ULONG)(reg - 1) << bus->shift];

    return &bus->nic[(ULONG)reg << bus->shift];
}

static inline UBYTE netdev_bus_r8(const NetdevBus *bus, UWORD reg)
{
    NETDEV_BUS_COUNT();
    return *netdev_bus_at(bus, reg);
}

static inline VOID netdev_bus_w8(const NetdevBus *bus, UWORD reg, UBYTE val)
{
    NETDEV_BUS_COUNT();
    *netdev_bus_at(bus, reg) = val;
}

/*
 * THE ASIC BLOCK IS PART OF THE SAME REGISTER FILE, so it takes the same
 * even/odd split.  ASIC register r is register 16 + r of the one file, and on
 * a bus with a split window the parity that decides which window it lands in
 * is the parity of 16 + r, which is the parity of r.
 *
 * These used to index bus->asic directly and never look at bus->odd, so the
 * NE2000 reset register -- ASIC 15, file register 31 -- came out at
 * 0xA2031F on the PCMCIA slot: an ODD address in the window Gayle reserves
 * for 16-bit and even 8-bit accesses, which is not a register access on a
 * real card at all.  cnet.device, written against the hardware, puts it at
 * 31+odd = 0xA3031E (cnet.i:29,85).  Routing through netdev_bus_at() with the
 * whole-file index gives exactly that, and is a no-op for every card whose
 * odd window is NULL, because asic == nic + 16 * stride by construction.
 */
static inline UBYTE netdev_bus_ra8(const NetdevBus *bus, UWORD reg)
{
    NETDEV_BUS_COUNT();
    return *netdev_bus_at(bus, (UWORD)(16u + (reg & 15u)));
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
