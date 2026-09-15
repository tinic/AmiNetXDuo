/*
 * The device-tree reader, against a tree built in memory in the shape Emu68
 * 1.1 publishes on a Raspberry Pi 4: /scb with two-cell addresses and sizes
 * and a `ranges` that moves the bus into the 68k space, the GENET node under
 * it, the PHY under an mdio child, and memory@0 at the root.  The eleven
 * devicetree.resource entry points are supplied here over that tree, which
 * is the seam netdev_dtree.c leaves for exactly this.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>

#define NETDEV_DTREE_TEST 1
#include "netdev_dtree.h"

struct ExecBase *SysBase;

/* ------------------------------------------------------------- the tree */

typedef struct Prop
{
    const char  *name;
    const UBYTE *value;
    ULONG        len;
} Prop;

typedef struct DtNode
{
    const char        *name;
    struct DtNode     *parent;
    struct DtNode   **children;    /* NULL-terminated */
    const Prop        *props;       /* name == NULL ends it */
} DtNode;

#define CELL(x) (UBYTE)((x) >> 24), (UBYTE)((x) >> 16), (UBYTE)((x) >> 8), (UBYTE)(x)

static const UBYTE root_ac[]  = { CELL(2) };
static const UBYTE root_sc[]  = { CELL(1) };
static const UBYTE one[]      = { CELL(1) };
static const UBYTE two[]      = { CELL(2) };
static const UBYTE zero[]     = { CELL(0) };

static const UBYTE mem_reg[]  = { CELL(0), CELL(0x01000000), CELL(0x3BA00000),
                                  CELL(0), CELL(0x40000000), CELL(0x39E00000) };
static const UBYTE scb_ranges[] =
    { CELL(0), CELL(0x7C000000), CELL(0), CELL(0xF6000000), CELL(0), CELL(0x03800000),
      CELL(0), CELL(0x40000000), CELL(0), CELL(0xF9800000), CELL(0), CELL(0x00800000) };
static const UBYTE genet_reg[] = { CELL(0), CELL(0x7D580000), CELL(0), CELL(0x10000) };
static const UBYTE genet_irq[] = { CELL(0), CELL(0x9D), CELL(4), CELL(0), CELL(0x9E), CELL(4) };
static const UBYTE genet_mac[] = { 0x98, 0xFE, 0x54, 0x2D, 0xA5, 0x1E };
static const UBYTE mdio_reg[]  = { CELL(0xe14), CELL(8) };
static const UBYTE phy_reg[]   = { CELL(1) };
static const UBYTE decoy_reg[] = { CELL(0), CELL(0x7D000000), CELL(0), CELL(0x100) };

#define STR(s) (const UBYTE *)(s), sizeof(s)

static DtNode root, memory0, scb, genet, mdio, phy, soc, decoy;

static const Prop root_props[] =
{
    { "#address-cells", root_ac, 4 },
    { "#size-cells",    root_sc, 4 },
    { "compatible",     STR("raspberrypi,4-model-b\0brcm,bcm2711") },
    { NULL, NULL, 0 }
};
static const Prop memory0_props[] =
{
    { "device_type", STR("memory") },
    { "reg",         mem_reg, sizeof(mem_reg) },
    { NULL, NULL, 0 }
};
static const Prop scb_props[] =
{
    { "compatible",     STR("simple-bus") },
    { "#address-cells", two, 4 },
    { "#size-cells",    two, 4 },
    { "ranges",         scb_ranges, sizeof(scb_ranges) },
    { NULL, NULL, 0 }
};
static const Prop genet_props[] =
{
    { "compatible",        STR("brcm,bcm2711-genet-v5") },
    { "reg",               genet_reg, sizeof(genet_reg) },
    { "#address-cells",    one, 4 },
    { "#size-cells",       one, 4 },
    { "interrupts",        genet_irq, sizeof(genet_irq) },
    { "local-mac-address", genet_mac, sizeof(genet_mac) },
    { "status",            STR("okay") },
    { "phy-mode",          STR("rgmii-rxid") },
    { NULL, NULL, 0 }
};
static const Prop mdio_props[] =
{
    { "compatible",     STR("brcm,genet-mdio-v5") },
    { "reg",            mdio_reg, sizeof(mdio_reg) },
    { "#address-cells", one, 4 },
    { "#size-cells",    zero, 4 },
    { NULL, NULL, 0 }
};
static const Prop phy_props[] =
{
    { "reg", phy_reg, sizeof(phy_reg) },
    { NULL, NULL, 0 }
};
static const Prop soc_props[] =
{
    { "compatible",     STR("simple-bus") },
    { "#address-cells", one, 4 },
    { "#size-cells",    one, 4 },
    { NULL, NULL, 0 }
};
/* A second GENET node, disabled, and listed BEFORE the real one: the walk
   must pass it by. */
static const Prop decoy_props[] =
{
    { "compatible", STR("brcm,bcm2711-genet-v5") },
    { "reg",        decoy_reg, sizeof(decoy_reg) },
    { "status",     STR("disabled") },
    { NULL, NULL, 0 }
};

