/*
 * Adapted from NetBSD sys/net/if_ethersubr.c (ether_crc32_be) and
 * sys/dev/ic/dp8390.c (dp8390_getmcaf).
 *
 * Copyright (c) 1994, 1995 Charles M. Hannum.  All rights reserved.
 *
 * Copyright (C) 1993, David Greenman.  This software may be used, modified,
 * copied, distributed, and sold, in both source and binary form provided that
 * the above copyright and these terms are retained.  Under no circumstances is
 * the author responsible for the proper functioning of this software, nor does
 * the author assume any responsibility for damages incurred with its use.
 *
 * AmiNetXDuo: the arithmetic is unchanged.  dp8390_getmcaf() walked a
 * NetBSD ether_multi list and wrote the filter; here the shell keeps the list
 * and calls netdev_mar_set() per address, because SANA-II hands joins in one
 * at a time and the list has to be reference counted per opener.
 *
 * SPDX-License-Identifier: MIT AND BSD-2-Clause-NetBSD
 */

#include "netdev_mcaf.h"

/*
 * ETHER_CRC_POLY_BE is 0x04c11db6 in NetBSD and the carry supplies bit 0,
 * which is the same thing as xoring 0x04c11db7 after the shift.
 */
ULONG netdev_ether_crc32_be(const UBYTE *addr, UWORD len)
{
    ULONG crc = 0xffffffffUL;
    UWORD i;

    for (i = 0; i < len; i++)
    {
        ULONG c = addr[i];
        UWORD j;

        for (j = 0; j < 8; j++)
        {
            ULONG carry = ((crc & 0x80000000UL) ? 1UL : 0UL) ^ (c & 1UL);

            crc <<= 1;
            crc &= 0xffffffffUL;
            c   >>= 1;
            if (carry != 0)
                crc = (crc ^ 0x04c11db6UL) | carry;
        }
    }

    return crc & 0xffffffffUL;
}

VOID netdev_mar_clear(UBYTE mar[8])
{
    UWORD i;

    for (i = 0; i < 8; i++)
        mar[i] = 0;
}

VOID netdev_mar_all(UBYTE mar[8])
{
    UWORD i;

    for (i = 0; i < 8; i++)
        mar[i] = 0xff;
}

/*
 * The top six bits of the CRC index the 64-bit filter: bits 5..3 pick the
 * byte, bits 2..0 the bit within it.
 */
VOID netdev_mar_set(UBYTE mar[8], const UBYTE addr[NETDEV_ADDR_LEN])
{
    ULONG crc = netdev_ether_crc32_be(addr, NETDEV_ADDR_LEN) >> 26;

    mar[crc >> 3] |= (UBYTE)(1u << (crc & 7u));
}
