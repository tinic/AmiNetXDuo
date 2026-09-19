/*
 * anxnet.device: reading a board out of Emu68's device tree, and putting an
 * interrupt server on its GIC.
 *
 * devicetree.resource is Emu68's, and its vector table has no header in any
 * SDK this tree builds against, so the eleven entry points are called by
 * offset.  Every property value is big-endian 32-bit cells, as the flattened
 * tree format says, so nothing here swaps.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_dtree.h"

#ifndef NETDEV_DTREE_TEST
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#else
/* The host test supplies these three over its own tree. */
#include <exec/libraries.h>
#ifndef CONST_STRPTR
#define CONST_STRPTR const UBYTE *
#endif
APTR            OpenResource(CONST_STRPTR name);
struct Library *OpenLibrary(CONST_STRPTR name, ULONG version);
VOID            CloseLibrary(struct Library *lib);
#endif

extern struct ExecBase *SysBase;

/* ---------------------------------------------------- devicetree.resource */

static APTR dt_base;

/*
 * The eleven entry points, called by offset.  Behind a seam: the host test
 * (src/netdev/test/test_netdev_dtree.c) supplies the same eleven over a tree
 * it builds in memory, which is how the ranges arithmetic and the cell counts
 * are checked without an Emu68.
 */
#ifndef NETDEV_DTREE_TEST

static APTR dt_openkey(const char *name)
{
    register APTR a0 __asm("a0") = (APTR)name;
    register APTR a6 __asm("a6") = dt_base;
    register APTR d0 __asm("d0");

    __asm volatile ("jsr a6@(-6:W)"
                    : "=r" (d0) : "r" (a6), "r" (a0)
                    : "cc", "memory", "d1", "a1");
    return d0;
}

static VOID dt_closekey(APTR key)
{
    register APTR a0 __asm("a0") = key;
    register APTR a6 __asm("a6") = dt_base;

    __asm volatile ("jsr a6@(-12:W)"
                    : : "r" (a6), "r" (a0)
                    : "cc", "memory", "d0", "d1", "a1");
}

static APTR dt_getchild(APTR key, APTR prev)
{
    register APTR a0 __asm("a0") = key;
    register APTR a1 __asm("a1") = prev;
    register APTR a6 __asm("a6") = dt_base;
    register APTR d0 __asm("d0");

    __asm volatile ("jsr a6@(-18:W)"
                    : "=r" (d0) : "r" (a6), "r" (a0), "r" (a1)
                    : "cc", "memory", "d1");
    return d0;
}

static APTR dt_findprop(APTR key, const char *name)
{
    register APTR a0 __asm("a0") = key;
    register APTR a1 __asm("a1") = (APTR)name;
    register APTR a6 __asm("a6") = dt_base;
    register APTR d0 __asm("d0");

    __asm volatile ("jsr a6@(-24:W)"
                    : "=r" (d0) : "r" (a6), "r" (a0), "r" (a1)
                    : "cc", "memory", "d1");
    return d0;
}

static ULONG dt_proplen(APTR prop)
{
    register APTR  a0 __asm("a0") = prop;
    register APTR  a6 __asm("a6") = dt_base;
    register ULONG d0 __asm("d0");

    __asm volatile ("jsr a6@(-36:W)"
                    : "=r" (d0) : "r" (a6), "r" (a0)
                    : "cc", "memory", "d1", "a1");
    return d0;
}

static const UBYTE *dt_propvalue(APTR prop)
{
    register APTR         a0 __asm("a0") = prop;
    register APTR         a6 __asm("a6") = dt_base;
    register const UBYTE *d0 __asm("d0");

    __asm volatile ("jsr a6@(-48:W)"
                    : "=r" (d0) : "r" (a6), "r" (a0)
                    : "cc", "memory", "d1", "a1");
    return d0;
}

static APTR dt_getparent(APTR key)
{
    register APTR a0 __asm("a0") = key;
    register APTR a6 __asm("a6") = dt_base;
    register APTR d0 __asm("d0");

    __asm volatile ("jsr a6@(-54:W)"
                    : "=r" (d0) : "r" (a6), "r" (a0)
                    : "cc", "memory", "d1", "a1");
    return d0;
}

static const char *dt_keyname(APTR key)
{
    register APTR        a0 __asm("a0") = key;
    register APTR        a6 __asm("a6") = dt_base;
    register const char *d0 __asm("d0");

    __asm volatile ("jsr a6@(-60:W)"
                    : "=r" (d0) : "r" (a6), "r" (a0)
                    : "cc", "memory", "d1", "a1");
    return d0;
}

#else
APTR         dt_openkey(const char *name);
VOID         dt_closekey(APTR key);
APTR         dt_getchild(APTR key, APTR prev);
APTR         dt_findprop(APTR key, const char *name);
ULONG        dt_proplen(APTR prop);
const UBYTE *dt_propvalue(APTR prop);
APTR         dt_getparent(APTR key);
const char  *dt_keyname(APTR key);
#endif

