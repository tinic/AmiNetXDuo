/*
 * AmiNetXDuo, RFC 8106 default-domain ownership.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dns_domain.h"

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
    AmiResolverConfig resolver;
    AmiNsDhcpDomainState dhcp;
    char owner[AMI_CFG_NAME_LEN];
    char applied[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN];
    char long_domain[201];
    char rooted_domain[AMI_CFG_DOMAIN_LEN];
    char underscored_domain[AMI_CFG_DOMAIN_LEN];
    char malformed_domain[AMI_CFG_DOMAIN_LEN];
    size_t i;

    memset(&resolver, 0, sizeof(resolver));
    memset(&dhcp, 0, sizeof(dhcp));
    memset(owner, 0, sizeof(owner));
    memset(applied, 0, sizeof(applied));

    h_set(applied[0], "one.test", sizeof(applied[0]));
    h_set(applied[1], "two.test", sizeof(applied[1]));
    ami_ns_dns_ra_default_reconcile(&resolver, owner, applied, 2U);
    h_check(strcmp(resolver.domain, "one.test") == 0 &&
            strcmp(owner, "one.test") == 0,
            "the first advertised suffix supplies an otherwise empty default");

    h_set(applied[0], "two.test", sizeof(applied[0]));
    applied[1][0] = '\0';
    ami_ns_dns_ra_default_reconcile(&resolver, owner, applied, 1U);
    h_check(strcmp(resolver.domain, "two.test") == 0 &&
            strcmp(owner, "two.test") == 0,
            "expiry selects another still-advertised suffix");

    ami_ns_dns_ra_default_reconcile(&resolver, owner, applied, 0U);
    h_check(resolver.domain[0] == '\0' && owner[0] == '\0',
            "expiry of the last advertised suffix clears its default");

    h_set(resolver.domain, "file.test", sizeof(resolver.domain));
    h_set(applied[0], "ra.test", sizeof(applied[0]));
    ami_ns_dns_ra_default_reconcile(&resolver, owner, applied, 1U);
    h_check(strcmp(resolver.domain, "file.test") == 0 && owner[0] == '\0',
            "an administrator's default is never claimed by RA");

    resolver.domain[0] = '\0';
    ami_ns_dns_ra_default_reconcile(&resolver, owner, applied, 1U);
    h_set(resolver.domain, "caller.test", sizeof(resolver.domain));
    ami_ns_dns_ra_default_reconcile(&resolver, owner, applied, 0U);
    h_check(strcmp(resolver.domain, "caller.test") == 0 && owner[0] == '\0',
            "a later caller override survives advertised suffix expiry");

    resolver.domain[0] = '\0';
    h_set(applied[0], "same.test", sizeof(applied[0]));
    ami_ns_dns_ra_default_reconcile(&resolver, owner, applied, 1U);
    /* netstack_set_domain_name() clears the marker even when the caller uses
       exactly the text the router supplied. */
    owner[0] = '\0';
    ami_ns_dns_ra_default_reconcile(&resolver, owner, applied, 0U);
    h_check(strcmp(resolver.domain, "same.test") == 0 && owner[0] == '\0',
            "an explicit same-text override is not later withdrawn by RA");

    /* DHCP owns its option 15 only while some interface still offers it. */
    memset(&resolver, 0, sizeof(resolver));
    memset(&dhcp, 0, sizeof(dhcp));
    memset(owner, 0, sizeof(owner));
    memset(applied, 0, sizeof(applied));
    ami_ns_dns_dhcp_default_update(&dhcp, 1U, "second.test");
    ami_ns_dns_dhcp_default_update(&dhcp, 0U, "first.test");
    ami_ns_dns_dhcp_default_reconcile(&resolver, &dhcp, owner, applied, 0U);
    h_check(strcmp(resolver.domain, "first.test") == 0 &&
            strcmp(dhcp.owner, "first.test") == 0,
            "the first interface's DHCP domain supplies an empty default");

    ami_ns_dns_dhcp_default_update(&dhcp, 0U, NULL);
    ami_ns_dns_dhcp_default_reconcile(&resolver, &dhcp, owner, applied, 0U);
    h_check(strcmp(resolver.domain, "second.test") == 0 &&
            strcmp(dhcp.owner, "second.test") == 0,
            "lease loss falls through to another DHCP interface");

    h_set(applied[0], "ra.test", sizeof(applied[0]));
    ami_ns_dns_dhcp_default_update(&dhcp, 1U, NULL);
    ami_ns_dns_dhcp_default_reconcile(&resolver, &dhcp, owner, applied, 1U);
    h_check(strcmp(resolver.domain, "ra.test") == 0 &&
            strcmp(owner, "ra.test") == 0 && dhcp.owner[0] == '\0',
            "loss of the last lease falls back to a live RA suffix");

    ami_ns_dns_dhcp_default_update(&dhcp, 0U, "dhcp.test");
    ami_ns_dns_dhcp_default_reconcile(&resolver, &dhcp, owner, applied, 1U);
    h_check(strcmp(resolver.domain, "dhcp.test") == 0 &&
            strcmp(dhcp.owner, "dhcp.test") == 0 && owner[0] == '\0',
            "DHCP takes precedence over an RA-owned default");

    /* DHCPv6's search list supplies the final DHCP candidate. IPv4 option 15
       takes the earlier interface slots, and withdrawal falls through both
       families before it reaches RA. */
    memset(&resolver, 0, sizeof(resolver));
    memset(&dhcp, 0, sizeof(dhcp));
    memset(owner, 0, sizeof(owner));
    h_set(applied[0], "ra.test", sizeof(applied[0]));
    ami_ns_dns_dhcpv6_default_update(&dhcp, "v6.test");
    ami_ns_dns_dhcp_default_reconcile(&resolver, &dhcp, owner, applied, 1U);
    h_check(strcmp(resolver.domain, "v6.test") == 0 &&
            strcmp(dhcp.owner, "v6.test") == 0,
            "DHCPv6 supplies a default when IPv4 DHCP is absent");

    ami_ns_dns_dhcp_default_update(&dhcp, 1U, "v4.test");
    ami_ns_dns_dhcp_default_reconcile(&resolver, &dhcp, owner, applied, 1U);
    h_check(strcmp(resolver.domain, "v4.test") == 0 &&
            strcmp(dhcp.owner, "v4.test") == 0,
            "IPv4 DHCP precedes the DHCPv6 fallback slot");

    ami_ns_dns_dhcp_default_update(&dhcp, 1U, NULL);
    ami_ns_dns_dhcp_default_reconcile(&resolver, &dhcp, owner, applied, 1U);
    h_check(strcmp(resolver.domain, "v6.test") == 0 &&
            strcmp(dhcp.owner, "v6.test") == 0,
            "IPv4 lease loss falls through to DHCPv6");

    ami_ns_dns_dhcpv6_default_update(&dhcp, NULL);
    ami_ns_dns_dhcp_default_reconcile(&resolver, &dhcp, owner, applied, 1U);
    h_check(strcmp(resolver.domain, "ra.test") == 0 &&
            strcmp(owner, "ra.test") == 0 && dhcp.owner[0] == '\0',
            "DHCPv6 withdrawal falls through to a live RA suffix");

    h_set(resolver.domain, "file.test", sizeof(resolver.domain));
    dhcp.owner[0] = '\0';
    ami_ns_dns_dhcp_default_reconcile(&resolver, &dhcp, owner, applied, 1U);
    h_check(strcmp(resolver.domain, "file.test") == 0,
            "static or caller configuration remains above DHCP");

    memset(&resolver, 0, sizeof(resolver));
    memset(&dhcp, 0, sizeof(dhcp));
    for (i = 0; i < sizeof(long_domain) - 1U; i++)
        long_domain[i] = (i == 50U || i == 101U || i == 152U) ? '.' : 'a';
    long_domain[sizeof(long_domain) - 1U] = '\0';
    ami_ns_dns_dhcp_default_update(&dhcp, 0U, long_domain);
    ami_ns_dns_dhcp_default_reconcile(&resolver, &dhcp, NULL, NULL, 0U);
    h_check(strcmp(resolver.domain, long_domain) == 0 &&
            strcmp(dhcp.owner, long_domain) == 0,
            "a valid long option 15 is retained without truncation");

    /* What a DHCP server may actually put in option 15. Refusing any of the
       first four blanked the default domain and the search suffix together,
       because both are taken from this one option. */
    h_check(ami_ns_domain_valid("example.com"), "a plain domain is accepted");
    h_check(ami_ns_domain_valid("example.com."),
            "a trailing root dot is not an empty label");
    h_check(ami_ns_domain_valid("home_network"),
            "an underscore is accepted, as consumer routers offer it");
    h_check(ami_ns_domain_valid("lan"), "a single label is accepted");
    h_check(!ami_ns_domain_valid(""), "an empty option is refused");
    h_check(!ami_ns_domain_valid("."), "a bare dot is refused");
    h_check(!ami_ns_domain_valid("a..b"), "an empty inner label is refused");
    h_check(!ami_ns_domain_valid("example.."),
            "only one trailing dot is allowed");
    h_check(!ami_ns_domain_valid("-lead.test"),
            "a label may not start with a hyphen");
    h_check(!ami_ns_domain_valid("trail-.test"),
            "a label may not end with a hyphen");
    h_check(!ami_ns_domain_valid("bad space.test"),
            "a space is refused");

    /* The option reader stores this canonical form.  Validation by itself is
       insufficient: the resolver's unqualified-name joiner cannot consume a
       suffix that still carries the root marker. */
    h_set(rooted_domain, "example.com.", sizeof(rooted_domain));
    h_check(ami_ns_domain_canonicalize(rooted_domain) &&
            strcmp(rooted_domain, "example.com") == 0,
            "a DHCP root marker is removed before resolver storage");

    h_set(underscored_domain, "home_network", sizeof(underscored_domain));
    h_check(ami_ns_domain_canonicalize(underscored_domain) &&
            strcmp(underscored_domain, "home_network") == 0,
            "canonicalization preserves an accepted router domain");

    h_set(malformed_domain, "bad..domain.", sizeof(malformed_domain));
    h_check(!ami_ns_domain_canonicalize(malformed_domain) &&
            strcmp(malformed_domain, "bad..domain.") == 0,
            "canonicalization does not repair malformed option text");

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}
