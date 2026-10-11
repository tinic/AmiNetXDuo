/*
 * InstallNetProbe's PC Card decision (netprobe_card.c), over the identity
 * tuples of real cards.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netprobe_card.h"

#include <stdio.h>

static unsigned long checks;
static unsigned long failures;

static void expect_kind(const NetprobeCis *cis, int want, const char *what)
{
    int got = netprobe_pc_kind(cis);

    checks++;
    if (got != want)
    {
        failures++;
        printf("  FAIL %s: kind %d, want %d\n", what, got, want);
    }
}

int main(void)
{
    /* Amiberry's CNet CN40BC: FUNCID LAN, no MANFID. */
    NetprobeCis cnet = { 0, 0, 6, { 0, 0 }, 0, 1, 0 };
    /* D-Link DFE-670TXD, DL10019: a MANFID and FUNCID LAN, as most branded
       NE2000 clones. */
    NetprobeCis dfe670 = { 0x0149, 0x4530, 6, { 0, 0 }, 1, 1, 0 };
    /* The same, with a FUNCE stating Ethernet technology. */
    NetprobeCis dfe670e = { 0x0149, 0x4530, 6, { 0x01, 0x02 }, 1, 1, 1 };
    NetprobeCis el3 = { 0x0101, 0x0589, 6, { 0, 0 }, 1, 1, 0 };
    NetprobeCis cfem = { 0x0101, 0x0556, 0, { 0, 0 }, 1, 1, 0 };
    NetprobeCis cxem = { 0x0101, 0x0035, 0, { 0, 0 }, 1, 1, 0 };
    /* Intersil Prism2 reference and Linksys WPC11 v3: LAN by FUNCID. */
    NetprobeCis prism2 = { 0x0156, 0x0002, 6, { 0, 0 }, 1, 1, 0 };
    NetprobeCis wpc11 = { 0x0274, 0x1613, 6, { 0, 0 }, 1, 1, 0 };
    NetprobeCis airconnect = { 0x0101, 0x0777, 6, { 0, 0 }, 1, 1, 0 };
    NetprobeCis aironet = { 0x015f, 0x0007, 6, { 0, 0 }, 1, 1, 0 };
    /* An unknown maker whose FUNCE says wireless. */
    NetprobeCis wlan = { 0x1234, 0x0001, 6, { 0x01, 0x07 }, 1, 1, 1 };
    /* Not network cards. */
    NetprobeCis modem = { 0x0149, 0x4530, 2, { 0, 0 }, 1, 1, 0 };
    NetprobeCis disk = { 0, 0, 4, { 0, 0 }, 0, 1, 0 };
    NetprobeCis bare = { 0x0149, 0x4530, 0, { 0, 0 }, 1, 0, 0 };

    expect_kind(&cnet, NETPROBE_PC_NE2000, "CNet CN40BC");
    expect_kind(&dfe670, NETPROBE_PC_NE2000, "DFE-670TXD");
    expect_kind(&dfe670e, NETPROBE_PC_NE2000, "DFE-670TXD, FUNCE Ethernet");
    expect_kind(&el3, NETPROBE_PC_3C589, "3C589");
    expect_kind(&cfem, NETPROBE_PC_3CCFEM556, "3CCFEM556");
    expect_kind(&cxem, NETPROBE_PC_3CXEM556, "3CXEM556");
    expect_kind(&prism2, NETPROBE_PC_NONE, "Prism2");
    expect_kind(&wpc11, NETPROBE_PC_NONE, "Linksys WPC11");
    expect_kind(&airconnect, NETPROBE_PC_NONE, "3Com AirConnect");
    expect_kind(&aironet, NETPROBE_PC_NONE, "Cisco Aironet");
    expect_kind(&wlan, NETPROBE_PC_NONE, "FUNCE wireless");
    expect_kind(&modem, NETPROBE_PC_NONE, "modem");
    expect_kind(&disk, NETPROBE_PC_NONE, "CompactFlash disk");
    /* The driver would try this one; the probe leaves it to the card page. */
    expect_kind(&bare, NETPROBE_PC_NONE, "MANFID, no FUNCID");

    printf("netprobe_card: %lu checks, %lu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