/* ------------------------------------------------------------- helpers -- */

static ULONG dt_cell(const UBYTE *v, ULONG i)
{
    v += i * 4;
    return ((ULONG)v[0] << 24) | ((ULONG)v[1] << 16) |
           ((ULONG)v[2] << 8) | (ULONG)v[3];
}

/* A one-cell property, or `dflt` when the node does not carry it. */
static ULONG dt_cells(APTR key, const char *name, ULONG dflt)
{
    APTR p = dt_findprop(key, name);

    if (p == NULL || dt_proplen(p) < 4)
        return dflt;
    return dt_cell(dt_propvalue(p), 0);
}

static BOOL dt_str_eq(const char *a, const char *b)
{
    while (*a != 0 && *a == *b)
    {
        a++;
        b++;
    }
    return (BOOL)(*a == *b);
}

/* A stringlist property (`compatible` is one) carrying `want`. */
static BOOL dt_stringlist_has(APTR key, const char *name, const char *want)
{
    APTR         p = dt_findprop(key, name);
    const char  *v;
    ULONG        len;
    ULONG        at = 0;

    if (p == NULL)
        return FALSE;
    v   = (const char *)dt_propvalue(p);
    len = dt_proplen(p);

    while (at < len)
    {
        ULONG n = 0;

        while (at + n < len && v[at + n] != 0)
            n++;
        if (dt_str_eq(v + at, want))
            return TRUE;
        at += n + 1;
    }
    return FALSE;
}

/*
 * A child's address translated through its parent's `ranges`, up to the root.
 * Every level takes the low cell of an address, which is what a 32-bit bus
 * can express; a range whose high cell is not zero is not one we can reach.
 * No `ranges` at all is the identity, as the binding says.
 */
/* `addr` in the address space `parent` gives its children, translated up
   through every `ranges` to the CPU's. */
static BOOL dt_translate_up(APTR parent, ULONG addr, ULONG *out);

static BOOL dt_translate(APTR node, ULONG addr, ULONG *out)
{
    return dt_translate_up(dt_getparent(node), addr, out);
}

static BOOL dt_translate_up(APTR parent, ULONG addr, ULONG *out)
{

    while (parent != NULL)
    {
        APTR   grand = dt_getparent(parent);
        ULONG  ca = dt_cells(parent, "#address-cells", 2);
        ULONG  cs = dt_cells(parent, "#size-cells", 1);
        ULONG  pa = (grand != NULL) ? dt_cells(grand, "#address-cells", 2) : 0;
        APTR   p  = dt_findprop(parent, "ranges");

        if (grand == NULL)
            break;                      /* the root: addresses are the CPU's */

        if (p != NULL && dt_proplen(p) != 0)
        {
            const UBYTE *v   = dt_propvalue(p);
            ULONG        n   = dt_proplen(p) / 4;
            ULONG        per = ca + pa + cs;
            ULONG        i;
            BOOL         hit = FALSE;

            if (per == 0 || ca == 0 || pa == 0 || cs == 0)
                return FALSE;

            for (i = 0; i + per <= n; i += per)
            {
                ULONG child_hi = (ca > 1) ? dt_cell(v, i + ca - 2) : 0;
                ULONG child    = dt_cell(v, i + ca - 1);
                ULONG par      = dt_cell(v, i + ca + pa - 1);
                ULONG size     = dt_cell(v, i + ca + pa + cs - 1);

                if (child_hi != 0)
                    continue;
                if (addr >= child && addr - child < size)
                {
                    addr = par + (addr - child);
                    hit  = TRUE;
                    break;
                }
            }
            if (!hit)
                return FALSE;
        }

        parent = grand;
    }

    *out = addr;
    return TRUE;
}

/* The first `reg` entry of `node`, translated.  Cells come from the parent. */
static BOOL dt_reg(APTR node, ULONG *base, ULONG *size)
{
    APTR         parent = dt_getparent(node);
    ULONG        ca = (parent != NULL) ? dt_cells(parent, "#address-cells", 2)
                                       : 2;
    ULONG        cs = (parent != NULL) ? dt_cells(parent, "#size-cells", 1)
                                       : 1;
    APTR         p  = dt_findprop(node, "reg");
    const UBYTE *v;

    if (p == NULL || ca == 0 || dt_proplen(p) < (ca + cs) * 4)
        return FALSE;
    v = dt_propvalue(p);

    if (ca > 1 && dt_cell(v, ca - 2) != 0)
        return FALSE;                   /* above 4 GB: not on this bus */

    *size = (cs != 0) ? dt_cell(v, ca + cs - 1) : 0;
    return dt_translate(node, dt_cell(v, ca - 1), base);
}

