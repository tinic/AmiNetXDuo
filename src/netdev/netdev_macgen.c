/*
 * The derived station address.  See netdev_macgen.h for why it exists.
 *
 * SPDX-License-Identifier: MIT
 */

#include "netdev_macgen.h"

BOOL netdev_mac_usable(const UBYTE *mac)
{
    UBYTE ored  = 0;
    UBYTE anded = 0xff;
    UWORD i;

    for (i = 0; i < NETDEV_ADDR_LEN; i++)
    {
        ored  |= mac[i];
        anded &= mac[i];
    }

    /*
     * All-zero and all-ones are both "the window is not answering" rather
     * than an address, and a group bit in octet 0 is not a station address
     * whatever else it is: the DP8390 will not match unicast against it and
     * every frame transmitted carries a multicast source.  This is the test
     * ed.c has made at ed.c:406 since it was written; the NE2000 path did not
     * make it at all.
     */
    return (BOOL)(ored != 0 && anded != 0xff && (mac[0] & 1u) == 0);
}

VOID netdev_mac_derive(const UBYTE *fp, UWORD len, UBYTE *mac)
{
    /*
     * ONE CRC-32, the one netdev_mcaf.c already carries for the multicast
     * filter, rather than a second hash implementation.  Thirty-two bits is
     * the whole of the entropy and the address is honest about it: two
     * fingerprints that differ anywhere collide with probability 2^-32, and
     * two machines whose fingerprints are identical collide with certainty.
     * Widening the output would hide the second case, not fix it.
     */
    ULONG crc = netdev_ether_crc32_be(fp, len);

    mac[0] = (UBYTE)NETDEV_MAC_LOCAL;
    mac[1] = (UBYTE)NETDEV_MAC_TAG;
    mac[2] = (UBYTE)(crc >> 24);
    mac[3] = (UBYTE)(crc >> 16);
    mac[4] = (UBYTE)(crc >> 8);
    mac[5] = (UBYTE)crc;
}
