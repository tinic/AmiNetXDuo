/*
 * InstallNetProbe, read-only network-device discovery for the Installer.
 *
 * The Installer language cannot open an Exec resource.  This deliberately
 * tiny helper exposes yes/no answers and lives beside the Installer rather
 * than in C:.  It reads Emu68's device tree through the same code as
 * anxgenet.device and the ConfigDev list expansion.library built at boot.  It
 * never opens a network device and therefore cannot reset, claim or otherwise
 * disturb hardware on a running machine.
 *
 * Exit status is RETURN_OK when the requested feature is present and
 * RETURN_WARN when it is not.  Bad arguments are RETURN_ERROR.
 * MEMORY checks installed RAM, including allocations, against a byte count.
 * DEVICE checks an exact Exec device/resident name without opening it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/interrupts.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <libraries/configvars.h>
#include <resources/card.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/expansion.h>

#include "aminetxduo/version.h"
#include "../netdev/netdev_dtree.h"
#include "netprobe_card.h"
#include "netprobe_memory.h"
#include "netprobe_device.h"

const char *const tool_name = "InstallNetProbe";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("InstallNetProbe");

#define TEMPLATE "EMU68/S,GENET/S,GENETOK/S,WIFIPI/S,CARD/K,MEMORY/N/K,DEVICE/K"

enum
{
    ARG_EMU68 = 0,
    ARG_GENET,
    ARG_GENETOK,
    ARG_WIFIPI,
    ARG_CARD,
    ARG_MEMORY,
    ARG_DEVICE,
    ARG_COUNT
};

struct ExpansionBase *ExpansionBase;
struct Library       *CardResource;

/* card.resource's proto headers are not portable across the two supported
   Amiga GCC layouts.  These LVOs are small stubs like the driver's:
   OwnCard -6, ReleaseCard -12, ReadCardStatus -36, CopyTuple -72.  Each may
   destroy d0/d1/a0/a1, so an argument register is a "+r" operand (#70). */
static struct CardHandle *probe_own_card(struct CardHandle *handle)
{
    register struct Library    *_a6 __asm("a6") = CardResource;
    register struct CardHandle *_a1 __asm("a1") = handle;
    register struct CardHandle *result __asm("d0");

    __asm __volatile ("jsr a6@(-0x6)"
                      : "=r" (result), "+r" (_a1)
                      : "r" (_a6)
                      : "d1", "a0", "cc", "memory");
    return result;
}

static VOID probe_release_card(struct CardHandle *handle, ULONG flags)
{
    register struct Library    *_a6 __asm("a6") = CardResource;
    register struct CardHandle *_a1 __asm("a1") = handle;
    register ULONG              _d0 __asm("d0") = flags;

    __asm __volatile ("jsr a6@(-0xc)"
                      : "+r" (_d0), "+r" (_a1)
                      : "r" (_a6)
                      : "d1", "a0", "cc", "memory");
}

static BOOL probe_copy_tuple(struct CardHandle *handle, UBYTE *buffer,
                             ULONG code, ULONG size)
{
    register struct Library    *_a6 __asm("a6") = CardResource;
    register struct CardHandle *_a1 __asm("a1") = handle;
    register UBYTE             *_a0 __asm("a0") = buffer;
    register ULONG              _d1 __asm("d1") = code;
    register ULONG              _d0 __asm("d0") = size;
    register LONG               result __asm("d0");

    __asm __volatile ("jsr a6@(-0x48)"
                      : "=r" (result), "+r" (_a1), "+r" (_a0), "+r" (_d1)
                      : "r" (_a6), "0" (_d0)
                      : "cc", "memory");
    return (BOOL)(result != 0);
}

/* ReadCardStatus -36: Gayle's status byte, nothing claimed. */
static UBYTE probe_card_status(VOID)
{
    register struct Library *_a6 __asm("a6") = CardResource;
    register ULONG           result __asm("d0");

    __asm __volatile ("jsr a6@(-0x24)"
                      : "=r" (result)
                      : "r" (_a6)
                      : "d1", "a0", "a1", "cc", "memory");
    return (UBYTE)result;
}

