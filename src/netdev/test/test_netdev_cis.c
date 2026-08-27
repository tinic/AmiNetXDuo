/*
 * CISTPL_CFTABLE_ENTRY, parsed against real cards' bytes.
 *
 * The vectors are not invented.  The CNet one is the CIS Amiberry replays for
 * its PCMCIA network card, which is itself a dump of a CNet CN40BC -- see the
 * table its gayle.cpp calls ne2000pcmcia[] -- so it is the exact tuple this
 * driver has been reading in every emulator run there has ever been, and the
 * exact card family the backlog says does not attach on real hardware.  The
 * rest exercise the shapes that CIS never takes: a range descriptor naming its
 * own base, a memory-only entry, a 16-bit-only entry, and a truncated one.
 *
 * A parser that only wants the I/O descriptor still has to skip the power and
 * timing descriptors byte for byte to reach it, and getting that wrong reads
 * an interrupt mask as an address.  That is what these cases are for.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "netdev_cis.h"

static int failures;

static void expect_int(const char *what, long got, long want)
{
    if (got == want)
    {
        printf("ok   %s = %ld\n", what, got);
        return;
    }

    printf("FAIL %s: got %ld, want %ld\n", what, got, want);
    failures++;
}

static void expect_hex(const char *what, unsigned long got, unsigned long want)
{
    if (got == want)
    {
        printf("ok   %s = $%04lx\n", what, got);
        return;
    }

    printf("FAIL %s: got $%04lx, want $%04lx\n", what, got, want);
    failures++;
}

/*
 * CNet CN40BC, the whole tuple body less TPL_CODE and TPL_LINK.
 *
 *   e0        TPCE_INDX: interface follows, default entry, index 32
 *   81        TPCE_IF:   wait required, interface 1 = I/O and memory
 *   1d        TPCE_FS:   one power descriptor, timing, I/O, IRQ
 *   3f        TPCE_PD:   Vnom Vmin Vmax Istatic Iavg Ipeak present
 *   55 4d 5d 06 86 46 26   those six, Ipeak two bytes because $86 continues
 *   fc        TPCE_TD:   wait present, ready and reserved absent
 *   24        the wait value
 *   65        TPCE_IO:   5 address lines, 8-bit and 16-bit, no range
 *   30 ff ff  TPCE_IR:   level mode, a mask follows, the mask
 *
 * Seventeen bytes, which is the TPL_LINK the card states.  The absence of a
 * range descriptor is the whole reason a fixed offset of $300 has worked: the
 * card decodes five address lines and leaves placement to the machine.
 */
static const UBYTE cnet_cn40bc[] =
{
    0xe0, 0x81, 0x1d, 0x3f, 0x55, 0x4d, 0x5d, 0x06, 0x86,
    0x46, 0x26, 0xfc, 0x24, 0x65, 0x30, 0xff, 0xff
};

/*
 * The same card with a range descriptor bolted on, which is the shape a card
 * that names its own base takes: TPCE_IO $8a is 10 address lines and a range,
 * then $20 is one window with a two-byte base and no length bytes, then the
 * base $0320 little-endian.  Ten decoded lines mean the card means it.
 */
static const UBYTE fixed_base_320[] =
{
    0xe0, 0x81, 0x1d, 0x3f, 0x55, 0x4d, 0x5d, 0x06, 0x86,
    0x46, 0x26, 0xfc, 0x24, 0xaa, 0x20, 0x20, 0x03, 0x30, 0xff, 0xff
};

/* An entry with no I/O space at all: TPCE_FS $01 is one power descriptor and
   nothing else.  A card whose first entry looks like this is the case the
   walk exists for. */
static const UBYTE memory_only[] =
{
    0x81, 0x00, 0x01, 0x01, 0x55
};

/* 16-bit accesses offered, 8-bit refused: TPCE_IO $45 is bit 6 without bit 5.
   Every register path here is byte-wide, so this entry is the one to take
   only when the card offers no other. */
static const UBYTE wide_only[] =
{
    0x01, 0x08, 0x45
};

/* Both width bits clear is "unspecified", which is 8-bit everywhere in the
   field and must not be read as a refusal. */
static const UBYTE width_unstated[] =
{
    0x01, 0x08, 0x05
};

/* TPCE_FS says an I/O descriptor follows and the tuple ends before it.  This
   is what a body longer than the driver's copy buffer looks like. */
