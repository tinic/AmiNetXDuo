/*
 * AmiNetXDuo, ownership of the default domain derived from RFC 8106 DNSSL.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_dns_domain.h"


static char ami_ns_domain_fold(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}


static BOOL ami_ns_domain_same(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        if (ami_ns_domain_fold(*a++) != ami_ns_domain_fold(*b++))
            return FALSE;
    }

    return (BOOL)(*a == *b);
}


static VOID ami_ns_domain_copy(char *dst, const char *src, UWORD size)
{
    UWORD i = 0;

    while (src[i] != '\0' && (UWORD)(i + 1U) < size)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}


VOID ami_ns_dns_ra_default_reconcile(
    AmiResolverConfig *resolver,
    char owner[AMI_CFG_NAME_LEN],
    const char applied[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN],
    UWORD applied_count)
{
    UWORD i;

    if (resolver == NULL || owner == NULL || applied == NULL)
        return;

    if (applied_count > (UWORD)AMI_CFG_MAX_SEARCH)
        applied_count = (UWORD)AMI_CFG_MAX_SEARCH;

    if (owner[0] == '\0')
    {
        /* A file, DHCP, or SetDefaultDomainName() already owns this field. */
        if (resolver->domain[0] != '\0' || applied_count == 0U)
            return;

        ami_ns_domain_copy(resolver->domain, applied[0],
                           (UWORD)sizeof(resolver->domain));
        ami_ns_domain_copy(owner, applied[0], (UWORD)AMI_CFG_NAME_LEN);
        return;
    }

    /* A caller replaced even the same text through SetDefaultDomainName().
       That path clears owner first.  This comparison also protects any future
       writer that replaces it directly with different text. */
    if (!ami_ns_domain_same(resolver->domain, owner))
    {
        owner[0] = '\0';
        return;
    }

    for (i = 0; i < applied_count; i++)
        if (ami_ns_domain_same(owner, applied[i]))
            return;

    if (applied_count == 0U)
    {
        resolver->domain[0] = '\0';
        owner[0] = '\0';
        return;
    }

    /* The selected advertised suffix expired, but another remains. */
    ami_ns_domain_copy(resolver->domain, applied[0],
                       (UWORD)sizeof(resolver->domain));
    ami_ns_domain_copy(owner, applied[0], (UWORD)AMI_CFG_NAME_LEN);
}


VOID ami_ns_dns_dhcp_default_update(AmiNsDhcpDomainState *state,
                                    UWORD interface_index,
                                    const char *domain)
{
    if (state == NULL || interface_index >= AMI_CFG_MAX_INTERFACES)
        return;

    if (domain == NULL)
        domain = "";
    ami_ns_domain_copy(state->lease[interface_index], domain,
                       (UWORD)AMI_CFG_DOMAIN_LEN);
}


VOID ami_ns_dns_dhcp_default_reconcile(
    AmiResolverConfig *resolver,
    AmiNsDhcpDomainState *dhcp,
    char ra_owner[AMI_CFG_NAME_LEN],
    const char ra_applied[AMI_CFG_MAX_SEARCH][AMI_CFG_NAME_LEN],
    UWORD ra_applied_count)
{
    const char *offered = NULL;
    UWORD       iface;

    if (resolver == NULL || dhcp == NULL ||
        (ra_applied_count != 0U &&
         (ra_owner == NULL || ra_applied == NULL)))
        return;

    for (iface = 0; iface < AMI_CFG_MAX_INTERFACES; iface++)
        if (dhcp->lease[iface][0] != '\0')
        {
            offered = dhcp->lease[iface];
            break;
        }

    if (dhcp->owner[0] != '\0')
    {
        if (!ami_ns_domain_same(resolver->domain, dhcp->owner))
        {
            /* An explicit or future direct writer displaced DHCP. */
            dhcp->owner[0] = '\0';
            return;
        }

        if (offered != NULL)
        {
            if (!ami_ns_domain_same(dhcp->owner, offered))
            {
                ami_ns_domain_copy(resolver->domain, offered,
                                   (UWORD)sizeof(resolver->domain));
                ami_ns_domain_copy(dhcp->owner, offered,
                                   (UWORD)AMI_CFG_DOMAIN_LEN);
            }
            return;
        }

        resolver->domain[0] = '\0';
        dhcp->owner[0] = '\0';

        /* DHCP has gone; a still-live RA suffix may now supply the fallback. */
        if (ra_owner != NULL && ra_applied != NULL)
            ami_ns_dns_ra_default_reconcile(resolver, ra_owner, ra_applied,
                                            ra_applied_count);
        return;
    }

    if (offered == NULL)
        return;

    if (ra_owner != NULL && ra_owner[0] != '\0' &&
        ami_ns_domain_same(resolver->domain, ra_owner))
    {
        /* RFC 8106 gives DHCP precedence over unauthenticated RA data. */
        ra_owner[0] = '\0';
        resolver->domain[0] = '\0';
    }

    /* A file or SetDefaultDomainName() remains authoritative. */
    if (resolver->domain[0] != '\0')
        return;

    ami_ns_domain_copy(resolver->domain, offered,
                       (UWORD)sizeof(resolver->domain));
    ami_ns_domain_copy(dhcp->owner, offered, (UWORD)AMI_CFG_DOMAIN_LEN);
}