typedef struct ClassicCard
{
    const char *name;
    UWORD       manufacturer;
    UBYTE       product;
} ClassicCard;

/* Autoconfig identities, not driver file names.  ZZ9000 exposes the same
   Ethernet function through its Zorro II and Zorro III products. */
static const ClassicCard classic_cards[] =
{
    { "a2065",     514,    112 },
    { "ariadne",   2167,   201 },
    { "ariadne2",  2167,   202 },
    { "hydra",     2121,     1 },
    { "lanrover",  1023,   254 },
    { "xsurf",     4626,    23 },
    { "xsurf100",  4626,   100 },
    { "zz9000z2",  0x6d6e,   3 },
    { "zz9000z3",  0x6d6e,   4 }
};

static BOOL same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b)
    {
        a++;
        b++;
    }
    return (BOOL)(*a == *b);
}

static BOOL configdev_present(UWORD manufacturer, UBYTE product)
{
    struct ConfigDev *cd;

    cd = FindConfigDev(NULL, (LONG)manufacturer, (LONG)product);
    return (BOOL)(cd != NULL);
}

static BOOL classic_card_present(const char *name)
{
    struct Library *library;
    BOOL            present = FALSE;
    UWORD           i;

    library = OpenLibrary((CONST_STRPTR)"expansion.library", 36);
    if (library == NULL)
        return FALSE;
    ExpansionBase = (struct ExpansionBase *)library;

    if (same(name, "zz9000"))
    {
        present = (BOOL)(configdev_present(0x6d6e, 3) ||
                         configdev_present(0x6d6e, 4));
    }
    else
    {
        for (i = 0; i < (UWORD)(sizeof(classic_cards) /
                                sizeof(classic_cards[0])); i++)
        {
            if (same(name, classic_cards[i].name))
            {
                present = configdev_present(classic_cards[i].manufacturer,
                                            classic_cards[i].product);
                break;
            }
        }
    }

    ExpansionBase = NULL;
    CloseLibrary(library);
    return present;
}

/* Gayle's attribute window: CIS byte n at 0xA00000 + 2n (netdev_pcmcia.c).
   The walk stops at the first END, long before the 64 KB of CIS bytes. */
#define PC_ATTR_BASE      0x00a00000UL
#define PC_ATTR_CIS_BYTES 0x00010000UL

static uint8_t attr_read(void *ctx, uint32_t n)
{
    (VOID)ctx;
    return *(volatile UBYTE *)(PC_ATTR_BASE + n * 2UL);
}

/* The owner's handle name, read under Forbid while the handle is live. */
static BOOL owner_is(struct CardHandle *owner, const char *name)
{
    BOOL  is = FALSE;
    char *n;

    if (owner == (struct CardHandle *)-1)
        return FALSE;
    Forbid();
    if (TypeOfMem(owner) != 0)
    {
        n = owner->cah_CardNode.ln_Name;
        if (n != NULL && TypeOfMem(n) != 0)
            is = same(n, name);
    }
    Permit();
    return is;
}

static ULONG ignore_card_event(VOID)
{
    return 0;
}

/* Read only the identity tuples.  IFAVAILABLE means an active driver is never
   displaced or queued behind.  A card another driver owns -- anxnet.device
   started from the startup files, on a reinstall -- is read straight from
   attribute memory instead: CopyTuple() needs the owner's handle, and a read
   of the CIS changes nothing on a configured card.  No reset, COR write or
   device open occurs. */
