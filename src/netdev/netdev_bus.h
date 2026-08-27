/*
 * anxnet.device, layer 3 of 3: bus access.
 *
 * Everything above addresses the chip by register index, 0..15 for the NIC and
 * 0..15 for the ASIC.  Only this layer knows the indices are not adjacent
 * bytes: an Amiga NE2000 card wires the chip's 8-bit port onto one byte lane of
 * a 16- or 32-bit Zorro bus, so consecutive registers land 2 or 4 bytes apart.
 * The ASIC block always starts NE2000_ASIC_OFFSET (16) registers past the NIC.
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
 * back through the narrow one, and only a match promotes the mode.
 */
#define NETDEV_DMODE_BYTE   0       /* 8-bit port, NE1000-style              */
#define NETDEV_DMODE_WORD   1       /* 16-bit port at the ASIC data register */
#define NETDEV_DMODE_LONG   2       /* 32-bit mirrored window, movem-driven  */

typedef struct NetdevBus NetdevBus;

struct NetdevBusOps
{
    /*
     * Bursts only.  The port does not advance an address, the chip does, so a
     * burst is repeated access to one location.  len is rounded up to the
     * transfer unit by the caller, never here.
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
     * Gayle splits PCMCIA I/O in two: 0xA20000 carries 16-bit accesses and the
     * even 8-bit registers, 0xA30000 the odd ones, and both are addressed at
     * EVEN offsets.  An odd register reached at an odd address in the even
     * window is not a register access on the real card.
     */
    volatile UBYTE *odd;

    /*
     * A 32-entry offset table when the register file is not evenly spaced, NULL
     * otherwise.  The X-Surf 500 is the case that needs it.  Entries 0..15 are
     * the NIC file, 16..31 the ASIC block.
     */
    const ULONG *regmap;
    UWORD           stride;     /* bytes between consecutive register indices */
    UBYTE           shift;      /* log2(stride).  1, 2 and 4 are the only ones */
    UBYTE           dmode;      /* NETDEV_DMODE_*, set by the probe */

    /*
     * Read odd registers as the low half of a word, which is cnet16's GETODD:
     * a Fast-Ethernet clone that asserts -IOIS16 unconditionally decodes 16-bit
     * I/O cycles and nothing else, so a byte read of an odd register returns
     * whatever the bus held.  Set by a probe in ne2000_detect().
     *
     * It is not a 16-bit data mode and it is not `odd`: writes are unaffected
     * and still go byte-wide into that window.  The arithmetic below is valid
     * only at stride 1, which netdev_bus_set_getodd() enforces.
     */
    UBYTE           getodd;

    const struct NetdevBusOps *ops;
};

/* The stride-driven implementation every card in the family uses today. */
extern const struct NetdevBusOps netdev_bus_generic;

/* base is the board's register window, and stride is 1, 2 or 4. */
VOID netdev_bus_setup(NetdevBus *bus, APTR base, UWORD stride, APTR wide);

/*
 * The fused drain.  netdev_bus_rdata_sum() moves exactly `len` bytes off the
 * data port and returns the ones-complement longword sum of them, in
 * n68k_copy_sum_longwords()'s convention, for the price of the drain alone.
 * A frame that arrives this way is not walked a second time for a checksum.
 *
 * Ask netdev_bus_can_sum() first and take the ordinary path when it says no:
 * the two consume the same number of port accesses for a given length, but
 * only netdev_bus_rdata_sum() writes exactly `len` bytes, and the caller has
 * to know which of the two it is about to use before it commits the chip.
 */
BOOL netdev_bus_can_sum(const NetdevBus *bus, const UBYTE *dst);
ULONG netdev_bus_rdata_sum(const NetdevBus *bus, UBYTE *dst, UWORD len);

/* Call after setup for a card whose odd registers live in a second window. */
VOID netdev_bus_split(NetdevBus *bus, APTR odd);

/* Call after setup for a card whose register file is not evenly spaced. */
VOID netdev_bus_regmap(NetdevBus *bus, const ULONG *map, APTR data_port);

/*
 * Turn on the word-read path for odd registers.  FALSE, and nothing changed, on
 * a bus where the arithmetic does not hold: it reads the word at reg-1 out of
 * the even window, which needs stride 1 and no scatter table.
 */
BOOL netdev_bus_set_getodd(NetdevBus *bus);

/*
 * Promote to NETDEV_DMODE_LONG only if the wide window really is the same FIFO.
 * Called with the chip already set up for a remote-DMA write of
 * NETDEV_BUS_PROBE_LEN bytes.  Returns the mode it settled on.
 */
#define NETDEV_BUS_PROBE_LEN    32

/*
 * The four scalar accessors do not go through ops: the indirection cost more
 * than the access it was wrapping.  The stride is 1, 2 or 4, so it is a shift,
 * and inline it is one indexed move rather than a 28-cycle mulu.w per register.
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
 * GETODD is here rather than inside netdev_bus_at(), which returns a UBYTE *:
 * no address it could return makes a byte load do the right thing.  The word
 * comes out of the even window at reg-1 and the odd register is its low half.
 * The two write accessors do not branch, because the trick is read-only.  The
 * burst functions are untouched: they address bus->asic, register 16 and even.
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
 * The ASIC block is part of the same register file and takes the same even/odd
 * split: ASIC register r is register 16 + r of the one file, whose parity is
 * the parity of r.  Indexing bus->asic directly puts the NE2000 reset register
 * at an odd address in the even window, which is not a register access on a
 * real card.
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