static DtNode *root_children[]  = { &soc, &memory0, &scb, NULL };
static DtNode *soc_children[]   = { &decoy, NULL };
static DtNode *scb_children[]   = { &genet, NULL };
static DtNode *genet_children[] = { &mdio, NULL };
static DtNode *mdio_children[]  = { &phy, NULL };
static DtNode *none[]           = { NULL };

static void build(void)
{
    root    = (DtNode){ "/", NULL, root_children, root_props };
    memory0 = (DtNode){ "memory@0", &root, none, memory0_props };
    scb     = (DtNode){ "scb", &root, scb_children, scb_props };
    genet   = (DtNode){ "ethernet@7d580000", &scb, genet_children, genet_props };
    mdio    = (DtNode){ "mdio@e14", &genet, mdio_children, mdio_props };
    phy     = (DtNode){ "ethernet-phy@1", &mdio, none, phy_props };
    soc     = (DtNode){ "soc", &root, soc_children, soc_props };
    decoy   = (DtNode){ "ethernet@7d000000", &soc, none, decoy_props };
}

/* ------------------------------------------ the eleven, over that tree */

static int opens, closes;

APTR dt_openkey(const char *name)
{
    opens++;
    return (strcmp(name, "/") == 0) ? (APTR)&root : NULL;
}

VOID dt_closekey(APTR key)
{
    (void)key;
    closes++;
}

APTR dt_getchild(APTR key, APTR prev)
{
    DtNode **c = ((DtNode *)key)->children;
    int    i;

    if (prev == NULL)
        return c[0];
    for (i = 0; c[i] != NULL; i++)
        if (c[i] == (DtNode *)prev)
            return c[i + 1];
    return NULL;
}

APTR dt_findprop(APTR key, const char *name)
{
    const Prop *p;

    for (p = ((DtNode *)key)->props; p->name != NULL; p++)
        if (strcmp(p->name, name) == 0)
            return (APTR)p;
    return NULL;
}

ULONG        dt_proplen(APTR prop)   { return ((const Prop *)prop)->len; }
const UBYTE *dt_propvalue(APTR prop) { return ((const Prop *)prop)->value; }
APTR         dt_getparent(APTR key)  { return ((DtNode *)key)->parent; }
const char  *dt_keyname(APTR key)    { return ((DtNode *)key)->name; }

/* Exec, as much of it as netdev_dtree.c reaches. */
static int resource_present = 1;
APTR OpenResource(const UBYTE *name)
{
    (void)name;
    return resource_present ? (APTR)&root : NULL;
}
struct Library *OpenLibrary(const UBYTE *name, ULONG ver)
{
    (void)name; (void)ver;
    return NULL;
}
VOID CloseLibrary(struct Library *lib) { (void)lib; }

/* ---------------------------------------------------------------- tests */

static int failures;

static void expect_ulong(const char *what, ULONG got, ULONG want)
{
    if (got == want)
    {
        printf("ok   %s = 0x%lx\n", what, (unsigned long)got);
        return;
    }
    printf("FAIL %s: got 0x%lx, want 0x%lx\n", what, (unsigned long)got,
           (unsigned long)want);
    failures++;
}

int main(void)
{
    NetdevDtInfo dt;

    build();

    /* The real node, through /scb's ranges, past the disabled decoy. */
    expect_ulong("find genet", netdev_dtree_find("brcm,bcm2711-genet-v5", &dt), 1);
    expect_ulong("base", dt.base, 0xF7580000UL);
    expect_ulong("size", dt.size, 0x10000UL);
    expect_ulong("irq (SPI 157 + 32)", dt.irq, 189);
    expect_ulong("mac_ok", dt.mac_ok, 1);
    expect_ulong("mac[0]", dt.mac[0], 0x98);
    expect_ulong("mac[5]", dt.mac[5], 0x1E);
    expect_ulong("phy", dt.phy, 1);
    expect_ulong("every open closed", (ULONG)(opens - closes), 0);

    /* Nothing else answers to a name the tree does not carry. */
    expect_ulong("find nonsense", netdev_dtree_find("acme,frobnicator", &dt), 0);

    /* RAM: inside a memory@0 range, or not. */
    expect_ulong("ram covers the rings", netdev_dtree_ram_covers(0x08383C80UL, 256UL << 10), 1);
    expect_ulong("ram: chip RAM is not it", netdev_dtree_ram_covers(0x00100000UL, 16), 0);
    expect_ulong("ram: crossing the end", netdev_dtree_ram_covers(0x3CA00000UL - 100UL, 4096), 0);
    expect_ulong("ram: the second bank", netdev_dtree_ram_covers(0x50000000UL, 1UL << 20), 1);
    expect_ulong("ram: zero length", netdev_dtree_ram_covers(0x08000000UL, 0), 0);

    /* No devicetree.resource: not an Emu68, no unit. */
    resource_present = 0;
    expect_ulong("no resource", netdev_dtree_find("brcm,bcm2711-genet-v5", &dt), 0);

    if (failures != 0)
    {
        printf("test_netdev_dtree: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_netdev_dtree: all passed\n");
    return 0;
}
