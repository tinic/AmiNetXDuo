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

/* ------------------------------------------------------ the raw CIS walk -- */

/*
 * A cursor over the card's CIS, one tuple at a time.
 *
 * Every read is bounded by the source's size, so a link that points past the
 * attribute window ends the walk rather than reading the card's I/O space.
 * CISTPL_NULL is a pad byte with no length byte after it and is skipped;
 * CISTPL_END ends the chain.  The tuple count is bounded because a CIS that
 * links to itself would otherwise spin the romtag init forever.
 */
typedef struct
{
    const NetdevCisSource *src;
    ULONG                  at;
    UWORD                  seen;
} CisChain;

static VOID cis_chain_begin(CisChain *c, const NetdevCisSource *src, ULONG at)
{
    c->src  = src;
    c->at   = at;
    c->seen = 0;
}

static UBYTE cis_src_byte(const NetdevCisSource *src, ULONG off, BOOL *over)
{
    if (src == NULL || src->read == NULL || off >= src->size)
    {
        *over = TRUE;
        return 0;
    }

    return src->read(src->ctx, off);
}

/*
 * The next tuple, or FALSE at CISTPL_END, at the end of the window, or after
 * NETDEV_CIS_MAX_TUPLES.  *body is where the body starts and *len is TPL_LINK
 * clamped to what the window can actually hand back.
 */
static BOOL cis_chain_next(CisChain *c, UBYTE *code, ULONG *body, UWORD *len)
{
    BOOL over = FALSE;

    for (;;)
    {
        UBYTE tcode;
        UBYTE link;

        if (c->seen >= NETDEV_CIS_MAX_TUPLES)
            return FALSE;

        tcode = cis_src_byte(c->src, c->at, &over);
        if (over)
            return FALSE;

        if (tcode == (UBYTE)CISTPL_END)
            return FALSE;

        if (tcode == (UBYTE)CISTPL_NULL)
        {
            c->at++;
            c->seen++;              /* a run of pad bytes is still bounded */
            continue;
        }

        link = cis_src_byte(c->src, c->at + 1u, &over);
        if (over)
            return FALSE;

        *code = tcode;
        *body = c->at + 2u;
        *len  = (UWORD)link;

        if (*body + (ULONG)link > c->src->size)
        {
            if (*body >= c->src->size)
                return FALSE;
            *len = (UWORD)(c->src->size - *body);
        }

        c->at = c->at + 2u + (ULONG)link;
        c->seen++;

        return TRUE;
    }
}

/* A tuple body, copied out so the existing parsers can be handed a pointer. */
static UWORD cis_body_copy(const NetdevCisSource *src, ULONG body, UWORD len,
                           UBYTE *dst, UWORD max)
{
    BOOL  over = FALSE;
    UWORD i;

    if (len > max)
        len = max;

    for (i = 0; i < len; i++)
    {
        dst[i] = cis_src_byte(src, body + (ULONG)i, &over);
        if (over)
            return i;
    }

    return len;
}

/* The longest tuple body this walk keeps.  A CISTPL_CFTABLE_ENTRY is the only
   one it parses in full and no card in the field states a longer one. */
#define CIS_BODY_MAX    64

UWORD netdev_cis_mfc_chains(const NetdevCisSource *src, ULONG *chains,
                            UWORD max)
{
    CisChain c;
    UBYTE    code;
    ULONG    body;
    UWORD    len;

    if (src == NULL || chains == NULL || max == 0)
        return 0;

    cis_chain_begin(&c, src, 0);

    while (cis_chain_next(&c, &code, &body, &len))
    {
        UBYTE nfn;
        UWORD i;
        UWORD kept = 0;

        if (code != (UBYTE)CISTPL_LONGLINK_MFC)
            continue;

        if (len < 1u)
            return 0;

        {
            BOOL over = FALSE;

            nfn = cis_src_byte(src, body, &over);
            if (over)
                return 0;
        }

        /* The body is one count byte and five per function.  A tuple that
           states more functions than it carries is corrupt, not a card with
           an unreachable function. */
        if (nfn == 0 || (UWORD)len < (UWORD)(1u + (UWORD)nfn * 5u))
            return 0;

        if (nfn > (UBYTE)NETDEV_CIS_MAX_FUNC)
            nfn = (UBYTE)NETDEV_CIS_MAX_FUNC;

        for (i = 0; i < (UWORD)nfn && kept < max; i++)
        {
            BOOL  over = FALSE;
            ULONG at   = body + 1u + (ULONG)i * 5u;
            ULONG addr = 0;
            UWORD j;

            /* TPLMFC_TAS is read and not acted on.  It names common or
               attribute memory, and on this machine the chains of every
               multifunction card whose CIS has been seen are in the same
               attribute window the link itself was read from -- so the
               reader decides the space, and a chain that is not there fails
               the CISTPL_LINKTARGET check in netdev_cis_func(). */
            (VOID)cis_src_byte(src, at, &over);
            for (j = 0; j < 4u; j++)
                addr |= ((ULONG)cis_src_byte(src, at + 1u + j, &over)) << (8u * j);

            if (over)
                return 0;

            chains[kept++] = addr;
        }

        return kept;
    }

    return 0;
}

