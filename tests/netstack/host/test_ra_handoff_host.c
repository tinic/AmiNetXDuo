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
static ULONG           h_inject_now;
static UWORD           h_inject_iface;


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
        ami_ns_ra_rdnss(pending, h_inject_iface, rdnss, lifetime,
                        h_inject_now);
    else
        ami_ns_ra_dnssl(pending, h_inject_iface, dnssl, dnssl_len, lifetime,
                        h_inject_now);
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

    ami_ns_ra_rdnss(&pending, 0U, one, 600UL, 100UL);

    h_inject_pending = &pending;
    h_inject_rdnss = two;
    h_inject_lifetime = 600UL;
    h_inject_now = 101UL;
    h_inject_iface = 0U;

    h_check(ami_ns_ra_snapshot(&pending, &first, 101UL),
            "the first RDNSS advertisement is pending");
    h_check(first.rdnss_pending && first.rdnss_count == 1,
            "the first snapshot has one coherent server");
    h_check(h_address_is(&first.rdnss[0], 1UL),
            "the first snapshot was not overwritten by the second option");
    h_check(pending.rdnss_pending,
            "an RDNSS option arriving after the snapshot remains pending");

    h_check(ami_ns_ra_snapshot(&pending, &second, 101UL),
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

    ami_ns_ra_dnssl(&pending, 0U, old_list, (UINT)sizeof(old_list), 300UL,
                    200UL);

    h_inject_pending = &pending;
    h_inject_dnssl = new_list;
    h_inject_dnssl_len = (UINT)sizeof(new_list);
    h_inject_lifetime = 900UL;
    h_inject_now = 201UL;
    h_inject_iface = 0U;

    h_check(ami_ns_ra_snapshot(&pending, &first, 201UL),
            "the first DNSSL advertisement is pending");
    h_check(first.dnssl_pending && first.dnssl_count == 1U &&
            strcmp(first.dnssl[0], "old") == 0,
            "the first DNSSL snapshot is internally consistent");
    h_check(pending.dnssl_pending,
            "a DNSSL option arriving after the snapshot remains pending");

    h_check(ami_ns_ra_snapshot(&pending, &second, 201UL),
            "the injected DNSSL option is consumed on the next pass");
    h_check(second.dnssl_count == 2U &&
            strcmp(second.dnssl[0], "old") == 0 &&
            strcmp(second.dnssl[1], "new") == 0,
            "the next snapshot retains independently live domains");
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

    ami_ns_ra_rdnss(&pending, 0U, one, 600UL, 0UL);
    (void)ami_ns_ra_snapshot(&pending, &snapshot, 0UL);
    ami_ns_ra_rdnss(&pending, 0U, one, 0UL, 1UL);

    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 1UL) &&
            snapshot.rdnss_count == 0,
            "a zero lifetime publishes an empty RDNSS set");

    ami_ns_ra_dnssl(&pending, 0U, too_long, (UINT)sizeof(too_long), 600UL,
                    0UL);
    h_check(!ami_ns_ra_snapshot(&pending, &snapshot, 0UL),
            "an overlong DNSSL option is refused whole");
}