static const UBYTE truncated[] =
{
    0xe0, 0x81, 0x1d, 0x3f, 0x55
};


/* ============================================================================
 * The raw CIS walk: multifunction cards, which CopyTuple() cannot reach.
 *
 * A multifunction PC Card puts nothing in the shared chain but MANFID, a
 * CISTPL_FUNCID of 0 and a CISTPL_LONGLINK_MFC naming one chain per function.
 * Each function's CISTPL_CONFIG and CISTPL_CFTABLE_ENTRY live in its own
 * chain.  card.resource's CopyTuple() follows CISTPL_LONGLINK_A,
 * CISTPL_LONGLINK_C, CISTPL_NO_LINK and CISTPL_LINKTARGET -- cardres.doc names
 * those four -- so none of that is reachable through it, and a walk that reads
 * the CIS itself is the only way in.
 *
 * The three images below carry the per-function tuples three real cards state,
 * laid out as one CIS image each.  The configuration bytes are the cards':
 *
 *   3CCFEM556  Megahertz/3Com, MANFID 0101:0556, LAN + 56k modem.  Function 0
 *              is an EtherLink III, configuration base $1000, index 7, a
 *              16-port window, 8- and 16-bit, level interrupt.
 *   3CXEM556   Megahertz/3Com, MANFID 0101:0035, the same card with a shorter
 *              CISTPL_CONFIG: one mask byte rather than two, which is the case
 *              a parser that assumed a fixed layout gets wrong.
 *   DP83903    National Semiconductor MF LAN/Modem, MANFID 0175:0000.
 *              Function 0 is a DP8390 at base $1020, index $17, a 32-port
 *              window, and its entry carries a memory descriptor after the
 *              interrupt -- so the parse has to walk past one to end clean.
 *
 * Function 1 of all three is a modem, and the walk has to pass over it.
 * ==========================================================================*/

/* A CIS in host memory, read the way the card's is: one byte at a time
   through a reader, so the walk has no idea where the bytes came from. */
typedef struct
{
    const UBYTE *p;
    UWORD        len;
    int          reads;
} MemCis;

static UBYTE mem_read(APTR ctx, ULONG off)
{
    MemCis *m = (MemCis *)ctx;

    m->reads++;
    if (off >= (ULONG)m->len)
        return 0xff;

    return m->p[off];
}

/*
 * The same bytes behind Gayle's attribute window, where the card's byte n is
 * at 0xA00000 + 2n.  netdev_pcmcia.c's reader is this arithmetic and nothing
 * else, so running the walk over it here is what says the driver's reader and
 * the walk agree about what an address means.
 */
static UBYTE attr_read(APTR ctx, ULONG off)
{
    MemCis         *m = (MemCis *)ctx;
    const UBYTE    *window = m->p;      /* stands in for 0xA00000 */
    ULONG           at = off * 2u;

    m->reads++;
    if (at >= (ULONG)m->len)
        return 0xff;

    return window[at];
}

static void mem_source(NetdevCisSource *src, MemCis *m, const UBYTE *p,
                       UWORD len)
{
    m->p     = p;
    m->len   = len;
    m->reads = 0;
    src->read = mem_read;
    src->ctx  = m;
    src->size = len;
}

/* Megahertz/3Com 3CCFEM556, MANFID 0101:0556.  Chains at $3b and $58. */
static const UBYTE mfc_3ccfem556[] =
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

/* Megahertz/3Com 3CXEM556, MANFID 0101:0035.  Chains at $3a and $56. */
static const UBYTE mfc_3cxem556[] =
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

/* NSC MF LAN/Modem, DP83903, MANFID 0175:0000.  Chains at $46 and $66. */
static const UBYTE mfc_dp83903[] =
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

/*
 * A single-function card's CIS, in the same shape.  The CNet CN40BC entry
 * above is its CISTPL_CFTABLE_ENTRY; this is a whole chain around it, and the
 * one thing it must produce is "not a multifunction card" -- because the walk
 * runs on every card and must not find a second CIS in a card that has one.
 */
