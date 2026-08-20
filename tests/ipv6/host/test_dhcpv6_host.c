/*
 * AmiNetXDuo, the DHCPv6 decisions that are ours rather than NetX Duo's,
 * driven directly.
 *
 * WHAT IS UNDER TEST, AND WHY IT IS WORTH A HOST BINARY
 *
 *   1. The router advertisement's M and O flags decide whether an interface
 *      with CONFIGURE6=AUTO asks a DHCPv6 server for an address, for the rest
 *      of the configuration, or for nothing.  The lab router sets M and O
 *      together and cannot be reconfigured, so the emulator arm can only ever
 *      exercise one of the three combinations.  All eight values of the two
 *      bits are here, plus the six bits that must be ignored.
 *
 *   2. The DUID this machine identifies itself by.  A DUID is meant to be the
 *      same on the next boot as on this one, and nothing on the wire shows
 *      that until the next boot: a capture of one exchange looks identical
 *      whether the identity is stable or freshly invented.  So the wire form
 *      is pinned here, byte for byte, and src/netstack/netstack_dhcpv6.c
 *      checks what the vendored client actually built against the same
 *      function.  A DUID-LLT would fail the first assertion below, which is
 *      the point: on a machine with no battery-backed clock, a DUID-LLT is a
 *      new identity every boot.
 *
 * These are src/netstack/dhcpv6_wire.c, compiled into this binary. Nothing is
 * stubbed and nothing is reimplemented here; the expected bytes are written
 * out from RFC 8415 11.4 rather than from the implementation.
 *
 * The DUID in test_duid_matches_the_wire() is the one a real dnsmasq matched
 * during the emulator arm, so the two halves of the verification are pinned
 * to the same ten octets.
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

/* --------------------------------------------------- the M and O flags ---- */

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

    /*
     * M AND O, which is what the lab router advertises and what most managed
     * networks advertise: STATEFUL, not both.  A client that answered this
     * with a Solicit and an Information-Request would ask one question twice
     * and merge two answers into one resolver.
     */
    CHECK(ami_dhcpv6_action_for_ra(0xC0U) == AMI_DHCPV6_ACT_STATEFUL);

    /*
     * The other six bits belong to other RFCs -- H (RFC 6275), Prf and P (RFC
     * 4191, RFC 4389) and the reserved bit -- and must change nothing.  A
     * router setting router-preference high is common; reading it as a
     * request for DHCPv6 would put every such machine on a network that has
     * no DHCPv6 server into a retransmission loop.
     */
    CHECK(ami_dhcpv6_action_for_ra(0x3FU) == AMI_DHCPV6_ACT_NONE);
    CHECK(ami_dhcpv6_action_for_ra(0x7FU) == AMI_DHCPV6_ACT_STATELESS);
    CHECK(ami_dhcpv6_action_for_ra(0xBFU) == AMI_DHCPV6_ACT_STATEFUL);
    CHECK(ami_dhcpv6_action_for_ra(0xFFU) == AMI_DHCPV6_ACT_STATEFUL);
}

/* ------------------------------------------------------ link lifecycle ---- */

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

/*
 * The same lifecycle again, but with the fourth argument coming from the
 * counter the stack actually has rather than from a literal.
 *
 * nx_dhcpv6_inform_req_responses is cumulative and is never reset, so the
 * KEEP branch above was unreachable in the built stack the moment one
 * stateless exchange succeeded: every later exchange, successful or not, read
 * as answered.  This drives the sequence that produced it -- a success, a
 * link flap, then a failure on a network with no DHCPv6 server -- so the KEEP
 * arm is asserted through the mechanism instead of past it.
 */
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

    /*
     * Network B has no DHCPv6 server.  The retransmissions are exhausted and
     * the client returns to INIT without incrementing, so this exchange was
     * not answered even though the lifetime total is still 1.
     */
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

    /*
     * A discard and rebuild memsets the whole client, so the counter restarts
     * at zero.  The watermark restarts with it at the create; without that
     * reset the first transition after a rebuild would read 0 against a
     * stale 2 and answer "replied" for an exchange that had not happened.
     */
    responses = 0;
    seen = 0;
    reply = ami_dhcpv6_inform_reply_seen(responses, &seen);
    CHECK(reply == 0);

    CHECK(ami_dhcpv6_inform_reply_seen(1, NULL) == 0);
}

/* ------------------------------------------------------------- the DUID ---- */

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

    /*
     * DUID-LLT is type 1 and carries four octets of timestamp between the
     * hardware type and the address, so it is 14 octets and begins 00 01.
     * Spelled out because it is the choice this is not: on a machine whose
     * clock reads 1978 every boot the timestamp is invented afresh each time,
     * and the machine gets a new address every reboot.
     */
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

    /*
     * All zeroes is what a card that has not answered S2_GETSTATIONADDRESS
     * reads as, and it is the one value that would give every such machine on
     * the link the same identity -- so it is refused rather than encoded.
     */
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