static void h_case_dnssl_interfaces_own_independently(void)
{
    AmiNsRaPending pending;
    AmiNsRaSnapshot snapshot;
    static const UCHAR shared[] = {
        6, 's', 'h', 'a', 'r', 'e', 'd', 4, 't', 'e', 's', 't', 0
    };
    static const UCHAR other[] = {
        5, 'o', 't', 'h', 'e', 'r', 4, 't', 'e', 's', 't', 0
    };

    memset(&pending, 0, sizeof(pending));
    memset(&snapshot, 0, sizeof(snapshot));

    ami_ns_ra_dnssl(&pending, 0U, shared, (UINT)sizeof(shared), 300UL, 1UL);
    ami_ns_ra_dnssl(&pending, 1U, shared, (UINT)sizeof(shared), 600UL, 1UL);
    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 1UL) &&
            snapshot.dnssl_count == 1U &&
            strcmp(snapshot.dnssl[0], "shared.test") == 0,
            "the same suffix on two interfaces appears once in the union");

    ami_ns_ra_dnssl(&pending, 0U, shared, (UINT)sizeof(shared), 0UL, 2UL);
    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 2UL) &&
            snapshot.dnssl_count == 1U &&
            strcmp(snapshot.dnssl[0], "shared.test") == 0,
            "one interface cannot withdraw another interface's suffix");

    ami_ns_ra_dnssl(&pending, 0U, other, (UINT)sizeof(other), 300UL, 3UL);
    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 3UL) &&
            snapshot.dnssl_count == 2U &&
            strcmp(snapshot.dnssl[0], "other.test") == 0 &&
            strcmp(snapshot.dnssl[1], "shared.test") == 0,
            "distinct interface suffixes form one resolver union");

    ami_ns_ra_dnssl(&pending, 1U, shared, (UINT)sizeof(shared), 0UL, 4UL);
    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 4UL) &&
            snapshot.dnssl_count == 1U &&
            strcmp(snapshot.dnssl[0], "other.test") == 0,
            "the last owner withdrawal removes only that suffix");
}


static void h_case_dnssl_lifetime_expires_and_refreshes(void)
{
    AmiNsRaPending pending;
    AmiNsRaSnapshot snapshot;
    static const UCHAR finite[] = {
        6, 'f', 'i', 'n', 'i', 't', 'e', 4, 't', 'e', 's', 't', 0
    };

    memset(&pending, 0, sizeof(pending));
    memset(&snapshot, 0, sizeof(snapshot));

    ami_ns_ra_dnssl(&pending, 0U, finite, (UINT)sizeof(finite), 10UL, 100UL);
    (void)ami_ns_ra_snapshot(&pending, &snapshot, 100UL);
    h_check(!ami_ns_ra_needs_snapshot(&pending, 599UL),
            "DNSSL remains valid until its complete lifetime");

    ami_ns_ra_dnssl(&pending, 0U, finite, (UINT)sizeof(finite), 10UL, 400UL);
    h_check(!ami_ns_ra_needs_snapshot(&pending, 899UL),
            "a repeated DNSSL advertisement refreshes its lifetime");
    h_check(ami_ns_ra_needs_snapshot(&pending, 900UL),
            "DNSSL expiry wakes a report without a new packet");
    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 900UL) &&
            snapshot.dnssl_pending && snapshot.dnssl_count == 0U,
            "an expired DNSSL suffix is published as a withdrawal");
}


static void h_case_dnssl_expiry_preserves_other_interface(void)
{
    AmiNsRaPending pending;
    AmiNsRaSnapshot snapshot;
    static const UCHAR shared[] = {
        6, 's', 'h', 'a', 'r', 'e', 'd', 4, 't', 'e', 's', 't', 0
    };

    memset(&pending, 0, sizeof(pending));
    memset(&snapshot, 0, sizeof(snapshot));

    ami_ns_ra_dnssl(&pending, 0U, shared, (UINT)sizeof(shared), 1UL,
                    (ULONG)~0UL - 20UL);
    ami_ns_ra_dnssl(&pending, 1U, shared, (UINT)sizeof(shared), (ULONG)~0UL,
                    10UL);
    (void)ami_ns_ra_snapshot(&pending, &snapshot, (ULONG)~0UL - 20UL);

    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 29UL) &&
            snapshot.dnssl_count == 1U &&
            strcmp(snapshot.dnssl[0], "shared.test") == 0,
            "wrapped expiry on one interface preserves an infinite owner");
    h_check(!ami_ns_ra_needs_snapshot(&pending, (ULONG)~0UL),
            "the RFC infinite DNSSL lifetime does not expire");
}