/* Depth-first, to `depth` levels below `key`. */
static APTR dt_find_compat(APTR key, const char *compat, UWORD depth)
{
    APTR child = NULL;

    while ((child = dt_getchild(key, child)) != NULL)
    {
        APTR hit;

        if (dt_stringlist_has(child, "compatible", compat) &&
            !dt_stringlist_has(child, "status", "disabled"))
            return child;

        if (depth != 0 &&
            (hit = dt_find_compat(child, compat, (UWORD)(depth - 1))) != NULL)
            return hit;
    }
    return NULL;
}

/* -------------------------------------------------------------- lookup -- */

BOOL netdev_dtree_present(VOID)
{
    dt_base = OpenResource((CONST_STRPTR)"devicetree.resource");
    return (BOOL)(dt_base != NULL);
}

BOOL netdev_dtree_root_compatible(const char *compat)
{
    APTR root;
    BOOL found;

    dt_base = OpenResource((CONST_STRPTR)"devicetree.resource");
    if (dt_base == NULL)
        return FALSE;
    root = dt_openkey("/");
    if (root == NULL)
        return FALSE;
    found = dt_stringlist_has(root, "compatible", compat);
    dt_closekey(root);
    return found;
}

BOOL netdev_dtree_alias_present(const char *alias)
{
    APTR        aliases;
    APTR        target = NULL;
    APTR        prop;
    const char *path;
    ULONG       len;
    ULONG       n;
    BOOL        found = FALSE;

    dt_base = OpenResource((CONST_STRPTR)"devicetree.resource");
    if (dt_base == NULL)
        return FALSE;
    aliases = dt_openkey("/aliases");
    if (aliases == NULL)
        return FALSE;

    prop = dt_findprop(aliases, alias);
    if (prop != NULL)
    {
        path = (const char *)dt_propvalue(prop);
        len = dt_proplen(prop);
        /* DT_OpenKey needs a complete string inside the property. */
        for (n = 0; n < len && path[n] != 0; n++)
            ;
        if (n < len)
        {
            target = dt_openkey(path);
            if (target != NULL)
                found = (BOOL)!dt_stringlist_has(target, "status", "disabled");
        }
    }

    if (target != NULL)
        dt_closekey(target);
    dt_closekey(aliases);
    return found;
}

BOOL netdev_dtree_bus_addr(const char *bus, ULONG addr, ULONG *out)
{
    APTR node;
    BOOL ok;

    dt_base = OpenResource((CONST_STRPTR)"devicetree.resource");
    if (dt_base == NULL)
        return FALSE;
    node = dt_openkey(bus);
    if (node == NULL)
        return FALSE;
    ok = dt_translate_up(node, addr, out);
    dt_closekey(node);
    return ok;
}

BOOL netdev_dtree_find(const char *compat, NetdevDtInfo *out)
{
    APTR  root;
    APTR  node;
    APTR  p;
    UWORD i;

    out->base   = 0;
    out->size   = 0;
    out->irq    = 0;
    out->mac_ok = 0;
    out->phy    = 0xff;

    dt_base = OpenResource((CONST_STRPTR)"devicetree.resource");
    if (dt_base == NULL)
        return FALSE;

    root = dt_openkey("/");
    if (root == NULL)
        return FALSE;

    node = dt_find_compat(root, compat, 3);
    if (node == NULL)
    {
        dt_closekey(root);
        return FALSE;
    }

    if (!dt_reg(node, &out->base, &out->size))
    {
        dt_closekey(root);
        return FALSE;
    }

    /*
     * `interrupts` in the GIC's three-cell form: type, number, flags.  The
     * first entry is the one taken.  A type-0 entry is an SPI, whose GIC
     * number is 32 above what the tree says; a type-1 entry is a PPI, 16
     * above.  gic400.library takes the GIC number.
     */
    p = dt_findprop(node, "interrupts");
    if (p != NULL && dt_proplen(p) >= 12)
    {
        const UBYTE *v    = dt_propvalue(p);
        ULONG        type = dt_cell(v, 0);
        ULONG        num  = dt_cell(v, 1);

        out->irq = num + ((type == 0) ? 32UL : (type == 1) ? 16UL : 0UL);
    }

    p = dt_findprop(node, "local-mac-address");
    if (p == NULL)
        p = dt_findprop(node, "mac-address");
    if (p != NULL && dt_proplen(p) == 6)
    {
        const UBYTE *v  = dt_propvalue(p);
        UBYTE        or = 0;

        for (i = 0; i < 6; i++)
        {
            out->mac[i] = v[i];
            or = (UBYTE)(or | v[i]);
        }
        /* A zero address is what a tree carries before firmware fills it in,
           and the group bit is not a station address either. */
        out->mac_ok = (UBYTE)(or != 0 && (v[0] & 1) == 0);
    }

    /*
     * The PHY: the tree points at it through phy-handle, but a phandle
     * lookup is a walk of the whole tree, and the node it names is the MDIO
     * bus child of this very node on every board this row runs on.  Its
     * `reg` is the MDIO address.
     */
    {
        APTR mdio = NULL;

        while ((mdio = dt_getchild(node, mdio)) != NULL)
        {
            const char *name = dt_keyname(mdio);
            APTR        phy  = NULL;

            if (name == NULL || name[0] != 'm' || name[1] != 'd' ||
                name[2] != 'i' || name[3] != 'o')
                continue;

            while ((phy = dt_getchild(mdio, phy)) != NULL)
            {
                ULONG a = dt_cells(phy, "reg", 0xffffffffUL);

                if (a < 32)
                {
                    out->phy = (UBYTE)a;
                    break;
                }
            }
            break;
        }
    }

    dt_closekey(root);
    return TRUE;
}

