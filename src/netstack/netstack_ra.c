/*
 * AmiNetXDuo, router-advertisement resolver handoff.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_ra.h"

#include <proto/exec.h>


static BOOL ami_ns_ra_same(const ULONG a[4], const ULONG b[4])
{
    return (BOOL)(a[0] == b[0] && a[1] == b[1] &&
                  a[2] == b[2] && a[3] == b[3]);
}


static char ami_ns_ra_fold(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}


static BOOL ami_ns_ra_domain_same(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        if (ami_ns_ra_fold(*a++) != ami_ns_ra_fold(*b++))
            return FALSE;
    }

    return (BOOL)(*a == *b);
}


/* One uncompressed RFC 1035 name from RFC 8106's DNSSL payload. */
static BOOL ami_ns_ra_dnssl_name(const UCHAR *domains, UINT length, UINT *pos,
                                 char out[AMI_CFG_NAME_LEN])
{
    UINT n = 0;

    for (;;)
    {
        UINT label;

        if (*pos >= length)
            return FALSE;

        label = (UINT)domains[(*pos)++];
        if (label == 0U)
        {
            out[n] = '\0';
            return (BOOL)(n != 0U);
        }

        /* RFC 8106 expressly forbids compression, and an ordinary DNS label
           is at most 63 octets. */
        if (label > 63U || *pos + label > length ||
            domains[*pos] == (UCHAR)'-' ||
            domains[*pos + label - 1U] == (UCHAR)'-')
            return FALSE;

        if (n != 0U)
        {
            if (n + 1U >= (UINT)AMI_CFG_NAME_LEN)
                return FALSE;
            out[n++] = '.';
        }

        if (n + label >= (UINT)AMI_CFG_NAME_LEN)
            return FALSE;

        while (label-- != 0U)
        {
            UCHAR c = domains[(*pos)++];

            if (!((c >= (UCHAR)'a' && c <= (UCHAR)'z') ||
                  (c >= (UCHAR)'A' && c <= (UCHAR)'Z') ||
                  (c >= (UCHAR)'0' && c <= (UCHAR)'9') ||
                  c == (UCHAR)'-'))
                return FALSE;
            out[n++] = (char)c;
        }
    }
}


static BOOL ami_ns_ra_expired(ULONG received, ULONG lifetime, ULONG now)
{
    ULONG limit;

    /* RFC 8106 uses all ones for infinity.  A lifetime longer than the
       ThreadX tick counter can represent is likewise not due during this
       counter epoch.  Router lifetimes in ordinary use are minutes. */
    if (lifetime == 0UL || lifetime == (ULONG)~0UL ||
        lifetime > ((ULONG)~0UL / (ULONG)NX_IP_PERIODIC_RATE))
        return FALSE;

    limit = lifetime * (ULONG)NX_IP_PERIODIC_RATE;
    return (BOOL)((ULONG)(now - received) >= limit);
}


VOID ami_ns_ra_rdnss(AmiNsRaPending *pending, UWORD interface_index,
                     const ULONG address[4], ULONG lifetime, ULONG now)
{
    UWORD i;

    if (pending == NULL || address == NULL ||
        interface_index >= AMI_CFG_MAX_INTERFACES)
        return;

    /* The consumer snapshots under the same Forbid().  The producer is the
       NetX IP thread and the consumer is an Exec caller task, so this guards
       both the array contents and the notification that describes them. */
    Forbid();

    for (i = 0; i < pending->rdnss_count[interface_index]; i++)
        if (ami_ns_ra_same(
                pending->rdnss[interface_index][i].address.nxd_ip_address.v6,
                           address))
            break;

    if (lifetime == 0UL)
    {
        if (i != pending->rdnss_count[interface_index])
        {
            for (; (UWORD)(i + 1) < pending->rdnss_count[interface_index]; i++)
                pending->rdnss[interface_index][i] =
                    pending->rdnss[interface_index][i + 1];

            pending->rdnss_count[interface_index]--;
            pending->rdnss_pending = TRUE;
        }

        Permit();
        return;
    }

    if (i == pending->rdnss_count[interface_index] &&
        pending->rdnss_count[interface_index] < (UWORD)AMI_RDNSS_MAX)
    {
        pending->rdnss[interface_index][i].address.nxd_ip_version =
            NX_IP_VERSION_V6;
        pending->rdnss[interface_index][i].address.nxd_ip_address.v6[0] =
            address[0];
        pending->rdnss[interface_index][i].address.nxd_ip_address.v6[1] =
            address[1];
        pending->rdnss[interface_index][i].address.nxd_ip_address.v6[2] =
            address[2];
        pending->rdnss[interface_index][i].address.nxd_ip_address.v6[3] =
            address[3];

        pending->rdnss_count[interface_index] = (UWORD)(i + 1);
        pending->rdnss_pending = TRUE;
    }

    if (i < pending->rdnss_count[interface_index])
    {
        /* A repeated advertisement refreshes the lifetime without changing
           the resolver set, so it need not wake the consumer. */
        pending->rdnss[interface_index][i].lifetime = lifetime;
        pending->rdnss[interface_index][i].received = now;
    }

    Permit();
}