BOOL netdev_cis_func(const NetdevCisSource *src, ULONG chain,
                     NetdevCisFunc *out)
{
    CisChain c;
    UBYTE    code;
    ULONG    body;
    UWORD    len;
    UBYTE    buf[CIS_BODY_MAX];
    BOOL     linktarget = FALSE;
    UWORD    i;

    if (src == NULL || out == NULL)
        return FALSE;

    out->chain    = chain;
    out->cfg_base = 0;
    out->cfg_mask = 0;
    out->cfg_last = 0;
    out->funcid   = 0;
    out->index    = 0;
    out->score    = NETDEV_CIS_SCORE_NONE;
    out->flags    = 0;
    for (i = 0; i < (UWORD)NETDEV_CIS_NODE_LEN; i++)
        out->node_id[i] = 0;
    netdev_cis_cftable(NULL, 0, &out->pick);

    cis_chain_begin(&c, src, chain);

    while (cis_chain_next(&c, &code, &body, &len))
    {
        UWORD got = cis_body_copy(src, body, len, buf, (UWORD)sizeof(buf));

        /*
         * The chain has to open with CISTPL_LINKTARGET carrying "CIS".  That
         * is the standard's own answer to "was the link address right", and
         * it is the whole reason a walk that reads raw memory is safe: a
         * wrong address lands on bytes that are not a link target and the
         * function is refused rather than configured from noise.
         */
        if (!linktarget)
        {
            if (code != (UBYTE)CISTPL_LINKTARGET || got < 3u ||
                buf[0] != 'C' || buf[1] != 'I' || buf[2] != 'S')
                return FALSE;

            linktarget = TRUE;
            continue;
        }

        switch (code)
        {
        case CISTPL_FUNCID:
            if (got >= 1u)
            {
                out->funcid = buf[0];
                out->flags |= NETDEV_CISF_HAS_FUNCID;
            }
            break;

        case CISTPL_FUNCE:
            if (got >= 8u && buf[0] == (UBYTE)CIS_FUNCE_LAN_NODE_ID &&
                buf[1] == (UBYTE)NETDEV_CIS_NODE_LEN)
            {
                for (i = 0; i < (UWORD)NETDEV_CIS_NODE_LEN; i++)
                    out->node_id[i] = buf[2 + i];
                out->flags |= NETDEV_CISF_HAS_NODEID;
            }
            break;

        case CISTPL_CONFIG:
        {
            /* TPCC_SZ, TPCC_LAST, TPCC_RADR, TPCC_RMSK.  Bits 1..0 of TPCC_SZ
               are the address size less one and bits 5..2 the mask size less
               one, so both are variable and neither can be assumed. */
            UBYTE rasz;
            UBYTE rmsz;

            if (got < 3u)
                break;

            rasz = (UBYTE)((buf[0] & 0x03u) + 1u);
            rmsz = (UBYTE)(((buf[0] >> 2) & 0x0fu) + 1u);
            if ((UWORD)got < (UWORD)(2u + rasz + rmsz))
                break;

            out->cfg_last = buf[1];
            out->cfg_base = 0;
            for (i = 0; i < (UWORD)rasz && i < 4u; i++)
                out->cfg_base |= ((ULONG)buf[2 + i]) << (8u * i);

            /* TPCC_RADR is an attribute-memory address, not a CIS offset --
               see the note in netdev_cis.h -- so the bound is the 128 KB
               window and not the 64 KB of CIS bytes inside it.  Clamped so a
               corrupt TPCC_RADR cannot put the COR write past the window and
               into the card's I/O space. */
            out->cfg_base &= 0x0001ffffUL;

            out->cfg_mask = 0;
            for (i = 0; i < (UWORD)rmsz && i < 2u; i++)
                out->cfg_mask |= (UWORD)((UWORD)buf[2 + rasz + i] << (8u * i));

            out->flags |= NETDEV_CISF_HAS_CONFIG;
            break;
        }

        case CISTPL_CFTABLE:
        {
            NetdevCisEntry e;
            UWORD          score;

            if (!netdev_cis_cftable(buf, got, &e))
                break;

            score = netdev_cis_score(&e);
            if (score <= out->score)
                break;

            out->score = score;
            out->pick  = e;
            out->index = e.index;
            out->flags |= NETDEV_CISF_HAS_PICK;
            break;
        }

        default:
            break;
        }
    }

    if (!linktarget || (out->flags & NETDEV_CISF_HAS_CONFIG) == 0)
        return FALSE;

    return TRUE;
}

