/*
 * anxnet.device: CISTPL_CFTABLE_ENTRY, walked rather than assumed.
 *
 * The layout, PC Card standard release 2, section 3.3.4:
 *
 *   TPCE_INDX   1 byte    bit 7 interface follows, bit 6 default,
 *                         bits 5..0 the configuration index
 *   TPCE_IF     1 byte    present only when TPCE_INDX bit 7 is set
 *   TPCE_FS     1 byte    which of the rest are present
 *   TPCE_PD     variable  0 to 3 power descriptors, TPCE_FS bits 1..0
 *   TPCE_TD     variable  timing, TPCE_FS bit 2
 *   TPCE_IO     variable  the I/O space, TPCE_FS bit 3
 *   TPCE_IR     variable  the interrupt, TPCE_FS bit 4
 *   TPCE_MS     variable  memory space, TPCE_FS bits 6..5
 *   TPCE_MI     variable  misc, TPCE_FS bit 7
 *
 * Every one of the variable parts has to be skipped correctly to reach the
 * next, so a parser that only wants TPCE_IO still has to understand TPCE_PD
 * and TPCE_TD.  That is why this file exists rather than an offset.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_cis.h"

/*
 * A cursor over the tuple body.  Every read goes through cis_byte(), which
 * sets `over` and returns 0 past the end, so a truncated tuple ends the parse
 * instead of reading the caller's stack.
 */
typedef struct
{
    const UBYTE *p;
    UWORD        len;
    UWORD        at;
    BOOL         over;
} CisCursor;

static UBYTE cis_byte(CisCursor *c)
{
    if (c->at >= c->len)
    {
        c->over = TRUE;
        return 0;
    }

    return c->p[c->at++];
}

/* A power or timing parameter is one byte plus a continuation byte for as long
   as bit 7 stays set.  Nothing here wants the value, only its length. */
static VOID cis_skip_extended(CisCursor *c)
{
    UBYTE b;
    UBYTE guard = 8;        /* a runaway CIS must not spin the romtag init */

    do
        b = cis_byte(c);
    while ((b & 0x80u) != 0 && !c->over && --guard != 0);
}

/* TPCE_PD: a selection byte, then one extended value per bit 0..6 set. */
static VOID cis_skip_power(CisCursor *c)
{
    UBYTE present = cis_byte(c);
    UBYTE i;

    for (i = 0; i < 7 && !c->over; i++)
    {
        if ((present & (UBYTE)(1u << i)) != 0)
            cis_skip_extended(c);
    }
}

/*
 * TPCE_TD: one scale byte carrying three scale fields.  A field at its
 * all-ones value means that parameter is absent; anything else means one
 * extended value follows.  Wait is 2 bits, ready and reserved 3 each.
 */
static VOID cis_skip_timing(CisCursor *c)
{
    UBYTE scale = cis_byte(c);

    if ((scale & 0x03u) != 0x03u)
        cis_skip_extended(c);
    if (((scale >> 2) & 0x07u) != 0x07u)
        cis_skip_extended(c);
    if (((scale >> 5) & 0x07u) != 0x07u)
        cis_skip_extended(c);
}

/*
 * TPCE_IO.  The first byte is bits 4..0 the number of address lines decoded,
 * bit 5 8-bit access, bit 6 16-bit access, bit 7 a range descriptor follows.
 *
 * With no range the card describes 2^lines bytes and leaves the base to the
 * host, which is the whole reason a fixed 0x300 has worked on the Amiga.  With
 * a range the card names its own bases and the host has no say.
 */
