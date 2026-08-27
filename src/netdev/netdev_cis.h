/*
 * anxnet.device: the PC Card configuration table, parsed.
 *
 * CISTPL_CFTABLE_ENTRY is the one tuple in a card's CIS that cannot be read by
 * looking at fixed offsets.  Everything in it is optional and every optional
 * part is variable length, so the I/O descriptor -- the only field that says
 * where the card decodes -- can only be reached by walking the power and
 * timing descriptors in front of it.  netdev_pcmcia.c assumed the register
 * offset instead, which is right for a card that decodes five address lines
 * and wrong for one that decodes ten.
 *
 * Split out of netdev_pcmcia.c because that file is m68k inline asm and Gayle
 * addresses from its first line, and this is byte arithmetic that a host can
 * run.  src/netdev/test/test_netdev_cis.c is the whole reason.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETDEV_CIS_H
#define NETDEV_CIS_H

#include <exec/types.h>

/* PC Card standard, release 2, CISTPL_CFTABLE_ENTRY.  Only the parts that
   decide whether this driver can drive the entry, and where. */
typedef struct
{
    UBYTE index;        /* TPCE_INDX bits 5..0: the byte the COR takes       */
    UBYTE iface;        /* TPCE_IF bits 3..0, valid with NETDEV_CIS_HAS_IF   */
    UBYTE io_lines;     /* address lines the card decodes; 0 = decodes none  */
    UBYTE io_nwin;      /* I/O windows described; 0 = no I/O space at all    */
    UWORD io_base;      /* first window's base in the card's I/O space       */
    UWORD io_len;       /* first window's length in bytes                    */
    UBYTE flags;
} NetdevCisEntry;

#define NETDEV_CIS_DEFAULT   0x01u  /* TPCE_INDX bit 6: entry carries defaults */
#define NETDEV_CIS_HAS_IF    0x02u  /* TPCE_IF was present                     */
#define NETDEV_CIS_HAS_IO    0x04u  /* an I/O space descriptor was present     */
#define NETDEV_CIS_IO8       0x08u  /* the window answers 8-bit accesses       */
#define NETDEV_CIS_IO16      0x10u  /* the window answers 16-bit accesses      */
#define NETDEV_CIS_HAS_IRQ   0x20u  /* an IRQ descriptor was present           */
#define NETDEV_CIS_IRQ_LEVEL 0x40u  /* it offers level mode, which Gayle wants */

/* TPCE_IF interface types.  0 is a memory card; 1 is the I/O and memory
   interface every LAN card uses.  2..3 are custom. */
#define NETDEV_CIS_IF_MEMORY 0u
#define NETDEV_CIS_IF_IO     1u

/* A register file smaller than this is not a NE2000 (32) or an EtherLink III
   (16), so an entry offering less than 16 ports is not our configuration. */
#define NETDEV_CIS_IO_MIN    16u

/*
 * Parse one CISTPL_CFTABLE_ENTRY body -- the bytes after TPL_CODE and
 * TPL_LINK, len of them.  FALSE means the entry ran off the end of what was
 * copied, which is either a truncated read or a corrupt CIS; *out is then
 * filled as far as the parse got and must not be used.
 */
BOOL netdev_cis_cftable(const UBYTE *body, UWORD len, NetdevCisEntry *out);

/* Can this driver configure the card into this entry?  An entry with no I/O
   space, a memory-only interface, or a window too small for a register file
   describes a configuration we have no way to drive. */
BOOL netdev_cis_usable(const NetdevCisEntry *e);

/*
 * How well an entry suits this driver.  Higher wins and zero is unusable, so
 * a walk keeps the best-scoring entry and can stop at NETDEV_CIS_SCORE_BEST.
 *
 * The one thing that separates two usable entries is access width.  Every
 * register path here is byte-wide over Gayle's split window, so an entry that
 * offers 16-bit accesses and refuses 8-bit ones is a configuration the card
 * would accept and this driver could not then read.  Such an entry still
 * scores above nothing, because a card that offers no other is better tried
 * than skipped.
 */
UWORD netdev_cis_score(const NetdevCisEntry *e);

#define NETDEV_CIS_SCORE_NONE  0u
#define NETDEV_CIS_SCORE_WIDE  1u   /* 16-bit only: the byte path may not read it */
#define NETDEV_CIS_SCORE_BEST  3u