static BOOL pcmcia_identity(NetprobeCis *cis)
{
    static struct Interrupt  removed;
    static struct Interrupt  inserted;
    static struct Interrupt  status;
    static struct CardHandle handle;
    struct CardHandle       *owner;
    /* CopyTuple() writes the two header bytes, then up to size body
       bytes; the autodoc asks for size + 8. */
    UBYTE                    tuple[4 + 8] = { 0 };

    CardResource = OpenResource((CONST_STRPTR)CARDRESNAME);
    if (CardResource == NULL)
        return FALSE;

    removed.is_Node.ln_Type = NT_INTERRUPT;
    removed.is_Node.ln_Name = (char *)"InstallNetProbe removed";
    removed.is_Code = (VOID (*)())ignore_card_event;
    inserted.is_Node.ln_Type = NT_INTERRUPT;
    inserted.is_Node.ln_Name = (char *)"InstallNetProbe inserted";
    inserted.is_Code = (VOID (*)())ignore_card_event;
    status.is_Node.ln_Type = NT_INTERRUPT;
    status.is_Node.ln_Name = (char *)"InstallNetProbe status";
    status.is_Code = (VOID (*)())ignore_card_event;

    handle.cah_CardNode.ln_Type = 0;
    handle.cah_CardNode.ln_Pri = 0;
    handle.cah_CardNode.ln_Name = (char *)"InstallNetProbe";
    handle.cah_CardFlags = CARDF_IFAVAILABLE;
    handle.cah_CardRemoved = &removed;
    handle.cah_CardInserted = &inserted;
    handle.cah_CardStatus = &status;

    owner = probe_own_card(&handle);
    if (owner != NULL)
    {
        /* IFAVAILABLE did not enqueue this handle, so there is nothing to
           release or remove from card.resource's owner list. */
        handle.cah_CardNode.ln_Name = NULL;
        if ((probe_card_status() & CARD_STATUSF_CCDET) == 0)
            return FALSE;
        /* prism2.device's card: a wireless card whose CIS need not say so,
           so never an NE2000 here.  The driver file's Prism2 default stands
           and the PCMCIA type page is drawn. */
        if (owner_is(owner, "prism2.device"))
            return FALSE;
        return (BOOL)netprobe_cis_walk(attr_read, NULL, PC_ATTR_CIS_BYTES,
                                       cis);
    }

    if (probe_copy_tuple(&handle, tuple, 0x20, 4) &&
        tuple[1] >= 4)
    {
        cis->manf = (UWORD)(((UWORD)tuple[3] << 8) | tuple[2]);
        cis->prod = (UWORD)(((UWORD)tuple[5] << 8) | tuple[4]);
        cis->have_manf = 1;
    }
    if (probe_copy_tuple(&handle, tuple, 0x21, 4) &&
        tuple[1] >= 1)
    {
        cis->funcid = tuple[2];
        cis->have_funcid = 1;
    }
    /* The first FUNCE: on an 802.11 card, its LAN technology. */
    if (probe_copy_tuple(&handle, tuple, 0x22, 4) &&
        tuple[1] >= 2)
    {
        cis->funce[0] = tuple[2];
        cis->funce[1] = tuple[3];
        cis->have_funce = 1;
    }

    probe_release_card(&handle, CARDF_REMOVEHANDLE);
    handle.cah_CardNode.ln_Name = NULL;
    return (BOOL)(cis->have_manf || cis->have_funcid);
}

static BOOL pcmcia_card_present(const char *name)
{
    NetprobeCis cis = { 0 };
    int         kind;

    if (!pcmcia_identity(&cis))
        return FALSE;

    kind = netprobe_pc_kind(&cis);
    if (same(name, "pcmcia"))
        return (BOOL)(kind != NETPROBE_PC_NONE);
    if (same(name, "prism2"))
        return (BOOL)(kind == NETPROBE_PC_PRISM2);
    if (same(name, "xircom"))
        return (BOOL)(kind == NETPROBE_PC_XIRCOM);
    if (same(name, "cnet"))
        return (BOOL)(kind == NETPROBE_PC_NE2000);
    if (same(name, "3c589"))
        return (BOOL)(kind == NETPROBE_PC_3C589);
    if (same(name, "3ccfem556"))
        return (BOOL)(kind == NETPROBE_PC_3CCFEM556);
    if (same(name, "3cxem556"))
        return (BOOL)(kind == NETPROBE_PC_3CXEM556);
    return FALSE;
}

