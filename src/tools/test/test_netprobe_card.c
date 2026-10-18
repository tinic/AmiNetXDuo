/*
 * InstallNetProbe's PC Card decision and CheckNetDevice's slot report
 * (netprobe_card.c), over the identity tuples of real cards.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netprobe_card.h"

#include <stdio.h>
#include <string.h>

static unsigned long checks;
static unsigned long failures;

/* A 3C589's primary chain as attribute memory holds it, CIS byte n at n:
   DEVICE, a NULL byte, VERS_1, MANFID, FUNCID LAN, FUNCE, END. */
static const unsigned char el3_cis[] =
{
    0x01, 0x02, 0xdf, 0xff,
    0x00,
    0x15, 0x31, 0x04, 0x01,
    '3', 'C', 'o', 'm', ' ', 'C', 'o', 'r', 'p', 'o', 'r', 'a', 't', 'i',
    'o', 'n', 0x00,
    '3', 'C', '5', '8', '9', 0x00,
    'T', 'P', '/', 'B', 'N', 'C', ' ', 'L', 'A', 'N', ' ', 'C', 'a', 'r',
    'd', 0x00,
    '0', '0', '0', '0', '0', '2', 0x00,
    0xff,
    0x20, 0x04, 0x01, 0x01, 0x89, 0x05,
    0x21, 0x02, 0x06, 0x00,
    0x22, 0x02, 0x01, 0x02,
    0xff
};

/* A link running past the end stops the walk. */
static const unsigned char short_cis[] = { 0x21, 0x08, 0x06 };

/* A CompactFlash disk: VERS_1 with a control byte, FUNCID fixed disk. */
static const unsigned char cf_cis[] =
{
    0x15, 0x0b, 0x04, 0x01, 'S', 'a', 'n', 0x07, 0x00, 'C', 'F', 0x00, 0xff,
    0x21, 0x02, 0x04, 0x01,
    0xff
};

/* A VERS_1 longer than the report keeps. */
static unsigned char long_cis[2 + 200 + 1];

static uint8_t cis_read(void *ctx, uint32_t n)
{
    return ((const unsigned char *)ctx)[n];
}

/* The report as CheckNetDevice prints it, kept for comparison. */
static char report[1024];
static unsigned report_used;

static void report_line(void *ctx, const char *text)
{
    (void)ctx;
    printf("  %s\n", text);
    while (*text != '\0' && report_used + 2 < sizeof(report))
        report[report_used++] = *text++;
    report[report_used++] = '\n';
    report[report_used] = '\0';
}

