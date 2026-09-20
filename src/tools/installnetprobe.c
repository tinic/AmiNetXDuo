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
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/interrupts.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <exec/libraries.h>
#include <libraries/configvars.h>
#include <resources/card.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/expansion.h>

#include "aminetxduo/version.h"
#include "../netdev/netdev_dtree.h"

const char *const tool_name = "InstallNetProbe";

static const char version_tag[] __attribute__((used)) =
    TOOL_VERSTAG("InstallNetProbe");

#define TEMPLATE "EMU68/S,GENET/S,WIFIPI/S,CARD/K"

enum
{
    ARG_EMU68 = 0,
    ARG_GENET,
    ARG_WIFIPI,
    ARG_CARD,
    ARG_COUNT
};

struct ExpansionBase *ExpansionBase;
struct Library       *CardResource;

/* card.resource's proto headers are not portable across the two supported
   Amiga GCC layouts.  These three LVOs are the same small stubs the driver
   uses: OwnCard -6, ReleaseCard -12, CopyTuple -72. */
static struct CardHandle *probe_own_card(struct CardHandle *handle)
{
    register struct Library    *_a6 __asm("a6") = CardResource;
    register struct CardHandle *_a1 __asm("a1") = handle;
    register struct CardHandle *result __asm("d0");

    __asm __volatile ("jsr a6@(-0x6)"
                      : "=r" (result)
                      : "r" (_a6), "r" (_a1)
                      : "d1", "a0", "cc", "memory");
    return result;
}

static VOID probe_release_card(struct CardHandle *handle, ULONG flags)
{
    register struct Library    *_a6 __asm("a6") = CardResource;
    register struct CardHandle *_a1 __asm("a1") = handle;
    register ULONG              _d0 __asm("d0") = flags;

    __asm __volatile ("jsr a6@(-0xc)"
                      : "+r" (_d0)
                      : "r" (_a6), "r" (_a1)
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
                      : "=r" (result)
                      : "r" (_a6), "r" (_a1), "r" (_a0), "r" (_d1),
                        "0" (_d0)
                      : "cc", "memory");
    return (BOOL)(result != 0);
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

static ULONG ignore_card_event(VOID)
{
    return 0;
}

/* Read only the identity tuples.  IFAVAILABLE means an active driver is never
   displaced or queued behind; in that case the Installer's driver-file
   fallback supplies the answer.  No reset, COR write or device open occurs. */
static BOOL pcmcia_identity(UWORD *manufacturer, UWORD *product,
                            UBYTE *function, BOOL *have_function)
{
    static struct Interrupt  removed;
    static struct Interrupt  inserted;
    static struct Interrupt  status;
    static struct CardHandle handle;
    struct CardHandle       *owner;
    UBYTE                    tuple[6] = { 0 };
    BOOL                     have_manf = FALSE;

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
        return FALSE;
    }

    if (probe_copy_tuple(&handle, tuple, 0x20, sizeof(tuple)) &&
        tuple[1] >= 4)
    {
        *manufacturer = (UWORD)(((UWORD)tuple[3] << 8) | tuple[2]);
        *product = (UWORD)(((UWORD)tuple[5] << 8) | tuple[4]);
        have_manf = TRUE;
    }
    if (probe_copy_tuple(&handle, tuple, 0x21, sizeof(tuple)) &&
        tuple[1] >= 1)
    {
        *function = tuple[2];
        *have_function = TRUE;
    }

    probe_release_card(&handle, CARDF_REMOVEHANDLE);
    handle.cah_CardNode.ln_Name = NULL;
    return have_manf || *have_function;
}

static BOOL pcmcia_card_present(const char *name)
{
    UWORD manufacturer = 0;
    UWORD product = 0;
    UBYTE function = 0xff;
    BOOL  have_function = FALSE;
    BOOL  known_3com;
    BOOL  cnet_style;

    if (!pcmcia_identity(&manufacturer, &product, &function,
                         &have_function))
        return FALSE;

    known_3com = (BOOL)(manufacturer == 0x0101 &&
                         (product == 0x0589 || product == 0x0556 ||
                          product == 0x0035));
    /* CNet/CN40BC and compatible NE2000 cards commonly omit MANFID and state
       only the LAN function.  Do not call every unknown LAN tuple NE2000:
       Prism2 is also a LAN function and needs a different driver. */
    cnet_style = (BOOL)(manufacturer == 0 && product == 0 &&
                        have_function && function == 6);
    if (same(name, "pcmcia"))
        return (BOOL)(known_3com || cnet_style);
    if (same(name, "cnet"))
        return cnet_style;
    if (same(name, "3c589"))
        return (BOOL)(manufacturer == 0x0101 && product == 0x0589);
    if (same(name, "3ccfem556"))
        return (BOOL)(manufacturer == 0x0101 && product == 0x0556);
    if (same(name, "3cxem556"))
        return (BOOL)(manufacturer == 0x0101 && product == 0x0035);
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

int main(int argc, char **argv)
{
    LONG           args[ARG_COUNT] = { 0, 0, 0 };
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
        (args[ARG_WIFIPI] != 0) + (args[ARG_CARD] != 0) != 1)
    {
        FreeArgs(rda);
        return RETURN_ERROR;
    }

    if (args[ARG_EMU68] != 0)
        present = netdev_dtree_present();
    else if (args[ARG_GENET] != 0)
        present = netdev_dtree_find("brcm,bcm2711-genet-v5", &info);
    else if (args[ARG_WIFIPI] != 0)
        present = (BOOL)(supported_wifi_model() &&
                         netdev_dtree_alias_present("mmc"));
    else
    {
        const char *card = (const char *)args[ARG_CARD];

        if (same(card, "pcmcia") || same(card, "cnet") ||
            same(card, "3c589") ||
            same(card, "3ccfem556") || same(card, "3cxem556"))
            present = pcmcia_card_present(card);
        else if (same(card, "uae"))
            present = (BOOL)(FindResident((CONST_STRPTR)"uaenet.device") !=
                             NULL);
        else
            present = classic_card_present(card);
    }

    FreeArgs(rda);
    return present ? RETURN_OK : RETURN_WARN;
}
