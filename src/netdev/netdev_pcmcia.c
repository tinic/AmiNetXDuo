/*
 * anxnet.device: claiming the A600/A1200 PCMCIA slot.
 *
 * Every other card in the table announces itself through autoconfig and the
 * probe finds it by walking the ConfigDev list.  The PCMCIA slot announces
 * nothing.  What is there instead is card.resource, which owns the slot, and
 * a CIS in attribute memory describing what is plugged into it.
 *
 * WHAT THIS HAS TO DO THAT A ZORRO BOARD DOES NOT
 *
 *   Own the slot.  OwnCard() is how two drivers do not both drive one card,
 *   and it is also what powers the socket up.  A driver that pokes Gayle
 *   directly works right up until the machine has a second PCMCIA driver.
 *
 *   Read the CIS.  A PCMCIA slot holds an SRAM card or an IDE adapter just as
 *   readily as a network card, and driving the wrong one writes to somebody's
 *   disk.  CopyTuple() walks the tuple chain; CISTPL_FUNCID says what the card
 *   is and the answer has to be 6, LAN adapter.
 *
 *   Configure it.  A PCMCIA card decodes nothing until its Configuration
 *   Option Register is written with an index from CISTPL_CFTABLE_ENTRY.  Until
 *   then 0xA20300 reads as bus noise, which is the failure mode this file
 *   exists to avoid reporting as "no card fitted".
 *
 * The station address is NOT taken from the CIS.  The chip core reads it from
 * the RTL8019's own PROM through remote DMA, the same as every other NE2000
 * row, and a card whose CIS disagreed with its chip would be the CIS that was
 * wrong.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_internal.h"
#include "netdev_cards.h"

#include <exec/types.h>
#include <exec/nodes.h>
#include <resources/card.h>

#include <proto/exec.h>

/* proto/card.h in this NDK includes clib/card_protos.h, which the NDK does
   not ship -- the protos are clib/cardres_protos.h.  inline/card.h is the
   half that matters here: the LVO stubs, against the CardResource below. */
#include <inline/card.h>

#ifdef NETDEV_TRACE
/* Local, because netdev_device.c's is static there and this runs before any
   of it.  Same channel: raw SERDAT, which needs nothing to be open. */
static VOID pc_trace(const char *s, ULONG v)
{
    static const char hex[] = "0123456789abcdef";
    volatile UWORD   *serdat  = (volatile UWORD *)0xdff030;
    volatile UWORD   *serdatr = (volatile UWORD *)0xdff018;
    char              buf[12];
    const char       *p;
    int               i;

    for (i = 0; i < 8; i++)
        buf[i] = hex[(v >> ((7 - i) * 4)) & 0xf];
    buf[8] = '\r'; buf[9] = '\n'; buf[10] = '\0';

    for (p = s; *p != '\0'; p++)
    {
        ULONG guard = 200000;
        while ((*serdatr & 0x2000) == 0 && --guard != 0)
            ;
        *serdat = (UWORD)(0x100 | (UBYTE)*p);
    }
    for (p = buf; *p != '\0'; p++)
    {
        ULONG guard = 200000;
        while ((*serdatr & 0x2000) == 0 && --guard != 0)
            ;
        *serdat = (UWORD)(0x100 | (UBYTE)*p);
    }
}
#else
#define pc_trace(s, v)  ((VOID)0)
#endif

/* Attribute memory is byte-per-word: the card's byte n is at 2n. */
#define PC_ATTR_STRIDE      2

/* Card Information Structure tuples, PC Card standard, release 2. */
#define CISTPL_FUNCID       0x21
#define CISTPL_CFTABLE      0x1b
#define CISTPL_CONFIG       0x1a
#define CIS_FUNC_LAN        6

/* Configuration Option Register, offset from the config base the CISTPL_CONFIG
   tuple gives.  Bit 6 is the level-mode interrupt select every LAN card wants. */
#define PC_COR_OFF          0
#define PC_COR_LEVEL_IRQ    0x40

struct Library *CardResource;

/*
 * A tuple, copied out of attribute memory by card.resource.  The buffer is
 * the largest a tuple can be plus its two header bytes.
 */
