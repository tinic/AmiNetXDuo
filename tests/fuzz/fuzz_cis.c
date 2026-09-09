/*
 * AmiNetXDuo, host fuzz driver for the PC Card CIS walk.
 *
 * WHY THIS PARSER AND NOT ANOTHER.  Every other driver here reads bytes off
 * the network.  These bytes come off a card in the PCMCIA slot, which is a
 * weaker guarantee than it sounds: the walk runs on WHATEVER is in the slot,
 * before anything has identified it, and an empty or half-seated slot reads as
 * a floating bus rather than as an error.  netdev_cis.c decides from those
 * bytes where the card decodes and what gets written to its Configuration
 * Option Register, so a walk that runs off the end reads the card's I/O space
 * at $A20000 as if it were more CIS.  The header says `size` bounds every
 * read; this driver is what asserts it rather than repeating it.
 *
 * IT COVERS THE THREE CARD ROWS NOTHING ELSE CAN.  netdev_cards.c carries
 * 3c589, 3ccfem556 and 3cxem556, and the last two are marked UNVERIFIED ON
 * HARDWARE in their own comment -- neither card is on this network and no
 * emulator models one.  Amiberry's PCMCIA support is an NE2000 replay, so the
 * multifunction path has never run anywhere but in a test.
 *
 * THE READER ABORTS RATHER THAN CLAMPS.  src/netdev/test/test_netdev_cis.c
 * returns $ff past the end, which is what the hardware does and what that test
 * wants.  Here the buffer is a heap allocation of exactly the CIS length and
 * the reader indexes it unchecked, so an offset the walk believed was in range
 * is an ASan report at the byte that proves it, and an offset the walk itself
 * caught never reaches the reader at all.
 *
 * PROVEN TO CATCH, five mutations of netdev_cis.c against `-r 1 20000`:
 *
 *   the source bound in cis_src_byte() widened  -> read past the source size
 *   cis_body_copy()'s clamp to `max` removed    -> ASan stack-buffer-overflow
 *   TPCC_RADR's clamp to the 128 KB window gone -> cfg_base outside it
 *   netdev_cis_usable()'s window check relaxed  -> usable with no window
 *   the COR taking the whole six-bit index      -> address decode not followed
 *
 * AND ONE IT DOES NOT, deliberately recorded rather than papered over:
 * removing the NETDEV_CIS_MAX_TUPLES cap changes nothing here.  cis_chain_next()
 * advances c->at strictly forward and cis_src_byte() refuses off >= size, so a
 * chain walk already terminates on the window bound; the cap bounds the COUNT
 * of tuples, not the walk.  A driver that ran off it would need a link that
 * moves backwards, which the tuple loop cannot express.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_cis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every field the parsers produced is read into this, so nothing is elided. */
static volatile unsigned long fz_sink;

static void fz_fail(const char *what)
{
    printf("fuzz_cis: %s\n", what);
    fflush(stdout);
    abort();
}

/* ------------------------------------------------------------- the source -- */

typedef struct
{
    const UBYTE  *p;        /* exactly `size` bytes, heap, ASan owns both ends */
    ULONG         size;
    unsigned long reads;
} FzCis;

/*
 * A runaway bound, not a tuning knob.  NETDEV_CIS_MAX_TUPLES is 64 and a body
 * is at most 255 bytes, so one chain cannot honestly read more than about 16 K
 * bytes; eight chains plus the probes below leave an order of magnitude of
 * headroom.  Crossing it means a link walked into a cycle, which is a hang in
 * the driver and a ctest timeout here -- worth a named abort instead.
 */
#define FZ_READ_LIMIT   2000000UL

static UBYTE fz_read(APTR ctx, ULONG off)
{
    FzCis *c = (FzCis *)ctx;

    if (++c->reads > FZ_READ_LIMIT)
        fz_fail("the walk did not terminate");

    /* netdev_cis.c's cis_src_byte() refuses off >= src->size before it gets
       here, so this is unreachable unless that guard is lost. */
    if (off >= c->size)
        fz_fail("a read past the source size reached the reader");

    return c->p[off];
}

