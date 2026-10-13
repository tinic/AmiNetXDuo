/*
 * InstallNetProbe: which driver a PC Card in the A600/A1200 slot needs, from
 * the identity tuples alone; CheckNetDevice: the slot report.  No AmigaOS
 * calls, so test/test_netprobe_card.c runs it on the build machine.
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
    NETPROBE_PC_PRISM2,
    NETPROBE_PC_XIRCOM
};

/*
 * Prism2 and Xircom use their packaged drivers' supported MANFID tables.
 * For the other cards, anxnet.device's rule (netdev_card_by_cis) applies:
 * a known EtherLink III MANFID
 * names its row, and any other LAN card is the NE2000 one.  Less the 802.11
 * cards without a supported Prism2 identity, whose FUNCID says LAN as an
 * NE2000's does: a FUNCE stating wireless
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

/* ---- what is in the slot, for CheckNetDevice ---- */

#define NETPROBE_VERS1_MAX 128
#define NETPROBE_OWNER_MAX 32

enum
{
    NETPROBE_SLOT_NONE = 0, /* no card.resource: no PCMCIA slot            */
    NETPROBE_SLOT_EMPTY,    /* card detect clear                           */
    NETPROBE_SLOT_FREE,     /* read through CopyTuple(), slot then released */
    NETPROBE_SLOT_OWNED     /* another driver's: read from attribute memory */
};

typedef struct
{
    NetprobeCis cis;
    uint8_t     slot;                       /* NETPROBE_SLOT_*             */
    uint8_t     have_vers1;
    char        vers1[NETPROBE_VERS1_MAX];  /* "a", "b", ... as printed    */
    char        owner[NETPROBE_OWNER_MAX];  /* owning handle's name or ""  */
} NetprobeSlot;

/*
 * CISTPL_VERS_1's strings from its body (link bytes at body), as
 * "a", "b", ... into out.  Unprintable bytes show as '?'; a list that does
 * not fit ends in "...".  Nonzero when at least one string was there.
 */
int netprobe_vers1_text(uint8_t (*read)(void *ctx, uint32_t n), void *ctx,
                        uint32_t body, uint32_t link, char *out,
                        uint32_t outsize);

/* The same, found in a primary CIS chain as netprobe_cis_walk() reads it. */
int netprobe_cis_vers1(uint8_t (*read)(void *ctx, uint32_t n), void *ctx,
                       uint32_t size, char *out, uint32_t outsize);

/* The driver in the archive for a netprobe_pc_kind(), or NULL for none. */
const char *netprobe_pc_driver(int kind);

/* The slot report, one line per call, without indent or newline. */
void netprobe_slot_report(const NetprobeSlot *slot,
                          void (*line)(void *ctx, const char *text),
                          void *ctx);

#endif /* NETPROBE_CARD_H */
