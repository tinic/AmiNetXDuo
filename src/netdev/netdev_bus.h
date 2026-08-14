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
    UWORD           stride;     /* bytes between consecutive register indices */
    UBYTE           shift;      /* log2(stride); 1, 2 and 4 are the only ones */
    UBYTE           dmode;      /* NETDEV_DMODE_*, set by the probe */

    const struct NetdevBusOps *ops;
};

/* The stride-driven implementation every card in the family uses today. */
extern const struct NetdevBusOps netdev_bus_generic;

/* base is the board's register window; stride is 1, 2 or 4. */
VOID netdev_bus_setup(NetdevBus *bus, APTR base, UWORD stride, APTR wide);

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
static inline UBYTE netdev_bus_r8(const NetdevBus *bus, UWORD reg)
{
    return bus->nic[(ULONG)reg << bus->shift];
}

static inline VOID netdev_bus_w8(const NetdevBus *bus, UWORD reg, UBYTE val)
{
    bus->nic[(ULONG)reg << bus->shift] = val;
}

static inline UBYTE netdev_bus_ra8(const NetdevBus *bus, UWORD reg)
{
    return bus->asic[(ULONG)reg << bus->shift];
}

static inline VOID netdev_bus_wa8(const NetdevBus *bus, UWORD reg, UBYTE val)
{
    bus->asic[(ULONG)reg << bus->shift] = val;
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