/* --------------------------------------------------------- the invariants -- */

static void fz_check_entry(const NetdevCisEntry *e)
{
    UWORD score = netdev_cis_score(e);
    BOOL  ok    = netdev_cis_usable(e);

    if ((score == NETDEV_CIS_SCORE_NONE) != (ok == FALSE))
        fz_fail("score and usable disagree");

    if (score != NETDEV_CIS_SCORE_NONE &&
        score != NETDEV_CIS_SCORE_WIDE &&
        score != NETDEV_CIS_SCORE_BEST)
        fz_fail("score is not one of the three");

    if (ok && (e->io_nwin == 0 || e->io_len < NETDEV_CIS_IO_MIN))
        fz_fail("a usable entry has no window big enough to be one");

    /* The offset is the row's assumption or the card's own base, never a
       third number, and a card that named neither leaves the assumption. */
    {
        UWORD off = netdev_cis_io_off(e, 0x0300u);

        if (off != 0x0300u && off != e->io_base)
            fz_fail("io_off invented a base");

        if (((e->flags & NETDEV_CIS_HAS_IO) == 0 || e->io_lines == 0 ||
             e->io_base == 0) && off != 0x0300u)
            fz_fail("io_off overrode an assumption it had no base for");
    }

    fz_sink += (unsigned long)e->index + e->iface + e->io_lines +
               e->io_nwin + e->io_base + e->io_len + e->flags + score;
}

static void fz_check_func(const NetdevCisFunc *fn)
{
    if ((fn->flags & NETDEV_CISF_HAS_CONFIG) == 0)
        fz_fail("a function was accepted without a CISTPL_CONFIG");

    /* Clamped so a corrupt TPCC_RADR cannot put the COR write past the
       attribute window and into the card's I/O space at $A20000. */
    if (fn->cfg_base > 0x0001ffffUL)
        fz_fail("cfg_base is outside the attribute window");

    fz_check_entry(&fn->pick);

    {
        UBYTE cor  = netdev_cis_mfc_cor(fn);
        UBYTE known = (UBYTE)(CIS_COR_MFC_MASK | CIS_COR_FUNC_ENA |
                              CIS_COR_ADDR_DECODE | CIS_COR_IREQ_ENA |
                              CIS_COR_LEVEL_REQ);

        if ((cor & CIS_COR_FUNC_ENA) == 0)
            fz_fail("the COR would leave the function disabled");

        if ((cor & (UBYTE)~known) != 0)
            fz_fail("the COR carries a bit nothing defines");

        /* A single-function card takes the whole six-bit index; a
           multifunction one takes bits 5..3 and nothing below them.  Bits
           2..0 are function enable, address decode and interrupt enable, so
           an index bit that reached them would disable the function it was
           configuring -- which is why the low three are checked for their
           own values and not merely for the index's absence. */
        if ((UBYTE)(cor & CIS_COR_MFC_MASK) !=
            (UBYTE)(fn->index & CIS_COR_MFC_MASK))
            fz_fail("the COR does not carry the chosen index");

        if (((cor & CIS_COR_ADDR_DECODE) != 0) !=
            (netdev_cis_has_iobase(fn) != FALSE))
            fz_fail("address decode does not follow the I/O base registers");

        if (((cor & CIS_COR_LEVEL_REQ) != 0) !=
            ((fn->pick.flags & NETDEV_CIS_IRQ_LEVEL) != 0))
            fz_fail("the interrupt mode does not follow the entry");

        fz_sink += cor;
    }

    if ((fn->flags & NETDEV_CISF_HAS_PICK) != 0 &&
        netdev_cis_usable(&fn->pick) &&
        netdev_cis_mfc_iosize(fn) < (UBYTE)(NETDEV_CIS_IO_MIN - 1u))
        fz_fail("a usable window sized below a register file");

    fz_sink += (unsigned long)fn->chain + fn->cfg_base + fn->cfg_mask +
               fn->cfg_last + fn->funcid + fn->index + fn->score +
               fn->flags + netdev_cis_mfc_iosize(fn) +
               (unsigned long)netdev_cis_has_iobase(fn) + fn->node_id[0];
}