static VOID cis_parse_io(CisCursor *c, NetdevCisEntry *out)
{
    UBYTE first = cis_byte(c);
    UBYTE sizes;
    UBYTE bsz;
    UBYTE lsz;
    UBYTE i;
    UBYTE j;
    ULONG base = 0;
    ULONG len  = 1;

    if (c->over)
        return;

    out->flags   |= NETDEV_CIS_HAS_IO;
    out->io_lines = (UBYTE)(first & 0x1fu);
    if ((first & 0x20u) != 0)
        out->flags |= NETDEV_CIS_IO8;
    if ((first & 0x40u) != 0)
        out->flags |= NETDEV_CIS_IO16;

    if ((first & 0x80u) == 0)
    {
        out->io_nwin = 1;
        out->io_base = 0;
        out->io_len  = (UWORD)(out->io_lines >= 16u
                                   ? 0xffffu
                                   : (UWORD)(1u << out->io_lines));
        return;
    }

    sizes = cis_byte(c);
    if (c->over)
        return;

    /* Bits 1..0 are the count less one; 3..2 are reserved and read as zero on
       every card, but Linux has taken four bits here since 1999 and no CIS has
       ever punished it. */
    out->io_nwin = (UBYTE)((sizes & 0x0fu) + 1u);
    bsz = (UBYTE)((sizes >> 4) & 0x03u);
    if (bsz == 3)
        bsz = 4;
    lsz = (UBYTE)((sizes >> 6) & 0x03u);
    if (lsz == 3)
        lsz = 4;

    /* Only the first window is kept: a LAN card describes one register file,
       and a second window would need a second Gayle mapping the machine has
       no way to give it.  The rest are still walked so io_nwin is honest. */
    for (i = 0; i < out->io_nwin && !c->over; i++)
    {
        ULONG b = 0;
        ULONG l = 1;

        for (j = 0; j < bsz; j++)
            b |= ((ULONG)cis_byte(c)) << (8u * j);
        for (j = 0; j < lsz; j++)
            l += ((ULONG)cis_byte(c)) << (8u * j);

        if (i == 0)
        {
            base = b;
            len  = l;
        }
    }

    out->io_base = (UWORD)(base & 0xffffUL);
    out->io_len  = (UWORD)(len > 0xffffUL ? 0xffffUL : len);
}

/* TPCE_IR: one byte, and two more when bit 4 says an IRQ mask follows. */
static VOID cis_parse_irq(CisCursor *c, NetdevCisEntry *out)
{
    UBYTE info = cis_byte(c);

    if (c->over)
        return;

    out->flags |= NETDEV_CIS_HAS_IRQ;
    /* Bit 5 is level mode and bit 6 is pulse mode; a card may offer both.
       Gayle's card interrupt is a level, so bit 5 is the one that matters. */
    if ((info & 0x20u) != 0)
        out->flags |= NETDEV_CIS_IRQ_LEVEL;

    if ((info & 0x10u) != 0)
    {
        (VOID)cis_byte(c);
        (VOID)cis_byte(c);
    }
}

/*
 * TPCE_MS, in the four shapes TPCE_FS bits 6..5 select.  Nothing here wants a
 * memory window; it is walked only because TPCE_MI comes after it.
 */
static VOID cis_skip_mem(CisCursor *c, UBYTE features)
{
    UBYTE shape = (UBYTE)(features & 0x60u);
    UBYTE first;
    UBYTE nwin;
    UBYTE lsz;
    UBYTE asz;
    BOOL  host_addr;
    UBYTE i;
    UBYTE j;

    if (shape == 0x00u)
        return;

    if (shape == 0x20u)         /* one window, a length and nothing else */
    {
        (VOID)cis_byte(c);
        (VOID)cis_byte(c);
        return;
    }

    if (shape == 0x40u)         /* one window, a length and a card address */
    {
        (VOID)cis_byte(c);
        (VOID)cis_byte(c);
        (VOID)cis_byte(c);
        (VOID)cis_byte(c);
        return;
    }

    first = cis_byte(c);
    if (c->over)
        return;

    nwin      = (UBYTE)((first & 0x07u) + 1u);
    lsz       = (UBYTE)((first >> 3) & 0x03u);
    asz       = (UBYTE)((first >> 5) & 0x03u);
    host_addr = (BOOL)((first & 0x80u) != 0);

    for (i = 0; i < nwin && !c->over; i++)
    {
        for (j = 0; j < lsz; j++)
            (VOID)cis_byte(c);
        for (j = 0; j < asz; j++)
            (VOID)cis_byte(c);
        if (host_addr)
        {
            for (j = 0; j < asz; j++)
                (VOID)cis_byte(c);
        }
    }
}

