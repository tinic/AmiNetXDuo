/*
 * 802.11 PC Cards by MANFID, for anxnet.device (netdev_pcmcia.c) and
 * InstallNetProbe/CheckNetDevice (src/tools/netprobe_card.c): Neil
 * Cafferkey's prism2.device 2.9 identity list and the tables of Linux
 * hostap_cs.c, orinoco_cs.c, spectrum_cs.c, airo_cs.c and atmel_cs.c (v6.6).
 * A Prism2 card need not say it is wireless in its CIS, and its FUNCID says
 * LAN as an NE2000's does, so the identity is what keeps an NE2000 probe off
 * it.  Header-only, so both sides use one list.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NETDEV_WIRELESS_H
#define NETDEV_WIRELESS_H

static const unsigned long netdev_wireless_ids[] =
{
    0x000b7100UL, 0x000b7300UL, 0x0032000cUL, 0x00890001UL, 0x00890002UL,
    0x01010001UL, 0x01010620UL, 0x01010696UL, 0x01010777UL, 0x01040001UL,
    0x01050000UL, 0x01050007UL, 0x01260002UL, 0x01268000UL, 0x01380002UL,
    0x014d0801UL, 0x01560002UL, 0x01560003UL, 0x015f0005UL, 0x015f0007UL,
    0x015f000aUL, 0x016b0001UL, 0x016c0001UL, 0x01bf2328UL, 0x01bf3301UL,
    0x01bf3302UL, 0x01eb080aUL, 0x01ff0008UL, 0x02500002UL, 0x02610002UL,
    0x02680001UL, 0x02680003UL, 0x026c0001UL, 0x026f0305UL, 0x026f030bUL,
    0x02741612UL, 0x02741613UL, 0x02743301UL, 0x028a0002UL, 0x028a0673UL,
    0x02aa0002UL, 0x02ac0002UL, 0x02ac3021UL, 0x02d20001UL, 0x14eab001UL,
    0x16680101UL, 0x50c20001UL, 0x50c27300UL, 0x90050021UL, 0xc0010008UL,
    0xc00f0000UL, 0xc0380000UL, 0xc2500002UL, 0xd6010002UL, 0xd6010004UL,
    0xd6010005UL, 0xd6010007UL, 0xd6010010UL,
    0
};

/* Nonzero when MANFID manf/prod is an 802.11 card. */
static inline int netdev_wireless_id(unsigned manf, unsigned prod)
{
    unsigned long        id = ((unsigned long)manf << 16) | (unsigned long)prod;
    const unsigned long *p;

    for (p = netdev_wireless_ids; *p != 0; p++)
        if (*p == id)
            return 1;
    return 0;
}

#endif /* NETDEV_WIRELESS_H */
