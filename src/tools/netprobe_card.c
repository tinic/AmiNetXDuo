/*
 * InstallNetProbe: the PC Card decision.  See netprobe_card.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netprobe_card.h"

#define CIS_FUNC_LAN            6
#define CIS_FUNCE_LAN_TECH      0x01
#define CIS_LAN_TECH_WIRELESS   0x07
#define CISTPL_NULL             0x00
#define CISTPL_MANFID           0x20
#define CISTPL_FUNCID           0x21
#define CISTPL_FUNCE            0x22
#define CISTPL_END              0xff

/* 802.11 PC Cards by MANFID: the tables of Linux hostap_cs.c, orinoco_cs.c,
   spectrum_cs.c, airo_cs.c and atmel_cs.c (v6.6).  Pairs, because 3Com's 0x0101 also
   makes the EtherLink III. */
static const uint16_t wireless_ids[][2] =
{
    { 0x000b, 0x7100 }, { 0x000b, 0x7300 }, { 0x0089, 0x0002 },
    { 0x0101, 0x0620 }, { 0x0101, 0x0696 }, { 0x0101, 0x0777 },
    { 0x0104, 0x0001 }, { 0x0105, 0x0007 }, { 0x0126, 0x0002 },
    { 0x0126, 0x8000 }, { 0x0138, 0x0002 }, { 0x0156, 0x0002 },
    { 0x015f, 0x0005 }, { 0x015f, 0x0007 }, { 0x015f, 0x000a },
    { 0x016b, 0x0001 }, { 0x01bf, 0x3301 }, { 0x01bf, 0x3302 },
    { 0x01eb, 0x080a }, { 0x01ff, 0x0008 }, { 0x0250, 0x0002 }, { 0x0261, 0x0002 }, { 0x0268, 0x0001 },
    { 0x0268, 0x0003 }, { 0x026f, 0x0305 }, { 0x026f, 0x030b },
    { 0x0274, 0x1612 }, { 0x0274, 0x1613 }, { 0x028a, 0x0002 },
    { 0x028a, 0x0673 }, { 0x02aa, 0x0002 }, { 0x02ac, 0x0002 },
    { 0x026c, 0x0001 }, { 0x02ac, 0x3021 }, { 0x02d2, 0x0001 },
    { 0x14ea, 0xb001 },
    { 0x50c2, 0x0001 }, { 0x50c2, 0x7300 }, { 0x9005, 0x0021 },
    { 0xc001, 0x0008 }, { 0xc250, 0x0002 }, { 0xd601, 0x0002 },
    { 0xd601, 0x0005 }, { 0xd601, 0x0007 }, { 0xd601, 0x0010 }
};

static int wireless(const NetprobeCis *cis)
{
    unsigned i;

    if (cis->have_funce && cis->funce[0] == CIS_FUNCE_LAN_TECH &&
        cis->funce[1] == CIS_LAN_TECH_WIRELESS)
        return 1;
    if (!cis->have_manf)
        return 0;
    for (i = 0; i < sizeof(wireless_ids) / sizeof(wireless_ids[0]); i++)
        if (wireless_ids[i][0] == cis->manf && wireless_ids[i][1] == cis->prod)
            return 1;
    return 0;
}

/* Known wireless identities must be excluded before LAN-to-NE2000 fallback.
   Retain the Prism2 2.9 identity list solely to reject those cards, including
   cards whose CIS does not state wireless technology. */