static BOOL supported_wifi_model(VOID)
{
    static const char *const models[] =
    {
        "raspberrypi,model-zero-2-w",
        "raspberrypi,3-model-b",
        "raspberrypi,3-model-a-plus",
        "raspberrypi,3-model-b-plus",
        "raspberrypi,4-model-b",
        "raspberrypi,4-compute-module"
    };
    UWORD i;

    for (i = 0; i < (UWORD)(sizeof(models) / sizeof(models[0])); i++)
        if (netdev_dtree_root_compatible(models[i]))
            return TRUE;
    return FALSE;
}

/* Emu68 1.1 alpha.1 maps /scb's ranges into its 68k peripheral window, from
   0xF2000000 up and below 0xFC000000, and rewrites the tree to say so.  1.0.x
   leaves /scb at the BCM2711's own physical addresses, 0xFC000000 and up,
   which no 68k mapping covers.  anxgenet.device uses the translated base as
   it stands, so a GENET up there is one it cannot reach. */
#define BCM2711_PERIPH_PHYS 0xFC000000UL

static BOOL genet_usable(VOID)
{
    NetdevDtInfo info;

    return (BOOL)(netdev_dtree_find("brcm,bcm2711-genet-v5", &info) &&
                  info.base != 0 && info.base < BCM2711_PERIPH_PHYS);
}

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT] = { 0 };
    struct RDArgs *rda;
    BOOL           present;
    NetdevDtInfo   info;

    (VOID)argv;

    if (argc == 0)
        return RETURN_FAIL;
    rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL);
    if (rda == NULL)
        return RETURN_ERROR;
    if ((args[ARG_EMU68] != 0) + (args[ARG_GENET] != 0) +
        (args[ARG_GENETOK] != 0) + (args[ARG_WIFIPI] != 0) +
        (args[ARG_CARD] != 0) + (args[ARG_MEMORY] != 0) +
        (args[ARG_DEVICE] != 0) != 1)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (args[ARG_DEVICE] != 0)
        present = netprobe_device_present((const char *)args[ARG_DEVICE]);
    else if (args[ARG_MEMORY] != 0)
    {
        LONG wanted = *(const LONG *)args[ARG_MEMORY];
        ULONG total = netprobe_memory_total();

        /* Exec excludes its memory headers and the low Chip RAM reserved at
           boot. Round to the next 64 KiB so a fitted 8 MiB is still 8 MiB.
           MEMF_TOTAL counts allocated RAM too: an open Installer, another
           stack or programs in RAM: must not change the hardware tier. */
        if (total <= 0xffff0000UL)
            total = (total + 65535UL) & ~65535UL;
        if (wanted <= 0)
        {
            FreeArgs(rda);
            return RETURN_ERROR;
        }
        present = (BOOL)(total >= (ULONG)wanted);
    }
    else if (args[ARG_EMU68] != 0)
        present = netdev_dtree_present();
    else if (args[ARG_GENET] != 0)
        present = netdev_dtree_find("brcm,bcm2711-genet-v5", &info);
    else if (args[ARG_GENETOK] != 0)
        present = genet_usable();
    else if (args[ARG_WIFIPI] != 0)
        /* The alias is what anxwifipi.device reads its base through; the
           live brcmfmac node under an enabled mmcnr is the radio itself.
           A CM4S, a CM4 without wireless or dtoverlay=disable-wifi has
           mmcnr disabled. */
        present = (BOOL)(supported_wifi_model() &&
                         netdev_dtree_alias_present("mmc") &&
                         netdev_dtree_live("brcm,bcm4329-fmac"));
    else
    {
        const char *card = (const char *)args[ARG_CARD];

        if (same(card, "pcmcia") || same(card, "cnet") ||
            same(card, "3c589") ||
            same(card, "3ccfem556") || same(card, "3cxem556") ||
            same(card, "prism2") || same(card, "xircom"))
            present = pcmcia_card_present(card);
        else if (same(card, "uae"))
            present = netprobe_device_present("uaenet.device");
        else
            present = classic_card_present(card);
    }

    FreeArgs(rda);
    return present ? RETURN_OK : RETURN_WARN;
}
