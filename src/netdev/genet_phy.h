/*
 * anxgenet.device: what the PHY's registers mean and what the link side of
 * the MAC is told, without the MDIO transaction or the register write.
 *
 * genet.c's link poll runs once a second from the link task, and its PHY
 * set-up once at init; every read and write stays there, in its order.  The
 * decisions between them -- which UMAC speed a Broadcom auxiliary status
 * names, whether a BMSR says negotiation is done, the BCM54213PE's shadow
 * register words -- are here, where a host test can pin them.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_GENET_PHY_H
#define AMINETXDUO_GENET_PHY_H

#include <exec/types.h>

/*
 * The BCM54213PE's RGMII delays: the receive clock skew on (the tree says
 * rgmii-rxid), the transmit clock delay off.  Both live behind shadow
 * registers reached through AUXCTL (0x18) and register 0x1c: select the
 * shadow, read, mask the data bits, write with the write-enable bit.
 */
#define BCM54_AUXCTL            0x18
#define  BCM54_AUXCTL_SHD_MISC  0x0007
#define  BCM54_AUXCTL_MISC_RD   (BCM54_AUXCTL_SHD_MISC << 12)
#define  BCM54_AUXCTL_MISC_WREN 0x8000
#define  BCM54_AUXCTL_MISC_DATA 0x7ff8
#define  BCM54_AUXCTL_MISC_RXSKEW 0x0200
#define BCM54_SHD1C             0x1c
#define  BCM54_SHD1C_CLKCTRL    (0x03 << 10)
#define  BCM54_SHD1C_WREN       0x8000
#define  BCM54_SHD1C_DATA       0x03ff
#define  BCM54_SHD1C_GTXCLK     0x0200

/* The word that selects the AUXCTL misc shadow for a read. */
#define GENET_PHY_AUXCTL_MISC_SELECT \
    (BCM54_AUXCTL_SHD_MISC | BCM54_AUXCTL_MISC_RD)
/* The word that selects the 0x1c clock-control shadow for a read. */
#define GENET_PHY_SHD1C_CLKCTRL_SELECT  BCM54_SHD1C_CLKCTRL

/* TRUE when a BMSR read (-1 on failure) says the link is up. */
BOOL  genet_phy_link_up(LONG bmsr);

/* TRUE when a BMSR read says the link is up and negotiation is complete,
   so a restart would only cost the seconds it takes. */
BOOL  genet_phy_negotiated(LONG bmsr);

/* The GENET_UMAC_CMD_SPEED_* code for a Broadcom auxiliary status word's
   resolved speed: 1000 for either 1000 result, 100 for the three 100
   results, 10 for everything else. */
UBYTE genet_phy_speed(LONG aux);

/* The GE_ST_LINK counter's value: 0 down, else the speed code plus one. */
ULONG genet_link_stat(UBYTE up, UBYTE speed);

/* The PHY identifier the two ID registers make; all ones when either read
   failed. */
ULONG genet_phyid(LONG id1, LONG id2);

/* The AUXCTL misc shadow written back from what was read: data bits kept,
   receive skew on, write-enable and the shadow select set. */
UWORD genet_phy_auxctl_misc(LONG v);

/* The 0x1c clock-control shadow written back from what was read: data bits
   kept, the GTXCLK delay off, write-enable and the shadow select set. */
UWORD genet_phy_shd1c_clkctrl(LONG v);

/* EXT_RGMII_OOB_CTRL for a link that is up in rgmii-rxid mode: OOB off,
   RGMII on with the link bit, and the ID-mode-disable bit clear because the
   PHY supplies the receive delay and the MAC keeps its transmit one. */
ULONG genet_rgmii_oob_word(ULONG v);

/* UMAC_CMD with its speed field replaced. */
ULONG genet_umac_cmd_speed(ULONG cmd, UBYTE speed);

#endif /* AMINETXDUO_GENET_PHY_H */
