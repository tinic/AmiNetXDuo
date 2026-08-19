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
    char owner[AMI_CFG_NAME_LEN];
    char applied[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN];

    memset(&resolver, 0, sizeof(resolver));
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

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}