BOOL netdev_cis_mfc_lan(const NetdevCisSource *src, NetdevCisFunc *out,
                        UWORD *nfunc)
{
    ULONG chains[NETDEV_CIS_MAX_FUNC];
    UWORD n;
    UWORD i;

    if (nfunc != NULL)
        *nfunc = 0;

    if (src == NULL || out == NULL)
        return FALSE;

    n = netdev_cis_mfc_chains(src, chains, (UWORD)NETDEV_CIS_MAX_FUNC);
    if (nfunc != NULL)
        *nfunc = n;
    if (n == 0)
        return FALSE;

    for (i = 0; i < n; i++)
    {
        NetdevCisFunc fn;

        if (!netdev_cis_func(src, chains[i], &fn))
            continue;

        /*
         * The function has to say it is a LAN adapter.  A multifunction card
         * states CISTPL_FUNCID in every function chain -- that is what the
         * chains are for -- so an absent one here is not the "assume LAN" case
         * a single-function card's missing tuple is.
         */
        if ((fn.flags & NETDEV_CISF_HAS_FUNCID) == 0 ||
            fn.funcid != (UBYTE)CIS_FUNC_LAN)
            continue;

        if ((fn.flags & NETDEV_CISF_HAS_PICK) == 0)
            continue;

        *out = fn;

        return TRUE;
    }

    return FALSE;
}

BOOL netdev_cis_has_iobase(const NetdevCisFunc *fn)
{
    if (fn == NULL || (fn->flags & NETDEV_CISF_HAS_CONFIG) == 0)
        return FALSE;

    return (BOOL)((fn->cfg_mask & (UWORD)(1u << CIS_REG_IOBASE_0)) != 0 &&
                  (fn->cfg_mask & (UWORD)(1u << CIS_REG_IOBASE_1)) != 0);
}

UBYTE netdev_cis_mfc_cor(const NetdevCisFunc *fn)
{
    UBYTE cor;

    if (fn == NULL)
        return 0;

    cor = (UBYTE)((fn->index & CIS_COR_MFC_MASK) |
                  CIS_COR_FUNC_ENA | CIS_COR_IREQ_ENA);

    /* Address decode is what makes the card use the base the host wrote into
       IOBASE_0/1 instead of the one its own CIS named.  Only offered when
       those registers exist. */
    if (netdev_cis_has_iobase(fn))
        cor |= CIS_COR_ADDR_DECODE;

    /* Gayle's card interrupt is a level. */
    if ((fn->pick.flags & NETDEV_CIS_IRQ_LEVEL) != 0)
        cor |= CIS_COR_LEVEL_REQ;

    return cor;
}

UBYTE netdev_cis_mfc_iosize(const NetdevCisFunc *fn)
{
    ULONG len;

    if (fn == NULL || (fn->flags & NETDEV_CISF_HAS_PICK) == 0)
        return 0;

    len = (ULONG)fn->pick.io_len;
    if (len == 0)
        return 0;
    if (len > 256UL)
        len = 256UL;

    /* The register is the size less one, so a 32-port window is 31. */
    return (UBYTE)(len - 1UL);
}