static const UBYTE single_function[] =
{
    0x01, 0x03, 0x00, 0x00, 0xff,
    0x20, 0x04, 0x23, 0x00, 0x02, 0x00,             /* MANFID              */
    0x21, 0x02, 0x06, 0x00,                         /* FUNCID 6 = LAN      */
    0x1a, 0x05, 0x01, 0x20, 0x00, 0x02, 0x03,       /* CONFIG, base $200   */
    0x1b, 0x11, 0xe0, 0x81, 0x1d, 0x3f, 0x55, 0x4d, /* the CN40BC entry    */
    0x5d, 0x06, 0x86, 0x46, 0x26, 0xfc, 0x24, 0x65,
    0x30, 0xff, 0xff,
    0xff
};

static void test_mfc_card(const char *name, const UBYTE *cis, UWORD len,
                          unsigned want_chain, unsigned want_base,
                          unsigned want_index, unsigned want_ports,
                          unsigned want_cor, unsigned want_iosize)
{
    NetdevCisSource src;
    MemCis          m;
    NetdevCisFunc   fn;
    ULONG           chains[NETDEV_CIS_MAX_FUNC];
    UWORD           nfunc = 0;
    char            what[96];

    mem_source(&src, &m, cis, len);

    snprintf(what, sizeof(what), "%s: functions named", name);
    expect_int(what, netdev_cis_mfc_chains(&src, chains,
                                           NETDEV_CIS_MAX_FUNC), 2);

    snprintf(what, sizeof(what), "%s: the LAN function's chain", name);
    expect_hex(what, chains[0], want_chain);

    snprintf(what, sizeof(what), "%s: the LAN function is reached", name);
    expect_int(what, netdev_cis_mfc_lan(&src, &fn, &nfunc), 1);

    snprintf(what, sizeof(what), "%s: and the card named two functions", name);
    expect_int(what, nfunc, 2);

    snprintf(what, sizeof(what), "%s: FUNCID is LAN", name);
    expect_int(what, fn.funcid, CIS_FUNC_LAN);

    snprintf(what, sizeof(what), "%s: it is function 0's chain", name);
    expect_hex(what, fn.chain, want_chain);

    snprintf(what, sizeof(what), "%s: configuration base", name);
    expect_hex(what, fn.cfg_base, want_base);

    snprintf(what, sizeof(what), "%s: configuration index", name);
    expect_hex(what, fn.index, want_index);

    snprintf(what, sizeof(what), "%s: the entry is usable", name);
    expect_int(what, netdev_cis_usable(&fn.pick), 1);

    snprintf(what, sizeof(what), "%s: I/O window ports", name);
    expect_int(what, fn.pick.io_len, (long)want_ports);

    /* No range descriptor, so the host places the window -- which on this
       machine means the card row's own offset stands. */
    snprintf(what, sizeof(what), "%s: the host places the window", name);
    expect_hex(what, netdev_cis_io_off(&fn.pick, 0x0300), 0x0300);

    snprintf(what, sizeof(what), "%s: the I/O base registers exist", name);
    expect_int(what, netdev_cis_has_iobase(&fn), 1);

    /*
     * The COR a multifunction card takes is not the configuration index.
     * Bits 2..0 are function enable, address decode and interrupt enable, so
     * only bits 5..3 of the index survive; writing the plain index leaves the
     * function disabled and decoding nothing.
     */
    snprintf(what, sizeof(what), "%s: COR", name);
    expect_hex(what, netdev_cis_mfc_cor(&fn), want_cor);

    snprintf(what, sizeof(what), "%s: IOSIZE register", name);
    expect_hex(what, netdev_cis_mfc_iosize(&fn), want_iosize);

    /* The same walk over Gayle's doubled attribute window.  The reader is the
       only thing that changes and every answer above must be unchanged. */
    {
        static UBYTE doubled[1024];
        UWORD        i;
        MemCis       dm;
        NetdevCisSource dsrc;
        NetdevCisFunc   dfn;

        for (i = 0; i < len && (UWORD)(i * 2u + 1u) < (UWORD)sizeof(doubled);
             i++)
        {
            doubled[i * 2u]      = cis[i];
            doubled[i * 2u + 1u] = 0xa5;    /* the half the card does not drive */
        }

        dm.p     = doubled;
        dm.len   = (UWORD)(len * 2u);
        dm.reads = 0;
        dsrc.read = attr_read;
        dsrc.ctx  = &dm;
        dsrc.size = len;

        snprintf(what, sizeof(what),
                 "%s: the same card through the doubled attribute window",
                 name);
        expect_int(what, netdev_cis_mfc_lan(&dsrc, &dfn, NULL), 1);
        snprintf(what, sizeof(what), "%s: and the same configuration base",
                 name);
        expect_hex(what, dfn.cfg_base, want_base);
    }
}

