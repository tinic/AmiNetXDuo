/*
 * InstallNetProbe: the PC Card decision.  See netprobe_card.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netprobe_card.h"

#define CIS_FUNC_LAN            6
#define CIS_FUNCE_LAN_TECH      0x01
#define CIS_LAN_TECH_WIRELESS   0x07

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

int netprobe_pc_kind(const NetprobeCis *cis)
{
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