/* ---------------------------------------------------------------- one run -- */

static void fz_run_once(const UBYTE *data, size_t len)
{
    UBYTE          *heap = (UBYTE *)malloc(len != 0 ? len : 1u);
    FzCis           ctx;
    NetdevCisSource src;
    NetdevCisEntry  e;
    NetdevCisFunc   fn;
    ULONG           chains[NETDEV_CIS_MAX_FUNC];
    UWORD           n;
    UWORD           i;
    UWORD           nfunc = 0xffffu;

    if (heap == NULL)
        return;

    if (len != 0)
        memcpy(heap, data, len);

    ctx.p     = heap;
    ctx.size  = (ULONG)len;
    ctx.reads = 0;

    src.read = fz_read;
    src.ctx  = &ctx;
    src.size = (ULONG)len;

    /*
     * The tuple body parser on its own, over the raw bytes.  netdev_pcmcia.c
     * reaches it this way through card.resource's CopyTuple() on a
     * single-function card, so the body is whatever that copied out.
     */
    if (netdev_cis_cftable(heap, (UWORD)(len > 0xffffu ? 0xffffu : len), &e))
        fz_check_entry(&e);

    /* NULL is a documented input: netdev_cis_func() clears its pick with it. */
    if (netdev_cis_cftable(NULL, 0, &e))
        fz_fail("an empty body parsed as an entry");

    n = netdev_cis_mfc_chains(&src, chains, (UWORD)NETDEV_CIS_MAX_FUNC);
    if (n > (UWORD)NETDEV_CIS_MAX_FUNC)
        fz_fail("more chains than the caller offered room for");

    for (i = 0; i < n; i++)
    {
        if (netdev_cis_func(&src, chains[i], &fn))
            fz_check_func(&fn);
    }

    /*
     * Chains the card did not name.  A link is four bytes off the card and can
     * point anywhere in a 32-bit space, so the walk is entered at the end, one
     * short of it, in the middle, and past every possible window.
     */
    {
        static const ULONG at[] = { 0u, 1u, 0x7fffffffUL, 0xfffffffcUL,
                                    0xffffffffUL };
        ULONG              probe[7];
        UWORD              k;

        probe[0] = (ULONG)len;
        probe[1] = (len != 0) ? (ULONG)(len - 1u) : 0u;
        for (k = 0; k < 5u; k++)
            probe[2 + k] = at[k];

        for (k = 0; k < 7u; k++)
        {
            if (netdev_cis_func(&src, probe[k], &fn))
                fz_check_func(&fn);
        }
    }

    if (netdev_cis_mfc_lan(&src, &fn, &nfunc))
    {
        if ((fn.flags & NETDEV_CISF_HAS_FUNCID) == 0 ||
            fn.funcid != (UBYTE)CIS_FUNC_LAN)
            fz_fail("the LAN function is not one");

        if ((fn.flags & NETDEV_CISF_HAS_PICK) == 0)
            fz_fail("the LAN function was taken with no entry to configure");

        fz_check_func(&fn);
    }

    if (nfunc > (UWORD)NETDEV_CIS_MAX_FUNC)
        fz_fail("the function count was not written, or is impossible");

    /* mfc_lan() answers from the same chains, so it cannot name more. */
    if (nfunc != n)
        fz_fail("two walks of one CIS counted different functions");

    free(heap);
}

/* ------------------------------------------------------------------ seeds -- */

/*
 * Real cards' bytes.  These are the images src/netdev/test/test_netdev_cis.c
 * asserts against, and that file is where the tuple-by-tuple reading of them
 * is written down; they are here so the sweep mutates something that parses,
 * because uniform random bytes almost never reach a CISTPL_LINKTARGET, let
 * alone a second chain behind one.
 */