static void expect_report(const NetprobeSlot *slot, const char *want,
                          const char *what)
{
    unsigned i;

    report_used = 0;
    report[0] = '\0';
    printf("%s:\n", what);
    netprobe_slot_report(slot, report_line, NULL);
    checks++;
    for (i = 0; want[i] != '\0' && want[i] == report[i]; i++)
        ;
    if (want[i] != '\0' || report[i] != '\0')
    {
        failures++;
        printf("  FAIL %s report, want:\n%s", what, want);
    }
}

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
    NetprobeCis xircom = { 0x0105, 0x010a, 6, { 0x01, 0x02 }, 1, 1, 1 };
    NetprobeCis xircom_combo = { 0x0105, 0x110b, 0, { 0, 0 }, 1, 0, 0 };
    /* Same vendor, unsupported model: never select the Xircom driver. */
    NetprobeCis xircom_other = { 0x0105, 0x9999, 2, { 0, 0 }, 1, 1, 0 };
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
    expect_kind(&xircom, NETPROBE_PC_XIRCOM, "Xircom CE3");
    expect_kind(&xircom_combo, NETPROBE_PC_XIRCOM, "Xircom combo, no LAN FUNCID");
    expect_kind(&xircom_other, NETPROBE_PC_NONE, "unsupported Xircom modem");
    expect_kind(&modem, NETPROBE_PC_NONE, "modem");
    expect_kind(&disk, NETPROBE_PC_NONE, "CompactFlash disk");
    /* The driver would try this one; the probe leaves it to the card page. */
    expect_kind(&bare, NETPROBE_PC_NONE, "MANFID, no FUNCID");

    /* CARD wireless: the 802.11 cards above, and none of the others. */
    {
        /* A Prism2 2.9 identity outside Linux's tables, no FUNCID. */
        NetprobeCis prism2_only = { 0x0032, 0x000c, 0, { 0, 0 }, 1, 0, 0 };
        /* Netgear MA401RA, an A1200's: FUNCID LAN, no FUNCE. */
        NetprobeCis ma401ra = { 0x000b, 0x7300, 6, { 0, 0 }, 1, 1, 0 };
        const NetprobeCis *yes[] = { &prism2, &wpc11, &airconnect, &aironet,
                                     &wlan, &prism2_only, &ma401ra };

        expect_kind(&ma401ra, NETPROBE_PC_NONE, "Netgear MA401RA");
        const NetprobeCis *no[] = { &cnet, &dfe670, &dfe670e, &el3, &cfem,
                                    &cxem, &xircom, &xircom_combo,
                                    &xircom_other, &modem, &disk, &bare };
        unsigned i;

        for (i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
        {
            checks++;
            if (!netprobe_pc_wireless(yes[i]))
            {
                failures++;
                printf("  FAIL wireless card %u not called wireless\n", i);
            }
        }
        for (i = 0; i < sizeof(no) / sizeof(no[0]); i++)
        {
            checks++;
            if (netprobe_pc_wireless(no[i]))
            {
                failures++;
                printf("  FAIL card %u called wireless\n", i);
            }
        }
    }

    {
        NetprobeCis walked = { 0, 0, 0, { 0, 0 }, 0, 0, 0 };
        NetprobeCis none = { 0, 0, 0, { 0, 0 }, 0, 0, 0 };

        checks++;
        if (!netprobe_cis_walk(cis_read, (void *)el3_cis,
                               (uint32_t)sizeof(el3_cis), &walked) ||
            walked.manf != 0x0101 || walked.prod != 0x0589 ||
            !walked.have_funce || walked.funce[1] != 0x02)
        {
            failures++;
            printf("  FAIL raw walk of a 3C589\n");
        }
        expect_kind(&walked, NETPROBE_PC_3C589, "3C589, raw CIS");
        checks++;
        if (netprobe_cis_walk(cis_read, (void *)short_cis,
                              (uint32_t)sizeof(short_cis), &none))
        {
            failures++;
            printf("  FAIL a truncated tuple was read\n");
        }
    }

    {
        static NetprobeSlot slot;
        unsigned            i;

        slot.slot = NETPROBE_SLOT_NONE;
        expect_report(&slot, "No card.resource; no PCMCIA slot.\n",
                      "no card.resource");
        slot.slot = NETPROBE_SLOT_EMPTY;
        expect_report(&slot, "No card.\n", "empty slot");

        slot.slot = NETPROBE_SLOT_OWNED;
        strcpy(slot.owner, "anxnet.device");
        (void)netprobe_cis_walk(cis_read, (void *)el3_cis,
                                (uint32_t)sizeof(el3_cis), &slot.cis);
        slot.have_vers1 = (uint8_t)netprobe_cis_vers1(
            cis_read, (void *)el3_cis, (uint32_t)sizeof(el3_cis),
            slot.vers1, sizeof(slot.vers1));
        expect_report(&slot,
            "Card present, owned by anxnet.device; CIS read from attribute "
            "memory.\n"
            "CISTPL_MANFID: manufacturer $0101, product $0589.\n"
            "CISTPL_VERS_1: \"3Com Corporation\", \"3C589\", "
            "\"TP/BNC LAN Card\", \"000002\".\n"
            "CISTPL_FUNCID 6 (LAN).\n"
            "First CISTPL_FUNCE: type $01, value $02.\n"
            "Driver: anxnet.device, CARD=3c589.\n", "3C589, owned");

        memset(&slot, 0, sizeof(slot));
        slot.slot = NETPROBE_SLOT_FREE;
        (void)netprobe_cis_walk(cis_read, (void *)cf_cis,
                                (uint32_t)sizeof(cf_cis), &slot.cis);
        slot.have_vers1 = (uint8_t)netprobe_cis_vers1(
            cis_read, (void *)cf_cis, (uint32_t)sizeof(cf_cis),
            slot.vers1, sizeof(slot.vers1));
        expect_report(&slot,
            "Card present, slot free; CIS read through card.resource.\n"
            "No CISTPL_MANFID.\n"
            "CISTPL_VERS_1: \"San?\", \"CF\".\n"
            "CISTPL_FUNCID 4 (fixed disk).\n"
            "No CISTPL_FUNCE.\n"
            "Driver: none in this archive.\n", "CompactFlash disk, free");

        long_cis[0] = 0x15;
        long_cis[1] = 200;
        long_cis[2] = 0x04;
        long_cis[3] = 0x01;
        for (i = 4; i < 2 + 200; i++)
            long_cis[i] = 'x';
        long_cis[2 + 200] = 0xff;
        memset(&slot, 0, sizeof(slot));
        checks++;
        if (!netprobe_cis_vers1(cis_read, long_cis, (uint32_t)sizeof(long_cis),
                                slot.vers1, sizeof(slot.vers1)) ||
            strlen(slot.vers1) != sizeof(slot.vers1) - 1 ||
            strcmp(slot.vers1 + sizeof(slot.vers1) - 5, "\"...") != 0)
        {
            failures++;
            printf("  FAIL long VERS_1: %s\n", slot.vers1);
        }
    }

    printf("netprobe_card: %lu checks, %lu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
