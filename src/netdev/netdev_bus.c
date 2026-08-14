/*
 * anxnet.device, the stride-driven bus implementation.
 *
 * Two facts about the emulated and the real cards decide the word path:
 * the register window is byte-swapped in hardware, so a 68k `move.w` from the
 * data port already delivers the two bytes in wire order and nothing here may
 * swap them again; and the port does not advance a host address, so a burst is
 * repeated access to ONE location.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_bus.h"
#include "n68k_iocopy.h"

/*
 * unsigned long, not ULONG: identical on the 68000 and 32 bits too narrow for
 * a pointer on the 64-bit host that builds this file for src/netdev/test.
 */
#define BUS_ALIGN(p, n) (((unsigned long)(const void *)(p)) & (unsigned long)(n))

/*
 * The 32-bit window is 128 bytes of the same FIFO mirrored end to end, so a
 * `movem.l` reads sixteen longwords from sixteen consecutive addresses and
 * every one of them is the port.  That is the only reason a burst wider than
 * the port exists at all: the address does not have to hold still.
 *
 * Eight registers is 32 bytes in two instructions.  The same 32 bytes as eight
 * separate `move.l` is sixteen, and the bus cycles are identical either way --
 * eight long accesses to the window, eight to memory -- so what the movem
 * form saves is instruction fetch and decode, not bus time.  That is a real
 * saving on a 68020 with a 256-byte cache and a much larger one under an
 * emulator, which charges per instruction; on real Zorro II, where a long
 * access is 0.28-0.56 us against a handful of CPU cycles, the bus dominates
 * and the win is small.  Nothing else in the family reaches this path: the
 * word port is one non-mirrored address and cannot be batched at all.
 */
static VOID bus_rdata_long(const NetdevBus *bus, UBYTE *dst, UWORD len)
{
    volatile ULONG *port = (volatile ULONG *)bus->wide;
    ULONG          *out  = (ULONG *)(APTR)dst;
    UWORD           i    = (UWORD)(len & (UWORD)~31u);

    if (i != 0)
    {
        n68k_port_in(out, bus->wide, (ULONG)(i >> 5));
        out += (i >> 2);
    }

    for (; i + 4 <= len; i += 4)
        *out++ = *port;

    /* The chip only ever hands out whole words; a 2-byte tail is possible. */
    if (i < len)
    {
        volatile UWORD *w = (volatile UWORD *)bus->asic;
        *(UWORD *)(APTR)out = *w;
    }
}

static VOID bus_wdata_long(const NetdevBus *bus, const UBYTE *src, UWORD len)
{
    volatile ULONG *port = (volatile ULONG *)bus->wide;
    const ULONG    *in   = (const ULONG *)(const void *)src;
    UWORD           i    = (UWORD)(len & (UWORD)~31u);

    if (i != 0)
    {
        n68k_port_out(bus->wide, in, (ULONG)(i >> 5));
        in += (i >> 2);
    }

    for (; i + 4 <= len; i += 4)
        *port = *in++;

    if (i < len)
    {
        volatile UWORD *w = (volatile UWORD *)bus->asic;
        *w = *(const UWORD *)(const void *)in;
    }
}

/*
 * The long path dereferences ULONG*, so it is only reachable for a buffer the
 * 68020 can address that way.  A ring-wrapped read resumes at an offset that
 * is only guaranteed even, which is exactly where an unchecked long burst
 * would take an address error on a 68000 and a silent misread nowhere else.
 */
static BOOL bus_long_ok(const NetdevBus *bus, const void *p, UWORD len)
{
    return (BOOL)(bus->dmode == NETDEV_DMODE_LONG && bus->wide != NULL &&
                  BUS_ALIGN(p, 3) == 0 && len >= 4);
}

