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

#endif /* NETDEV_CIS_H */
