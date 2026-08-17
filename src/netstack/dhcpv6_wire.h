/*
 * AmiNetXDuo, the two DHCPv6 decisions that are neither NetX Duo's nor the
 * network's.
 *
 * Split out of netstack_dhcpv6.c so that both the stack and a host test
 * compile the same code. Plain C types and no includes: this file is reached
 * from an m68k build that has ThreadX and NetX Duo underneath it and from a
 * host binary that has neither.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AMINETXDUO_DHCPV6_WIRE_H
#define AMINETXDUO_DHCPV6_WIRE_H

#ifdef __cplusplus
extern "C" {
#endif

/* What a router advertisement is asking this machine to do about DHCPv6. */
typedef enum {
    AMI_DHCPV6_ACT_NONE = 0,    /* neither flag set: SLAAC and nothing else */
    AMI_DHCPV6_ACT_STATEFUL,    /* M: Solicit, and take an address          */
    AMI_DHCPV6_ACT_STATELESS    /* O without M: Information-Request only    */
} AmiDhcpv6Action;

/*
 * RFC 4861 4.2's M and O flags, from the flags octet of the advertisement as
 * NetX Duo hands it over.
 *
 * M implies O and the two never both fire: RFC 8415 4.3 has the stateful
 * exchange return the other configuration options in the same Reply, so a
 * client that has M does not also send an Information-Request. Doing both
 * would ask one question twice and put two sets of name servers into one
 * resolver.
 *
 * Every other bit in the octet -- H (home agent), Prf (router preference),
 * P and the reserved bit -- is somebody else's and is ignored here.
 */
AmiDhcpv6Action ami_dhcpv6_action_for_ra(unsigned int ra_flag);

/*
 * The DUID-LL of RFC 8415 11.4, on the wire: two octets of DUID type (3), two
 * of hardware type (1 for Ethernet, RFC 826), then the link-layer address.
 * Network byte order, which for these fields is written out here rather than
 * assumed from the target.
 *
 * Returns the number of octets written, or 0 when `mac` is NULL, `maclen` is
 * not a length this writes (6, an Ethernet address), the address is all
 * zeroes -- a card that has not been read yet, and an identity two such
 * machines would share -- or `out` cannot hold the result.
 *
 * netstack_dhcpv6.c's file header says why DUID-LL and not DUID-LLT.
 */
unsigned long ami_dhcpv6_duid_ll(const unsigned char *mac, unsigned long maclen,
                                 unsigned char *out, unsigned long size);

/* What ami_dhcpv6_duid_ll() writes for an Ethernet address. */
#define AMI_DHCPV6_DUID_LL_LEN      10

#ifdef __cplusplus
}
#endif

#endif /* AMINETXDUO_DHCPV6_WIRE_H */