static const uint32_t excluded_wireless_ids[] =
{
    0x000b7100UL,
    0x000b7300UL,
    0x0032000cUL,
    0x00890001UL,
    0x00890002UL,
    0x01010001UL,
    0x01010777UL,
    0x01050000UL,
    0x01260002UL,
    0x01268000UL,
    0x01380002UL,
    0x014d0801UL,
    0x01560002UL,
    0x01560003UL,
    0x016b0001UL,
    0x016c0001UL,
    0x01bf2328UL,
    0x01bf3301UL,
    0x01eb080aUL,
    0x01ff0008UL,
    0x02500002UL,
    0x02610002UL,
    0x02680001UL,
    0x02680003UL,
    0x026f0305UL,
    0x026f030bUL,
    0x02741612UL,
    0x02741613UL,
    0x02743301UL,
    0x028a0002UL,
    0x028a0673UL,
    0x02aa0002UL,
    0x02ac0002UL,
    0x02ac3021UL,
    0x02d20001UL,
    0x14eab001UL,
    0x16680101UL,
    0x50c20001UL,
    0x50c27300UL,
    0x90050021UL,
    0xc0010008UL,
    0xc00f0000UL,
    0xc0380000UL,
    0xc2500002UL,
    0xd6010002UL,
    0xd6010004UL,
    0xd6010005UL,
    0xd6010010UL,
    0
};

/* Wired Xircom identities from the packaged xircomce2.device 1.1. */
static const uint32_t xircom_ids[] =
{
    0x0089010aUL,
    0x0089110aUL,
    0x01050108UL,
    0x0105010aUL,
    0x0105010bUL,
    0x0105110aUL,
    0x0105110bUL,
    0x0105110cUL,
    0x0105110dUL,
    0x0138110aUL,
    0x0183010aUL,
    0x01bf010aUL,
    0
};

static int has_id(const uint32_t *ids, uint32_t id)
{
    while (*ids != 0)
        if (*ids++ == id)
            return 1;
    return 0;
}

int netprobe_pc_kind(const NetprobeCis *cis)
{
    if (cis->have_manf)
    {
        uint32_t id = ((uint32_t)cis->manf << 16) | cis->prod;
        if (has_id(excluded_wireless_ids, id))
            return NETPROBE_PC_NONE;
        if (has_id(xircom_ids, id))
            return NETPROBE_PC_XIRCOM;
    }
    /* The EtherLink III rows, netdev_cards.c, by MANFID alone as the driver
       matches them. */
    if (cis->have_manf && cis->manf == 0x0101)
    {
        if (cis->prod == 0x0589)
            return NETPROBE_PC_3C589;
        if (cis->prod == 0x0556)
            return NETPROBE_PC_3CCFEM556;
        if (cis->prod == 0x0035)
            return NETPROBE_PC_3CXEM556;
    }
    /* Only a card that says LAN.  The driver also tries a card with no
       FUNCID, or a multifunction one; the probe does not call either an
       NE2000, so a modem, a disk or an SRAM card is never preselected. */
    if (!cis->have_funcid || cis->funcid != CIS_FUNC_LAN)
        return NETPROBE_PC_NONE;
    return wireless(cis) ? NETPROBE_PC_NONE : NETPROBE_PC_NE2000;
}

int netprobe_cis_walk(uint8_t (*read)(void *ctx, uint32_t n), void *ctx,
                      uint32_t size, NetprobeCis *cis)
{
    uint32_t pos = 0;

    while (pos + 1 < size)
    {
        uint8_t  code = read(ctx, pos);
        uint8_t  link;
        uint32_t body;

        if (code == CISTPL_END)
            break;
        if (code == CISTPL_NULL)
        {
            pos++;
            continue;
        }
        link = read(ctx, pos + 1);
        if (link == 0xff)
            break;
        body = pos + 2;
        if (body + link > size)
            break;

        if (code == CISTPL_MANFID && link >= 4 && !cis->have_manf)
        {
            cis->manf = (uint16_t)(((uint16_t)read(ctx, body + 1) << 8) |
                                   read(ctx, body));
            cis->prod = (uint16_t)(((uint16_t)read(ctx, body + 3) << 8) |
                                   read(ctx, body + 2));
            cis->have_manf = 1;
        }
        else if (code == CISTPL_FUNCID && link >= 1 && !cis->have_funcid)
        {
            cis->funcid = read(ctx, body);
            cis->have_funcid = 1;
        }
        else if (code == CISTPL_FUNCE && link >= 2 && !cis->have_funce)
        {
            cis->funce[0] = read(ctx, body);
            cis->funce[1] = read(ctx, body + 1);
            cis->have_funce = 1;
        }
        pos = body + link;
    }
    return cis->have_manf || cis->have_funcid;
}
