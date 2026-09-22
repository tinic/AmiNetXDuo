/*
 * What the GENET core makes of the PHY (genet_phy.h), pinned.
 *
 * The BMSR, the Broadcom auxiliary status and the BCM54213PE's two shadow
 * registers were read and decoded inline in the link poll and the delay
 * set-up; these are the words the poll must derive from them, and the words
 * the set-up must write back, against a link that came up once a second on
 * the A1200 and never off it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "genet_phy.h"
#include "genetreg.h"

static int failures;
static int checks;

static void expect_u32(const char *what, unsigned long got, unsigned long want)
{
    checks++;
    if (got == want)
        return;

    printf("FAIL %s: got 0x%lx (%lu), want 0x%lx (%lu)\n",
           what, got, got, want, want);
    failures++;
}

/* ---------------------------------------------------------------- BMSR --- */

static void a_link_up(void)
{
    expect_u32("link: a failed read is down", genet_phy_link_up(-1), FALSE);
    expect_u32("link: zero is down", genet_phy_link_up(0), FALSE);
    expect_u32("link: the link bit alone", genet_phy_link_up(BMSR_LINK), TRUE);
    expect_u32("link: 0x7949, no link", genet_phy_link_up(0x7949), FALSE);
    expect_u32("link: 0x796d, link and negotiated", genet_phy_link_up(0x796d), TRUE);
    expect_u32("link: all ones", genet_phy_link_up(0xffff), TRUE);
}

static void b_negotiated(void)
{
    expect_u32("negotiated: a failed read is not", genet_phy_negotiated(-1), FALSE);
    expect_u32("negotiated: link alone is not", genet_phy_negotiated(BMSR_LINK), FALSE);
    expect_u32("negotiated: ACOMP alone is not", genet_phy_negotiated(BMSR_ACOMP), FALSE);
    expect_u32("negotiated: both", genet_phy_negotiated(BMSR_LINK | BMSR_ACOMP), TRUE);
    expect_u32("negotiated: 0x796d", genet_phy_negotiated(0x796d), TRUE);
    expect_u32("negotiated: 0x7949", genet_phy_negotiated(0x7949), FALSE);
}

/* -------------------------------------------------------------- AUXSTS --- */

static void c_speed(void)
{
    expect_u32("speed: 1000FD", genet_phy_speed(BRGPHY_RES_1000FD), GENET_UMAC_CMD_SPEED_1000);
    expect_u32("speed: 1000HD", genet_phy_speed(BRGPHY_RES_1000HD), GENET_UMAC_CMD_SPEED_1000);
    expect_u32("speed: 100FD", genet_phy_speed(BRGPHY_RES_100FD), GENET_UMAC_CMD_SPEED_100);
    expect_u32("speed: 100T4", genet_phy_speed(BRGPHY_RES_100T4), GENET_UMAC_CMD_SPEED_100);
    expect_u32("speed: 100HD", genet_phy_speed(BRGPHY_RES_100HD), GENET_UMAC_CMD_SPEED_100);
    expect_u32("speed: 10FD", genet_phy_speed(BRGPHY_RES_10FD), GENET_UMAC_CMD_SPEED_10);
    expect_u32("speed: 10HD", genet_phy_speed(BRGPHY_RES_10HD), GENET_UMAC_CMD_SPEED_10);
    expect_u32("speed: unresolved", genet_phy_speed(0), GENET_UMAC_CMD_SPEED_10);
    expect_u32("speed: the other bits do not count",
               genet_phy_speed(0x8f7d), GENET_UMAC_CMD_SPEED_1000);
    expect_u32("speed: bits above the field do not count",
               genet_phy_speed(0x7000), GENET_UMAC_CMD_SPEED_10);
    expect_u32("speed: the codes are 0, 4, 8",
               (unsigned long)GENET_UMAC_CMD_SPEED_10 |
               (unsigned long)GENET_UMAC_CMD_SPEED_100 << 8 |
               (unsigned long)GENET_UMAC_CMD_SPEED_1000 << 16, 0x080400UL);
}