/* Megahertz/3Com 3CCFEM556, MANFID 0101:0556.  Chains at $3b and $58. */
static const UBYTE fzs_3ccfem556[] =
{
    0x01, 0x03, 0x00, 0x00, 0xff, 0x15, 0x1c, 0x05, 0x00, 0x33, 0x43, 0x6f,
    0x6d, 0x00, 0x4d, 0x65, 0x67, 0x61, 0x68, 0x65, 0x72, 0x74, 0x7a, 0x20,
    0x33, 0x43, 0x43, 0x46, 0x45, 0x4d, 0x35, 0x35, 0x36, 0x00, 0xff, 0x20,
    0x04, 0x01, 0x01, 0x56, 0x05, 0x21, 0x02, 0x00, 0x00, 0x06, 0x0b, 0x02,
    0x00, 0x3b, 0x00, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0xff, 0x13,
    0x03, 0x43, 0x49, 0x53, 0x21, 0x02, 0x06, 0x00, 0x1a, 0x06, 0x05, 0x07,
    0x00, 0x10, 0x67, 0x02, 0x1b, 0x09, 0x87, 0x01, 0x19, 0x01, 0x55, 0x64,
    0x30, 0xff, 0xff, 0xff, 0x13, 0x03, 0x43, 0x49, 0x53, 0x21, 0x02, 0x02,
    0x00, 0x1a, 0x06, 0x05, 0x27, 0x00, 0x11, 0x77, 0x02, 0x1b, 0x09, 0xa7,
    0x01, 0x19, 0x01, 0x55, 0x23, 0x30, 0xff, 0xff, 0xff
};

/* Megahertz/3Com 3CXEM556, MANFID 0101:0035.  Chains at $3a and $56.  Its
   CISTPL_CONFIG carries one mask byte where the card above carries two. */
static const UBYTE fzs_3cxem556[] =
{
    0x01, 0x03, 0x00, 0x00, 0xff, 0x15, 0x1b, 0x05, 0x00, 0x33, 0x43, 0x6f,
    0x6d, 0x00, 0x4d, 0x65, 0x67, 0x61, 0x68, 0x65, 0x72, 0x74, 0x7a, 0x20,
    0x33, 0x43, 0x58, 0x45, 0x4d, 0x35, 0x35, 0x36, 0x00, 0xff, 0x20, 0x04,
    0x01, 0x01, 0x35, 0x00, 0x21, 0x02, 0x00, 0x00, 0x06, 0x0b, 0x02, 0x00,
    0x3a, 0x00, 0x00, 0x00, 0x00, 0x56, 0x00, 0x00, 0x00, 0xff, 0x13, 0x03,
    0x43, 0x49, 0x53, 0x21, 0x02, 0x06, 0x00, 0x1a, 0x05, 0x01, 0x07, 0x00,
    0x08, 0x63, 0x1b, 0x09, 0x87, 0x01, 0x19, 0x01, 0x55, 0x64, 0x30, 0xff,
    0xff, 0xff, 0x13, 0x03, 0x43, 0x49, 0x53, 0x21, 0x02, 0x02, 0x00, 0x1a,
    0x05, 0x01, 0x27, 0x00, 0x09, 0x63, 0x1b, 0x09, 0xa7, 0x01, 0x19, 0x01,
    0x55, 0x23, 0x30, 0xff, 0xff, 0xff
};

/* NSC MF LAN/Modem, DP83903, MANFID 0175:0000.  Chains at $46 and $66, and
   function 0's entry carries a memory descriptor after the interrupt one. */
