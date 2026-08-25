/*
 * AmiNetXDuo, the DHCPv6 decisions that are ours rather than NetX Duo's,
 * driven directly.
 *
 * SPDX-License-Identifier: MIT
 */

#include "dhcpv6_wire.h"

#include <stdio.h>
#include <string.h>

static unsigned long h_checks;
static unsigned long h_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        h_checks++;                                                          \
        if (!(cond)) {                                                       \
            h_failures++;                                                    \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

static void test_ra_flags(void)
{
    printf("dhcpv6: the M and O flags of a router advertisement\n");

    /* Neither: SLAAC and nothing else.  This is the ordinary home router and
       it must produce no DHCPv6 traffic at all. */
    CHECK(ami_dhcpv6_action_for_ra(0x00U) == AMI_DHCPV6_ACT_NONE);

    /* O alone: the addresses come from the prefix, everything else from a
       server.  RFC 8415 4.3, the stateless mode. */
    CHECK(ami_dhcpv6_action_for_ra(0x40U) == AMI_DHCPV6_ACT_STATELESS);

    /* M alone: stateful. */
    CHECK(ami_dhcpv6_action_for_ra(0x80U) == AMI_DHCPV6_ACT_STATEFUL);

    CHECK(ami_dhcpv6_action_for_ra(0xC0U) == AMI_DHCPV6_ACT_STATEFUL);

    CHECK(ami_dhcpv6_action_for_ra(0x3FU) == AMI_DHCPV6_ACT_NONE);
    CHECK(ami_dhcpv6_action_for_ra(0x7FU) == AMI_DHCPV6_ACT_STATELESS);
    CHECK(ami_dhcpv6_action_for_ra(0xBFU) == AMI_DHCPV6_ACT_STATEFUL);
    CHECK(ami_dhcpv6_action_for_ra(0xFFU) == AMI_DHCPV6_ACT_STATEFUL);
}

static void test_interface_resume(void)
{
    printf("dhcpv6: resuming after an interface returns\n");

    CHECK(ami_dhcpv6_resume_action(1, 0, 1, 1, 1, 1) ==
          AMI_DHCPV6_ACT_STATEFUL);
    CHECK(ami_dhcpv6_resume_action(1, 0, 1, 0, 1, 1) ==
          AMI_DHCPV6_ACT_STATELESS);

    /* The original deferred event owns a client not created yet. */
    CHECK(ami_dhcpv6_resume_action(0, 0, 1, 1, 1, 1) ==
          AMI_DHCPV6_ACT_NONE);

    /* Do not duplicate a request on a client that is still running. */
    CHECK(ami_dhcpv6_resume_action(1, 1, 1, 1, 1, 1) ==
          AMI_DHCPV6_ACT_NONE);

    /* AUTO has not chosen stateful or stateless until a router asks. */
    CHECK(ami_dhcpv6_resume_action(1, 0, 0, 1, 1, 1) ==
          AMI_DHCPV6_ACT_NONE);

    /* Raising another interface must not restart this client's exchange. */
    CHECK(ami_dhcpv6_resume_action(1, 0, 1, 1, 1, 0) ==
          AMI_DHCPV6_ACT_NONE);
}

static void test_option_lifecycle(void)
{
    printf("dhcpv6: resolver option lifecycle\n");

    CHECK(ami_dhcpv6_option_change(1, 0, 0, 0) ==
          AMI_DHCPV6_OPTIONS_REPLACE);
    CHECK(ami_dhcpv6_option_change(0, 1, 1, 1) ==
          AMI_DHCPV6_OPTIONS_REPLACE);

    /* A failed Information-Request also returns to INIT, but supplies no
       replacement and therefore cannot erase the last coherent response. */
    CHECK(ami_dhcpv6_option_change(0, 1, 1, 0) ==
          AMI_DHCPV6_OPTIONS_KEEP);

    CHECK(ami_dhcpv6_option_change(0, 0, 1, 0) ==
          AMI_DHCPV6_OPTIONS_WITHDRAW);
    CHECK(ami_dhcpv6_option_change(0, 0, 0, 0) ==
          AMI_DHCPV6_OPTIONS_KEEP);
}

static void test_inform_reply_is_about_this_exchange(void)
{
    unsigned long responses = 0;    /* the client's own field */
    unsigned long seen = 0;         /* ns_Dhcpv6InformSeen */
    unsigned int  reply;

    printf("dhcpv6: an Information-Request reply counts once\n");

    /* Network A: the exchange is answered, so the options are replaced. */
    responses++;
    reply = ami_dhcpv6_inform_reply_seen(responses, &seen);
    CHECK(reply == 1);
    CHECK(ami_dhcpv6_option_change(0, 1, 1, reply) ==
          AMI_DHCPV6_OPTIONS_REPLACE);

    /* Link down.  The transition is classified like any other, which is what
       keeps the watermark level with the counter. */
    reply = ami_dhcpv6_inform_reply_seen(responses, &seen);
    CHECK(reply == 0);

    reply = ami_dhcpv6_inform_reply_seen(responses, &seen);
    CHECK(reply == 0);
    CHECK(ami_dhcpv6_option_change(0, 1, 1, reply) ==
          AMI_DHCPV6_OPTIONS_KEEP);

    /* And a later success on network B is still seen as one. */
    responses++;
    reply = ami_dhcpv6_inform_reply_seen(responses, &seen);
    CHECK(reply == 1);
    CHECK(ami_dhcpv6_option_change(0, 1, 1, reply) ==
          AMI_DHCPV6_OPTIONS_REPLACE);

    responses = 0;
    seen = 0;
    reply = ami_dhcpv6_inform_reply_seen(responses, &seen);
    CHECK(reply == 0);

    CHECK(ami_dhcpv6_inform_reply_seen(1, NULL) == 0);
}

static void test_duid_matches_the_wire(void)
{
    /* 00:80:10:49:44:36, the address the a2065's LANCE puts on the wire in
       the emulator arm.  dnsmasq matched DUID 00:03:00:01:00:80:10:49:44:36
       against this machine, which is the ten octets below. */
    static const unsigned char mac[6] =
        { 0x00, 0x80, 0x10, 0x49, 0x44, 0x36 };
    static const unsigned char want[AMI_DHCPV6_DUID_LL_LEN] =
    {
        0x00, 0x03,                             /* DUID-LL, RFC 8415 11.4  */
        0x00, 0x01,                             /* Ethernet, RFC 826       */
        0x00, 0x80, 0x10, 0x49, 0x44, 0x36      /* the link-layer address  */
    };
    unsigned char got[AMI_DHCPV6_DUID_LL_LEN];

    printf("dhcpv6: the DUID on the wire\n");

    memset(got, 0xAA, sizeof(got));

    CHECK(ami_dhcpv6_duid_ll(mac, 6UL, got, sizeof(got)) ==
          (unsigned long)AMI_DHCPV6_DUID_LL_LEN);
    CHECK(memcmp(got, want, sizeof(want)) == 0);

    CHECK(got[0] == 0x00 && got[1] == 0x03);
    CHECK(AMI_DHCPV6_DUID_LL_LEN == 10);

    /* Two machines differ only where their addresses do, and in that octet. */
    {
        static const unsigned char other[6] =
            { 0x00, 0x80, 0x10, 0x49, 0x44, 0x37 };
        unsigned char got2[AMI_DHCPV6_DUID_LL_LEN];

        CHECK(ami_dhcpv6_duid_ll(other, 6UL, got2, sizeof(got2)) ==
              (unsigned long)AMI_DHCPV6_DUID_LL_LEN);
        CHECK(memcmp(got, got2, sizeof(got)) != 0);
        CHECK(memcmp(got, got2, 9) == 0);
    }

    /* The same address twice is the same DUID.  This is the whole property. */
    {
        unsigned char again[AMI_DHCPV6_DUID_LL_LEN];

        memset(again, 0x55, sizeof(again));
        CHECK(ami_dhcpv6_duid_ll(mac, 6UL, again, sizeof(again)) ==
              (unsigned long)AMI_DHCPV6_DUID_LL_LEN);
        CHECK(memcmp(got, again, sizeof(got)) == 0);
    }
}

static void test_duid_refusals(void)
{
    static const unsigned char mac[6] =
        { 0x00, 0x80, 0x10, 0x49, 0x44, 0x36 };
    static const unsigned char zero[6] = { 0, 0, 0, 0, 0, 0 };
    unsigned char out[AMI_DHCPV6_DUID_LL_LEN];
    unsigned char guard[AMI_DHCPV6_DUID_LL_LEN + 1];

    printf("dhcpv6: what the DUID refuses\n");

    CHECK(ami_dhcpv6_duid_ll(0, 6UL, out, sizeof(out)) == 0UL);
    CHECK(ami_dhcpv6_duid_ll(mac, 6UL, 0, sizeof(out)) == 0UL);

    CHECK(ami_dhcpv6_duid_ll(zero, 6UL, out, sizeof(out)) == 0UL);

    /* A length this does not write.  EUI-64 is a real hardware type and this
       function does not produce one, so it must say so rather than guess. */
    CHECK(ami_dhcpv6_duid_ll(mac, 8UL, out, sizeof(out)) == 0UL);
    CHECK(ami_dhcpv6_duid_ll(mac, 0UL, out, sizeof(out)) == 0UL);

    /* A buffer one octet short writes nothing, rather than nine octets. */
    memset(guard, 0x5A, sizeof(guard));
    CHECK(ami_dhcpv6_duid_ll(mac, 6UL, guard,
                             (unsigned long)AMI_DHCPV6_DUID_LL_LEN - 1UL)
          == 0UL);
    {
        unsigned long i;
        int untouched = 1;

        for (i = 0; i < sizeof(guard); i++)
            if (guard[i] != 0x5A)
                untouched = 0;

        CHECK(untouched);
    }
}

int main(void)
{
    test_ra_flags();
    test_interface_resume();
    test_option_lifecycle();
    test_inform_reply_is_about_this_exchange();
    test_duid_matches_the_wire();
    test_duid_refusals();

    printf("\n%lu checks, %lu failure(s)\n", h_checks, h_failures);

    return (h_failures == 0) ? 0 : 1;
}