static void h_case_rdnss_lifetime_expires_and_refreshes(void)
{
    AmiNsRaPending pending;
    AmiNsRaSnapshot snapshot;
    const ULONG one[4] = {0x20010db8UL, 0UL, 0x53UL, 1UL};

    memset(&pending, 0, sizeof(pending));
    memset(&snapshot, 0, sizeof(snapshot));

    ami_ns_ra_rdnss(&pending, 0U, one, 10UL, 100UL);
    (void)ami_ns_ra_snapshot(&pending, &snapshot, 100UL);

    h_check(!ami_ns_ra_needs_snapshot(&pending, 599UL),
            "RDNSS remains valid until its complete lifetime");

    ami_ns_ra_rdnss(&pending, 0U, one, 10UL, 400UL);
    h_check(!ami_ns_ra_needs_snapshot(&pending, 899UL),
            "a repeated RDNSS advertisement refreshes its lifetime");
    h_check(ami_ns_ra_needs_snapshot(&pending, 900UL),
            "RDNSS expiry wakes a report even without a new packet");
    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 900UL) &&
            snapshot.rdnss_pending && snapshot.rdnss_count == 0,
            "an expired RDNSS server is published as a withdrawal");

}


static void h_case_infinite_and_wrapped_time(void)
{
    AmiNsRaPending pending;
    AmiNsRaSnapshot snapshot;
    const ULONG one[4] = {0x20010db8UL, 0UL, 0x53UL, 1UL};

    memset(&pending, 0, sizeof(pending));
    memset(&snapshot, 0, sizeof(snapshot));

    ami_ns_ra_rdnss(&pending, 0U, one, (ULONG)~0UL, 20UL);
    (void)ami_ns_ra_snapshot(&pending, &snapshot, 20UL);
    h_check(!ami_ns_ra_needs_snapshot(&pending, (ULONG)~0UL),
            "the RFC infinite lifetime does not expire");

    ami_ns_ra_rdnss(&pending, 0U, one, 1UL, (ULONG)~0UL - 20UL);
    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 29UL) &&
            snapshot.rdnss_count == 0,
            "RDNSS expiry survives the ThreadX tick counter wrapping");
}


static void h_case_rdnss_interfaces_own_independently(void)
{
    AmiNsRaPending pending;
    AmiNsRaSnapshot snapshot;
    const ULONG one[4] = {0x20010db8UL, 0UL, 0x53UL, 1UL};
    const ULONG two[4] = {0x20010db8UL, 0UL, 0x53UL, 2UL};

    memset(&pending, 0, sizeof(pending));
    memset(&snapshot, 0, sizeof(snapshot));

    ami_ns_ra_rdnss(&pending, 0U, one, 10UL, 0UL);
    ami_ns_ra_rdnss(&pending, 1U, one, 20UL, 0UL);
    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 0UL) &&
            snapshot.rdnss_count == 1 &&
            h_address_is(&snapshot.rdnss[0], 1UL),
            "the same server on two interfaces appears once in the resolver");

    ami_ns_ra_rdnss(&pending, 0U, one, 0UL, 1UL);
    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 1UL) &&
            snapshot.rdnss_count == 1 &&
            h_address_is(&snapshot.rdnss[0], 1UL),
            "withdrawal on one interface preserves the other advertisement");

    ami_ns_ra_rdnss(&pending, 0U, two, 30UL, 2UL);
    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 2UL) &&
            snapshot.rdnss_count == 2,
            "distinct interface advertisements form one resolver union");

    h_check(ami_ns_ra_snapshot(&pending, &snapshot, 1600UL) &&
            snapshot.rdnss_count == 0,
            "each interface advertisement expires on its own lifetime");
}


int main(void)
{
    h_case_rdnss_arrives_after_snapshot();
    h_case_dnssl_arrives_after_snapshot();
    h_case_withdraw_and_limits();
    h_case_rdnss_lifetime_expires_and_refreshes();
    h_case_infinite_and_wrapped_time();
    h_case_rdnss_interfaces_own_independently();
    h_case_dnssl_interfaces_own_independently();
    h_case_dnssl_lifetime_expires_and_refreshes();
    h_case_dnssl_expiry_preserves_other_interface();

    h_check(h_forbid_depth == 0, "all handoff critical sections are balanced");
    h_check(h_forbid_max == 1, "the handoff never nests its critical section");

    printf("%lu checks, %lu failures\n", h_checks, h_failures);
    return (h_failures == 0) ? 0 : 1;
}