static const UBYTE fzs_dp83903[] =
{
    0x01, 0x03, 0x00, 0x00, 0xff, 0x15, 0x27, 0x05, 0x00, 0x4d, 0x75, 0x6c,
    0x74, 0x69, 0x66, 0x75, 0x6e, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x20, 0x43,
    0x61, 0x72, 0x64, 0x00, 0x4e, 0x53, 0x43, 0x20, 0x4d, 0x46, 0x20, 0x4c,
    0x41, 0x4e, 0x2f, 0x4d, 0x6f, 0x64, 0x65, 0x6d, 0x00, 0xff, 0x20, 0x04,
    0x75, 0x01, 0x00, 0x00, 0x21, 0x02, 0x00, 0x00, 0x06, 0x0b, 0x02, 0x00,
    0x46, 0x00, 0x00, 0x00, 0x00, 0x66, 0x00, 0x00, 0x00, 0xff, 0x13, 0x03,
    0x43, 0x49, 0x53, 0x21, 0x02, 0x06, 0x00, 0x1a, 0x06, 0x05, 0x17, 0x20,
    0x10, 0x77, 0x02, 0x1b, 0x0c, 0x97, 0x01, 0x79, 0x01, 0x55, 0x65, 0x30,
    0xff, 0xff, 0x28, 0x40, 0x00, 0xff, 0x13, 0x03, 0x43, 0x49, 0x53, 0x21,
    0x02, 0x02, 0x00, 0x1a, 0x06, 0x05, 0x07, 0x40, 0x10, 0x77, 0x02, 0x1b,
    0x09, 0x87, 0x01, 0x19, 0x01, 0x55, 0x23, 0x30, 0xff, 0xff, 0xff
};

/* A single-function card around the CNet CN40BC entry -- the CIS Amiberry has
   replayed at this driver in every emulator run there has ever been.  The one
   thing it must produce is "not a multifunction card". */
static const UBYTE fzs_single[] =
{
    0x01, 0x03, 0x00, 0x00, 0xff,
    0x20, 0x04, 0x23, 0x00, 0x02, 0x00,
    0x21, 0x02, 0x06, 0x00,
    0x1a, 0x05, 0x01, 0x20, 0x00, 0x02, 0x03,
    0x1b, 0x11, 0xe0, 0x81, 0x1d, 0x3f, 0x55, 0x4d,
    0x5d, 0x06, 0x86, 0x46, 0x26, 0xfc, 0x24, 0x65,
    0x30, 0xff, 0xff,
    0xff
};

/* The CN40BC CISTPL_CFTABLE_ENTRY body alone, which is what CopyTuple() hands
   over, and the range-descriptor shape that names its own base. */
static const UBYTE fzs_cn40bc[] =
{
    0xe0, 0x81, 0x1d, 0x3f, 0x55, 0x4d, 0x5d, 0x06, 0x86,
    0x46, 0x26, 0xfc, 0x24, 0x65, 0x30, 0xff, 0xff
};

static const UBYTE fzs_fixed_base[] =
{
    0xe0, 0x81, 0x1d, 0x3f, 0x55, 0x4d, 0x5d, 0x06, 0x86,
    0x46, 0x26, 0xfc, 0x24, 0xaa, 0x20, 0x20, 0x03, 0x30, 0xff, 0xff
};

/*
 * A chain naming itself, which is the shape a corrupt CISTPL_LONGLINK_MFC
 * takes when the four address bytes land on the tuple that carries them.  The
 * tuple cap is what has to stop it.
 */
static const UBYTE fzs_self_link[] =
{
    0x13, 0x03, 0x43, 0x49, 0x53,
    0x21, 0x02, 0x00, 0x00,
    0x06, 0x0b, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x09, 0x00, 0x00,
    0x00,
    0x1a, 0x05, 0x01, 0x00, 0x00, 0x02, 0x03,
    0xff
};

/* A CISTPL_LONGLINK_MFC naming eight functions, all of them the same chain,
   and that chain is a valid one -- eight walks of the same bytes. */
static const UBYTE fzs_eight_same[] =
{
    0x06, 0x29, 0x08,
    0x01, 0x2b, 0x00, 0x00, 0x00,
    0x01, 0x2b, 0x00, 0x00, 0x00,
    0x01, 0x2b, 0x00, 0x00, 0x00,
    0x01, 0x2b, 0x00, 0x00, 0x00,
    0x01, 0x2b, 0x00, 0x00, 0x00,
    0x01, 0x2b, 0x00, 0x00, 0x00,
    0x01, 0x2b, 0x00, 0x00, 0x00,
    0x01, 0x2b, 0x00, 0x00, 0x00,
    0xff,
    /* $2b: */
    0x13, 0x03, 0x43, 0x49, 0x53,
    0x21, 0x02, 0x06, 0x00,
    0x1a, 0x05, 0x01, 0x00, 0x00, 0x02, 0x03,
    0x1b, 0x03, 0x01, 0x08, 0x65,
    0xff
};

