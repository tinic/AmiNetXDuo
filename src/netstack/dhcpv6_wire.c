/*
 * AmiNetXDuo, the DHCPv6 decisions that are ours. See dhcpv6_wire.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "dhcpv6_wire.h"

/* RFC 4861 4.2, the flags octet of a router advertisement. */
#define AMI_RA_MANAGED              0x80U
#define AMI_RA_OTHER                0x40U

/* RFC 8415 11, DUID type codes; RFC 826, hardware types. */
#define AMI_DUID_TYPE_LL            3U
#define AMI_DUID_HW_ETHERNET        1U

#define AMI_ETHERNET_ADDR_LEN       6UL

AmiDhcpv6Action ami_dhcpv6_action_for_ra(unsigned int ra_flag)
{
    if ((ra_flag & AMI_RA_MANAGED) != 0U)
        return AMI_DHCPV6_ACT_STATEFUL;

    if ((ra_flag & AMI_RA_OTHER) != 0U)
        return AMI_DHCPV6_ACT_STATELESS;

    return AMI_DHCPV6_ACT_NONE;
}

AmiDhcpv6Action ami_dhcpv6_resume_action(unsigned int created,
                                         unsigned int started,
                                         unsigned int asked,
                                         unsigned int stateful,
                                         unsigned int selected_interface,
                                         unsigned int raised_interface)
{
    if (!created || started || !asked ||
        selected_interface != raised_interface)
        return AMI_DHCPV6_ACT_NONE;

    return stateful ? AMI_DHCPV6_ACT_STATEFUL : AMI_DHCPV6_ACT_STATELESS;
}

unsigned int ami_dhcpv6_inform_reply_seen(unsigned long responses,
                                          unsigned long *watermark)
{
    unsigned int seen;

    if (watermark == 0)
        return 0;

    seen = (responses != *watermark) ? 1U : 0U;
    *watermark = responses;

    return seen;
}

AmiDhcpv6OptionChange ami_dhcpv6_option_change(
    unsigned int reached_bound, unsigned int left_information_request,
    unsigned int reached_init, unsigned int information_reply_seen)
{
    if (reached_bound)
        return AMI_DHCPV6_OPTIONS_REPLACE;

    /* A successful stateless exchange returns to INIT by design. A failed
       one must preserve the last coherent response, if any. */
    if (left_information_request)
        return information_reply_seen ? AMI_DHCPV6_OPTIONS_REPLACE
                                      : AMI_DHCPV6_OPTIONS_KEEP;

    if (reached_init)
        return AMI_DHCPV6_OPTIONS_WITHDRAW;

    return AMI_DHCPV6_OPTIONS_KEEP;
}

unsigned long ami_dhcpv6_duid_ll(const unsigned char *mac, unsigned long maclen,
                                 unsigned char *out, unsigned long size)
{
    unsigned long i;
    unsigned char any = 0;

    if (mac == 0 || out == 0)
        return 0;

    if (maclen != AMI_ETHERNET_ADDR_LEN)
        return 0;

    if (size < (unsigned long)AMI_DHCPV6_DUID_LL_LEN)
        return 0;

    /*
     * An all-zero address is refused rather than encoded. It is what a card
     * that has not answered S2_GETSTATIONADDRESS yet reads as, and a DUID of
     * 00:03:00:01:00:00:00:00:00:00 is an identity every such machine on the
     * link would share -- which is the one failure a DUID exists to prevent.
     */
    for (i = 0; i < maclen; i++)
        any = (unsigned char)(any | mac[i]);

    if (any == 0)
        return 0;

    out[0] = (unsigned char)((AMI_DUID_TYPE_LL >> 8) & 0xFFU);
    out[1] = (unsigned char)(AMI_DUID_TYPE_LL & 0xFFU);
    out[2] = (unsigned char)((AMI_DUID_HW_ETHERNET >> 8) & 0xFFU);
    out[3] = (unsigned char)(AMI_DUID_HW_ETHERNET & 0xFFU);

    for (i = 0; i < maclen; i++)
        out[4 + i] = mac[i];

    return (unsigned long)AMI_DHCPV6_DUID_LL_LEN;
}