static void d_link_stat(void)
{
    expect_u32("stat: down", genet_link_stat(0, GENET_UMAC_CMD_SPEED_1000), 0);
    expect_u32("stat: 10", genet_link_stat(1, GENET_UMAC_CMD_SPEED_10), 1);
    expect_u32("stat: 100", genet_link_stat(1, GENET_UMAC_CMD_SPEED_100), 2);
    expect_u32("stat: 1000", genet_link_stat(1, GENET_UMAC_CMD_SPEED_1000), 3);
}

static void e_phyid(void)
{
    expect_u32("phyid: the BCM54213PE", genet_phyid(0x600d, 0x84a2), 0x600d84a2UL);
    expect_u32("phyid: first read failed", genet_phyid(-1, 0x84a2), 0xffffffffUL);
    expect_u32("phyid: second read failed", genet_phyid(0x600d, -1), 0xffffffffUL);
    expect_u32("phyid: zeros", genet_phyid(0, 0), 0);
}

/* --------------------------------------------------------- the shadows --- */

static void f_auxctl(void)
{
    expect_u32("auxctl select word", GENET_PHY_AUXCTL_MISC_SELECT, 0x7007);
    expect_u32("auxctl: from zero", genet_phy_auxctl_misc(0), 0x8207);
    expect_u32("auxctl: from all ones", genet_phy_auxctl_misc(0xffff), 0xffff);
    expect_u32("auxctl: the select bits read back are dropped",
               genet_phy_auxctl_misc(0x0007), 0x8207);
    expect_u32("auxctl: the write-enable read back is dropped",
               genet_phy_auxctl_misc(0x8000), 0x8207);
    expect_u32("auxctl: data kept, skew added", genet_phy_auxctl_misc(0x1c40), 0x9e47);
    expect_u32("auxctl: skew already on", genet_phy_auxctl_misc(0x0200), 0x8207);
}

static void g_shd1c(void)
{
    expect_u32("shd1c select word", GENET_PHY_SHD1C_CLKCTRL_SELECT, 0x0c00);
    expect_u32("shd1c: from zero", genet_phy_shd1c_clkctrl(0), 0x8c00);
    expect_u32("shd1c: from all ones", genet_phy_shd1c_clkctrl(0xffff), 0x8dff);
    expect_u32("shd1c: the delay bit is cleared", genet_phy_shd1c_clkctrl(0x0200), 0x8c00);
    expect_u32("shd1c: the select read back is dropped",
               genet_phy_shd1c_clkctrl(0x0c05), 0x8c05);
    expect_u32("shd1c: the whole data field", genet_phy_shd1c_clkctrl(0x03ff), 0x8dff);
}

/* --------------------------------------------------------- the MAC side --- */

static void h_rgmii(void)
{
    expect_u32("oob: from zero", genet_rgmii_oob_word(0), 0x50);
    expect_u32("oob: from all ones", genet_rgmii_oob_word(0xffffffffUL), 0xfffeffdfUL);
    expect_u32("oob: as the firmware leaves it",
               genet_rgmii_oob_word(GENET_EXT_RGMII_OOB_OOB_DISABLE |
                                    GENET_EXT_RGMII_OOB_ID_MODE_DISABLE), 0x50);
    expect_u32("oob: already set", genet_rgmii_oob_word(0x50), 0x50);
}

static void i_umac_speed(void)
{
    expect_u32("cmd speed: into zero",
               genet_umac_cmd_speed(0, GENET_UMAC_CMD_SPEED_1000), 0x8);
    expect_u32("cmd speed: 1000 down to 10",
               genet_umac_cmd_speed(0xc, GENET_UMAC_CMD_SPEED_10), 0);
    expect_u32("cmd speed: TX and RX enable kept",
               genet_umac_cmd_speed(0x3, GENET_UMAC_CMD_SPEED_100), 0x7);
    expect_u32("cmd speed: everything else kept",
               genet_umac_cmd_speed(0xffffffffUL, GENET_UMAC_CMD_SPEED_100),
               0xfffffff7UL);
}

int main(void)
{
    a_link_up();
    b_negotiated();
    c_speed();
    d_link_stat();
    e_phyid();
    f_auxctl();
    g_shd1c();
    h_rgmii();
    i_umac_speed();

    if (failures != 0)
    {
        printf("netdev_genet_phy: %d of %d checks failed\n", failures, checks);
        return 1;
    }

    printf("netdev_genet_phy: %d checks ok\n", checks);

    return 0;
}