/* A tuple whose TPL_LINK says $fe and which ends four bytes later: the body
   copy is bounded by the source, not by what the card claimed. */
static const UBYTE fzs_long_link[] =
{
    0x13, 0x03, 0x43, 0x49, 0x53,
    0x1a, 0xfe, 0x01, 0x00, 0x00
};

/* A chain of nothing but CISTPL_NULL, which advances one byte at a time. */
static const UBYTE fzs_null_run[] =
{
    0x13, 0x03, 0x43, 0x49, 0x53,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1a, 0x05, 0x01, 0x00, 0x00, 0x02, 0x03,
    0xff
};

typedef struct
{
    const char  *name;
    const UBYTE *p;
    size_t       len;
} FzSeed;

#define FZ_SEED(x)  { #x, x, sizeof(x) }

static const FzSeed fz_seeds[] =
{
    FZ_SEED(fzs_3ccfem556),
    FZ_SEED(fzs_3cxem556),
    FZ_SEED(fzs_dp83903),
    FZ_SEED(fzs_single),
    FZ_SEED(fzs_cn40bc),
    FZ_SEED(fzs_fixed_base),
    FZ_SEED(fzs_self_link),
    FZ_SEED(fzs_eight_same),
    FZ_SEED(fzs_long_link),
    FZ_SEED(fzs_null_run)
};

#define FZ_SEED_COUNT   (int)(sizeof(fz_seeds) / sizeof(fz_seeds[0]))

/* An empty slot reads as all ones; a card held in reset reads as all zeroes.
   Both are what the walk sees before anything has said a card is there. */
static void fz_degenerate(void)
{
    static UBYTE buf[4096];
    size_t       n;

    fz_run_once(buf, 0);

    for (n = 0; n < sizeof(buf); n++)
        buf[n] = 0xff;
    fz_run_once(buf, 1);
    fz_run_once(buf, sizeof(buf));

    for (n = 0; n < sizeof(buf); n++)
        buf[n] = 0x00;
    fz_run_once(buf, 1);
    fz_run_once(buf, sizeof(buf));

    /* Every tuple code in turn, each claiming a body it does not have. */
    for (n = 0; n < 256u; n++)
    {
        UBYTE t[3];

        t[0] = (UBYTE)n;
        t[1] = 0xff;
        t[2] = (UBYTE)n;
        fz_run_once(t, sizeof(t));
    }
}

static void fz_seed_run(void)
{
    int i;

    for (i = 0; i < FZ_SEED_COUNT; i++)
        fz_run_once(fz_seeds[i].p, fz_seeds[i].len);

    fz_degenerate();
}

/* --------------------------------------------------------------- mutation -- */

/* unsigned long long so a seed means the same sequence whatever the host. */
static unsigned long long fz_state = 1;

static unsigned fz_rand(void)
{
    fz_state = fz_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned)(fz_state >> 33);
}

static unsigned fz_below(unsigned n)
{
    return (n == 0) ? 0 : (fz_rand() % n);
}

/*
 * The bytes that decide where the walk goes next, not the payload.  A CIS is
 * a chain of (code, link, body): a mutated body changes what an entry says, a
 * mutated link changes where the next tuple is read from, and only the second
 * can walk off the card.
 */
static const UBYTE fz_codes[] =
{
    CISTPL_NULL, CISTPL_DEVICE, CISTPL_LONGLINK_MFC, CISTPL_CHECKSUM,
    CISTPL_LONGLINK_A, CISTPL_LONGLINK_C, CISTPL_LINKTARGET, CISTPL_NO_LINK,
    CISTPL_VERS_1, CISTPL_CONFIG, CISTPL_CFTABLE, CISTPL_MANFID,
    CISTPL_FUNCID, CISTPL_FUNCE, CISTPL_END
};

