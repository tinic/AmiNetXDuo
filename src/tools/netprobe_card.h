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
    NETPROBE_PC_NONE = 0,   /* no supported card identity                  */
    NETPROBE_PC_NE2000,     /* its NE2000 row: cnet.device or anxnet pcmcia */
    NETPROBE_PC_3C589,
    NETPROBE_PC_3CCFEM556,
    NETPROBE_PC_3CXEM556,
    NETPROBE_PC_XIRCOM
};

/*
 * Xircom uses its packaged driver's supported MANFID table.
 * For the other cards, anxnet.device's rule (netdev_card_by_cis) applies:
 * known EtherLink III identities select their row; other LAN cards select
 * NE2000, excluding known wireless identities and wireless FUNCE technology.
 * Cards with no FUNCID or only multifunction are not called NE2000.
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