BOOL netdev_cis_cftable(const UBYTE *body, UWORD len, NetdevCisEntry *out)
{
    CisCursor c;
    UBYTE     indx;
    UBYTE     features;
    UBYTE     npower;
    UBYTE     i;

    if (out == NULL)
        return FALSE;

    out->index    = 0;
    out->iface    = 0;
    out->io_lines = 0;
    out->io_nwin  = 0;
    out->io_base  = 0;
    out->io_len   = 0;
    out->flags    = 0;

    if (body == NULL || len == 0)
        return FALSE;

    c.p    = body;
    c.len  = len;
    c.at   = 0;
    c.over = FALSE;

    indx = cis_byte(&c);
    out->index = (UBYTE)(indx & 0x3fu);
    if ((indx & 0x40u) != 0)
        out->flags |= NETDEV_CIS_DEFAULT;

    if ((indx & 0x80u) != 0)
    {
        UBYTE iface = cis_byte(&c);

        out->flags |= NETDEV_CIS_HAS_IF;
        out->iface  = (UBYTE)(iface & 0x0fu);
    }

    features = cis_byte(&c);
    if (c.over)
        return FALSE;

    npower = (UBYTE)(features & 0x03u);
    for (i = 0; i < npower && !c.over; i++)
        cis_skip_power(&c);

    if ((features & 0x04u) != 0)
        cis_skip_timing(&c);

    if ((features & 0x08u) != 0)
        cis_parse_io(&c, out);

    if ((features & 0x10u) != 0)
        cis_parse_irq(&c, out);

    cis_skip_mem(&c, features);

    /* TPCE_MI, a chain of bytes each of which says whether another follows.
       Nothing after it is read, so this is here only to catch truncation. */
    if ((features & 0x80u) != 0)
        cis_skip_extended(&c);

    return (BOOL)(!c.over);
}

BOOL netdev_cis_usable(const NetdevCisEntry *e)
{
    if (e == NULL || (e->flags & NETDEV_CIS_HAS_IO) == 0)
        return FALSE;

    /* A stated interface that is not the I/O one describes a memory or custom
       configuration.  An absent TPCE_IF means the entry inherits the default
       entry's, and a LAN card's default is always the I/O interface. */
    if ((e->flags & NETDEV_CIS_HAS_IF) != 0 && e->iface != NETDEV_CIS_IF_IO)
        return FALSE;

    if (e->io_nwin == 0 || e->io_len < NETDEV_CIS_IO_MIN)
        return FALSE;

    return TRUE;
}

UWORD netdev_cis_score(const NetdevCisEntry *e)
{
    if (!netdev_cis_usable(e))
        return NETDEV_CIS_SCORE_NONE;

    /* Neither width bit set is "unspecified", which every 8-bit card in the
       field means as 8-bit.  Only 16 set with 8 clear is a refusal. */
    if ((e->flags & NETDEV_CIS_IO16) != 0 && (e->flags & NETDEV_CIS_IO8) == 0)
        return NETDEV_CIS_SCORE_WIDE;

    return NETDEV_CIS_SCORE_BEST;
}

UWORD netdev_cis_io_off(const NetdevCisEntry *e, UWORD assumed)
{
    UWORD mask;

    if (e == NULL || (e->flags & NETDEV_CIS_HAS_IO) == 0 || e->io_lines == 0)
        return assumed;

    if (e->io_base == 0)
        return assumed;

    mask = (UWORD)(e->io_lines >= 16u ? 0xffffu
                                      : (UWORD)((1u << e->io_lines) - 1u));

    /* The card ignores every address bit above io_lines, so the assumption is
       still right whenever it agrees inside the mask. */
    if ((UWORD)((assumed ^ e->io_base) & mask) == 0)
        return assumed;

    return e->io_base;
}