BOOL netdev_dtree_ram_covers(ULONG addr, ULONG len)
{
    APTR  root;
    APTR  child = NULL;
    ULONG ca;
    ULONG cs;
    BOOL  hit = FALSE;

    if (dt_base == NULL)
        dt_base = OpenResource((CONST_STRPTR)"devicetree.resource");
    if (dt_base == NULL || len == 0)
        return FALSE;

    root = dt_openkey("/");
    if (root == NULL)
        return FALSE;
    ca = dt_cells(root, "#address-cells", 2);
    cs = dt_cells(root, "#size-cells", 1);

    while (!hit && (child = dt_getchild(root, child)) != NULL)
    {
        APTR         p;
        const UBYTE *v;
        ULONG        n;
        ULONG        i;

        if (!dt_stringlist_has(child, "device_type", "memory"))
            continue;
        p = dt_findprop(child, "reg");
        if (p == NULL || ca == 0 || cs == 0)
            continue;
        v = dt_propvalue(p);
        n = dt_proplen(p) / 4;

        for (i = 0; i + ca + cs <= n; i += ca + cs)
        {
            ULONG lo   = dt_cell(v, i + ca - 1);
            ULONG size = dt_cell(v, i + ca + cs - 1);

            if (ca > 1 && dt_cell(v, i + ca - 2) != 0)
                continue;
            if (addr >= lo && addr - lo < size && len <= size - (addr - lo))
            {
                hit = TRUE;
                break;
            }
        }
    }

    dt_closekey(root);
    return hit;
}

/* ----------------------------------------------------------- gic400 ---- */

static struct Library *gic_base;
static UWORD           gic_users;

#ifndef NETDEV_DTREE_TEST
static ULONG gic_add(ULONG irq, struct Interrupt *is)
{
    register ULONG            d0 __asm("d0") = irq;
    register ULONG            d1 __asm("d1") = 0;       /* priority       */
    register ULONG            d2 __asm("d2") = 0;       /* level, not edge */
    register struct Interrupt *a1 __asm("a1") = is;
    register struct Library  *a6 __asm("a6") = gic_base;
    register ULONG            r  __asm("d0");

    __asm volatile ("jsr a6@(-30:W)"
                    : "=r" (r) : "r" (a6), "0" (d0), "r" (d1), "r" (d2), "r" (a1)
                    : "cc", "memory", "a0");
    return r;
}

static ULONG gic_rem(ULONG irq, struct Interrupt *is)
{
    register ULONG            d0 __asm("d0") = irq;
    register struct Interrupt *a1 __asm("a1") = is;
    register struct Library  *a6 __asm("a6") = gic_base;
    register ULONG            r  __asm("d0");

    __asm volatile ("jsr a6@(-36:W)"
                    : "=r" (r) : "r" (a6), "0" (d0), "r" (a1)
                    : "cc", "memory", "d1", "a0");
    return r;
}
#else
static ULONG gic_add(ULONG irq, struct Interrupt *is) { (VOID)irq; (VOID)is; return 0; }
static ULONG gic_rem(ULONG irq, struct Interrupt *is) { (VOID)irq; (VOID)is; return 0; }
#endif

BOOL netdev_dtree_int_add(ULONG irq, struct Interrupt *is)
{
    if (irq == 0)
        return FALSE;

    if (gic_base == NULL)
    {
        gic_base = OpenLibrary((CONST_STRPTR)"gic400.library", 0);
        if (gic_base == NULL)
            return FALSE;
    }

    gic_add(irq, is);
    gic_users++;
    return TRUE;
}

VOID netdev_dtree_int_rem(ULONG irq, struct Interrupt *is)
{
    if (gic_base == NULL || irq == 0)
        return;

    gic_rem(irq, is);

    if (gic_users != 0 && --gic_users == 0)
    {
        CloseLibrary(gic_base);
        gic_base = NULL;
    }
}
