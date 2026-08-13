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

static UBYTE bus_r8(const NetdevBus *bus, UWORD reg)
{
    return bus->nic[reg * bus->stride];
}

static VOID bus_w8(const NetdevBus *bus, UWORD reg, UBYTE val)
{
    bus->nic[reg * bus->stride] = val;
}

static UBYTE bus_ra8(const NetdevBus *bus, UWORD reg)
{
    return bus->asic[reg * bus->stride];
}

static VOID bus_wa8(const NetdevBus *bus, UWORD reg, UBYTE val)
{
    bus->asic[reg * bus->stride] = val;
}

/*
 * The 32-bit window is 128 bytes of the same FIFO mirrored end to end, so a
 * `movem.l` reads sixteen longwords from sixteen consecutive addresses and
 * every one of them is the port.  That is the only reason a burst wider than
 * the port exists at all: the address does not have to hold still.
 */
static VOID bus_rdata_long(const NetdevBus *bus, UBYTE *dst, UWORD len)
{
    volatile ULONG *port = (volatile ULONG *)bus->wide;
    ULONG          *out  = (ULONG *)(APTR)dst;
    UWORD           i;

    for (i = 0; i + 32 <= len; i += 32)
    {
        out[0] = port[0];
        out[1] = port[1];
        out[2] = port[2];
        out[3] = port[3];
        out[4] = port[4];
        out[5] = port[5];
        out[6] = port[6];
        out[7] = port[7];
        out += 8;
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
    UWORD           i;

    for (i = 0; i + 32 <= len; i += 32)
    {
        port[0] = in[0];
        port[1] = in[1];
        port[2] = in[2];
        port[3] = in[3];
        port[4] = in[4];
        port[5] = in[5];
        port[6] = in[6];
        port[7] = in[7];
        in += 8;
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
                  ((ULONG)p & 3u) == 0 && len >= 4);
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
    if (bus->dmode == NETDEV_DMODE_BYTE || ((ULONG)dst & 1u) != 0)
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
    for (i = 0; i + 2 <= len; i += 2)
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
    if (bus->dmode == NETDEV_DMODE_BYTE || ((ULONG)src & 1u) != 0)
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
    for (i = 0; i + 2 <= len; i += 2)
        *port = *in++;
    if (i < len)
        *port = (UWORD)(src[i] << 8);
}

const struct NetdevBusOps netdev_bus_generic =
{
    bus_r8, bus_w8, bus_ra8, bus_wa8, bus_rdata, bus_wdata
};

VOID netdev_bus_setup(NetdevBus *bus, APTR base, UWORD stride, APTR wide)
{
    bus->nic    = (volatile UBYTE *)base;
    bus->asic   = (volatile UBYTE *)base + 16u * stride;
    bus->wide   = (volatile UBYTE *)wide;
    bus->stride = stride;
    bus->dmode  = NETDEV_DMODE_WORD;
    bus->ops    = &netdev_bus_generic;
}