static BOOL pc_tuple(struct CardHandle *h, UBYTE code, UBYTE *buf, UWORD len)
{
    return (BOOL)(CopyTuple(h, buf, code, (ULONG)len) != 0);
}

/*
 * Configure the card and hand back the register base, or NULL.  The handle
 * stays owned for as long as the unit does; releasing it powers the socket
 * down under a driver that is still using it.
 */
/* One slot per machine, so the handle is here rather than threaded through
   the unit: there is nothing for a second one to point at. */
static struct CardHandle pc_handle;

APTR netdev_pcmcia_claim(const NetdevCard *card)
{
    struct CardHandle *handle = &pc_handle;
    UBYTE            buf[64];
    volatile UBYTE  *attr;
    ULONG            cfg_base;
    UBYTE            index;

    if (CardResource == NULL)
    {
        CardResource = OpenResource((STRPTR)CARDRESNAME);
        pc_trace("pc: resource ", (ULONG)CardResource);
        if (CardResource == NULL)
            return NULL;        /* no slot on this machine */
    }

    handle->cah_CardNode.ln_Name = (char *)"anxnet.device";
    handle->cah_CardNode.ln_Pri  = 0;
    handle->cah_CardFlags        = 0;

    {
        struct CardHandle *owner = OwnCard(handle);

        pc_trace("pc: own ", (ULONG)owner);
        if (owner != NULL)
            return NULL;        /* somebody else has it, or nothing is in it */
    }

    /* What is in the slot.  Anything but a LAN adapter is given straight
       back: an IDE adapter driven as an NE2000 is a write to a disk. */
    {
        BOOL got = pc_tuple(handle, CISTPL_FUNCID, buf, sizeof(buf));

        pc_trace("pc: funcid ", ((ULONG)got << 16) | buf[2]);
    }
    if (buf[2] != CIS_FUNC_LAN)
    {
        ReleaseCard(handle, 0);
        return NULL;
    }

    /* CISTPL_CONFIG carries the configuration register base, in the card's
       own attribute address space.  TPCC_SZ says how many bytes of base
       follow it; the low two bits are that count minus one. */
    if (!pc_tuple(handle, CISTPL_CONFIG, buf, sizeof(buf)))
    {
        pc_trace("pc: no config tuple ", 0);
        ReleaseCard(handle, 0);
        return NULL;
    }
    {
        UBYTE  nbytes = (UBYTE)((buf[2] & 0x03) + 1);
        UBYTE  i;

        cfg_base = 0;
        for (i = 0; i < nbytes && i < 4; i++)
            cfg_base |= ((ULONG)buf[4 + i]) << (8 * i);
    }

    /* CISTPL_CFTABLE_ENTRY's first byte holds the configuration index in its
       low six bits.  The first entry is the one to take: a LAN card's first
       entry is its I/O configuration. */
    pc_trace("pc: cfgbase ", cfg_base);
    if (!pc_tuple(handle, CISTPL_CFTABLE, buf, sizeof(buf)))
    {
        pc_trace("pc: no cftable ", 0);
        ReleaseCard(handle, 0);
        return NULL;
    }
    index = (UBYTE)(buf[2] & 0x3f);

    /* Write the COR.  Attribute memory is byte-per-word. */
    attr = (volatile UBYTE *)(ULONG)(0x00a00000UL +
                                     (cfg_base + PC_COR_OFF) * PC_ATTR_STRIDE);
    *attr = (UBYTE)(index | PC_COR_LEVEL_IRQ);

    /*
     * The card's interrupt reaches INT2 through Gayle, and Gayle will not
     * pass it until the status change is enabled.  card.resource owns that
     * register, so this goes through CardMiscControl() rather than a poke.
     */
    (VOID)CardMiscControl(handle, CARD_INTF_SETCLR | CARD_INTF_IRQ);

    pc_trace("pc: index ", (ULONG)index);
    return (APTR)(ULONG)card->base;
}

VOID netdev_pcmcia_release(VOID)
{
    struct CardHandle *handle = &pc_handle;

    if (CardResource != NULL && handle->cah_CardNode.ln_Name != NULL)
    {
        (VOID)CardMiscControl(handle, CARD_INTF_IRQ);   /* SETCLR clear */
        (VOID)ReleaseCard(handle, 0);
        handle->cah_CardNode.ln_Name = NULL;
    }
}