static void test_mfc(void)
{
    NetdevCisSource src;
    MemCis          m;
    NetdevCisFunc   fn;
    ULONG           chains[NETDEV_CIS_MAX_FUNC];
    UWORD           nfunc;

    /* index 7: 7 & $38 is 0, so the COR is the three control bits and the
       level-interrupt bit alone -- $47. */
    test_mfc_card("3CCFEM556", mfc_3ccfem556, (UWORD)sizeof(mfc_3ccfem556),
                  0x3b, 0x1000, 0x07, 16, 0x47, 15);
    /* The same, with a one-byte TPCC_RMSK instead of two. */
    test_mfc_card("3CXEM556", mfc_3cxem556, (UWORD)sizeof(mfc_3cxem556),
                  0x3a, 0x0800, 0x07, 16, 0x47, 15);
    /* index $17: $17 & $38 is $10, so the index really does reach the COR. */
    test_mfc_card("DP83903", mfc_dp83903, (UWORD)sizeof(mfc_dp83903),
                  0x46, 0x1020, 0x17, 32, 0x57, 31);

    /* ---- a card that is not multifunction ---- */
    mem_source(&src, &m, single_function, (UWORD)sizeof(single_function));
    expect_int("a single-function CIS names no function chains",
               netdev_cis_mfc_chains(&src, chains, NETDEV_CIS_MAX_FUNC), 0);
    nfunc = 99;
    expect_int("and has no LAN function to find",
               netdev_cis_mfc_lan(&src, &fn, &nfunc), 0);
    expect_int("and says so with a function count of zero", nfunc, 0);

    /* ---- a link that points at bytes which are not a chain ---- */
    {
        static UBYTE bad[sizeof(mfc_3ccfem556)];
        UWORD        i;

        for (i = 0; i < (UWORD)sizeof(mfc_3ccfem556); i++)
            bad[i] = mfc_3ccfem556[i];

        /* Move function 0's chain two bytes off, onto the middle of the
           CISTPL_LINKTARGET body.  This is the failure a raw walk has to
           refuse rather than configure a card from: without the "CIS"
           check it would read the bytes after it as tuples. */
        bad[49] = 0x3d;
        mem_source(&src, &m, bad, (UWORD)sizeof(bad));
        expect_int("a chain that does not open with a link target is refused",
                   netdev_cis_func(&src, 0x3d, &fn), 0);
        /* Function 1 is still a modem, so the card has no LAN function left. */
        expect_int("and the card is then not one this driver can drive",
                   netdev_cis_mfc_lan(&src, &fn, &nfunc), 0);
        expect_int("though it is still a multifunction card", nfunc, 2);
    }

    /* ---- a CISTPL_LONGLINK_MFC that claims more than it carries ---- */
    {
        static UBYTE bad[sizeof(mfc_3ccfem556)];
        UWORD        i;

        for (i = 0; i < (UWORD)sizeof(mfc_3ccfem556); i++)
            bad[i] = mfc_3ccfem556[i];

        bad[47] = 0x08;         /* eight functions in a body that holds two */
        mem_source(&src, &m, bad, (UWORD)sizeof(bad));
        expect_int("a function count longer than the tuple is refused",
                   netdev_cis_mfc_chains(&src, chains, NETDEV_CIS_MAX_FUNC), 0);
    }

    /* ---- a chain that runs off the end of the window ---- */
    {
        mem_source(&src, &m, mfc_3ccfem556, (UWORD)sizeof(mfc_3ccfem556));
        src.size = 0x40;        /* the link target is in, the CONFIG is not */
        expect_int("a chain the window cannot hold is refused",
                   netdev_cis_func(&src, 0x3b, &fn), 0);
    }

    /* ---- the modem function, asked for directly ---- */
    mem_source(&src, &m, mfc_3ccfem556, (UWORD)sizeof(mfc_3ccfem556));
    expect_int("function 1's chain parses", netdev_cis_func(&src, 0x58, &fn), 1);
    expect_int("function 1 is a serial port", fn.funcid, CIS_FUNC_SERIAL);
    expect_hex("function 1 has its own configuration base", fn.cfg_base,
               0x1100);
}

