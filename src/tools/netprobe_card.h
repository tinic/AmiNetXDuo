/*
 * InstallNetProbe: which driver a PC Card in the A600/A1200 slot needs, from
 * the identity tuples alone.  No AmigaOS calls, so test/test_netprobe_card.c
 * runs it on the build machine.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETPROBE_CARD_H
#define NETPROBE_CARD_H

#include <stdint.h>

/* What card.resource handed back.  A field is meaningful only with its have_
   flag: a card may omit any tuple. */
typedef struct
{
    uint16_t manf;      /* CISTPL_MANFID, TPLMID_MANF                     */
    uint16_t prod;      /* CISTPL_MANFID, TPLMID_CARD                     */
    uint8_t  funcid;    /* CISTPL_FUNCID                                  */
    uint8_t  funce[2];  /* the first CISTPL_FUNCE: type, then its value   */
    uint8_t  have_manf;
    uint8_t  have_funcid;
    uint8_t  have_funce;
} NetprobeCis;

enum
{
    NETPROBE_PC_NONE = 0,   /* not a card anxnet.device drives              */
    NETPROBE_PC_NE2000,     /* its NE2000 row: cnet.device or anxnet pcmcia */
    NETPROBE_PC_3C589,
    NETPROBE_PC_3CCFEM556,
    NETPROBE_PC_3CXEM556
};

/*
 * anxnet.device's own rule (netdev_card_by_cis): a known EtherLink III MANFID
 * names its row, and any other LAN card is the NE2000 one.  Less the 802.11
 * cards, whose FUNCID says LAN as an NE2000's does: a FUNCE stating wireless
 * technology, or a MANFID from Linux's 802.11 PC Card drivers.  Narrower
 * than the driver in one way: a card that states no FUNCID, or only the
 * multifunction one, is not called an NE2000.
 */
int netprobe_pc_kind(const NetprobeCis *cis);

/*
 * The same three tuples, from the card's primary CIS chain read byte by byte:
 * read(ctx, n) is CIS byte n, size the bytes there are.  For a card another
 * driver owns, where CopyTuple() needs the owner's handle.  Nonzero when a
 * MANFID or a FUNCID was found.
 */
int netprobe_cis_walk(uint8_t (*read)(void *ctx, uint32_t n), void *ctx,
                      uint32_t size, NetprobeCis *cis);

#endif /* NETPROBE_CARD_H */