/*
 * The register offset to use for an entry, given the card row's assumption.
 *
 * A card decodes io_lines address lines and ignores everything above them, so
 * the entry's window base binds only in those bits.  When the row's assumed
 * offset already agrees in them -- which is every card that decodes five lines
 * and leaves placement to the host -- the assumption stands, because Gayle
 * fixes the Amiga address and only the low bits are ours to choose.  When they
 * disagree the card answers at its own base and nowhere else.
 *
 * A window base of zero never overrides.  No LAN card decodes at absolute port
 * zero, and a range descriptor that says so is a CIS repeating the "the host
 * places this" default in the other notation.  Treating it as an address would
 * move a working card off its registers.
 */
UWORD netdev_cis_io_off(const NetdevCisEntry *e, UWORD assumed);

/* ------------------------------------------------------ the raw CIS walk -- */

/*
 * Card Information Structure tuple codes, PC Card standard release 2.  Here
 * rather than in netdev_pcmcia.c because the walk below reads them itself.
 */
#define CISTPL_NULL         0x00
#define CISTPL_DEVICE       0x01
#define CISTPL_LONGLINK_MFC 0x06
#define CISTPL_CHECKSUM     0x10
#define CISTPL_LONGLINK_A   0x11
#define CISTPL_LONGLINK_C   0x12
#define CISTPL_LINKTARGET   0x13
#define CISTPL_NO_LINK      0x14
#define CISTPL_VERS_1       0x15
#define CISTPL_CONFIG       0x1a
#define CISTPL_CFTABLE      0x1b
#define CISTPL_MANFID       0x20
#define CISTPL_FUNCID       0x21
#define CISTPL_FUNCE        0x22
#define CISTPL_END          0xff

/* CISTPL_FUNCID values.  0 is the one that matters here: a multifunction card
   states it in the shared chain and states the real function in each of the
   chains CISTPL_LONGLINK_MFC names. */
#define CIS_FUNC_MULTI      0
#define CIS_FUNC_SERIAL     2
#define CIS_FUNC_LAN        6

/* CISTPL_FUNCE subtuple: the LAN function's station address. */
#define CIS_FUNCE_LAN_NODE_ID   0x04

/* CISTPL_LONGLINK_MFC target address space, TPLMFC_TAS. */
#define CIS_MFC_COMMON      0
#define CIS_MFC_ATTR        1

#define NETDEV_CIS_NODE_LEN 6

/*
 * A reader over the card's own CIS address space, byte by byte.
 *
 * card.resource's CopyTuple() understands CISTPL_LONGLINK_A, CISTPL_LONGLINK_C,
 * CISTPL_NO_LINK and CISTPL_LINKTARGET -- the cardres.doc autodoc names those
 * four and no others -- so a multifunction card's per-function chains, which
 * hang off CISTPL_LONGLINK_MFC, are not reachable through it at all.  The walk
 * below reads the CIS itself instead, and the one Amiga-specific fact (the
 * card's byte n is at 0xA00000 + 2n) stays in the reader netdev_pcmcia.c
 * supplies, so everything here runs on a host over a recorded CIS.
 *
 * `size` bounds every read: a corrupt link that points past the window ends
 * the walk instead of wandering into the card's I/O space at 0xA20000.
 */
typedef UBYTE (*NetdevCisReadFn)(APTR ctx, ULONG off);

typedef struct
{
    NetdevCisReadFn read;
    APTR            ctx;
    ULONG           size;
} NetdevCisSource;

/* A CIS is 64 KB of card address space at most, and a chain of more tuples
   than this is a runaway rather than a card. */
#define NETDEV_CIS_MAX_FUNC     8
#define NETDEV_CIS_MAX_TUPLES   64

/* What one function's chain said about itself. */
typedef struct
{
    ULONG          chain;       /* where the chain began                     */
    ULONG          cfg_base;    /* TPCC_RADR, the configuration register base */
    UWORD          cfg_mask;    /* TPCC_RMSK, low 16 bits: registers present */
    UBYTE          cfg_last;    /* TPCC_LAST                                 */
    UBYTE          funcid;      /* CISTPL_FUNCID, valid with _HAS_FUNCID     */
    UBYTE          index;       /* the chosen entry's configuration index    */
    UWORD          score;       /* netdev_cis_score() of the chosen entry    */
    NetdevCisEntry pick;
    UBYTE          node_id[NETDEV_CIS_NODE_LEN];
    UBYTE          flags;
} NetdevCisFunc;