static size_t fz_mutate(UBYTE *buf, size_t len, size_t cap)
{
    unsigned rounds = fz_below(6) + 1u;

    while (rounds-- > 0 && len != 0)
    {
        switch (fz_below(9))
        {
        case 0:
            buf[fz_below((unsigned)len)] = (UBYTE)fz_rand();
            break;

        case 1:
            /* A tuple code, at a random place: a body byte read as a code is
               how a walk that lost its place carries on. */
            buf[fz_below((unsigned)len)] =
                fz_codes[fz_below((unsigned)(sizeof(fz_codes)))];
            break;

        case 2:
            /* A TPL_LINK, set to an extreme.  $ff is the "no more" encoding
               and 0 is a body of nothing. */
            buf[fz_below((unsigned)len)] =
                (UBYTE)((fz_below(3) == 0) ? 0x00u
                                           : ((fz_below(2) == 0) ? 0xffu
                                                                : fz_rand()));
            break;

        case 3:
            len = fz_below((unsigned)len + 1u);
            break;

        case 4:
        {
            /* A four-byte little-endian address, which is what every link
               tuple carries and the only field that names a place. */
            size_t at = fz_below((unsigned)len);
            unsigned k;

            for (k = 0; k < 4u && at + k < len; k++)
                buf[at + k] = (UBYTE)(fz_below(4) == 0 ? fz_rand()
                                                       : fz_below(0x100));
            break;
        }

        case 5:
        {
            /* Splice, so a chain address lands in the middle of a body. */
            size_t from = fz_below((unsigned)len);
            size_t to   = fz_below((unsigned)len);
            unsigned n  = fz_below(32) + 1u;

            while (n-- > 0 && from < len && to < len)
                buf[to++] = buf[from++];
            break;
        }

        case 6:
        {
            unsigned n = fz_below(64);

            while (n-- > 0 && len < cap)
                buf[len++] = (UBYTE)fz_rand();
            break;
        }

        case 7:
        {
            /* A run of one value, which is what a floating bus looks like
               across part of a window. */
            size_t   at = fz_below((unsigned)len);
            unsigned n  = fz_below(48) + 1u;
            UBYTE    v  = (UBYTE)fz_rand();

            while (n-- > 0 && at < len)
                buf[at++] = v;
            break;
        }

        default:
            /* Open the buffer with a link target, so a mutated image reaches
               the tuple loop instead of being refused at its first byte. */
            if (len >= 5u)
            {
                buf[0] = (UBYTE)CISTPL_LINKTARGET;
                buf[1] = 3u;
                buf[2] = 'C';
                buf[3] = 'I';
                buf[4] = 'S';
            }
            break;
        }
    }

    return len;
}

int main(int argc, char **argv)
{
    static UBYTE buf[8192];
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-s") == 0)
        {
            fz_seed_run();
            printf("fuzz_cis: %d seeds ok\n", FZ_SEED_COUNT);
            return 0;
        }

        if (strcmp(argv[i], "-r") == 0 && i + 2 < argc)
        {
            unsigned long seed  = strtoul(argv[++i], NULL, 0);
            unsigned long count = strtoul(argv[++i], NULL, 0);
            unsigned long n;

            fz_seed_run();
            fz_state = seed;

            for (n = 0; n < count; n++)
            {
                const FzSeed *s = &fz_seeds[fz_below(FZ_SEED_COUNT)];
                size_t        len = s->len;

                if (len > sizeof(buf))
                    len = sizeof(buf);

                memcpy(buf, s->p, len);
                len = fz_mutate(buf, len, sizeof(buf));
                fz_run_once(buf, len);
            }

            printf("fuzz_cis: %lu cases from seed %lu ok\n", count, seed);
            return 0;
        }
    }

    fprintf(stderr, "usage: fuzz_cis -s | -r <seed> <count>\n");

    return 2;
}
