/*
 * AmiNetXDuo, DHCP option-12 hostname ownership.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dhcp_hostname.h"

#include <stdio.h>
#include <string.h>


static unsigned long h_checks;
static unsigned long h_failures;


static void h_check(int ok, const char *what)
{
    h_checks++;
    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}


static void h_set(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);

    if (n >= size)
        n = size - 1U;
    memcpy(dst, src, n);
    dst[n] = '\0';
}


int main(void)
{
    AmiConfig cfg;
    AmiNsDhcpHostnameState dhcp;
    UBYTE wire[AMI_CFG_NAME_LEN];
    char decoded[AMI_CFG_NAME_LEN];
    size_t i;

    memset(wire, 'a', sizeof(wire));
    h_check(ami_ns_dhcp_hostname_decode(decoded, wire,
                                        AMI_CFG_NAME_LEN - 1U) &&
            strlen(decoded) == AMI_CFG_NAME_LEN - 1U,
            "the longest storable option 12 is accepted intact");
    h_check(!ami_ns_dhcp_hostname_decode(decoded, wire,
                                         AMI_CFG_NAME_LEN) &&
            decoded[0] == '\0',
            "an overlong option 12 is rejected rather than truncated");
    wire[3] = 0U;
    h_check(!ami_ns_dhcp_hostname_decode(decoded, wire, 8U) &&
            decoded[0] == '\0',
            "an embedded NUL makes option 12 unusable");
    for (i = 0U; i < sizeof(wire); i++)
        wire[i] = (UBYTE)'a';
    wire[3] = (UBYTE)' ';
    h_check(!ami_ns_dhcp_hostname_decode(decoded, wire, 8U) &&
            decoded[0] == '\0',
            "invalid hostname syntax is rejected");

    memset(&cfg, 0, sizeof(cfg));
    memset(&dhcp, 0, sizeof(dhcp));
    h_set(cfg.hostname, "amiga-490007", sizeof(cfg.hostname));

    ami_ns_dhcp_hostname_update(&dhcp, 1U, "second");
    ami_ns_dhcp_hostname_update(&dhcp, 0U, "first");
    h_check(ami_ns_dhcp_hostname_reconcile(&cfg, &dhcp) &&
            strcmp(cfg.hostname, "first") == 0 &&
            cfg.hostname_source == AMI_HOSTNAME_DHCP &&
            strcmp(dhcp.owner, "first") == 0,
            "the first DHCP interface replaces the generated name");

    ami_ns_dhcp_hostname_update(&dhcp, 0U, NULL);
    h_check(ami_ns_dhcp_hostname_reconcile(&cfg, &dhcp) &&
            strcmp(cfg.hostname, "second") == 0 &&
            strcmp(dhcp.owner, "second") == 0,
            "loss of the first lease falls through to the second");

    ami_ns_dhcp_hostname_update(&dhcp, 1U, "renewed");
    h_check(ami_ns_dhcp_hostname_reconcile(&cfg, &dhcp) &&
            strcmp(cfg.hostname, "renewed") == 0 &&
            strcmp(dhcp.owner, "renewed") == 0,
            "a renewal replaces the name owned by that lease");

    ami_ns_dhcp_hostname_update(&dhcp, 1U, NULL);
    h_check(ami_ns_dhcp_hostname_reconcile(&cfg, &dhcp) &&
            strcmp(cfg.hostname, "amiga-490007") == 0 &&
            cfg.hostname_source == AMI_HOSTNAME_NONE &&
            dhcp.owner[0] == '\0',
            "loss of the last lease restores the generated fallback");

    memset(&cfg, 0, sizeof(cfg));
    memset(&dhcp, 0, sizeof(dhcp));
    h_set(cfg.hostname, "from-env", sizeof(cfg.hostname));
    cfg.hostname_source = AMI_HOSTNAME_ENV;
    ami_ns_dhcp_hostname_update(&dhcp, 0U, "from-dhcp");
    h_check(ami_ns_dhcp_hostname_reconcile(&cfg, &dhcp) &&
            strcmp(cfg.hostname, "from-dhcp") == 0,
            "DHCP outranks an ENV hostname while its lease is live");
    ami_ns_dhcp_hostname_update(&dhcp, 0U, NULL);
    h_check(ami_ns_dhcp_hostname_reconcile(&cfg, &dhcp) &&
            strcmp(cfg.hostname, "from-env") == 0 &&
            cfg.hostname_source == AMI_HOSTNAME_ENV,
            "lease loss restores the lower-ranked configured hostname");

    memset(&cfg, 0, sizeof(cfg));
    memset(&dhcp, 0, sizeof(dhcp));
    h_set(cfg.hostname, "from-file", sizeof(cfg.hostname));
    cfg.hostname_source = AMI_HOSTNAME_NAMERES;
    ami_ns_dhcp_hostname_update(&dhcp, 0U, "from-dhcp");
    h_check(!ami_ns_dhcp_hostname_reconcile(&cfg, &dhcp) &&
            strcmp(cfg.hostname, "from-file") == 0 &&
            dhcp.owner[0] == '\0',
            "name_resolution remains authoritative over DHCP");

    memset(&cfg, 0, sizeof(cfg));
    memset(&dhcp, 0, sizeof(dhcp));
    h_set(cfg.hostname, "fallback", sizeof(cfg.hostname));
    cfg.hostname_source = AMI_HOSTNAME_ENV;
    ami_ns_dhcp_hostname_update(&dhcp, 0U, "leased");
    (void)ami_ns_dhcp_hostname_reconcile(&cfg, &dhcp);
    h_set(cfg.hostname, "explicit", sizeof(cfg.hostname));
    cfg.hostname_source = AMI_HOSTNAME_NAMERES;
    ami_ns_dhcp_hostname_displace(&dhcp);
    ami_ns_dhcp_hostname_update(&dhcp, 0U, NULL);
    h_check(!ami_ns_dhcp_hostname_reconcile(&cfg, &dhcp) &&
            strcmp(cfg.hostname, "explicit") == 0,
            "an explicit stronger offer cannot be undone by lease loss");

    memset(&cfg, 0, sizeof(cfg));
    memset(&dhcp, 0, sizeof(dhcp));
    h_set(cfg.hostname, "fallback", sizeof(cfg.hostname));
    cfg.hostname_source = AMI_HOSTNAME_ENV;
    ami_ns_dhcp_hostname_update(&dhcp, 0U, "not a name");
    h_check(!ami_ns_dhcp_hostname_reconcile(&cfg, &dhcp) &&
            strcmp(cfg.hostname, "fallback") == 0 &&
            dhcp.owner[0] == '\0',
            "an invalid option 12 never becomes the machine name");

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0U) ? 0 : 1;
}
