/*
 * The PC Card identity, read without disturbing the card.  See
 * netprobe_pcmcia.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include <exec/types.h>
#include <exec/interrupts.h>
#include <exec/memory.h>
#include <resources/card.h>
#include <proto/exec.h>

#include "netprobe_pcmcia.h"

extern const char *const tool_name;

static struct Library *CardResource;

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

/* Gayle's attribute window: CIS byte n at 0xA00000 + 2n (netdev_pcmcia.c).
   The walk stops at the first END, long before the 64 KB of CIS bytes. */
#define PC_ATTR_BASE      0x00a00000UL
#define PC_ATTR_CIS_BYTES 0x00010000UL

static uint8_t attr_read(void *ctx, uint32_t n)
{
    (VOID)ctx;
    return *(volatile UBYTE *)(PC_ATTR_BASE + n * 2UL);
}

static uint8_t buf_read(void *ctx, uint32_t n)
{
    return ((const UBYTE *)ctx)[n];
}

/* The owner's handle name, copied under Forbid while the handle is live. */
static VOID owner_name(struct CardHandle *owner, char *out)
{
    const char *n;
    UWORD       i = 0;

    if (owner != (struct CardHandle *)-1)
    {
        Forbid();
        if (TypeOfMem(owner) != 0)
        {
            n = owner->cah_CardNode.ln_Name;
            if (n != NULL && TypeOfMem((APTR)n) != 0)
                while (n[i] != '\0' && i < NETPROBE_OWNER_MAX - 1)
                {
                    out[i] = n[i];
                    i++;
                }
        }
        Permit();
    }
    out[i] = '\0';
}

static ULONG ignore_card_event(VOID)
{
    return 0;
}

void netprobe_pcmcia_read(NetprobeSlot *slot)
{
    static struct Interrupt  removed;
    static struct Interrupt  inserted;
    static struct Interrupt  status;
    static struct CardHandle handle;
    /* CopyTuple() writes the two header bytes, then up to size body bytes;
       the autodoc asks for size + 8.  Static: CheckNetDevice's frame is
       512 bytes. */
    static UBYTE             tuple[2 + 255 + 8];
    struct CardHandle       *owner;
    NetprobeCis             *cis = &slot->cis;

    cis->have_manf = 0;
    cis->have_funcid = 0;
    cis->have_funce = 0;
    slot->slot = NETPROBE_SLOT_NONE;
    slot->have_vers1 = 0;
    slot->vers1[0] = '\0';
    slot->owner[0] = '\0';

    CardResource = OpenResource((CONST_STRPTR)CARDRESNAME);
    if (CardResource == NULL)
        return;

    slot->slot = NETPROBE_SLOT_EMPTY;
    if ((probe_card_status() & CARD_STATUSF_CCDET) == 0)
        return;

    removed.is_Node.ln_Type = NT_INTERRUPT;
    removed.is_Node.ln_Name = (char *)tool_name;
    removed.is_Code = (VOID (*)())ignore_card_event;
    inserted.is_Node.ln_Type = NT_INTERRUPT;
    inserted.is_Node.ln_Name = (char *)tool_name;
    inserted.is_Code = (VOID (*)())ignore_card_event;
    status.is_Node.ln_Type = NT_INTERRUPT;
    status.is_Node.ln_Name = (char *)tool_name;
    status.is_Code = (VOID (*)())ignore_card_event;

    handle.cah_CardNode.ln_Type = 0;
    handle.cah_CardNode.ln_Pri = 0;
    handle.cah_CardNode.ln_Name = (char *)tool_name;
    handle.cah_CardFlags = CARDF_IFAVAILABLE;
    handle.cah_CardRemoved = &removed;
    handle.cah_CardInserted = &inserted;
    handle.cah_CardStatus = &status;

    /* IFAVAILABLE: an active driver is never displaced or queued behind. */
    owner = probe_own_card(&handle);
    if (owner != NULL)
    {
        /* Not enqueued, so nothing to release or remove. */
        handle.cah_CardNode.ln_Name = NULL;
        if (owner == (struct CardHandle *)-1 ||
            (probe_card_status() & CARD_STATUSF_CCDET) == 0)
            return;
        slot->slot = NETPROBE_SLOT_OWNED;
        owner_name(owner, slot->owner);
        /* A read of the CIS changes nothing on a configured card. */
        (VOID)netprobe_cis_walk(attr_read, NULL, PC_ATTR_CIS_BYTES, cis);
        slot->have_vers1 = (uint8_t)netprobe_cis_vers1(attr_read, NULL,
                                                       PC_ATTR_CIS_BYTES,
                                                       slot->vers1,
                                                       NETPROBE_VERS1_MAX);
        return;
    }

    slot->slot = NETPROBE_SLOT_FREE;
    if (probe_copy_tuple(&handle, tuple, 0x20, 4) && tuple[1] >= 4)
    {
        cis->manf = (UWORD)(((UWORD)tuple[3] << 8) | tuple[2]);
        cis->prod = (UWORD)(((UWORD)tuple[5] << 8) | tuple[4]);
        cis->have_manf = 1;
    }
    if (probe_copy_tuple(&handle, tuple, 0x21, 4) && tuple[1] >= 1)
    {
        cis->funcid = tuple[2];
        cis->have_funcid = 1;
    }
    /* The first FUNCE: on an 802.11 card, its LAN technology. */
    if (probe_copy_tuple(&handle, tuple, 0x22, 4) && tuple[1] >= 2)
    {
        cis->funce[0] = tuple[2];
        cis->funce[1] = tuple[3];
        cis->have_funce = 1;
    }
    if (probe_copy_tuple(&handle, tuple, 0x15, 255))
        slot->have_vers1 = (uint8_t)netprobe_vers1_text(buf_read, tuple, 2,
                                                        tuple[1],
                                                        slot->vers1,
                                                        NETPROBE_VERS1_MAX);

    probe_release_card(&handle, CARDF_REMOVEHANDLE);
    handle.cah_CardNode.ln_Name = NULL;
}