VOID ami_ns_ra_dnssl(AmiNsRaPending *pending, UWORD interface_index,
                     const UCHAR *domains, UINT length, ULONG lifetime,
                     ULONG now)
{
    char  name[AMI_CFG_NAME_LEN];
    UINT  pos = 0;

    if (pending == NULL || domains == NULL || length == 0 ||
        length > (UINT)AMI_DNSSL_MAX ||
        interface_index >= AMI_CFG_MAX_INTERFACES)
        return;

    /* RFC 8106 keeps an expiration time per domain and per interface.  Decode
       the bounded option while producer and consumer are excluded, so two
       options arriving before a lookup cannot overwrite one another. */
    Forbid();

    while (pos < length)
    {
        UWORD i;

        if (!ami_ns_ra_dnssl_name(domains, length, &pos, name))
            break;              /* root padding, truncation, or bad encoding */

        for (i = 0; i < pending->dnssl_count[interface_index]; i++)
            if (ami_ns_ra_domain_same(
                    pending->dnssl[interface_index][i].domain, name))
                break;

        if (lifetime == 0UL)
        {
            if (i < pending->dnssl_count[interface_index])
            {
                UWORD j;

                for (j = i; (UWORD)(j + 1U) <
                            pending->dnssl_count[interface_index]; j++)
                    pending->dnssl[interface_index][j] =
                        pending->dnssl[interface_index][j + 1U];
                pending->dnssl_count[interface_index]--;
                pending->dnssl_pending = TRUE;
            }
            continue;
        }

        if (i == pending->dnssl_count[interface_index])
        {
            UWORD j;

            if (i >= (UWORD)AMI_CFG_MAX_SEARCH)
                continue;

            for (j = 0; name[j] != '\0'; j++)
                pending->dnssl[interface_index][i].domain[j] = name[j];
            pending->dnssl[interface_index][i].domain[j] = '\0';
            pending->dnssl_count[interface_index] = (UWORD)(i + 1U);
            pending->dnssl_pending = TRUE;
        }

        pending->dnssl[interface_index][i].lifetime = lifetime;
        pending->dnssl[interface_index][i].received = now;
    }

    Permit();
}


BOOL ami_ns_ra_snapshot(AmiNsRaPending *pending, AmiNsRaSnapshot *snapshot,
                        ULONG now)
{
    UWORD i;
    UWORD iface;

    if (pending == NULL || snapshot == NULL)
        return FALSE;

    snapshot->rdnss_pending = FALSE;
    snapshot->dnssl_pending = FALSE;

    /* Pending is cleared while the bytes it describes are copied.  An option
       arriving after Permit() therefore remains pending for the next pass
       instead of having its notification cleared underneath it. */
    Forbid();

    /* Expiry is a withdrawal even when no new packet arrived.  Compact before
       copying so the caller reconciles against the still-valid set. */
    for (iface = 0; iface < AMI_CFG_MAX_INTERFACES; iface++)
    {
        i = 0;
        while (i < pending->rdnss_count[iface])
        {
            if (ami_ns_ra_expired(pending->rdnss[iface][i].received,
                                  pending->rdnss[iface][i].lifetime, now))
            {
                UWORD j;

                for (j = i; (UWORD)(j + 1U) < pending->rdnss_count[iface];
                     j++)
                    pending->rdnss[iface][j] = pending->rdnss[iface][j + 1U];
                pending->rdnss_count[iface]--;
                pending->rdnss_pending = TRUE;
                continue;
            }
            i++;
        }
    }

    if (pending->rdnss_pending)
    {
        snapshot->rdnss_count = 0;
        for (iface = 0; iface < AMI_CFG_MAX_INTERFACES; iface++)
            for (i = 0; i < pending->rdnss_count[iface] &&
                        snapshot->rdnss_count < (UWORD)AMI_RDNSS_MAX; i++)
            {
                UWORD known;

                for (known = 0; known < snapshot->rdnss_count; known++)
                    if (ami_ns_ra_same(
                            snapshot->rdnss[known].nxd_ip_address.v6,
                            pending->rdnss[iface][i].address.nxd_ip_address.v6))
                        break;

                if (known == snapshot->rdnss_count)
                    snapshot->rdnss[snapshot->rdnss_count++] =
                        pending->rdnss[iface][i].address;
            }
        pending->rdnss_pending = FALSE;
        snapshot->rdnss_pending = TRUE;
    }

    if (pending->dnssl_pending)
    {
        snapshot->dnssl_count = 0;
        for (iface = 0; iface < AMI_CFG_MAX_INTERFACES; iface++)
            for (i = 0; i < pending->dnssl_count[iface] &&
                        snapshot->dnssl_count < (UWORD)AMI_CFG_MAX_SEARCH; i++)
            {
                UWORD known;
                UWORD c;

                for (known = 0; known < snapshot->dnssl_count; known++)
                    if (ami_ns_ra_domain_same(snapshot->dnssl[known],
                            pending->dnssl[iface][i].domain))
                        break;

                if (known != snapshot->dnssl_count)
                    continue;

                for (c = 0; pending->dnssl[iface][i].domain[c] != '\0'; c++)
                    snapshot->dnssl[snapshot->dnssl_count][c] =
                        pending->dnssl[iface][i].domain[c];
                snapshot->dnssl[snapshot->dnssl_count][c] = '\0';
                snapshot->dnssl_count++;
            }
        pending->dnssl_pending = FALSE;
        snapshot->dnssl_pending = TRUE;
    }

    Permit();

    return (BOOL)(snapshot->rdnss_pending || snapshot->dnssl_pending);
}


BOOL ami_ns_ra_needs_snapshot(AmiNsRaPending *pending, ULONG now)
{
    BOOL  needed = FALSE;
    UWORD i;
    UWORD iface;

    if (pending == NULL)
        return FALSE;

    Forbid();

    needed = (BOOL)(pending->rdnss_pending || pending->dnssl_pending);
    for (iface = 0; !needed && iface < AMI_CFG_MAX_INTERFACES; iface++)
        for (i = 0; !needed && i < pending->rdnss_count[iface]; i++)
            needed = ami_ns_ra_expired(pending->rdnss[iface][i].received,
                                       pending->rdnss[iface][i].lifetime, now);

    Permit();
    return needed;
}
