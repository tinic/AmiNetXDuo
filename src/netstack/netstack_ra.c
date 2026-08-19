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


VOID ami_ns_ra_rdnss(AmiNsRaPending *pending, const ULONG address[4],
                     ULONG lifetime)
{
    UWORD i;

    if (pending == NULL || address == NULL)
        return;

    /* The consumer snapshots under the same Forbid().  The producer is the
       NetX IP thread and the consumer is an Exec caller task, so this guards
       both the array contents and the notification that describes them. */
    Forbid();

    for (i = 0; i < pending->rdnss_count; i++)
        if (ami_ns_ra_same(pending->rdnss[i].nxd_ip_address.v6, address))
            break;

    if (lifetime == 0UL)
    {
        if (i != pending->rdnss_count)
        {
            for (; (UWORD)(i + 1) < pending->rdnss_count; i++)
                pending->rdnss[i] = pending->rdnss[i + 1];

            pending->rdnss_count--;
            pending->rdnss_pending = TRUE;
        }

        Permit();
        return;
    }

    if (i == pending->rdnss_count &&
        pending->rdnss_count < (UWORD)AMI_RDNSS_MAX)
    {
        pending->rdnss[i].nxd_ip_version       = NX_IP_VERSION_V6;
        pending->rdnss[i].nxd_ip_address.v6[0] = address[0];
        pending->rdnss[i].nxd_ip_address.v6[1] = address[1];
        pending->rdnss[i].nxd_ip_address.v6[2] = address[2];
        pending->rdnss[i].nxd_ip_address.v6[3] = address[3];

        pending->rdnss_count = (UWORD)(i + 1);
        pending->rdnss_pending = TRUE;
    }

    Permit();
}


VOID ami_ns_ra_dnssl(AmiNsRaPending *pending, const UCHAR *domains,
                     UINT length, ULONG lifetime)
{
    UWORD i;

    if (pending == NULL || domains == NULL || length == 0 ||
        length > (UINT)AMI_DNSSL_MAX)
        return;

    /* A prefix of an encoded domain list is not a shorter list: it can end in
       the middle of a label.  Preserve the preceding complete option when a
       new one does not fit. */
    Forbid();

    for (i = 0; i < (UWORD)length; i++)
        pending->dnssl[i] = (UBYTE)domains[i];

    pending->dnssl_len = (UWORD)length;
    pending->dnssl_lifetime = lifetime;
    pending->dnssl_pending = TRUE;

    Permit();
}


BOOL ami_ns_ra_snapshot(AmiNsRaPending *pending, AmiNsRaSnapshot *snapshot)
{
    UWORD i;

    if (pending == NULL || snapshot == NULL)
        return FALSE;

    snapshot->rdnss_pending = FALSE;
    snapshot->dnssl_pending = FALSE;

    /* Pending is cleared while the bytes it describes are copied.  An option
       arriving after Permit() therefore remains pending for the next pass
       instead of having its notification cleared underneath it. */
    Forbid();

    if (pending->rdnss_pending)
    {
        snapshot->rdnss_count = pending->rdnss_count;
        for (i = 0; i < snapshot->rdnss_count; i++)
            snapshot->rdnss[i] = pending->rdnss[i];
        pending->rdnss_pending = FALSE;
        snapshot->rdnss_pending = TRUE;
    }

    if (pending->dnssl_pending)
    {
        snapshot->dnssl_len = pending->dnssl_len;
        for (i = 0; i < snapshot->dnssl_len; i++)
            snapshot->dnssl[i] = pending->dnssl[i];
        snapshot->dnssl_lifetime = pending->dnssl_lifetime;
        pending->dnssl_pending = FALSE;
        snapshot->dnssl_pending = TRUE;
    }

    Permit();

    return (BOOL)(snapshot->rdnss_pending || snapshot->dnssl_pending);
}