static VOID bus_rdata(const NetdevBus *bus, UBYTE *dst, UWORD len)
{
    volatile UWORD *port;
    UWORD          *out;
    UWORD           i;

    if (bus_long_ok(bus, dst, len))
    {
        bus_rdata_long(bus, dst, len);
        return;
    }

    port = (volatile UWORD *)bus->asic;

    /*
     * An odd destination cannot be written as words: a 68000 takes an address
     * error on it.  The port is still 16 bits wide -- the chip hands out whole
     * words and reading it a byte at a time would lose half of every one -- so
     * the word is read and split.
     */
    if (bus->dmode == NETDEV_DMODE_BYTE || BUS_ALIGN(dst, 1) != 0)
    {
        if (bus->dmode == NETDEV_DMODE_BYTE)
        {
            volatile UBYTE *b = bus->asic;

            for (i = 0; i < len; i++)
                dst[i] = *b;
            return;
        }

        for (i = 0; i + 2 <= len; i += 2)
        {
            UWORD w = *port;

            dst[i]     = (UBYTE)(w >> 8);
            dst[i + 1] = (UBYTE)w;
        }
        if (i < len)
            dst[i] = (UBYTE)(*port >> 8);
        return;
    }

    out = (UWORD *)(APTR)dst;

    /* Whole 32-byte blocks first.  The port is one address and cannot be
       batched; the block form is here for the other three quarters of the
       instructions, which is what a whole frame through this loop costs. */
    i = (UWORD)(len & (UWORD)~31u);
    if (i != 0)
    {
        n68k_port_in_w(out, port, (ULONG)(i >> 5));
        out += (i >> 1);
    }

    for (; i + 2 <= len; i += 2)
        *out++ = *port;
    if (i < len)
    {
        UWORD tail = *port;

        dst[i] = (UBYTE)(tail >> 8);
    }
}

static VOID bus_wdata(const NetdevBus *bus, const UBYTE *src, UWORD len)
{
    volatile UWORD *port;
    const UWORD    *in;
    UWORD           i;

    if (bus_long_ok(bus, src, len))
    {
        bus_wdata_long(bus, src, len);
        return;
    }

    port = (volatile UWORD *)bus->asic;

    /* Same reason as the read side: an odd source cannot be read as words. */
    if (bus->dmode == NETDEV_DMODE_BYTE || BUS_ALIGN(src, 1) != 0)
    {
        if (bus->dmode == NETDEV_DMODE_BYTE)
        {
            volatile UBYTE *b = bus->asic;

            for (i = 0; i < len; i++)
                *b = src[i];
            return;
        }

        for (i = 0; i + 2 <= len; i += 2)
            *port = (UWORD)(((UWORD)src[i] << 8) | src[i + 1]);
        if (i < len)
            *port = (UWORD)(src[i] << 8);
        return;
    }

    in = (const UWORD *)(const void *)src;

    i = (UWORD)(len & (UWORD)~31u);
    if (i != 0)
    {
        n68k_port_out_w(port, in, (ULONG)(i >> 5));
        in += (i >> 1);
    }

    for (; i + 2 <= len; i += 2)
        *port = *in++;
    if (i < len)
        *port = (UWORD)(src[i] << 8);
}

const struct NetdevBusOps netdev_bus_generic = { bus_rdata, bus_wdata };

/* The odd-register window, for a bus whose register file is not contiguous. */
VOID netdev_bus_split(NetdevBus *bus, APTR odd)
{
    bus->odd = (volatile UBYTE *)odd;
}

VOID netdev_bus_setup(NetdevBus *bus, APTR base, UWORD stride, APTR wide)
{
    bus->nic    = (volatile UBYTE *)base;
    bus->asic   = (volatile UBYTE *)base + 16u * stride;
    bus->wide   = (volatile UBYTE *)wide;
    bus->odd    = NULL;         /* netdev_bus_split() for the ones that need it */
    bus->stride = stride;
    bus->shift  = (UBYTE)((stride == 4u) ? 2u : ((stride == 2u) ? 1u : 0u));
    bus->dmode  = NETDEV_DMODE_WORD;
    bus->ops    = &netdev_bus_generic;
}
