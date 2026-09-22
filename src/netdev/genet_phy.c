/*
 * anxgenet.device: the PHY and link decisions (genet_phy.h).
 *
 * SPDX-License-Identifier: MIT
 */

#include "genet_phy.h"
#include "genetreg.h"

BOOL genet_phy_link_up(LONG bmsr)
{
    return (BOOL)(bmsr >= 0 && (bmsr & BMSR_LINK) != 0);
}

BOOL genet_phy_negotiated(LONG bmsr)
{
    const LONG done = BMSR_LINK | BMSR_ACOMP;

    return (BOOL)(bmsr >= 0 && (bmsr & done) == done);
}

/* The negotiated speed comes from the Broadcom auxiliary status register,
   which says what was resolved without decoding both sides'
   advertisements. */
UBYTE genet_phy_speed(LONG aux)
{
    switch (aux & BRGPHY_AUXSTS_AN_RES)
    {
    case BRGPHY_RES_1000FD:
    case BRGPHY_RES_1000HD:
        return GENET_UMAC_CMD_SPEED_1000;
    case BRGPHY_RES_100FD:
    case BRGPHY_RES_100T4:
    case BRGPHY_RES_100HD:
        return GENET_UMAC_CMD_SPEED_100;
    default:
        return GENET_UMAC_CMD_SPEED_10;
    }
}

ULONG genet_link_stat(UBYTE up, UBYTE speed)
{
    return up ? (ULONG)(speed >> 2) + 1UL : 0UL;
}

ULONG genet_phyid(LONG id1, LONG id2)
{
    return (id1 < 0 || id2 < 0)
         ? 0xffffffffUL
         : (((ULONG)id1 << 16) | (ULONG)id2);
}

UWORD genet_phy_auxctl_misc(LONG v)
{
    v = (v & BCM54_AUXCTL_MISC_DATA) | BCM54_AUXCTL_MISC_RXSKEW;
    return (UWORD)(BCM54_AUXCTL_MISC_WREN | BCM54_AUXCTL_SHD_MISC | v);
}

UWORD genet_phy_shd1c_clkctrl(LONG v)
{
    v = (v & BCM54_SHD1C_DATA) & ~BCM54_SHD1C_GTXCLK;
    return (UWORD)(BCM54_SHD1C_WREN | BCM54_SHD1C_CLKCTRL | v);
}

ULONG genet_rgmii_oob_word(ULONG v)
{
    v &= ~GENET_EXT_RGMII_OOB_OOB_DISABLE;
    v |= GENET_EXT_RGMII_OOB_RGMII_LINK | GENET_EXT_RGMII_OOB_RGMII_MODE_EN;
    /* rgmii-rxid: the PHY supplies the receive delay and the MAC keeps its
       internal transmit one, so the ID-mode-disable bit stays clear. */
    v &= ~GENET_EXT_RGMII_OOB_ID_MODE_DISABLE;
    return v;
}

ULONG genet_umac_cmd_speed(ULONG cmd, UBYTE speed)
{
    return (cmd & ~GENET_UMAC_CMD_SPEED_MASK) | speed;
}
