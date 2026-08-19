/*
 * AmiNetXDuo, RFC 8106 producer/consumer handoff.
 *
 * The NetX IP thread writes RDNSS and DNSSL state while an application task
 * consumes it.  Permit() below can inject a second advertisement at the exact
 * boundary after the consumer has taken its snapshot.  The first snapshot
 * must remain coherent, and the injected option must remain pending for the
 * next pass rather than being erased by the first one.
 *
 * Real, compiled into this binary: src/netstack/netstack_ra.c.
 * Stubbed: Exec's Forbid()/Permit(), with the latter acting as the scheduler.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netstack_ra.h"

#include <stdio.h>
#include <string.h>


static unsigned long h_checks;
static unsigned long h_failures;
static unsigned long h_forbid_depth;
static unsigned long h_forbid_max;

static AmiNsRaPending *h_inject_pending;
static const ULONG    *h_inject_rdnss;
static const UCHAR    *h_inject_dnssl;
static UINT            h_inject_dnssl_len;
static ULONG           h_inject_lifetime;


static void h_check(int ok, const char *what)
{
    h_checks++;

    if (!ok)
    {
        h_failures++;
        printf("FAIL %s\n", what);
    }
}


VOID Forbid(VOID)
{
    h_forbid_depth++;
    if (h_forbid_depth > h_forbid_max)
        h_forbid_max = h_forbid_depth;
}


VOID Permit(VOID)
{
    AmiNsRaPending *pending;
    const ULONG    *rdnss;
    const UCHAR    *dnssl;
    UINT            dnssl_len;
    ULONG           lifetime;

    h_check(h_forbid_depth != 0, "Permit has a matching Forbid");
    if (h_forbid_depth == 0)
        return;

    h_forbid_depth--;
    if (h_forbid_depth != 0 || h_inject_pending == NULL)
        return;

    pending = h_inject_pending;
    rdnss = h_inject_rdnss;
    dnssl = h_inject_dnssl;
    dnssl_len = h_inject_dnssl_len;
    lifetime = h_inject_lifetime;

    /* Clear first: the injected producer has its own Permit(). */
    h_inject_pending = NULL;
    h_inject_rdnss = NULL;
    h_inject_dnssl = NULL;

    if (rdnss != NULL)
        ami_ns_ra_rdnss(pending, rdnss, lifetime);
    else
        ami_ns_ra_dnssl(pending, dnssl, dnssl_len, lifetime);
}


static int h_address_is(const NXD_ADDRESS *address, ULONG last)
{
    return address->nxd_ip_version == NX_IP_VERSION_V6 &&
           address->nxd_ip_address.v6[0] == 0x20010db8UL &&
           address->nxd_ip_address.v6[1] == 0UL &&
           address->nxd_ip_address.v6[2] == 0x53UL &&
           address->nxd_ip_address.v6[3] == last;
}


static void h_case_rdnss_arrives_after_snapshot(void)
{
    AmiNsRaPending pending;
    AmiNsRaSnapshot first;
    AmiNsRaSnapshot second;
    const ULONG one[4] = {0x20010db8UL, 0UL, 0x53UL, 1UL};
    const ULONG two[4] = {0x20010db8UL, 0UL, 0x53UL, 2UL};

    memset(&pending, 0, sizeof(pending));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));

    ami_ns_ra_rdnss(&pending, one, 600UL);

    h_inject_pending = &pending;
    h_inject_rdnss = two;
    h_inject_lifetime = 600UL;

    h_check(ami_ns_ra_snapshot(&pending, &first),
            "the first RDNSS advertisement is pending");
    h_check(first.rdnss_pending && first.rdnss_count == 1,
            "the first snapshot has one coherent server");
    h_check(h_address_is(&first.rdnss[0], 1UL),
            "the first snapshot was not overwritten by the second option");
    h_check(pending.rdnss_pending,
            "an RDNSS option arriving after the snapshot remains pending");

    h_check(ami_ns_ra_snapshot(&pending, &second),
            "the injected RDNSS option is consumed on the next pass");
    h_check(second.rdnss_count == 2 &&
            h_address_is(&second.rdnss[0], 1UL) &&
            h_address_is(&second.rdnss[1], 2UL),
            "the next snapshot contains the complete new server set");
    h_check(!pending.rdnss_pending,
            "RDNSS pending clears only after its own snapshot");
}


static void h_case_dnssl_arrives_after_snapshot(void)
{
    AmiNsRaPending pending;
    AmiNsRaSnapshot first;
    AmiNsRaSnapshot second;
    static const UCHAR old_list[] = {3, 'o', 'l', 'd', 0};
    static const UCHAR new_list[] = {3, 'n', 'e', 'w', 0};

    memset(&pending, 0, sizeof(pending));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));

    ami_ns_ra_dnssl(&pending, old_list, (UINT)sizeof(old_list), 300UL);

    h_inject_pending = &pending;
    h_inject_dnssl = new_list;
    h_inject_dnssl_len = (UINT)sizeof(new_list);
    h_inject_lifetime = 900UL;

    h_check(ami_ns_ra_snapshot(&pending, &first),
            "the first DNSSL advertisement is pending");
    h_check(first.dnssl_pending &&
            first.dnssl_len == (UWORD)sizeof(old_list) &&
            first.dnssl_lifetime == 300UL &&
            memcmp(first.dnssl, old_list, sizeof(old_list)) == 0,
            "the first DNSSL snapshot is internally consistent");
    h_check(pending.dnssl_pending,
            "a DNSSL option arriving after the snapshot remains pending");

    h_check(ami_ns_ra_snapshot(&pending, &second),
            "the injected DNSSL option is consumed on the next pass");
    h_check(second.dnssl_len == (UWORD)sizeof(new_list) &&
            second.dnssl_lifetime == 900UL &&
            memcmp(second.dnssl, new_list, sizeof(new_list)) == 0,
            "the next snapshot contains one complete replacement list");
}


static void h_case_withdraw_and_limits(void)
{
    AmiNsRaPending pending;
    AmiNsRaSnapshot snapshot;
    UCHAR too_long[AMI_DNSSL_MAX + 1];
    const ULONG one[4] = {0x20010db8UL, 0UL, 0x53UL, 1UL};

    memset(&pending, 0, sizeof(pending));
    memset(&snapshot, 0, sizeof(snapshot));
    memset(too_long, 1, sizeof(too_long));

    ami_ns_ra_rdnss(&pending, one, 600UL);
    (void)ami_ns_ra_snapshot(&pending, &snapshot);
    ami_ns_ra_rdnss(&pending, one, 0UL);

    h_check(ami_ns_ra_snapshot(&pending, &snapshot) &&
            snapshot.rdnss_count == 0,
            "a zero lifetime publishes an empty RDNSS set");

    ami_ns_ra_dnssl(&pending, too_long, (UINT)sizeof(too_long), 600UL);
    h_check(!ami_ns_ra_snapshot(&pending, &snapshot),
            "an overlong DNSSL option is refused whole");
}


int main(void)
{
    h_case_rdnss_arrives_after_snapshot();
    h_case_dnssl_arrives_after_snapshot();
    h_case_withdraw_and_limits();

    h_check(h_forbid_depth == 0, "all handoff critical sections are balanced");
    h_check(h_forbid_max == 1, "the handoff never nests its critical section");

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}