#define NETDEV_CISF_HAS_CONFIG  0x01u
#define NETDEV_CISF_HAS_PICK    0x02u
#define NETDEV_CISF_HAS_FUNCID  0x04u
#define NETDEV_CISF_HAS_NODEID  0x08u

/*
 * Configuration register numbers.  Register n is at TPCC_RADR + 2n, which is
 * why TPCC_RMSK is a bit per register and not a bit per byte.
 *
 * TWO UNITS MEET HERE AND THEY ARE NOT THE SAME.  A CIS offset -- what
 * CISTPL_LONGLINK_MFC names, and what NetdevCisSource reads -- counts the
 * card's CIS bytes, and attribute memory holds byte n at address 2n.  TPCC_RADR
 * does not: it is already an address in that space, which is why its registers
 * are two apart rather than one.  So the COR of a function is at
 * attribute_base + cfg_base, and CIS byte n is at attribute_base + 2n.  Linux
 * says the same thing in one line each: pcmcia_read_cis_mem() doubles a CIS
 * offset, and pcmcia_access_config() halves TPCC_RADR before handing it to the
 * same routine.
 */
#define CIS_REG_COR         0u
#define CIS_REG_CCSR        1u
#define CIS_REG_IOBASE_0    5u
#define CIS_REG_IOBASE_1    6u
#define CIS_REG_IOSIZE      9u

/*
 * Configuration Option Register bits.
 *
 * A single-function card takes the whole six-bit configuration index.  A
 * multifunction card does not: bits 2..0 are function enable, address decode
 * and interrupt enable, so only bits 5..3 of the index are written and the
 * three control bits go in underneath it.  Writing a single-function COR to a
 * multifunction card leaves the function disabled and decoding nothing.
 */
#define CIS_COR_CONFIG_MASK 0x3fu
#define CIS_COR_MFC_MASK    0x38u
#define CIS_COR_FUNC_ENA    0x01u
#define CIS_COR_ADDR_DECODE 0x02u
#define CIS_COR_IREQ_ENA    0x04u
#define CIS_COR_LEVEL_REQ   0x40u

/*
 * How many function chains CISTPL_LONGLINK_MFC names, and where each begins.
 * Zero means the card is not multifunction -- no such tuple, or one this walk
 * could not believe.  Writes at most `max` chain addresses.
 */
UWORD netdev_cis_mfc_chains(const NetdevCisSource *src, ULONG *chains,
                            UWORD max);

/*
 * Walk one function's chain and fill *out.  FALSE means the chain is not one:
 * it must open with CISTPL_LINKTARGET carrying "CIS", which is what says the
 * link address was right, and it must carry a CISTPL_CONFIG, which is what
 * says the function can be configured at all.
 */
BOOL netdev_cis_func(const NetdevCisSource *src, ULONG chain,
                     NetdevCisFunc *out);

/*
 * The LAN function of a multifunction card, or FALSE when there is none.
 * *nfunc, when it is not NULL, is left holding how many functions the card
 * named whatever the answer is -- 0 for a card that is not multifunction --
 * so a caller can tell "not a multifunction card" from "multifunction and no
 * function of it is a LAN adapter this driver could drive".
 */
BOOL netdev_cis_mfc_lan(const NetdevCisSource *src, NetdevCisFunc *out,
                        UWORD *nfunc);

/*
 * The byte to write to a multifunction card's COR for this function, and the
 * value for its I/O size register.  io_off is the base the host has chosen,
 * which on this machine is fixed by Gayle and by the card row.
 */
UBYTE netdev_cis_mfc_cor(const NetdevCisFunc *fn);
UBYTE netdev_cis_mfc_iosize(const NetdevCisFunc *fn);

/* Does this function's CISTPL_CONFIG say the I/O base registers exist?  Only
   then can the host tell the card where to decode. */
BOOL netdev_cis_has_iobase(const NetdevCisFunc *fn);

#endif /* NETDEV_CIS_H */