int main(void)
{
    NetdevCisEntry e;

    /* ---- the CNet CN40BC, byte for byte the emulator's card ---- */
    expect_int("cn40bc parses",
               netdev_cis_cftable(cnet_cn40bc, (UWORD)sizeof(cnet_cn40bc), &e),
               1);
    expect_int("cn40bc index", e.index, 32);
    expect_int("cn40bc is the default entry",
               (e.flags & NETDEV_CIS_DEFAULT) != 0, 1);
    expect_int("cn40bc interface is I/O", e.iface, NETDEV_CIS_IF_IO);
    expect_int("cn40bc address lines", e.io_lines, 5);
    expect_int("cn40bc windows", e.io_nwin, 1);
    expect_hex("cn40bc window base", e.io_base, 0x0000);
    expect_int("cn40bc window length", e.io_len, 32);
    expect_int("cn40bc offers 8-bit", (e.flags & NETDEV_CIS_IO8) != 0, 1);
    expect_int("cn40bc offers 16-bit", (e.flags & NETDEV_CIS_IO16) != 0, 1);
    expect_int("cn40bc offers a level interrupt",
               (e.flags & NETDEV_CIS_IRQ_LEVEL) != 0, 1);
    expect_int("cn40bc is usable", netdev_cis_usable(&e), 1);
    expect_int("cn40bc scores best", netdev_cis_score(&e),
               NETDEV_CIS_SCORE_BEST);
    /* The one that matters: the walk must not move a card that already works
       off the offset it works at. */
    expect_hex("cn40bc keeps the assumed offset",
               netdev_cis_io_off(&e, 0x0300), 0x0300);

    /* ---- a card that names its own base ---- */
    expect_int("fixed-base parses",
               netdev_cis_cftable(fixed_base_320,
                                  (UWORD)sizeof(fixed_base_320), &e), 1);
    expect_int("fixed-base address lines", e.io_lines, 10);
    expect_int("fixed-base windows", e.io_nwin, 1);
    expect_hex("fixed-base window base", e.io_base, 0x0320);
    expect_int("fixed-base window length", e.io_len, 1);
    /* Ten decoded lines and a base of $320: $300 misses it. */
    expect_hex("fixed-base overrides the assumption",
               netdev_cis_io_off(&e, 0x0300), 0x0320);
    /* One byte of window is not a register file. */
    expect_int("fixed-base one-byte window is unusable",
               netdev_cis_usable(&e), 0);

    /* ---- a memory-only entry ---- */
    expect_int("memory-only parses",
               netdev_cis_cftable(memory_only, (UWORD)sizeof(memory_only), &e),
               1);
    expect_int("memory-only has no I/O space",
               (e.flags & NETDEV_CIS_HAS_IO) != 0, 0);
    expect_int("memory-only is unusable", netdev_cis_usable(&e), 0);
    expect_int("memory-only scores nothing", netdev_cis_score(&e),
               NETDEV_CIS_SCORE_NONE);
    expect_hex("memory-only keeps the assumption",
               netdev_cis_io_off(&e, 0x0300), 0x0300);

    /* ---- widths ---- */
    expect_int("wide-only parses",
               netdev_cis_cftable(wide_only, (UWORD)sizeof(wide_only), &e), 1);
    expect_int("wide-only address lines", e.io_lines, 5);
    expect_int("wide-only is usable", netdev_cis_usable(&e), 1);
    expect_int("wide-only scores below best", netdev_cis_score(&e),
               NETDEV_CIS_SCORE_WIDE);

    expect_int("unstated width parses",
               netdev_cis_cftable(width_unstated,
                                  (UWORD)sizeof(width_unstated), &e), 1);
    expect_int("unstated width scores best", netdev_cis_score(&e),
               NETDEV_CIS_SCORE_BEST);

    /* ---- a body that ends early ---- */
    expect_int("truncated is refused",
               netdev_cis_cftable(truncated, (UWORD)sizeof(truncated), &e), 0);
    expect_int("an empty body is refused",
               netdev_cis_cftable(cnet_cn40bc, 0, &e), 0);
    expect_int("a null body is refused", netdev_cis_cftable(NULL, 17, &e), 0);

    /* A refused parse still leaves a zeroed entry, so a caller that ignores
       the return value cannot act on a stale window. */
    expect_int("a refused parse leaves no I/O window", e.io_nwin, 0);

    /* ---- multifunction cards, whose CIS CopyTuple() cannot reach ---- */
    test_mfc();

    if (failures != 0)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }

    printf("all cases pass\n");
    return 0;
}
